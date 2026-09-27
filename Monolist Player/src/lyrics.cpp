#include "lyrics.h"
#include "appdatabase.h"
#include "artistlinks.h"
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
// timing to be trusted, and for its words to be used at all, in seconds. The
// second was 20 s; at that distance an entry is as likely another song's as
// another edit's. 20 s stays for lyrics.lrclib=search, the rules from before.
constexpr double kSyncTolerance = 3.0;
constexpr double kLooseTolerance = 10.0;
constexpr double kOldLooseTolerance = 20.0;
// How long "none found" stands before a song is looked up again, and found
// lyrics before they are checked again behind the ones shown.
const QString kAskAgainAfter = QStringLiteral("-3 days");
const QString kCheckAgainAfter = QStringLiteral("-60 days");
// A provisional answer that stays on show this long (one song on repeat) is
// asked for again without waiting for the next view.
constexpr int kRecheckMs = 60 * 60 * 1000;
// LRCLIB as a whole, every request in it: a wall clock, where Qt's own timeout
// only measures silence and restarts with each request.
constexpr int kLrclibDeadlineMs = 6000;
// /api/get, the exact lookup, usually answers in one round trip (~200 ms
// here), but on a miss LRCLIB may ask elsewhere before it answers. Not
// answered this long after it was sent, /api/search is asked as well. The
// second figure covers a connection that never reports the request as sent.
// (/api/get-cached, which would stay in LRCLIB's own database, answers an
// empty 404 for every song, as a path that does not exist does: September
// 2026.)
constexpr int kExactPatienceMs = 250;
constexpr int kExactPatienceFallbackMs = 1500;
// The exact answer outranks the search's: a search can hold nothing but
// another song's words under the right title (アイドル: 20 entries, all
// wrong). So an answer the search has first is held until the exact lookup
// has had this long since it was sent (/api/get: median 195 ms, p90 521 ms,
// n=29), and only then taken.
constexpr int kExactGraceMs = 1000;

const QByteArray kUserAgent = QByteArrayLiteral("Monolist/0.1 (desktop music player)");

QString encoded(const QString &text)
{
    return QString::fromLatin1(QUrl::toPercentEncoding(text));
}

QString joinedText(const QList<LyricsModel::Line> &lines)
{
    QStringList texts;
    for (const LyricsModel::Line &line : lines)
        texts << line.text;
    return texts.join(QLatin1Char('\n'));
}

// Detached first: abort() delivers finished() at once, and its handler must
// see itself as superseded.
void abortRequest(QPointer<QNetworkReply> &request)
{
    if (QNetworkReply *reply = request) {
        request = nullptr;
        reply->abort();
    }
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

    m_recheck.setSingleShot(true);
    m_recheck.setInterval(kRecheckMs);
    connect(&m_recheck, &QTimer::timeout, this, [this]() {
        if (m_active && m_hasStored && m_stored.provisional && !m_checking)
            checkAgain();
    });
    m_hedge.setSingleShot(true);
    connect(&m_hedge, &QTimer::timeout, this, [this]() {
        if (m_exact == Step::Pending && m_search == Step::Idle) {
            m_lrclibTrail << QStringLiteral("get slow, search asked too");
            askSearch(false);
        }
    });
    m_exactWait.setSingleShot(true);
    connect(&m_exactWait, &QTimer::timeout, this, [this]() {
        m_exactOverdue = true;
        settleLrclib();
    });
    m_deadline.setSingleShot(true);
    m_deadline.setInterval(kLrclibDeadlineMs);
    connect(&m_deadline, &QTimer::timeout, this, [this]() {
        m_exactOverdue = true;
        if (m_search == Step::Found)
            return settleLrclib();   // the search's answer, held for the exact one
        m_lrclibError = QStringLiteral("LRCLIB did not answer within %1 s").arg(kLrclibDeadlineMs / 1000);
        endLrclib(QStringLiteral("failed"));
        askYouTube();
    });
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
    if (!m_active)
        return;
    if (m_state == QLatin1String("idle"))
        load(m_player->currentTrack(), false);
    else if (m_hasStored && m_stored.provisional && !m_checking)
        checkAgain();   // looked at again: a stand-in is asked for again
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
    m_hasStored = false;
    m_checking = false;
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
    ++m_generation;   // YouTube Music's answer, when it comes, is for nobody
    abortRequest(m_exactRequest);
    abortRequest(m_searchRequest);
    m_hedge.stop();
    m_exactWait.stop();
    m_deadline.stop();
    m_recheck.stop();
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
    m_hasStored = false;
    m_checking = false;
    m_lines.replace({});
    updateCurrentLine();

    if (m_videoId.isEmpty() || track.value(QStringLiteral("title")).toString().isEmpty()) {
        setState(QStringLiteral("none"));
        return;
    }
    // The artist line as names: the song's own credits, else the names
    // ArtistLinks has seen linked, else the line whole.
    const QVariant credits = track.value(QStringLiteral("credits"));
    const QVariantList pieces = m_artistLinks
        ? m_artistLinks->credits(track.value(QStringLiteral("artist")).toString(), credits)
        : credits.toList();
    m_query = LyricsQuery::fromTrack(track, pieces);

    Answer kept;
    const Stored stored = readStored(&kept);
    if (!askAgain && stored == Stored::Show) {
        show(kept);
        return;
    }
    if (!askAgain && stored == Stored::ShowAndCheck) {
        // Shown at once, and asked for again behind it.
        m_checking = true;
        show(kept);
        startLrclib();
        return;
    }
    // Asked again by hand: what is kept stays the answer if nothing better
    // is found.
    if (stored != Stored::Nothing && (kept.instrumental || !kept.synced.isEmpty() || !kept.plain.isEmpty())) {
        m_stored = kept;
        m_hasStored = true;
    }
    setState(QStringLiteral("loading"));
    startLrclib();
}

// The answer on show, asked for again behind it.
void Lyrics::checkAgain()
{
    ++m_generation;
    abortRequest(m_exactRequest);
    abortRequest(m_searchRequest);
    m_recheck.stop();
    m_checking = true;
    startLrclib();
}

// ------------------------------------------------------------- the database

Lyrics::Stored Lyrics::readStored(Answer *answer) const
{
    QSqlQuery q(AppDatabase::connection());
    q.prepare(QStringLiteral(
        "SELECT synced, plain, source, provisional,"
        " fetched_at > datetime('now', ?), fetched_at > datetime('now', ?)"
        " FROM lyrics WHERE video_id = ?"));
    q.addBindValue(kAskAgainAfter);
    q.addBindValue(kCheckAgainAfter);
    q.addBindValue(m_videoId);
    if (!q.exec() || !q.next())
        return Stored::Nothing;
    answer->synced = q.value(0).toString();
    answer->plain = q.value(1).toString();
    answer->source = q.value(2).toString();
    answer->instrumental = answer->source == QLatin1String("instrumental");
    if (answer->instrumental)
        answer->source.clear();
    answer->provisional = q.value(3).toBool();
    const bool recent = q.value(4).toBool();
    const bool fresh = q.value(5).toBool();
    if (answer->synced.isEmpty() && answer->plain.isEmpty() && !answer->instrumental)
        return recent ? Stored::Show : Stored::Nothing;   // none were found; long enough ago, look again
    return answer->provisional || !fresh ? Stored::ShowAndCheck : Stored::Show;
}

void Lyrics::store(const Answer &answer)
{
    QSqlQuery q(AppDatabase::connection());
    q.prepare(QStringLiteral(
        "INSERT INTO lyrics (video_id, synced, plain, source, provisional, fetched_at)"
        " VALUES (?, ?, ?, ?, ?, datetime('now'))"
        " ON CONFLICT(video_id) DO UPDATE SET synced = excluded.synced, plain = excluded.plain,"
        " source = excluded.source, provisional = excluded.provisional, fetched_at = excluded.fetched_at"));
    q.addBindValue(m_videoId);
    q.addBindValue(AppDatabase::text(answer.synced));
    q.addBindValue(AppDatabase::text(answer.plain));
    q.addBindValue(answer.instrumental ? QStringLiteral("instrumental") : AppDatabase::text(answer.source));
    q.addBindValue(answer.provisional ? 1 : 0);
    if (!q.exec())
        qWarning("Monolist: could not keep lyrics: %s", qPrintable(q.lastError().text()));
}

// Kept as it is, and looked at again only after the refresh age.
void Lyrics::touchStored()
{
    QSqlQuery q(AppDatabase::connection());
    q.prepare(QStringLiteral("UPDATE lyrics SET fetched_at = datetime('now'), provisional = 0 WHERE video_id = ?"));
    q.addBindValue(m_videoId);
    q.exec();
}

// ------------------------------------------------------------------- LRCLIB

void Lyrics::startLrclib()
{
    m_loosePlain.clear();
    m_lrclibError.clear();
    m_lrclibTrail.clear();
    m_lrclibBytes = 0;
    m_exact = Step::Idle;
    m_search = Step::Idle;
    m_exactUnreachable = false;
    m_searchLoose = false;
    m_searchEmpty = false;
    m_exactSent = false;
    m_exactOverdue = false;
    m_lrclibClock.start();
    m_deadline.start();
    // The exact lookup needs an artist; a song without one is searched for.
    if (m_lrclibExact && !m_query.leadArtist().isEmpty())
        askExact();
    else
        askSearch(false);
}

void Lyrics::askExact()
{
    // The song as it is filed: title, lead artist, album and length. LRCLIB
    // matches the length within a couple of seconds; an album it files under
    // another name answers 404, and the search below still finds the song.
    QString query = QStringLiteral("track_name=") + encoded(m_query.title)
                  + QStringLiteral("&artist_name=") + encoded(m_query.leadArtist());
    if (!m_query.album.isEmpty())
        query += QStringLiteral("&album_name=") + encoded(m_query.album);
    if (m_query.durationS > 0)
        query += QStringLiteral("&duration=") + QString::number(qRound(m_query.durationS));

    QNetworkRequest request(QUrl(m_lrclib + QStringLiteral("/api/get?") + query));
    request.setHeader(QNetworkRequest::UserAgentHeader, kUserAgent);
    QNetworkReply *reply = m_network->get(request);
    m_exactRequest = reply;
    m_exact = Step::Pending;
    countBytes(reply);
    // The patience is counted from the request reaching the server, not from
    // the connection being opened: a first lookup's TLS handshake alone takes
    // longer than it.
    connect(reply, &QNetworkReply::requestSent, this, [this, reply]() {
        if (reply == m_exactRequest && !m_exactSent) {
            m_exactSent = true;
            m_exactSentClock.start();
            m_hedge.start(kExactPatienceMs);
        }
    });
    m_hedge.start(kExactPatienceFallbackMs);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() { handleExact(reply); });
}

// Every byte LRCLIB sends counts, a request given up on included, so the log
// says what a lookup cost.
void Lyrics::countBytes(QNetworkReply *reply)
{
    const quint64 generation = m_generation;
    connect(reply, &QNetworkReply::downloadProgress, this, [this, reply, generation](qint64 received, qint64) {
        if (generation != m_generation)
            return;
        m_lrclibBytes += received - reply->property("counted").toLongLong();
        reply->setProperty("counted", received);
    });
}

void Lyrics::askSearch(bool loose)
{
    // First by title and artist; if that finds nothing at all, by both as
    // words anywhere, which forgives a title spelt differently, and without
    // its version markers, for a song filed under its bare title.
    const QString lead = m_query.leadArtist();
    QString query;
    if (loose)
        query = QStringLiteral("q=") + encoded(m_query.bareTitle + QLatin1Char(' ') + lead);
    else
        query = QStringLiteral("track_name=") + encoded(m_query.title)
              + (lead.isEmpty() ? QString() : QStringLiteral("&artist_name=") + encoded(lead));

    QNetworkRequest request(QUrl(m_lrclib + QStringLiteral("/api/search?") + query));
    request.setHeader(QNetworkRequest::UserAgentHeader, kUserAgent);
    QNetworkReply *reply = m_network->get(request);
    m_searchRequest = reply;
    m_search = Step::Pending;
    m_searchLoose = loose;
    countBytes(reply);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() { handleSearch(reply); });
}

void Lyrics::handleExact(QNetworkReply *reply)
{
    reply->deleteLater();
    if (reply != m_exactRequest)
        return;
    m_exactRequest = nullptr;
    m_hedge.stop();

    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray body = reply->readAll();
    const QString after = m_exactSent ? QStringLiteral(" %1 ms after sent").arg(m_exactSentClock.elapsed()) : QString();
    if (reply->error() == QNetworkReply::NoError && status == 200) {
        const QJsonObject entry = QJsonDocument::fromJson(body).object();
        m_exactAnswer = Answer();
        m_exactAnswer.synced = entry.value(QLatin1String("syncedLyrics")).toString();
        m_exactAnswer.plain = entry.value(QLatin1String("plainLyrics")).toString();
        m_exactAnswer.instrumental = entry.value(QLatin1String("instrumental")).toBool();
        m_exactAnswer.source = QStringLiteral("LRCLIB");
        const bool words = !m_exactAnswer.synced.trimmed().isEmpty() || !m_exactAnswer.plain.trimmed().isEmpty();
        m_exact = words || m_exactAnswer.instrumental ? Step::Found : Step::Missed;
        m_lrclibTrail << QStringLiteral("get %1%2 (%3 bytes)")
                             .arg(m_exact == Step::Found ? QStringLiteral("found") : QStringLiteral("empty"), after)
                             .arg(body.size());
    } else if (status == 404) {
        m_exact = Step::Missed;
        m_lrclibTrail << QStringLiteral("get 404") + after;
    } else {
        // An HTTP error may be this endpoint's alone, and the search is
        // asked; no HTTP answer at all means the host is out of reach, and
        // the search would only fail the same way, later.
        m_exact = Step::Failed;
        m_exactUnreachable = status == 0;
        m_lrclibError = status > 0 ? QStringLiteral("LRCLIB answered HTTP %1").arg(status) : reply->errorString();
        m_lrclibTrail << QStringLiteral("get failed: ") + m_lrclibError;
    }
    settleLrclib();
}

void Lyrics::handleSearch(QNetworkReply *reply)
{
    reply->deleteLater();
    if (reply != m_searchRequest)
        return;
    m_searchRequest = nullptr;

    const QByteArray body = reply->readAll();
    const QString which = m_searchLoose ? QStringLiteral("words search") : QStringLiteral("search");
    if (reply->error() != QNetworkReply::NoError) {
        m_search = Step::Failed;
        m_lrclibError = reply->errorString();
        m_lrclibTrail << which + QStringLiteral(" failed: ") + m_lrclibError;
        settleLrclib();
        return;
    }

    // The closest in length wins: a song has entries for every release of it,
    // and only one with this recording's length is timed to it. With the
    // exact lookup on, an entry must also be this song (LyricsQuery::match):
    // a search by words can return another song by the same artist, or one
    // whose title only contains this one's.
    const QJsonArray results = QJsonDocument::fromJson(body).array();
    const double wanted = m_query.durationS;
    const double looseTolerance = m_lrclibExact ? kLooseTolerance : kOldLooseTolerance;
    QJsonObject synced, plain, instrumental, nearest;
    double syncedGap = 1e9, plainGap = 1e9, nearestGap = 1e9;
    int rejected = 0;
    for (const QJsonValue &value : results) {
        const QJsonObject entry = value.toObject();
        const double duration = entry.value(QLatin1String("duration")).toDouble();
        if (m_lrclibExact
            && !LyricsQuery::match(m_query, entry.value(QLatin1String("trackName")).toString(),
                                   entry.value(QLatin1String("artistName")).toString(), duration).accepted) {
            ++rejected;
            continue;
        }
        const double gap = wanted > 0 ? qAbs(duration - wanted) : 0.0;
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
        } else if ((hasSynced || hasPlain) && gap <= looseTolerance && gap < nearestGap) {
            nearest = entry;
            nearestGap = gap;
        }
    }
    m_lrclibTrail << QStringLiteral("%1 %2 entries%3 (%4 bytes)")
                         .arg(which)
                         .arg(results.size())
                         .arg(rejected > 0 ? QStringLiteral(", %1 not this song").arg(rejected) : QString())
                         .arg(body.size());

    if (!synced.isEmpty() || !plain.isEmpty() || !instrumental.isEmpty()) {
        m_searchAnswer = Answer();
        m_searchAnswer.source = QStringLiteral("LRCLIB");
        if (!synced.isEmpty()) {
            m_searchAnswer.synced = synced.value(QLatin1String("syncedLyrics")).toString();
            m_searchAnswer.plain = synced.value(QLatin1String("plainLyrics")).toString();
        } else if (!plain.isEmpty()) {
            m_searchAnswer.plain = plain.value(QLatin1String("plainLyrics")).toString();
        } else {
            m_searchAnswer.instrumental = true;
        }
        m_search = Step::Found;
        settleLrclib();
        return;
    }
    // Another recording's lyrics: the words are right, the timing may not be.
    if (!nearest.isEmpty() && m_loosePlain.isEmpty()) {
        m_loosePlain = nearest.value(QLatin1String("plainLyrics")).toString();
        if (m_loosePlain.trimmed().isEmpty())
            m_loosePlain = joinedText(parseLrc(nearest.value(QLatin1String("syncedLyrics")).toString()));
    }
    m_search = Step::Missed;
    m_searchEmpty = !m_searchLoose && results.isEmpty();
    settleLrclib();
}

// Where the two requests have got to, and what follows: an answer, another
// request, waiting, or YouTube Music.
void Lyrics::settleLrclib()
{
    const auto answered = [this](const Answer &answer) {
        endLrclib(answer.instrumental ? QStringLiteral("instrumental")
                  : !answer.synced.isEmpty() ? QStringLiteral("synced") : QStringLiteral("plain"));
        settle(answer);
    };
    if (m_exact == Step::Found)
        return answered(m_exactAnswer);
    // The search's answer while the exact lookup is still out: held until
    // that has had its grace, then taken.
    if (m_search == Step::Found) {
        if (m_exact == Step::Pending) {
            const qint64 waited = m_exactSent ? m_exactSentClock.elapsed()
                                              : m_lrclibClock.elapsed() - kExactPatienceFallbackMs;
            if (!m_exactOverdue && waited < kExactGraceMs) {
                m_exactWait.start(int(kExactGraceMs - waited));
                return;
            }
            m_lrclibTrail << QStringLiteral("get given up on");
        }
        return answered(m_searchAnswer);
    }
    if (m_exact == Step::Pending || m_search == Step::Pending)
        return;   // the exact answer outranks whatever the search has

    if (m_search == Step::Idle) {
        if (m_exact == Step::Failed && m_exactUnreachable) {
            endLrclib(QStringLiteral("failed"));
            return askYouTube();
        }
        return askSearch(false);
    }
    if (m_search == Step::Missed && m_searchEmpty && !m_searchLoose)
        return askSearch(true);
    // A clean miss only when every request answered: the exact lookup that
    // failed may have held what the search did not (an instrumental flag,
    // the one right entry among wrong ones).
    if (m_search == Step::Failed || m_exact == Step::Failed) {
        endLrclib(QStringLiteral("failed"));
        return askYouTube();
    }
    m_lrclibError.clear();
    endLrclib(QStringLiteral("missed"));
    askYouTube();
}

void Lyrics::endLrclib(const QString &outcome)
{
    m_deadline.stop();
    m_hedge.stop();
    m_exactWait.stop();
    abortRequest(m_exactRequest);
    abortRequest(m_searchRequest);
    qInfo("lyrics: LRCLIB %s in %lld ms for %s: %s; %lld bytes", qPrintable(outcome),
          (long long)m_lrclibClock.elapsed(), qPrintable(m_videoId),
          qPrintable(m_lrclibTrail.isEmpty() ? QStringLiteral("no answer") : m_lrclibTrail.join(QStringLiteral(", "))),
          (long long)m_lrclibBytes);
}

void Lyrics::askYouTube()
{
    const QString videoId = m_videoId;
    const quint64 generation = m_generation;
    m_innerTube.lyrics(videoId, [this, generation](const QString &text, const QString &source, const QString &error) {
        if (generation != m_generation)
            return;   // another lookup since, of this song or another
        const bool lrclibFailed = !m_lrclibError.isEmpty();
        if (!text.isEmpty()) {
            Answer answer;
            answer.plain = text;
            answer.source = source.isEmpty() ? QStringLiteral("YouTube Music")
                                             : source + QStringLiteral(" via YouTube Music");
            // Shown in LRCLIB's place only because LRCLIB could not be asked.
            answer.provisional = lrclibFailed;
            settle(answer, lrclibFailed ? QStringLiteral("LRCLIB failed: ") + m_lrclibError : QString());
            return;
        }
        if (!m_loosePlain.isEmpty()) {
            Answer answer;
            answer.plain = m_loosePlain;
            answer.source = QStringLiteral("LRCLIB");
            // Another recording's words: the answer when YouTube Music has
            // none, only a stand-in when it could not be asked.
            answer.provisional = !error.isEmpty() || lrclibFailed;
            settle(answer, !error.isEmpty() ? QStringLiteral("YouTube Music failed: ") + error
                           : lrclibFailed ? QStringLiteral("LRCLIB failed: ") + m_lrclibError : QString());
            return;
        }
        if (error.isEmpty() && !lrclibFailed) {
            settleNone();
            return;
        }
        // One of them could not be asked, which says nothing about the song.
        settleError(lrclibFailed ? m_lrclibError : error);
    });
}

// ----------------------------------------------------------- how it ends

void Lyrics::settle(const Answer &answer, const QString &why)
{
    // What is kept for good is not given up for an answer that is only
    // standing in.
    if (m_hasStored && answer.provisional && !m_stored.provisional) {
        qInfo("lyrics: %s: kept lyrics stay; a stand-in was found (%s)", qPrintable(m_videoId), qPrintable(why));
        return keepStored();
    }
    store(answer);
    if (answer.provisional)
        qInfo("lyrics: %s kept as provisional, to be asked again (%s)", qPrintable(m_videoId), qPrintable(why));
    const bool same = m_hasStored && m_state != QLatin1String("loading") && answer.synced == m_stored.synced
                      && answer.plain == m_stored.plain && answer.instrumental == m_stored.instrumental;
    if (same) {
        // The lines on show are the answer: no reset under the reader.
        m_stored = answer;
        m_checking = false;
        if (answer.provisional)
            m_recheck.start();
        Q_EMIT stateChanged();
        return;
    }
    m_checking = false;
    show(answer);
}

void Lyrics::settleNone()
{
    if (m_hasStored) {
        // Lyrics were found before and nobody has them now: they stay, as
        // they are, until the refresh age comes round again.
        touchStored();
        m_stored.provisional = false;
        return keepStored();
    }
    store(Answer());
    m_checking = false;
    show(Answer());
}

void Lyrics::settleError(const QString &error)
{
    if (m_hasStored) {
        qInfo("lyrics: %s: kept lyrics stay; asking again failed (%s)", qPrintable(m_videoId), qPrintable(error));
        return keepStored();
    }
    // Nothing is stored, and the view offers to try again.
    qInfo("lyrics: %s: nothing stored, a source could not be asked (%s)", qPrintable(m_videoId), qPrintable(error));
    m_checking = false;
    m_source.clear();
    m_error = error;
    m_lines.replace({});
    m_state = QStringLiteral("error");
    updateCurrentLine();
    Q_EMIT stateChanged();
}

// The kept answer stays the answer: already on show when it was being
// checked, shown now when it was asked again by hand.
void Lyrics::keepStored()
{
    if (m_checking) {
        m_checking = false;
        if (m_stored.provisional)
            m_recheck.start();
        Q_EMIT stateChanged();   // the check is over; nothing on show changed
        return;
    }
    show(m_stored);
}

void Lyrics::show(const Answer &answer)
{
    QList<LyricsModel::Line> lines;
    QString state;
    if (!answer.synced.isEmpty()) {
        lines = parseLrc(answer.synced);
        if (!lines.isEmpty())
            state = QStringLiteral("synced");
    }
    if (state.isEmpty() && !answer.plain.isEmpty()) {
        lines = plainLines(answer.plain);
        if (!lines.isEmpty())
            state = QStringLiteral("plain");
    }
    if (state.isEmpty()) {
        lines.clear();
        state = answer.instrumental ? QStringLiteral("instrumental") : QStringLiteral("none");
    }
    const bool found = !lines.isEmpty() || answer.instrumental;
    m_stored = answer;
    m_hasStored = found;
    if (found && answer.provisional && !m_checking)
        m_recheck.start();
    m_source = lines.isEmpty() ? QString() : answer.source;
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
