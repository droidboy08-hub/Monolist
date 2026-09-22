#include "lyrics.h"
#include "appdatabase.h"
#include "playbackcontroller.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSqlError>
#include <QSqlQuery>
#include <QUrl>
#include <QVariant>

#include <algorithm>

namespace {

// A line lights this far ahead of the audio, so it shows as it is sung rather
// than just after.
constexpr qint64 kLeadMs = 150;
// How far LRCLIB's length for a song may be from this recording's for its
// timing to be trusted, and for its words to be used at all, in seconds.
constexpr double kSyncTolerance = 3.0;
constexpr double kLooseTolerance = 20.0;
// How long "none found" stands before a song is looked up again.
const QString kAskAgainAfter = QStringLiteral("-3 days");

const QByteArray kUserAgent = QByteArrayLiteral("Monolist/0.1 (desktop music player)");

QString encoded(const QString &text)
{
    return QString::fromLatin1(QUrl::toPercentEncoding(text));
}

// "Daft Punk, Pharrell Williams & Nile Rodgers" -> "Daft Punk": LRCLIB lists
// the lead artist, and matches on it.
QString leadArtist(const QString &artist)
{
    static const QRegularExpression separators(QStringLiteral(R"(\s*(?:,|&|\bfeat\.?|\bft\.?)\s*)"),
                                               QRegularExpression::CaseInsensitiveOption);
    return artist.split(separators, Qt::SkipEmptyParts).value(0).trimmed();
}

// The title as a lyrics database files it: without "(Official Video)",
// "[Remastered 2011]" or "(feat. ...)", and without the artist in front, which
// video titles often have ("Artist - Title").
QString searchTitle(QString title, const QString &artist)
{
    const int dash = title.indexOf(QStringLiteral(" - "));
    const QString lead = leadArtist(artist);
    if (dash > 0 && !lead.isEmpty() && title.left(dash).contains(lead, Qt::CaseInsensitive))
        title = title.mid(dash + 3);

    static const QRegularExpression noise(QStringLiteral(
        R"(\s*[\(\[][^\)\]]*\b(?:official|video|audio|lyrics?|visuali[sz]er|mv|hd|hq|4k|remaster(?:ed)?)\b[^\)\]]*[\)\]])"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression featuring(QStringLiteral(
        R"(\s*[\(\[]\s*(?:feat\.?|ft\.?|featuring|with)\s[^\)\]]*[\)\]])"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression featuringTail(QStringLiteral(R"(\s+(?:feat\.?|ft\.?|featuring)\s.*$)"),
                                                  QRegularExpression::CaseInsensitiveOption);
    title.remove(noise);
    title.remove(featuring);
    title.remove(featuringTail);
    return title.simplified();
}

QString joinedText(const QList<LyricsModel::Line> &lines)
{
    QStringList texts;
    for (const LyricsModel::Line &line : lines)
        texts << line.text;
    return texts.join(QLatin1Char('\n'));
}

} // namespace

// --------------------------------------------------------------- LyricsModel

LyricsModel::LyricsModel(QObject *parent)
    : QAbstractListModel(parent) {}

int LyricsModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_lines.size());
}

QVariant LyricsModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_lines.size())
        return {};
    const Line &line = m_lines.at(index.row());
    switch (role) {
    case TextRole: return line.text;
    case TimeRole: return line.timeMs;
    default:       return {};
    }
}

QHash<int, QByteArray> LyricsModel::roleNames() const
{
    return { { TextRole, "text" }, { TimeRole, "timeMs" } };
}

void LyricsModel::replace(const QList<Line> &lines)
{
    beginResetModel();
    m_lines = lines;
    endResetModel();
    Q_EMIT countChanged();
}

// -------------------------------------------------------------------- Lyrics

Lyrics::Lyrics(PlaybackController *player, QObject *parent)
    : QObject(parent)
    , m_player(player)
    , m_network(new QNetworkAccessManager(this))
{
    connect(m_player, &PlaybackController::currentTrackChanged, this, &Lyrics::trackChanged);
    connect(m_player, &PlaybackController::positionChanged, this, &Lyrics::updateCurrentLine);
}

void Lyrics::setLrclibUrl(const QString &url)
{
    const QString trimmed = url.trimmed();
    if (!trimmed.isEmpty())
        m_lrclib = trimmed.endsWith(QLatin1Char('/')) ? trimmed.chopped(1) : trimmed;
}

void Lyrics::setActive(bool active)
{
    if (active == m_active)
        return;
    m_active = active;
    Q_EMIT activeChanged();
    if (m_active && m_state == QLatin1String("idle"))
        load(m_player->currentTrack(), false);
}

void Lyrics::trackChanged()
{
    const QVariantMap track = m_player->currentTrack();
    const QString videoId = track.value(QStringLiteral("sourceId")).toString();
    if (!videoId.isEmpty() && videoId == m_videoId)
        return;   // the same song; the queue moved around it
    if (m_active) {
        load(track, false);
        return;
    }
    // Nobody is looking: let the last song's lines go, and fetch nothing.
    cancelRequest();
    m_track.clear();
    m_videoId.clear();
    m_source.clear();
    m_error.clear();
    m_lines.replace({});
    setState(QStringLiteral("idle"));
    updateCurrentLine();
}

void Lyrics::lookup(const QVariantMap &track)
{
    load(track, false);
}

void Lyrics::retry()
{
    load(m_track.isEmpty() ? m_player->currentTrack() : m_track, true);
}

void Lyrics::cancelRequest()
{
    // Detached first: abort() delivers finished() at once, and its handler
    // must see itself as superseded.
    if (QNetworkReply *reply = m_request) {
        m_request = nullptr;
        reply->abort();
    }
}

void Lyrics::setState(const QString &state)
{
    if (state == m_state)
        return;
    m_state = state;
    Q_EMIT stateChanged();
}

void Lyrics::load(const QVariantMap &track, bool askAgain)
{
    cancelRequest();
    m_track = track;
    m_videoId = track.value(QStringLiteral("sourceId")).toString();
    m_source.clear();
    m_error.clear();
    m_loosePlain.clear();
    m_lrclibError.clear();
    m_lines.replace({});
    updateCurrentLine();

    if (m_videoId.isEmpty() || track.value(QStringLiteral("title")).toString().isEmpty()) {
        setState(QStringLiteral("none"));
        return;
    }
    if (!askAgain && loadStored())
        return;
    setState(QStringLiteral("loading"));
    searchLrclib(false);
}

bool Lyrics::loadStored()
{
    QSqlQuery q(AppDatabase::connection());
    q.prepare(QStringLiteral(
        "SELECT synced, plain, source, fetched_at > datetime('now', ?) FROM lyrics WHERE video_id = ?"));
    q.addBindValue(kAskAgainAfter);
    q.addBindValue(m_videoId);
    if (!q.exec() || !q.next())
        return false;
    const QString synced = q.value(0).toString();
    const QString plain = q.value(1).toString();
    const QString source = q.value(2).toString();
    const bool recent = q.value(3).toBool();
    const bool instrumental = source == QLatin1String("instrumental");
    if (synced.isEmpty() && plain.isEmpty() && !instrumental && !recent)
        return false;   // none were found, long enough ago to look again
    show(synced, plain, source, instrumental);
    return true;
}

void Lyrics::searchLrclib(bool loose)
{
    const QString artist = m_track.value(QStringLiteral("artist")).toString();
    const QString title = searchTitle(m_track.value(QStringLiteral("title")).toString(), artist);
    const QString lead = leadArtist(artist);

    // First by title and artist; if that finds nothing at all, by both as
    // words anywhere, which forgives a title spelt differently.
    QString query;
    if (loose)
        query = QStringLiteral("q=") + encoded(title + QLatin1Char(' ') + lead);
    else
        query = QStringLiteral("track_name=") + encoded(title)
              + (lead.isEmpty() ? QString() : QStringLiteral("&artist_name=") + encoded(lead));

    QNetworkRequest request(QUrl(m_lrclib + QStringLiteral("/api/search?") + query));
    request.setHeader(QNetworkRequest::UserAgentHeader, kUserAgent);
    request.setTransferTimeout(10000);
    QNetworkReply *reply = m_network->get(request);
    m_request = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply, loose]() { handleLrclib(reply, loose); });
}

void Lyrics::handleLrclib(QNetworkReply *reply, bool loose)
{
    reply->deleteLater();
    if (reply != m_request)
        return;
    m_request = nullptr;

    if (reply->error() != QNetworkReply::NoError) {
        m_lrclibError = reply->errorString();
        askYouTube();
        return;
    }

    // The closest in length wins: a song has entries for every release of it,
    // and only one with this recording's length is timed to it.
    const QJsonArray results = QJsonDocument::fromJson(reply->readAll()).array();
    const double wanted = m_track.value(QStringLiteral("durationMs")).toLongLong() / 1000.0;
    QJsonObject synced, plain, instrumental, nearest;
    double syncedGap = 1e9, plainGap = 1e9, nearestGap = 1e9;
    for (const QJsonValue &value : results) {
        const QJsonObject entry = value.toObject();
        const double gap = wanted > 0 ? qAbs(entry.value(QLatin1String("duration")).toDouble() - wanted) : 0.0;
        const bool hasSynced = !entry.value(QLatin1String("syncedLyrics")).toString().trimmed().isEmpty();
        const bool hasPlain = !entry.value(QLatin1String("plainLyrics")).toString().trimmed().isEmpty();
        if (gap <= kSyncTolerance) {
            if (hasSynced && gap < syncedGap) {
                synced = entry;
                syncedGap = gap;
            }
            if (hasPlain && gap < plainGap) {
                plain = entry;
                plainGap = gap;
            }
            if (instrumental.isEmpty() && entry.value(QLatin1String("instrumental")).toBool())
                instrumental = entry;
        } else if ((hasSynced || hasPlain) && gap <= kLooseTolerance && gap < nearestGap) {
            nearest = entry;
            nearestGap = gap;
        }
    }

    if (!synced.isEmpty()) {
        finish(synced.value(QLatin1String("syncedLyrics")).toString(),
               synced.value(QLatin1String("plainLyrics")).toString(), QStringLiteral("LRCLIB"), false);
        return;
    }
    if (!plain.isEmpty()) {
        finish({}, plain.value(QLatin1String("plainLyrics")).toString(), QStringLiteral("LRCLIB"), false);
        return;
    }
    if (!instrumental.isEmpty()) {
        finish({}, {}, {}, true);
        return;
    }
    // Another recording's lyrics: the words are right, the timing may not be.
    if (!nearest.isEmpty()) {
        m_loosePlain = nearest.value(QLatin1String("plainLyrics")).toString();
        if (m_loosePlain.trimmed().isEmpty())
            m_loosePlain = joinedText(parseLrc(nearest.value(QLatin1String("syncedLyrics")).toString()));
    }
    if (!loose && results.isEmpty()) {
        searchLrclib(true);
        return;
    }
    askYouTube();
}

void Lyrics::askYouTube()
{
    const QString videoId = m_videoId;
    m_innerTube.lyrics(videoId, [this, videoId](const QString &text, const QString &source, const QString &error) {
        if (videoId != m_videoId)
            return;   // another song since
        if (!text.isEmpty()) {
            finish({}, text, source.isEmpty() ? QStringLiteral("YouTube Music")
                                              : source + QStringLiteral(" via YouTube Music"), false);
            return;
        }
        if (!m_loosePlain.isEmpty()) {
            finish({}, m_loosePlain, QStringLiteral("LRCLIB"), false);
            return;
        }
        if (!error.isEmpty() && !m_lrclibError.isEmpty()) {
            // Neither answered, which says nothing about the song: nothing
            // is stored, and the view offers to try again.
            m_error = m_lrclibError;
            setState(QStringLiteral("error"));
            return;
        }
        finish({}, {}, {}, false);
    });
}

void Lyrics::finish(const QString &synced, const QString &plain, const QString &source, bool instrumental)
{
    QSqlQuery q(AppDatabase::connection());
    q.prepare(QStringLiteral(
        "INSERT INTO lyrics (video_id, synced, plain, source, fetched_at) VALUES (?, ?, ?, ?, datetime('now'))"
        " ON CONFLICT(video_id) DO UPDATE SET synced = excluded.synced, plain = excluded.plain,"
        " source = excluded.source, fetched_at = excluded.fetched_at"));
    q.addBindValue(m_videoId);
    q.addBindValue(AppDatabase::text(synced));
    q.addBindValue(AppDatabase::text(plain));
    q.addBindValue(instrumental ? QStringLiteral("instrumental") : AppDatabase::text(source));
    if (!q.exec())
        qWarning("Monolist: could not keep lyrics: %s", qPrintable(q.lastError().text()));
    show(synced, plain, source, instrumental);
}

void Lyrics::show(const QString &synced, const QString &plain, const QString &source, bool instrumental)
{
    QList<LyricsModel::Line> lines;
    QString state;
    if (!synced.isEmpty()) {
        lines = parseLrc(synced);
        if (!lines.isEmpty())
            state = QStringLiteral("synced");
    }
    if (state.isEmpty() && !plain.isEmpty()) {
        lines = plainLines(plain);
        if (!lines.isEmpty())
            state = QStringLiteral("plain");
    }
    if (state.isEmpty()) {
        lines.clear();
        state = instrumental ? QStringLiteral("instrumental") : QStringLiteral("none");
    }
    m_source = lines.isEmpty() ? QString() : source;
    m_error.clear();
    m_lines.replace(lines);
    // Before the current line is worked out, which only follows synced lines.
    m_state = state;
    updateCurrentLine();
    Q_EMIT stateChanged();
}

void Lyrics::updateCurrentLine()
{
    int index = -1;
    if (m_state == QLatin1String("synced") && m_videoId == m_player->currentSourceId()) {
        const qint64 at = m_player->position() + kLeadMs;
        const QList<LyricsModel::Line> &lines = m_lines.lines();
        const auto after = std::upper_bound(lines.cbegin(), lines.cend(), at,
                                            [](qint64 time, const LyricsModel::Line &line) {
                                                return time < line.timeMs;
                                            });
        index = int(after - lines.cbegin()) - 1;
    }
    if (index == m_current)
        return;
    m_current = index;
    Q_EMIT currentLineChanged();
}

void Lyrics::seekToLine(int row)
{
    const QList<LyricsModel::Line> &lines = m_lines.lines();
    if (m_state != QLatin1String("synced") || m_videoId != m_player->currentSourceId()
        || row < 0 || row >= lines.size())
        return;
    m_player->setPosition(lines.at(row).timeMs);
    if (!m_player->playing())
        m_player->play();
}

// "[01:02.34] words", several stamps to a line allowed, "[offset:+120]" honoured,
// word-level "<01:02.50>" stamps (enhanced LRC) dropped. Lines without a stamp,
// like the "[ar:]" and "[ti:]" tags, are not lyrics.
QList<LyricsModel::Line> Lyrics::parseLrc(const QString &lrc)
{
    static const QRegularExpression stamp(QStringLiteral(R"(^\[(\d{1,3}):(\d{1,2})(?:[.:](\d{1,3}))?\])"));
    static const QRegularExpression offsetTag(QStringLiteral(R"(\[offset:\s*([+-]?\d+)\s*\])"),
                                              QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression wordStamp(QStringLiteral(R"(<\d{1,3}:\d{1,2}(?:[.:]\d{1,3})?>)"));

    // A positive offset shows the lyrics sooner.
    const QRegularExpressionMatch offsetMatch = offsetTag.match(lrc);
    const qint64 offset = offsetMatch.hasMatch() ? offsetMatch.captured(1).toLongLong() : 0;

    QList<LyricsModel::Line> lines;
    for (const QString &raw : lrc.split(QLatin1Char('\n'))) {
        QString rest = raw.trimmed();
        QList<qint64> times;
        for (QRegularExpressionMatch match = stamp.match(rest); match.hasMatch(); match = stamp.match(rest)) {
            const qint64 minutes = match.captured(1).toLongLong();
            const qint64 seconds = match.captured(2).toLongLong();
            // "5" is tenths, "05" hundredths, "005" thousandths.
            const qint64 fraction = match.captured(3).leftJustified(3, QLatin1Char('0')).left(3).toLongLong();
            times << (minutes * 60 + seconds) * 1000 + fraction - offset;
            rest = rest.mid(match.capturedLength()).trimmed();
        }
        if (times.isEmpty())
            continue;
        rest.remove(wordStamp);
        const QString text = rest.simplified();
        for (const qint64 time : std::as_const(times))
            lines.append({ qMax<qint64>(0, time), text });
    }
    std::stable_sort(lines.begin(), lines.end(), [](const LyricsModel::Line &a, const LyricsModel::Line &b) {
        return a.timeMs < b.timeMs;
    });
    // Nothing but empty lines is no lyrics.
    const bool anyWords = std::any_of(lines.cbegin(), lines.cend(),
                                      [](const LyricsModel::Line &line) { return !line.text.isEmpty(); });
    return anyWords ? lines : QList<LyricsModel::Line>();
}

// Plain text as lines, a stanza break kept as one empty line.
QList<LyricsModel::Line> Lyrics::plainLines(const QString &text)
{
    QList<LyricsModel::Line> lines;
    bool afterBreak = true;   // no break before the first line
    QString cleaned = text;
    cleaned.remove(QLatin1Char('\r'));
    for (const QString &raw : cleaned.split(QLatin1Char('\n'))) {
        const QString line = raw.simplified();
        if (line.isEmpty()) {
            if (!afterBreak)
                lines.append({ -1, QString() });
            afterBreak = true;
            continue;
        }
        afterBreak = false;
        lines.append({ -1, line });
    }
    while (!lines.isEmpty() && lines.last().text.isEmpty())
        lines.removeLast();
    return lines;
}
