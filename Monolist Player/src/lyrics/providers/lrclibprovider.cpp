#include "lrclibprovider.h"

#include "lyrics.h"

#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QStringList>
#include <QTimer>
#include <QUrl>

namespace {

// How far LRCLIB's length for a song may be from this recording's for its
// timing to be trusted, and for its words to be used at all, in seconds. The
// second was 20 s; at that distance an entry is as likely another song's as
// another edit's. 20 s stays for lyrics.lrclib=search, the rules from before.
constexpr double kSyncTolerance = 3.0;
constexpr double kLooseTolerance = 10.0;
constexpr double kOldLooseTolerance = 20.0;
// LRCLIB as a whole, every request in it: a wall clock, where Qt's own timeout
// only measures silence and restarts with each request.
constexpr int kDeadlineMs = 6000;
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

// One song's LRCLIB lookup: /api/get, then /api/search, under one deadline.
class LrclibLookup : public LyricsLookup
{
public:
    LrclibLookup(QNetworkAccessManager *network, const QString &base, bool exact, const LyricsRequest &request,
                 QObject *parent)
        : LyricsLookup(parent)
        , m_network(network)
        , m_base(base)
        , m_exactMode(exact)
        , m_videoId(request.videoId)
        , m_query(request.query)
    {
        m_hedge.setSingleShot(true);
        connect(&m_hedge, &QTimer::timeout, this, [this]() {
            if (m_exact == Step::Pending && m_search == Step::Idle) {
                m_trail << QStringLiteral("get slow, search asked too");
                askSearch(false);
            }
        });
        m_exactWait.setSingleShot(true);
        connect(&m_exactWait, &QTimer::timeout, this, [this]() {
            m_exactOverdue = true;
            settle();
        });
        m_deadline.setSingleShot(true);
        m_deadline.setInterval(kDeadlineMs);
        connect(&m_deadline, &QTimer::timeout, this, [this]() {
            m_exactOverdue = true;
            if (m_search == Step::Found)
                return settle();   // the search's answer, held for the exact one
            m_error = QStringLiteral("LRCLIB did not answer within %1 s").arg(kDeadlineMs / 1000);
            end(QStringLiteral("failed"));
            failed();
        });
    }

    void start() override
    {
        m_clock.start();
        m_deadline.start();
        // The exact lookup needs an artist; a song without one is searched for.
        if (m_exactMode && !m_query.leadArtist().isEmpty())
            askExact();
        else
            askSearch(false);
    }

    void cancel() override
    {
        if (m_done)
            return;
        m_done = true;
        onFinished = nullptr;
        end(QStringLiteral("cancelled"));
    }

private:
    // Where one request has got to.
    enum class Step { Idle, Pending, Found, Missed, Failed };

    void askExact()
    {
        // The song as it is filed: title, lead artist, album and length.
        // LRCLIB matches the length within a couple of seconds; an album it
        // files under another name answers 404, and the search below still
        // finds the song.
        QString query = QStringLiteral("track_name=") + encoded(m_query.title)
                      + QStringLiteral("&artist_name=") + encoded(m_query.leadArtist());
        if (!m_query.album.isEmpty())
            query += QStringLiteral("&album_name=") + encoded(m_query.album);
        if (m_query.durationS > 0)
            query += QStringLiteral("&duration=") + QString::number(qRound(m_query.durationS));

        QNetworkRequest request(QUrl(m_base + QStringLiteral("/api/get?") + query));
        request.setHeader(QNetworkRequest::UserAgentHeader, kUserAgent);
        QNetworkReply *reply = m_network->get(request);
        m_exactRequest = reply;
        m_exact = Step::Pending;
        countBytes(reply);
        // The patience is counted from the request reaching the server, not
        // from the connection being opened: a first lookup's TLS handshake
        // alone takes longer than it.
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

    void askSearch(bool loose)
    {
        // First by title and artist; if that finds nothing at all, by both
        // as words anywhere, which forgives a title spelt differently, and
        // without its version markers, for a song filed under its bare title.
        const QString lead = m_query.leadArtist();
        QString query;
        if (loose)
            query = QStringLiteral("q=") + encoded(m_query.bareTitle + QLatin1Char(' ') + lead);
        else
            query = QStringLiteral("track_name=") + encoded(m_query.title)
                  + (lead.isEmpty() ? QString() : QStringLiteral("&artist_name=") + encoded(lead));

        QNetworkRequest request(QUrl(m_base + QStringLiteral("/api/search?") + query));
        request.setHeader(QNetworkRequest::UserAgentHeader, kUserAgent);
        QNetworkReply *reply = m_network->get(request);
        m_searchRequest = reply;
        m_search = Step::Pending;
        m_searchLoose = loose;
        countBytes(reply);
        connect(reply, &QNetworkReply::finished, this, [this, reply]() { handleSearch(reply); });
    }

    // Every byte LRCLIB sends counts, a request given up on included, so the
    // log says what a lookup cost.
    void countBytes(QNetworkReply *reply)
    {
        connect(reply, &QNetworkReply::downloadProgress, this, [this, reply](qint64 received, qint64) {
            m_bytes += received - reply->property("counted").toLongLong();
            reply->setProperty("counted", received);
        });
    }

    void handleExact(QNetworkReply *reply)
    {
        reply->deleteLater();
        if (reply != m_exactRequest)
            return;
        m_exactRequest = nullptr;
        m_hedge.stop();

        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray body = reply->readAll();
        const QString after = m_exactSent ? QStringLiteral(" %1 ms after sent").arg(m_exactSentClock.elapsed())
                                          : QString();
        if (reply->error() == QNetworkReply::NoError && status == 200) {
            const QJsonObject entry = QJsonDocument::fromJson(body).object();
            m_exactAnswer = LyricsAnswer();
            m_exactAnswer.synced = entry.value(QLatin1String("syncedLyrics")).toString();
            m_exactAnswer.plain = entry.value(QLatin1String("plainLyrics")).toString();
            m_exactAnswer.instrumental = entry.value(QLatin1String("instrumental")).toBool();
            m_exactAnswer.source = QStringLiteral("LRCLIB");
            m_exactAnswer.durationS = lengthOf(entry);
            const bool words = !m_exactAnswer.synced.trimmed().isEmpty() || !m_exactAnswer.plain.trimmed().isEmpty();
            m_exact = words || m_exactAnswer.instrumental ? Step::Found : Step::Missed;
            m_trail << QStringLiteral("get %1%2 (%3 bytes)")
                           .arg(m_exact == Step::Found ? QStringLiteral("found") : QStringLiteral("empty"), after)
                           .arg(body.size());
        } else if (status == 404) {
            m_exact = Step::Missed;
            m_trail << QStringLiteral("get 404") + after;
        } else {
            // An HTTP error may be this endpoint's alone, and the search is
            // asked; no HTTP answer at all means the host is out of reach,
            // and the search would only fail the same way, later.
            m_exact = Step::Failed;
            m_exactUnreachable = status == 0;
            m_error = status > 0 ? QStringLiteral("LRCLIB answered HTTP %1").arg(status) : reply->errorString();
            m_trail << QStringLiteral("get failed: ") + m_error;
        }
        settle();
    }

    void handleSearch(QNetworkReply *reply)
    {
        reply->deleteLater();
        if (reply != m_searchRequest)
            return;
        m_searchRequest = nullptr;

        const QByteArray body = reply->readAll();
        const QString which = m_searchLoose ? QStringLiteral("words search") : QStringLiteral("search");
        if (reply->error() != QNetworkReply::NoError) {
            m_search = Step::Failed;
            m_error = reply->errorString();
            m_trail << which + QStringLiteral(" failed: ") + m_error;
            settle();
            return;
        }

        // The closest in length wins: a song has entries for every release
        // of it, and only one with this recording's length is timed to it.
        // With the exact lookup on, an entry must also be this song
        // (LyricsQuery::match): a search by words can return another song by
        // the same artist, or one whose title only contains this one's.
        const QJsonArray results = QJsonDocument::fromJson(body).array();
        const double wanted = m_query.durationS;
        const double looseTolerance = m_exactMode ? kLooseTolerance : kOldLooseTolerance;
        QJsonObject synced, plain, instrumental, nearest;
        double syncedGap = 1e9, plainGap = 1e9, nearestGap = 1e9;
        int rejected = 0;
        for (const QJsonValue &value : results) {
            const QJsonObject entry = value.toObject();
            const double duration = entry.value(QLatin1String("duration")).toDouble();
            if (m_exactMode
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
        m_trail << QStringLiteral("%1 %2 entries%3 (%4 bytes)")
                       .arg(which)
                       .arg(results.size())
                       .arg(rejected > 0 ? QStringLiteral(", %1 not this song").arg(rejected) : QString())
                       .arg(body.size());

        if (!synced.isEmpty() || !plain.isEmpty() || !instrumental.isEmpty()) {
            m_searchAnswer = LyricsAnswer();
            m_searchAnswer.source = QStringLiteral("LRCLIB");
            if (!synced.isEmpty()) {
                m_searchAnswer.synced = synced.value(QLatin1String("syncedLyrics")).toString();
                m_searchAnswer.plain = synced.value(QLatin1String("plainLyrics")).toString();
                m_searchAnswer.durationS = lengthOf(synced);
            } else if (!plain.isEmpty()) {
                m_searchAnswer.plain = plain.value(QLatin1String("plainLyrics")).toString();
                m_searchAnswer.durationS = lengthOf(plain);
            } else {
                m_searchAnswer.instrumental = true;
                m_searchAnswer.durationS = lengthOf(instrumental);
            }
            m_search = Step::Found;
            settle();
            return;
        }
        // Another recording's lyrics: the words are right, the timing may not be.
        if (!nearest.isEmpty() && m_loosePlain.isEmpty()) {
            m_loosePlain = nearest.value(QLatin1String("plainLyrics")).toString();
            if (m_loosePlain.trimmed().isEmpty())
                m_loosePlain = joinedText(Lyrics::parseLrc(nearest.value(QLatin1String("syncedLyrics")).toString()));
            m_looseDuration = lengthOf(nearest);
        }
        m_search = Step::Missed;
        m_searchEmpty = !m_searchLoose && results.isEmpty();
        settle();
    }

    // The entry's length, for the race's gate; unknown under the rules from
    // before, which judge lengths by their own tolerances.
    double lengthOf(const QJsonObject &entry) const
    {
        const double seconds = entry.value(QLatin1String("duration")).toDouble();
        return m_exactMode && seconds > 0 ? seconds : -1;
    }

    // Where the two requests have got to, and what follows: an answer,
    // another request, waiting, or the end.
    void settle()
    {
        if (m_done)
            return;
        if (m_exact == Step::Found)
            return answered(m_exactAnswer);
        // The search's answer while the exact lookup is still out: held until
        // that has had its grace, then taken.
        if (m_search == Step::Found) {
            if (m_exact == Step::Pending) {
                const qint64 waited = m_exactSent ? m_exactSentClock.elapsed()
                                                  : m_clock.elapsed() - kExactPatienceFallbackMs;
                if (!m_exactOverdue && waited < kExactGraceMs) {
                    m_exactWait.start(int(kExactGraceMs - waited));
                    return;
                }
                m_trail << QStringLiteral("get given up on");
            }
            return answered(m_searchAnswer);
        }
        if (m_exact == Step::Pending || m_search == Step::Pending)
            return;   // the exact answer outranks whatever the search has

        if (m_search == Step::Idle) {
            if (m_exact == Step::Failed && m_exactUnreachable) {
                end(QStringLiteral("failed"));
                return failed();
            }
            return askSearch(false);
        }
        if (m_search == Step::Missed && m_searchEmpty && !m_searchLoose)
            return askSearch(true);
        // A clean miss only when every request answered: the exact lookup
        // that failed may have held what the search did not (an instrumental
        // flag, the one right entry among wrong ones).
        if (m_search == Step::Failed || m_exact == Step::Failed) {
            end(QStringLiteral("failed"));
            return failed();
        }
        m_error.clear();
        end(QStringLiteral("missed"));
        missed();
    }

    void answered(const LyricsAnswer &answer)
    {
        end(answer.instrumental ? QStringLiteral("instrumental")
            : !answer.synced.isEmpty() ? QStringLiteral("synced") : QStringLiteral("plain"));
        LyricsOutcome outcome;
        outcome.kind = LyricsOutcome::Found;
        outcome.answer = answer;
        report(outcome);
    }

    // Another recording's words, when a search had them: the answer where
    // nobody has better, and only part of one when a request failed.
    LyricsOutcome loose() const
    {
        LyricsOutcome outcome;
        outcome.kind = LyricsOutcome::Found;
        outcome.answer.plain = m_loosePlain;
        outcome.answer.source = QStringLiteral("LRCLIB");
        outcome.answer.loose = true;
        outcome.answer.durationS = m_looseDuration;
        return outcome;
    }

    void failed()
    {
        LyricsOutcome outcome;
        if (!m_loosePlain.isEmpty()) {
            outcome = loose();
            outcome.partial = true;
        } else {
            outcome.kind = LyricsOutcome::Failed;
        }
        outcome.error = m_error.isEmpty() ? QStringLiteral("LRCLIB could not be asked") : m_error;
        report(outcome);
    }

    void missed()
    {
        LyricsOutcome outcome;
        if (!m_loosePlain.isEmpty())
            outcome = loose();
        else
            outcome.kind = LyricsOutcome::Missed;
        report(outcome);
    }

    void report(const LyricsOutcome &outcome)
    {
        m_done = true;
        finish(outcome);
    }

    void end(const QString &outcome)
    {
        m_deadline.stop();
        m_hedge.stop();
        m_exactWait.stop();
        abortRequest(m_exactRequest);
        abortRequest(m_searchRequest);
        qInfo("lyrics: LRCLIB %s in %lld ms for %s: %s; %lld bytes", qPrintable(outcome),
              (long long)m_clock.elapsed(), qPrintable(m_videoId),
              qPrintable(m_trail.isEmpty() ? QStringLiteral("no answer") : m_trail.join(QStringLiteral(", "))),
              (long long)m_bytes);
    }

    QNetworkAccessManager *m_network;
    const QString m_base;
    const bool m_exactMode;
    const QString m_videoId;
    const LyricsQuery::Query m_query;
    bool m_done = false;

    QPointer<QNetworkReply> m_exactRequest;
    QPointer<QNetworkReply> m_searchRequest;
    Step m_exact = Step::Idle;
    Step m_search = Step::Idle;
    bool m_exactUnreachable = false;  // no HTTP answer at all: search would fare no better
    bool m_searchLoose = false;
    bool m_searchEmpty = false;       // the strict search had no entries at all
    bool m_exactSent = false;
    bool m_exactOverdue = false;      // no longer waited for, once the search has an answer
    LyricsAnswer m_exactAnswer;
    LyricsAnswer m_searchAnswer;
    QTimer m_hedge;                   // the exact lookup is slow: search as well
    QTimer m_exactWait;               // the search's answer held for the exact one
    QTimer m_deadline;                // LRCLIB as a whole
    QElapsedTimer m_clock;
    QElapsedTimer m_exactSentClock;
    qint64 m_bytes = 0;
    QStringList m_trail;              // each request's outcome, for the log

    // A match not close enough to trust its timing, kept as plain text in
    // case nothing better turns up; and why LRCLIB failed.
    QString m_loosePlain;
    double m_looseDuration = -1;
    QString m_error;
};

} // namespace

LrclibProvider::LrclibProvider(QNetworkAccessManager *network)
    : m_network(network)
{
}

void LrclibProvider::setUrl(const QString &url)
{
    const QString trimmed = url.trimmed();
    if (!trimmed.isEmpty())
        m_url = trimmed.endsWith(QLatin1Char('/')) ? trimmed.chopped(1) : trimmed;
}

LyricsLookup *LrclibProvider::lookUp(const LyricsRequest &request, QObject *parent)
{
    return new LrclibLookup(m_network, m_url, m_exact, request, parent);
}
