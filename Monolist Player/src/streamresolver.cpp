#include "streamresolver.h"
#include "appdatabase.h"
#include "ytdlp.h"
#include "ytmsession.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSqlQuery>
#include <QTimeZone>
#include <QUrlQuery>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>

#include <algorithm>
#include <memory>
#include <utility>

namespace {

// How long YouTube's answer is held for JioSaavn's, counted from when both
// were asked. A warm YouTube link answers in milliseconds, and JioSaavn's
// search in several hundred on a good day, so this is enough for it to win
// most races it can win at all, and short enough that nobody notices the
// wait when it does not. Past it YouTube plays, and JioSaavn's answer is
// kept for the next time the song comes round. A first guess, to be tuned
// against measurements.
constexpr int kSaavnGraceMs = 1200;
// JioSaavn's answers, remembered. Its links are plain CDN paths that last,
// so a match is kept a week; "not there" a day, since catalogues grow.
constexpr int kSaavnMatchDays = 7;
constexpr int kSaavnNoMatchHours = 24;
// A link mpv refused: JioSaavn left out for this song for ten minutes, the
// time a fault at their end usually takes to pass, then asked afresh.
constexpr int kSaavnRefusedMinutes = 10;
// Three failures in a row (no answer at all, not "not found") and JioSaavn
// is left alone for five minutes.
constexpr int kSaavnFailuresBeforeRest = 3;
constexpr int kSaavnRestMinutes = 5;
// How long a song whose InnerTube links googlevideo refused is played from
// the link that rescued it (noteInnerTubeRefused). The refusals measured
// lasted at least as long as a replay 15 s later; an hour is a first guess
// between paying ~3 s again at every replay and hearing 96-128 kbps AAC for
// the five hours a link lasts, which is what B8 set out to end.
constexpr int kInnerTubeRefusedMinutes = 60;

constexpr int kPipedTimeoutMs = 8000;
constexpr int kInvidiousTimeoutMs = 6000;
// Roughly three times a healthy resolve on the slowest machine this is known
// to run on. Past this, something is wrong and waiting longer helps nobody.
// Counted from when the lookup starts, not while it waits its turn.
constexpr int kYtDlpTimeoutMs = 12000;
// A song someone is waiting for, from being asked for to its answer or its
// failure (setDeadline). The rungs' own limits add up to far more: /player
// has 3 s (InnerTube::player) and yt-dlp's two lookups 12 s each, so a song
// that has not played by now is not going to, and the player moves on.
constexpr int kResolveDeadlineMs = 20000;

// A rung as the log names it.
const char *tierName(int tier)
{
    switch (tier) {
    case StreamResolver::TierInnerTube: return "InnerTube";
    case StreamResolver::TierYtDlp:     return "yt-dlp";
    case StreamResolver::TierMuxed:     return "its muxed stream";
    case StreamResolver::TierPiped:     return "Piped";
    case StreamResolver::TierInvidious: return "Invidious";
    case StreamResolver::TierSignedIn:  return "yt-dlp with the account";
    default:                            return "another source";
    }
}

// An hour, for the account's limit (setAccountLimit).
constexpr qint64 kAccountWindowMs = 60 * 60 * 1000;

QNetworkRequest makeRequest(const QUrl &url)
{
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("Monolist/0.1 (+https://github.com/)"));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(kPipedTimeoutMs);
    return request;
}

} // namespace

// Both lists ship empty, and both tiers survive.
//
// The public instances were probed host by host: of the 15 documented Piped
// API hosts, 11 no longer answer at all and the two that do return an empty
// `audioStreams` list or a 500 — the extraction is broken upstream, so no
// refreshed list repairs this. Of the Invidious hosts, one still advertises an
// API and none serve /latest_version as media. What the tiers bought was a
// ~7.5s walk through hosts that cannot answer, paid at the exact moment
// something had already gone wrong.
//
// This is instance rot, not the projects ending: both are still developed. So
// the racing code stays, the `piped_instances` and `invidious_instances`
// settings rows still feed setPipedInstances()/setInvidiousInstances(), and a
// live host list revives either tier without a rebuild. With the list empty,
// startPipedRace()/startInvidiousRace() call tierExhausted() at once and the
// ladder walks past in no measurable time.
//
// The last known-good lists, for whoever repopulates them:
//   Piped      pipedapi.kavin.rocks, pipedapi.moomoo.me,
//              piped-api.garudalinux.org, api.piped.projectsegfau.lt,
//              piped.privacydev.net
//   Invidious  yewtu.be, invidious.projectsegfau.lt, iv.ggtyler.dev,
//              inv.nadeko.net, invidious.nerdvpn.de, invidious.privacydev.net,
//              yt.artemislena.eu, invidious.fdn.fr, invidious.slipfox.xyz,
//              invidious.lunar.icu, invidious.vps.sh, inv.tux.pizza,
//              invidious.io.lol
QStringList StreamResolver::defaultPipedInstances()
{
    return {};
}

QStringList StreamResolver::defaultInvidiousInstances()
{
    return {};
}

StreamResolver::StreamResolver(QObject *parent)
    : QObject(parent)
    , m_network(new QNetworkAccessManager(this))
    , m_piped(defaultPipedInstances())
    , m_invidious(defaultInvidiousInstances())
{
    m_accountClock.start();
}

StreamResolver::~StreamResolver()
{
    cancelAll();
    YtDlp::setPlaybackResolving(this, false);
}

void StreamResolver::setPipedInstances(const QStringList &hosts)
{
    if (!hosts.isEmpty())
        m_piped = hosts;
}

void StreamResolver::setInvidiousInstances(const QStringList &hosts)
{
    if (!hosts.isEmpty())
        m_invidious = hosts;
}

void StreamResolver::resolve(const QString &videoId, int firstTier)
{
    if (videoId.isEmpty()) {
        emitFailed(videoId, QStringLiteral("Empty video id."));
        return;
    }

    if (firstTier <= TierInnerTube) {
        // A song googlevideo refuses InnerTube's links for lately: the link
        // that rescued it, while it lasts, rather than both refusals again.
        if (innerTubeRefusedLately(videoId)) {
            const auto rescue = m_rescueLinks.constFind(videoId);
            if (rescue != m_rescueLinks.constEnd() && rescue->expires > QDateTime::currentDateTimeUtc()) {
                const CacheEntry entry = *rescue;
                qInfo("resolver: %s: its InnerTube links were refused lately; it plays from the link that "
                      "rescued it (%s)", qPrintable(videoId), tierName(entry.tier));
                QMetaObject::invokeMethod(this, [this, videoId, entry]() {
                    report(videoId, entry.url, entry.tier, /*fromCache=*/true);
                }, Qt::QueuedConnection);
                return;
            }
        }
        // Still-valid link from earlier or from a prefetch: answer at once,
        // but queued, so the caller sees the same order of events as always.
        const auto cached = m_cache.constFind(videoId);
        if (cached != m_cache.constEnd() && cached->expires > QDateTime::currentDateTimeUtc()) {
            const CacheEntry entry = *cached;
            QMetaObject::invokeMethod(this, [this, videoId, entry]() {
                report(videoId, entry.url, entry.tier, /*fromCache=*/true);
            }, Qt::QueuedConnection);
            return;
        }
        // A prefetch already on its way: let it finish, and report this time.
        if (Job *running = m_jobs.value(videoId); running && running->prefetch) {
            running->prefetch = false;
            armDeadline(videoId);
            foregroundChanged();
            // Waiting its turn for yt-dlp behind a lookup made ahead of
            // time: someone is waiting for this one now, so it goes first.
            if (m_ytdlpWaiting.contains(running) && m_ytdlpHolder && m_ytdlpHolder->prefetch) {
                preemptYtDlp();
                pumpYtDlp();
            }
            return;
        }
    }

    QList<int> tiers;
    for (int tier = qMax(int(TierInnerTube), firstTier); tier < TierExhausted; ++tier)
        tiers.append(tier);
    start(videoId, tiers);
}

void StreamResolver::resolveVia(const QString &videoId, const QList<int> &tiers, int homeTier)
{
    if (videoId.isEmpty()) {
        emitFailed(videoId, QStringLiteral("Empty video id."));
        return;
    }
    // This play's rescue link, when the walk starts on its rung: the play
    // goes back to it (the sound after its picture, say) rather than paying
    // for the rescue twice. Queued, as a remembered link always is.
    if (!tiers.isEmpty()) {
        const auto rescue = m_rescueLinks.constFind(videoId);
        if (rescue != m_rescueLinks.constEnd() && rescue->tier == tiers.first()
            && rescue->expires > QDateTime::currentDateTimeUtc()) {
            const CacheEntry entry = *rescue;
            qInfo("resolver: %s goes back to the link that rescued this play", qPrintable(videoId));
            QMetaObject::invokeMethod(this, [this, videoId, entry]() {
                report(videoId, entry.url, entry.tier, /*fromCache=*/true);
            }, Qt::QueuedConnection);
            return;
        }
    }
    start(videoId, tiers, homeTier, /*exact=*/true);
}

void StreamResolver::start(const QString &videoId, QList<int> tiers, int homeTier, bool exact)
{
    cancelJob(videoId);

    auto *job = new Job;
    job->videoId = videoId;
    job->homeTier = homeTier;
    job->mayAddAccount = !exact;
    m_jobs.insert(videoId, job);
    armDeadline(videoId);
    foregroundChanged();
    const int first = tiers.isEmpty() ? int(TierExhausted) : tiers.takeFirst();
    job->next = tiers;
    startTier(job, first);
}

// A link the player refused says something about where it came from. After
// InnerTube's, the muxed stream comes first: yt-dlp's sound would be fetched
// as the very client whose link was just turned down, where the muxed stream
// is asked of others. After yt-dlp's, the muxed stream is the one kind left
// to try; after the muxed stream's, yt-dlp's sound. The public instances come
// last either way. Before all of them, the caller may ask the refused tier
// once more (PlaybackController's loadFailed): a refusal is often the link's
// alone, and InnerTube's next one costs a fifth of a second where the muxed
// stream costs three. After the account's link, the anonymous rungs below
// it, yt-dlp first: googlevideo refusing a link is about the link, not the
// account, and the song may yet play signed out. The account's rung is in
// none of these: a song goes to it only when YouTube refuses it signed out.
QList<int> StreamResolver::afterRefusal(int tier)
{
    switch (tier) {
    case TierInnerTube: return { TierMuxed, TierYtDlp, TierPiped, TierInvidious };
    case TierYtDlp:     return { TierMuxed, TierPiped, TierInvidious };
    case TierMuxed:     return { TierYtDlp, TierPiped, TierInvidious };
    case TierPiped:     return { TierInvidious };
    case TierSignedIn:  return { TierYtDlp, TierMuxed, TierPiped, TierInvidious };
    default:            return {};
    }
}

QVariantMap StreamResolver::headersFor(const QString &videoId, const QString &url) const
{
    for (const QHash<QString, CacheEntry> *links : { &m_rescueLinks, &m_cache }) {
        const auto entry = links->constFind(videoId);
        if (entry != links->constEnd() && entry->url == url)
            return entry->headers;
    }
    return {};
}

// The picture: yt-dlp only. The public instances answer with sound, and a
// track's picture is asked for rarely enough that racing a dozen hosts for it
// would be more machinery than it is worth.
void StreamResolver::resolveVideo(const QString &videoId, int maxHeight)
{
    if (videoId.isEmpty())
        return;

    const auto cached = m_videoCache.constFind(videoId);
    if (cached != m_videoCache.constEnd() && cached->expires > QDateTime::currentDateTimeUtc()) {
        const VideoLinks links = *cached;
        QMetaObject::invokeMethod(this, [this, videoId, links]() {
            Q_EMIT videoResolved(videoId, links.video, links.audio, links.headers);
        }, Qt::QueuedConnection);
        return;
    }
    if (m_videoJobs.contains(videoId))
        return;   // already on its way
    if (!YtDlp::isAvailable()) {
        Q_EMIT videoFailed(videoId, QStringLiteral("Playing video needs yt-dlp."));
        return;
    }

    YtDlpRequest *request = YtDlp::resolveVideo(videoId, maxHeight, this);
    m_videoJobs.insert(videoId, request);

    connect(request, &YtDlpRequest::succeededJson, this,
            [this, videoId](const QJsonDocument &document) {
                if (m_videoJobs.take(videoId).isNull())
                    return;   // cancelled meanwhile
                const QJsonObject root = document.object();
                QString video = root.value(QStringLiteral("url")).toString();
                QString audio;
                QJsonObject chosen = root;
                // Two streams: yt-dlp lists them as it would merge them.
                const QJsonArray parts = root.value(QStringLiteral("requested_formats")).toArray();
                if (parts.size() >= 2) {
                    const QJsonObject first = parts.at(0).toObject();
                    const QJsonObject second = parts.at(1).toObject();
                    const bool firstIsVideo = first.value(QStringLiteral("vcodec")).toString()
                                              != QLatin1String("none");
                    chosen = firstIsVideo ? first : second;
                    video = chosen.value(QStringLiteral("url")).toString();
                    audio = (firstIsVideo ? second : first).value(QStringLiteral("url")).toString();
                }
                if (video.isEmpty()) {
                    Q_EMIT videoFailed(videoId, QStringLiteral("No picture is published for this track."));
                    return;
                }
                // The headers the link was fetched with. YouTube ties a link
                // to the client that asked for it and answers 403 to anyone
                // else — which is how a video that plays in yt-dlp refuses to
                // play here.
                const QVariantMap headers =
                    chosen.value(QStringLiteral("http_headers")).toObject().toVariantMap();
                m_videoCache.insert(videoId, { video, audio, headers, expiryOf(video) });
                Q_EMIT videoResolved(videoId, video, audio, headers);
            });

    connect(request, &YtDlpRequest::failed, this, [this, videoId](const QString &reason) {
        if (m_videoJobs.take(videoId).isNull())
            return;
        Q_EMIT videoFailed(videoId, reason);
    });
}

void StreamResolver::cancelVideo(const QString &videoId)
{
    if (QPointer<YtDlpRequest> request = m_videoJobs.take(videoId); request)
        request->cancel();
}

void StreamResolver::prefetch(const QString &videoId)
{
    if (videoId.isEmpty() || m_jobs.contains(videoId))
        return;
    const QDateTime now = QDateTime::currentDateTimeUtc();
    const auto cached = m_cache.constFind(videoId);
    if (cached != m_cache.constEnd() && cached->expires > now)
        return;
    // Its next play is the link that rescued it (resolve()): an InnerTube
    // link fetched now would be refused, and would come first.
    if (innerTubeRefusedLately(videoId)) {
        const auto rescue = m_rescueLinks.constFind(videoId);
        if (rescue != m_rescueLinks.constEnd() && rescue->expires > now)
            return;
    }

    auto *job = new Job;
    job->videoId = videoId;
    job->prefetch = true;
    job->next = { TierYtDlp, TierMuxed, TierPiped, TierInvidious };
    m_jobs.insert(videoId, job);
    startTier(job, TierInnerTube);
}

void StreamResolver::invalidate(const QString &videoId)
{
    m_cache.remove(videoId);
    m_rescueLinks.remove(videoId);
    m_videoCache.remove(videoId);
}

void StreamResolver::dropRescueLinks()
{
    for (auto it = m_rescueLinks.begin(); it != m_rescueLinks.end();) {
        if (innerTubeRefusedLately(it.key()))
            ++it;
        else
            it = m_rescueLinks.erase(it);
    }
}

void StreamResolver::noteInnerTubeRefused(const QString &videoId)
{
    if (videoId.isEmpty())
        return;
    m_innerTubeRefused.insert(videoId, QDateTime::currentDateTimeUtc().addSecs(kInnerTubeRefusedMinutes * 60));
    qInfo("resolver: %s: googlevideo refused its InnerTube links; for %d minutes its next plays start from the "
          "link that rescues it, and InnerTube is not asked twice again", qPrintable(videoId),
          kInnerTubeRefusedMinutes);
}

bool StreamResolver::innerTubeRefusedLately(const QString &videoId) const
{
    const auto until = m_innerTubeRefused.constFind(videoId);
    return until != m_innerTubeRefused.constEnd() && *until > QDateTime::currentDateTimeUtc();
}

// googlevideo links carry their own expiry ("expire=<unix time>"). Trust it
// with a margin, and give instance links, which carry none, half an hour.
QDateTime StreamResolver::expiryOf(const QString &url)
{
    const QDateTime now = QDateTime::currentDateTimeUtc();
    bool ok = false;
    const qint64 expire = QUrlQuery(QUrl(url)).queryItemValue(QStringLiteral("expire")).toLongLong(&ok);
    if (!ok || expire <= 0)
        return now.addSecs(30 * 60);
    return qMin(QDateTime::fromSecsSinceEpoch(expire, QTimeZone::UTC).addSecs(-10 * 60), now.addSecs(5 * 3600));
}

void StreamResolver::startTier(Job *job, int tier)
{
    job->tier = tier;
    ++job->generation;
    Q_EMIT tierChanged(job->videoId, tier);

    if (tier < TierExhausted) {
        const auto test = m_testAnswers.constFind(job->videoId);
        if (test != m_testAnswers.constEnd() && test->contains(tier)) {
            answerForTest(job, test->value(tier));
            return;
        }
    }

    switch (tier) {
    case TierInnerTube:  startInnerTube(job);     break;
    case TierYtDlp:      startYtDlp(job);         break;
    case TierMuxed:      startMuxed(job);         break;
    case TierPiped:      startPipedRace(job);     break;
    case TierInvidious:  startInvidiousRace(job); break;
    case TierSignedIn:   startSignedIn(job);      break;
    default: {
        const QString reason = job->errors.isEmpty()
            ? QStringLiteral("No source could resolve this track.")
            : job->errors.join(QStringLiteral(" | "));
        const QString id = job->videoId;
        const bool silent = job->prefetch;
        discard(job);
        if (!silent)
            reportFailure(id, reason);
        break;
    }
    }
}

// One request, in this process, to the endpoint YouTube's own clients use.
// Everything that can go wrong here — a refused track, a stream offered only
// behind a cipher, a network failure — is the same answer: let yt-dlp have it.
void StreamResolver::startInnerTube(Job *job)
{
    const QString videoId = job->videoId;
    const int generation = job->generation;

    m_innerTube.player(videoId, [this, videoId, generation](const QString &url, int itag,
                                                            const QString &error) {
        Job *job = m_jobs.value(videoId);
        if (!job || job->settled || job->generation != generation)
            return;
        if (url.isEmpty()) {
            tierExhausted(job, QStringLiteral("InnerTube: %1").arg(error));
            return;
        }
        qInfo("innertube: %s resolved as itag %d", qPrintable(videoId), itag);
        succeed(job, handOverInnerTube(job, url));
    });
}

QString StreamResolver::handOverInnerTube(Job *job, const QString &url)
{
    if (job->prefetch || job->videoId != m_spoil || m_spoilsLeft <= 0)
        return url;
    if (--m_spoilsLeft == 0)
        m_spoil.clear();
    // The expiry is signed into the link, so any change to it is a link the
    // CDN answers with 403 — the refusal this stands in for.
    QUrl spoiled(url);
    QUrlQuery query(spoiled);
    if (query.hasQueryItem(QStringLiteral("expire"))) {
        query.removeAllQueryItems(QStringLiteral("expire"));
        query.addQueryItem(QStringLiteral("expire"), QStringLiteral("1"));
        spoiled.setQuery(query);
    } else {
        spoiled.setHost(QStringLiteral("spoiled.invalid"));
    }
    qInfo("innertube: %s handed over spoiled, for the self-test", qPrintable(job->videoId));
    return spoiled.toString(QUrl::FullyEncoded);
}

void StreamResolver::answerForTest(Job *job, const QString &url)
{
    QString link = url;
    if (!link.isEmpty()) {
        QUrl answer(url);
        QUrlQuery query(answer);
        query.addQueryItem(QStringLiteral("g"), QString::number(++m_testAnswersGiven));
        query.addQueryItem(QStringLiteral("expire"),
                           QString::number(QDateTime::currentSecsSinceEpoch() + 6 * 3600));
        answer.setQuery(query);
        link = answer.toString(QUrl::FullyEncoded);
    }
    // Queued, as a real answer always comes.
    const QString videoId = job->videoId;
    const int generation = job->generation;
    QMetaObject::invokeMethod(this, [this, videoId, generation, link]() {
        Job *job = m_jobs.value(videoId);
        if (!job || job->settled || job->generation != generation)
            return;
        if (link.isEmpty()) {
            tierExhausted(job, QStringLiteral("self-test: this tier fails"));
            return;
        }
        qInfo("resolver: %s answered by the self-test as %s", qPrintable(videoId), tierName(job->tier));
        succeed(job, job->tier == TierInnerTube ? handOverInnerTube(job, link) : link);
    }, Qt::QueuedConnection);
}

void StreamResolver::startYtDlp(Job *job)
{
    if (!YtDlp::isAvailable()) {
        tierExhausted(job, QStringLiteral("yt-dlp not installed"));
        return;
    }
    queueYtDlp(job);
}

// The muxed stream: itag 18 where it is offered, the best other stream with
// the sound and the picture in one file where it is not. mpv plays it as sound
// (vid=no), and the picture's bytes are the price of a stream that plays when
// the sound-only ones would not — paid for this track alone, and only then.
void StreamResolver::startMuxed(Job *job)
{
    if (!YtDlp::isAvailable()) {
        tierExhausted(job, QStringLiteral("yt-dlp not installed, so no muxed stream"));
        return;
    }
    queueYtDlp(job);
}

// Both of yt-dlp's rungs, once the job has its turn: the sound (TierYtDlp)
// or the muxed stream (TierMuxed).
void StreamResolver::launchYtDlp(Job *job)
{
    if (job->tier == TierSignedIn) {
        launchSignedIn(job);
        return;
    }
    const bool muxed = job->tier == TierMuxed;
    // When each lookup starts: with one at a time, the log is where a wait
    // for the turn shows.
    qInfo("resolver: %s: yt-dlp asked for %s%s", qPrintable(job->videoId),
          muxed ? "the muxed stream" : "the sound", job->prefetch ? ", ahead of time" : "");
    YtDlpRequest *request = muxed ? YtDlp::resolveMuxed(job->videoId, this) : YtDlp::resolveAudio(job->videoId, this);
    job->ytdlp = request;
    const QString videoId = job->videoId;
    const int generation = job->generation;
    const int tier = job->tier;

    // Nothing else bounds this. --socket-timeout is per socket operation, and
    // yt-dlp's own retry budget lets one resolve legitimately run for minutes
    // while the app sits on "Resolving source…" with no way out. The guard is
    // the same shape as the lambdas below, so a job that has already settled
    // or moved on ignores it; tierExhausted -> abortPending kills the process.
    QTimer::singleShot(kYtDlpTimeoutMs, this, [this, videoId, generation, tier, muxed]() {
        Job *job = m_jobs.value(videoId);
        if (job && !job->settled && job->generation == generation && job->tier == tier)
            tierExhausted(job, muxed ? QStringLiteral("yt-dlp timed out finding the muxed stream")
                                     : QStringLiteral("yt-dlp timed out"));
    });

    connect(request, &YtDlpRequest::succeededJson, this,
            [this, videoId, generation, muxed](const QJsonDocument &document) {
                Job *job = m_jobs.value(videoId);
                if (!job || job->settled || job->generation != generation)
                    return;
                const QJsonObject root = document.object();
                const QString url = root.value(QStringLiteral("url")).toString();
                if (!muxed) {
                    if (url.isEmpty())
                        tierExhausted(job, QStringLiteral("yt-dlp returned no audio url"));
                    else
                        succeed(job, url);
                    return;
                }
                if (url.isEmpty()) {
                    tierExhausted(job, QStringLiteral("yt-dlp returned no muxed stream"));
                    return;
                }
                // Which itag, and as which client: the log is the only place
                // that says what a failed track was rescued with.
                const QString client = QUrlQuery(QUrl(url)).queryItemValue(QStringLiteral("c"));
                qInfo("muxed: %s resolved as itag %s (%s), for this track alone",
                      qPrintable(videoId),
                      qPrintable(root.value(QStringLiteral("format_id")).toString()),
                      qPrintable(client.isEmpty() ? QStringLiteral("client not named") : client));
                // Fetched as the client that asked for it, or YouTube refuses
                // it (see resolveVideo).
                succeed(job, url, root.value(QStringLiteral("http_headers")).toObject().toVariantMap());
            });

    connect(request, &YtDlpRequest::failed, this,
            [this, videoId, generation, muxed](const QString &reason) {
                Job *job = m_jobs.value(videoId);
                if (job && !job->settled && job->generation == generation)
                    tierExhausted(job, (muxed ? QStringLiteral("muxed: %1") : QStringLiteral("yt-dlp: %1")).arg(reason));
            });
}

void StreamResolver::queueYtDlp(Job *job)
{
    if (!m_ytdlpOneAtATime) {
        launchYtDlp(job);
        return;
    }
    m_ytdlpWaiting.append(job);
    if (m_ytdlpHolder) {
        if (!job->prefetch && m_ytdlpHolder->prefetch) {
            // Someone is waiting for this song; the one running was only
            // fetched ahead of time.
            preemptYtDlp();
        } else {
            job->waitingForYtDlp.start();
            qInfo("resolver: %s waits its turn for yt-dlp: %s's lookup%s is running", qPrintable(job->videoId),
                  qPrintable(m_ytdlpHolder->videoId), m_ytdlpHolder->prefetch ? " (ahead of time)" : "");
        }
    }
    pumpYtDlp();
}

void StreamResolver::pumpYtDlp()
{
    if (!m_ytdlpOneAtATime || m_ytdlpHolder || m_ytdlpWaiting.isEmpty())
        return;
    // Songs someone is waiting for first, each in the order it asked; then
    // the ones fetched ahead of time, the same way.
    auto next = std::find_if(m_ytdlpWaiting.begin(), m_ytdlpWaiting.end(), [](const Job *job) {
        return !job->prefetch;
    });
    if (next == m_ytdlpWaiting.end())
        next = m_ytdlpWaiting.begin();
    Job *job = *next;
    m_ytdlpWaiting.erase(next);
    m_ytdlpHolder = job;
    if (job->waitingForYtDlp.isValid()) {
        qInfo("resolver: %s's turn for yt-dlp (%s), after %lld ms", qPrintable(job->videoId), tierName(job->tier),
              (long long)job->waitingForYtDlp.elapsed());
        job->waitingForYtDlp.invalidate();
    }
    launchYtDlp(job);
}

void StreamResolver::preemptYtDlp()
{
    Job *held = std::exchange(m_ytdlpHolder, nullptr);
    if (!held)
        return;
    // Its answer, its failure and its time limit are stale from here; it
    // stays on its rung and starts that lookup again when its turn comes.
    ++held->generation;
    if (QPointer<YtDlpRequest> request = std::exchange(held->ytdlp, {}); request && request->isRunning())
        request->cancel();
    // Its turn with the account too, should it have had one (a song whose
    // YouTube leg carried on unheard once JioSaavn won its race).
    if (m_accountHolder == held)
        m_accountHolder = nullptr;
    qInfo("resolver: %s's yt-dlp lookup, ahead of time, stopped for a song someone is waiting for; "
          "it waits its turn", qPrintable(held->videoId));
    held->waitingForYtDlp.start();
    m_ytdlpWaiting.prepend(held);   // first of the prefetches
}

void StreamResolver::releaseYtDlp(Job *job)
{
    m_ytdlpWaiting.removeAll(job);
    job->waitingForYtDlp.invalidate();
    // The account's turn, held or awaited, goes with it; the next lookup
    // waiting for it (ytdlp.resolves=parallel only) starts, on the next turn
    // of the event loop, as below.
    m_accountWaiting.removeAll(job);
    if (m_accountHolder == job) {
        m_accountHolder = nullptr;
        if (!m_accountWaiting.isEmpty()) {
            QMetaObject::invokeMethod(this, [this]() {
                if (!m_accountHolder && !m_accountWaiting.isEmpty())
                    launchSignedIn(m_accountWaiting.takeFirst());
            }, Qt::QueuedConnection);
        }
    }
    if (m_ytdlpHolder != job)
        return;
    m_ytdlpHolder = nullptr;
    // On the next turn of the event loop, not now: a job letting go on its
    // way to the muxed rung asks again first, and keeps its place.
    QMetaObject::invokeMethod(this, [this]() { pumpYtDlp(); }, Qt::QueuedConnection);
}

void StreamResolver::setYtDlpOneAtATime(bool on)
{
    m_ytdlpOneAtATime = on;
    if (!on) {
        // Whatever waits for its turn starts now.
        m_ytdlpHolder = nullptr;
        const QList<Job *> waiting = std::exchange(m_ytdlpWaiting, {});
        for (Job *job : waiting) {
            job->waitingForYtDlp.invalidate();
            launchYtDlp(job);
        }
    }
    foregroundChanged();
}

void StreamResolver::foregroundChanged()
{
    bool foreground = false;
    if (m_ytdlpOneAtATime) {
        for (const Job *job : std::as_const(m_jobs)) {
            if (!job->prefetch) {
                foreground = true;
                break;
            }
        }
    }
    if (foreground == m_foreground)
        return;
    m_foreground = foreground;
    YtDlp::setPlaybackResolving(this, foreground);
}

// ------------------------------------------------------------ the account

bool StreamResolver::accountWouldHelp(const QString &reason)
{
    // /player's statuses as InnerTube reports them ("VISIONOS 1.02:
    // LOGIN_REQUIRED: Sign in to confirm you're not a bot"), and yt-dlp's
    // own words for the same refusals ("Sign in to confirm your age. This
    // video may be inappropriate for some users.", "Private video. Sign in
    // if you've been granted access to this video"). A song that is simply
    // gone ("Video unavailable") is not one of them: no account brings it
    // back.
    static const QRegularExpression wanted(
        QStringLiteral("LOGIN_REQUIRED|AGE_VERIFICATION_REQUIRED|AGE_CHECK_REQUIRED|CONTENT_CHECK_REQUIRED"
                       "|not a bot|confirm your age|age[- ]restricted|inappropriate for some users|\\bsign in\\b"),
        QRegularExpression::CaseInsensitiveOption);
    return wanted.match(reason).hasMatch();
}

void StreamResolver::followAccount(YtmSession *account)
{
    if (m_account)
        m_account->disconnect(this);
    m_account = account;
    if (!account)
        return;
    connect(account, &YtmSession::sessionChanged, this, &StreamResolver::accountChanged);
    connect(account, &YtmSession::playWhenNeededChanged, this, &StreamResolver::accountChanged);
    // A rest begun: a lookup with the account under way stops.
    connect(account, &YtmSession::accountUseChanged, this, &StreamResolver::accountChanged);
}

void StreamResolver::setAccountLimit(int perHour)
{
    m_accountLimit = qMax(1, perHour);
}

void StreamResolver::accountChanged()
{
    // A link fetched with the account serves no play once the session it
    // came with has changed, ended, or may not be used: every one goes,
    // and the song is asked for afresh the next time it plays.
    int dropped = 0;
    for (QHash<QString, CacheEntry> *links : { &m_cache, &m_rescueLinks }) {
        for (auto it = links->begin(); it != links->end();) {
            if (it->tier == TierSignedIn) {
                it = links->erase(it);
                ++dropped;
            } else {
                ++it;
            }
        }
    }
    if (dropped > 0)
        qInfo("resolver: the account changed; %d link(s) fetched with it forgotten", dropped);
    QString why;
    if (m_account && m_account->accountForPlayback(&why))
        return;
    // A lookup with it under way stops, and its song goes on down the
    // anonymous ladder.
    QStringList stopped;
    for (const Job *job : std::as_const(m_jobs)) {
        if (job->tier == TierSignedIn && !job->settled)
            stopped << job->videoId;
    }
    for (const QString &videoId : std::as_const(stopped)) {
        if (Job *job = m_jobs.value(videoId); job && job->tier == TierSignedIn && !job->settled) {
            qInfo("resolver: %s: its lookup with the account stops: %s", qPrintable(videoId),
                  qPrintable(why.isEmpty() ? QStringLiteral("no account is followed") : why));
            tierExhausted(job, QStringLiteral("signed in: ") + why);
        }
    }
}

bool StreamResolver::accountLimitReached()
{
    const qint64 now = m_accountClock.elapsed();
    while (!m_accountUses.isEmpty() && now - m_accountUses.first() >= kAccountWindowMs)
        m_accountUses.removeFirst();
    return m_accountUses.size() >= m_accountLimit;
}

void StreamResolver::considerAccount(Job *job, const QString &reason)
{
    // Refused for want of an account by one of the anonymous rungs: a
    // refusal of the account's own says nothing new.
    if (job->tier < TierExhausted && accountWouldHelp(reason))
        job->accountWanted = true;
    // Once a job; never ahead of time, which may be for a song that never
    // plays (a prefetch taken over by a resolve decides at its next
    // refusal); never in a walk the caller laid out itself.
    if (!job->accountWanted || job->accountDecided || job->prefetch || !job->mayAddAccount
        || job->next.contains(TierSignedIn))
        return;
    job->accountDecided = true;
    QString why = QStringLiteral("no account is followed");
    if (m_account && m_account->accountForPlayback(&why)) {
        // The account is the last resort, not the next thing tried: yt-dlp
        // signed out first when it is still to come, since a refusal of one
        // client is often not another's, and every song asked for with the
        // account is traffic on the account (AccountGuard).
        const int ytdlp = int(job->next.indexOf(TierYtDlp));
        if (ytdlp >= 0) {
            job->next.insert(ytdlp + 1, TierSignedIn);
            qInfo("resolver: %s: refused signed out (%s); asking yt-dlp signed out, then with the account",
                  qPrintable(job->videoId), qPrintable(reason.left(160)));
        } else {
            job->next.prepend(TierSignedIn);
            qInfo("resolver: %s: refused signed out (%s); asking with the account next", qPrintable(job->videoId),
                  qPrintable(reason.left(160)));
        }
        return;
    }
    qInfo("resolver: %s: refused signed out (%s), and %s; it goes on signed out", qPrintable(job->videoId),
          qPrintable(reason.left(160)), qPrintable(why));
}

// The rung itself, and what may stop it at once: each time it is reached,
// since a session can end and the hour fill between a song being put here
// and its turn.
void StreamResolver::startSignedIn(Job *job)
{
    QString why;
    if (job->prefetch) {
        why = QStringLiteral("never ahead of time");
    } else if (!m_account) {
        why = QStringLiteral("no account is followed");
    } else if (!m_account->accountForPlayback(&why)) {
        // `why` says
    } else if (!YtDlp::isAvailable()) {
        why = QStringLiteral("yt-dlp not installed");
    } else if (accountLimitReached()) {
        why = QStringLiteral("%1 songs were asked for with the account in the last hour, the most it may be")
                  .arg(m_accountLimit);
    }
    if (!why.isEmpty()) {
        qInfo("resolver: %s: not asked with the account: %s", qPrintable(job->videoId), qPrintable(why));
        tierExhausted(job, QStringLiteral("signed in: ") + why);
        return;
    }
    queueYtDlp(job);
}

void StreamResolver::launchSignedIn(Job *job)
{
    // Never ahead of time, here as well as where the rung is reached
    // (startSignedIn): a song someone was waiting for can become a prefetch
    // while it waits on this rung or runs on it (JioSaavn won its race), and
    // one stopped for a song someone waits for (preemptYtDlp) comes back
    // here on its turn. Its lookup would spend one of the hour's songs, and
    // write the cookies out once more, for a song nobody is waiting for.
    if (job->prefetch) {
        qInfo("resolver: %s: not asked with the account: never ahead of time", qPrintable(job->videoId));
        tierExhausted(job, QStringLiteral("signed in: never ahead of time"));
        return;
    }
    // Only with ytdlp.resolves=parallel is another lookup running now; with
    // one yt-dlp at a time the account's turn is always free here.
    if (m_accountHolder && m_accountHolder != job) {
        if (!m_accountWaiting.contains(job))
            m_accountWaiting.append(job);
        qInfo("resolver: %s waits its turn with the account: %s's lookup is running", qPrintable(job->videoId),
              qPrintable(m_accountHolder->videoId));
        return;
    }
    QString cookieFile;
    QString error;
    const quint64 session = m_account ? m_account->openCookieFile(&cookieFile, &error) : 0;
    if (session == 0 || cookieFile.isEmpty()) {
        qInfo("resolver: %s: not asked with the account: %s", qPrintable(job->videoId), qPrintable(error));
        tierExhausted(job, QStringLiteral("signed in: ") + error);
        return;
    }
    m_accountHolder = job;
    m_accountUses.append(m_accountClock.elapsed());
    qInfo("resolver: %s: yt-dlp asked for the sound with the account (%d of %d this hour)", qPrintable(job->videoId),
          int(m_accountUses.size()), m_accountLimit);
    YtDlpRequest *request = YtDlp::resolveAudioSignedIn(job->videoId, cookieFile, this);
    job->ytdlp = request;

    // The cookies file goes with the lookup, however it ends: read back
    // first when yt-dlp exited on its own (only then was its jar written
    // back whole), deleted either way. Nothing here is this object's, which
    // may be going when the lookup does.
    const QPointer<YtmSession> account = m_account;
    auto closed = std::make_shared<bool>(false);
    const auto close = [account, closed, session, cookieFile](bool readBack) {
        if (std::exchange(*closed, true))
            return;
        if (account)
            account->closeCookieFile(session, cookieFile, readBack);
        else
            QFile::remove(cookieFile);
    };
    connect(request, &YtDlpRequest::succeededJson, request, [close, request]() { close(request->exitedOnItsOwn()); });
    connect(request, &YtDlpRequest::failed, request, [close, request]() { close(request->exitedOnItsOwn()); });
    connect(request, &QObject::destroyed, [close]() { close(false); });

    const QString videoId = job->videoId;
    const int generation = job->generation;
    // The same bound as yt-dlp's own rung, for the same reasons.
    QTimer::singleShot(kYtDlpTimeoutMs, this, [this, videoId, generation]() {
        Job *job = m_jobs.value(videoId);
        if (job && !job->settled && job->generation == generation && job->tier == TierSignedIn)
            tierExhausted(job, QStringLiteral("signed in: yt-dlp timed out"));
    });

    // Its answer is read, never logged: yt-dlp's info JSON may carry the
    // cookies it was given (GHSA-v8mc-9377-rwjj). The itag and the client
    // are all the log gets.
    connect(request, &YtDlpRequest::succeededJson, this,
            [this, videoId, generation, request](const QJsonDocument &document) {
                checkAccountWarning(request);
                Job *job = m_jobs.value(videoId);
                if (!job || job->settled || job->generation != generation)
                    return;
                const QJsonObject root = document.object();
                const QString url = root.value(QStringLiteral("url")).toString();
                if (url.isEmpty()) {
                    tierExhausted(job, QStringLiteral("signed in: yt-dlp returned no audio url"));
                    return;
                }
                // Fetched as the client that asked for it (see resolveVideo),
                // but never with a cookie: googlevideo takes none, and the
                // account's go nowhere but YouTube itself.
                QVariantMap headers = root.value(QStringLiteral("http_headers")).toObject().toVariantMap();
                for (auto it = headers.begin(); it != headers.end();) {
                    if (it.key().compare(QLatin1String("Cookie"), Qt::CaseInsensitive) == 0)
                        it = headers.erase(it);
                    else
                        ++it;
                }
                const QString client = QUrlQuery(QUrl(url)).queryItemValue(QStringLiteral("c"));
                qInfo("resolver: %s resolved with the account as itag %s (%s)", qPrintable(videoId),
                      qPrintable(root.value(QStringLiteral("format_id")).toString()),
                      qPrintable(client.isEmpty() ? QStringLiteral("client not named") : client));
                succeed(job, url, headers);
            });

    connect(request, &YtDlpRequest::failed, this, [this, videoId, generation, request](const QString &reason) {
        checkAccountWarning(request);
        Job *job = m_jobs.value(videoId);
        if (job && !job->settled && job->generation == generation)
            tierExhausted(job, QStringLiteral("signed in: %1").arg(reason));
    });
}

void StreamResolver::checkAccountWarning(const YtDlpRequest *request)
{
    // yt-dlp's warning when YouTube answered its signed-in requests as
    // signed out: "The provided YouTube account cookies are no longer
    // valid." The session's own check decides whether that is so.
    if (!m_account)
        return;
    if (request->errorOutput().contains(QLatin1String("cookies are no longer valid"), Qt::CaseInsensitive))
        m_account->doubt(QStringLiteral("yt-dlp says the account's cookies are no longer valid"));
    // A bot check, a 429 or a rate limit given to the account itself rests
    // every use of it, this rung's included.
    m_account->ytDlpSaid(request->errorOutput());
}

// ------------------------------------------------------------ the deadline

void StreamResolver::armDeadline(const QString &videoId)
{
    if (!m_deadlineOn)
        return;
    const int generation = ++m_deadlineGeneration;
    m_deadlines.insert(videoId, generation);
    // Precise: a coarse timer may be 5% late on macOS and Linux, a second
    // here.
    QTimer::singleShot(kResolveDeadlineMs, Qt::PreciseTimer, this, [this, videoId, generation]() {
        if (m_deadlines.value(videoId) == generation)
            deadlinePassed(videoId);
    });
}

void StreamResolver::deadlinePassed(const QString &videoId)
{
    m_deadlines.remove(videoId);
    const auto race = m_races.find(videoId);
    // YouTube's answer is in hand, held a moment for JioSaavn's: it plays.
    if (race != m_races.end() && race->youtubeReady) {
        qInfo("jiosaavn: %s had not answered by the %d s limit; YouTube plays", qPrintable(videoId),
              kResolveDeadlineMs / 1000);
        youtubeWins(videoId);
        return;
    }
    QString reason = QStringLiteral("No source answered within %1 s").arg(kResolveDeadlineMs / 1000);
    Job *job = m_jobs.value(videoId);
    if (job && !job->prefetch) {
        const bool waiting = m_ytdlpWaiting.contains(job);
        qInfo("resolver: %s not resolved within %d s, still on %s%s; giving up", qPrintable(videoId),
              kResolveDeadlineMs / 1000, tierName(job->tier), waiting ? " (waiting its turn for yt-dlp)" : "");
        reason += QStringLiteral(" (still on %1)").arg(QString::fromLatin1(tierName(job->tier)));
        if (!job->errors.isEmpty())
            reason += QStringLiteral(": ") + job->errors.join(QStringLiteral(" | "));
        cancelJob(videoId);
    } else if (race != m_races.end() && race->youtubeFailed) {
        qInfo("resolver: %s: YouTube had failed and JioSaavn had not answered within %d s", qPrintable(videoId),
              kResolveDeadlineMs / 1000);
        reason += QStringLiteral(" (JioSaavn did not answer): ") + race->failure;
    } else {
        return;   // nothing left of it to give up on
    }
    m_races.remove(videoId);
    emitFailed(videoId, reason);
}

void StreamResolver::emitResolved(const QString &videoId, const QString &url, int tier, bool fromCache)
{
    m_deadlines.remove(videoId);
    Q_EMIT resolved(videoId, url, tier, fromCache);
}

void StreamResolver::emitFailed(const QString &videoId, const QString &reason)
{
    m_deadlines.remove(videoId);
    Q_EMIT failed(videoId, reason);
}

// Piped returns a JSON list of audio streams pointing straight at the YouTube
// CDN. Fine for playback; the CDN rejects byte-range fetches from other
// origins, which is why downloads prefer Invidious.
void StreamResolver::startPipedRace(Job *job)
{
    if (m_piped.isEmpty()) {
        tierExhausted(job, QStringLiteral("no Piped instances configured"));
        return;
    }

    const QString videoId = job->videoId;
    // Shared, not owned by any one lambda: aborting the losers fires their
    // finished() handlers synchronously, so a raw counter deleted by the winner
    // would be read after free.
    auto remaining = std::make_shared<int>(m_piped.size());
    const int generation = job->generation;

    for (const QString &host : std::as_const(m_piped)) {
        const QUrl url(QStringLiteral("https://%1/streams/%2").arg(host, videoId));
        QNetworkReply *reply = m_network->get(makeRequest(url));
        job->pending.append(reply);

        connect(reply, &QNetworkReply::finished, this, [this, reply, videoId, generation, remaining]() {
            reply->deleteLater();
            Job *job = m_jobs.value(videoId);
            const bool live = job && !job->settled && job->generation == generation;

            if (live && reply->error() == QNetworkReply::NoError) {
                const QJsonObject root = QJsonDocument::fromJson(reply->readAll()).object();
                const QJsonArray streams = root.value(QStringLiteral("audioStreams")).toArray();

                QString best;
                int bestBitrate = -1;
                for (const QJsonValue &value : streams) {
                    const QJsonObject stream = value.toObject();
                    const QString streamUrl = stream.value(QStringLiteral("url")).toString();
                    if (streamUrl.isEmpty())
                        continue;
                    const bool isMp4 = stream.value(QStringLiteral("mimeType")).toString()
                                           .contains(QLatin1String("mp4"));
                    const int bitrate = stream.value(QStringLiteral("bitrate")).toInt()
                                        + (isMp4 ? 1'000'000 : 0); // prefer m4a
                    if (bitrate > bestBitrate) {
                        bestBitrate = bitrate;
                        best = streamUrl;
                    }
                }

                if (!best.isEmpty()) {
                    // First instance to answer wins; the rest are aborted.
                    succeed(job, best);
                    return;
                }
            }

            if (--(*remaining) == 0 && live)
                tierExhausted(job, QStringLiteral("all Piped instances failed"));
        });
    }
}

// Invidious with local=true proxies the bytes through the instance itself, so
// the resulting URL is a normal HTTP resource — fetchable, seekable, and usable
// for downloads as well as playback.
void StreamResolver::startInvidiousRace(Job *job)
{
    if (m_invidious.isEmpty()) {
        tierExhausted(job, QStringLiteral("no Invidious instances configured"));
        return;
    }

    const QString videoId = job->videoId;
    auto remaining = std::make_shared<int>(m_invidious.size());
    const int generation = job->generation;

    for (const QString &host : std::as_const(m_invidious)) {
        const QUrl url(QStringLiteral("https://%1/latest_version?id=%2&itag=140&local=true")
                           .arg(host, videoId));
        QNetworkRequest request = makeRequest(url);
        request.setTransferTimeout(kInvidiousTimeoutMs);

        // HEAD is enough to learn whether the instance will serve this id,
        // and avoids pulling the file body during probing.
        QNetworkReply *reply = m_network->head(request);
        job->pending.append(reply);

        connect(reply, &QNetworkReply::finished, this,
                [this, reply, videoId, url, generation, remaining]() {
                    reply->deleteLater();
                    Job *job = m_jobs.value(videoId);
                    const bool live = job && !job->settled && job->generation == generation;

                    const int status = reply->attribute(
                        QNetworkRequest::HttpStatusCodeAttribute).toInt();
                    const QString contentType = reply->header(
                        QNetworkRequest::ContentTypeHeader).toString();

                    // A dead or rate-limited instance answers 200 with an HTML
                    // error page. Accepting on status alone handed mpv a web
                    // page and produced "unrecognized file format", so require
                    // the response to actually claim to be media.
                    const bool isMedia = contentType.startsWith(QLatin1String("audio/"))
                                      || contentType.startsWith(QLatin1String("video/"))
                                      || contentType.startsWith(QLatin1String("application/octet-stream"));

                    if (live && reply->error() == QNetworkReply::NoError
                        && status >= 200 && status < 300 && isMedia) {
                        succeed(job, url.toString());
                        return;
                    }

                    if (--(*remaining) == 0 && live)
                        tierExhausted(job, QStringLiteral("all Invidious instances failed"));
                });
    }
}

void StreamResolver::succeed(Job *job, const QString &url, const QVariantMap &headers)
{
    if (job->settled)
        return;
    job->settled = true;

    const QString videoId = job->videoId;
    const int tier = job->tier;
    const bool silent = job->prefetch;
    // A rescue link serves the play it rescued, and no replay: see
    // resolveVia. InnerTube's own links are never one. For a song whose
    // InnerTube links are refused lately every other link is one, however
    // it was found, and is kept apart while that lasts (dropRescueLinks).
    const bool rescue = tier != TierInnerTube
                        && ((job->homeTier >= 0 && tier != job->homeTier) || innerTubeRefusedLately(videoId));
    if (rescue && !m_keepRescueLinks) {
        m_rescueLinks.insert(videoId, { url, tier, expiryOf(url), headers });
        qInfo("resolver: %s rescued by %s, %s", qPrintable(videoId), tierName(tier),
              innerTubeRefusedLately(videoId) ? "kept for its next plays while its InnerTube links are refused"
                                              : "for this play only");
    } else {
        m_cache.insert(videoId, { url, tier, expiryOf(url), headers });
    }

    abortPending(job);
    discard(job);

    if (!silent)
        report(videoId, url, tier, /*fromCache=*/false);
}

void StreamResolver::tierExhausted(Job *job, const QString &reason)
{
    if (job->settled)
        return;
    job->errors.append(reason);
    abortPending(job);
    considerAccount(job, reason);
    startTier(job, job->next.isEmpty() ? int(TierExhausted) : job->next.takeFirst());
}

void StreamResolver::abortPending(Job *job)
{
    // Bump first: abort() and cancel() below deliver their callbacks
    // synchronously, and those callbacks must see themselves as stale rather
    // than advance the tier a second time.
    ++job->generation;

    // Detach the lists before touching them — starting the next tier appends to
    // job->pending, which must not happen mid-iteration.
    const QList<QPointer<QNetworkReply>> replies = std::exchange(job->pending, {});
    for (const QPointer<QNetworkReply> &reply : replies) {
        if (reply && reply->isRunning())
            reply->abort();
    }

    if (QPointer<YtDlpRequest> request = std::exchange(job->ytdlp, {}); request && request->isRunning())
        request->cancel();
    // Its turn at yt-dlp, held or awaited, is over.
    releaseYtDlp(job);
}

void StreamResolver::discard(Job *job)
{
    releaseYtDlp(job);
    m_jobs.remove(job->videoId);
    delete job;
    foregroundChanged();
}

void StreamResolver::cancel(const QString &videoId)
{
    m_races.remove(videoId);
    m_deadlines.remove(videoId);
    cancelJob(videoId);
}

void StreamResolver::cancelJob(const QString &videoId)
{
    Job *job = m_jobs.value(videoId);
    if (!job)
        return;
    job->settled = true;
    abortPending(job);
    discard(job);
}

void StreamResolver::cancelAll()
{
    m_races.clear();
    m_deadlines.clear();
    const QStringList ids = m_jobs.keys();
    for (const QString &id : ids)
        cancelJob(id);
}

// ------------------------------------------------------------------ the race

void StreamResolver::resolveTrack(const Saavn::Target &track)
{
    const QString videoId = track.videoId;
    if (videoId.isEmpty()) {
        emitFailed(videoId, QStringLiteral("Empty video id."));
        return;
    }
    // Asked again for the same song: the earlier race is over, unheard, and
    // a late answer to it is no longer news for what is playing.
    m_races.remove(videoId);
    m_saavnLate.remove(videoId);

    const bool resting = m_saavnRestUntil.isValid() && m_saavnRestUntil > QDateTime::currentDateTimeUtc();
    if (!m_saavnEnabled || track.title.trimmed().isEmpty()) {
        resolve(videoId);
        return;
    }

    const SaavnVerdict verdict = saavnVerdict(track);

    // For the self-tests (setTestAnswer with TierJioSaavn): JioSaavn's link
    // given as a match found before is, asking no one, until mpv refuses it;
    // an empty one is a song JioSaavn does not have.
    const auto test = m_testAnswers.constFind(videoId);
    if (test != m_testAnswers.constEnd() && test->contains(TierJioSaavn)) {
        const QString url = test->value(TierJioSaavn);
        if (url.isEmpty() || verdict.kind == SaavnVerdict::Refused) {
            resolve(videoId);
            return;
        }
        qInfo("resolver: %s answered by the self-test as JioSaavn", qPrintable(videoId));
        QMetaObject::invokeMethod(this, [this, videoId, url]() {
            emitResolved(videoId, url, TierJioSaavn, /*fromCache=*/true);
        }, Qt::QueuedConnection);
        prefetch(videoId);
        return;
    }
    if (verdict.kind == SaavnVerdict::Match) {
        qInfo("jiosaavn: %s plays from the match found before (%s, %d kbps)", qPrintable(videoId),
              qPrintable(verdict.saavnId), verdict.kbps);
        const QString url = handOverSaavn(videoId, verdict.url);
        // Queued, like a remembered YouTube link, so the caller sees the
        // same order of events either way.
        QMetaObject::invokeMethod(this, [this, videoId, url]() {
            emitResolved(videoId, url, TierJioSaavn, /*fromCache=*/true);
        }, Qt::QueuedConnection);
        // YouTube's link, fetched quietly beside it: should mpv refuse
        // JioSaavn's, the way back starts warm rather than from nothing.
        prefetch(videoId);
        return;
    }
    if (verdict.kind != SaavnVerdict::Unknown || resting) {
        resolve(videoId);
        return;
    }

    // Both at once. The ladder's answer comes back through report(), which
    // finds the race and holds it there.
    Race race;
    race.clock.start();
    race.generation = ++m_raceGeneration;
    m_races.insert(videoId, race);
    startSaavnLookup(track);
    resolve(videoId);
}

void StreamResolver::prefetchTrack(const Saavn::Target &track)
{
    if (track.videoId.isEmpty())
        return;
    const bool resting = m_saavnRestUntil.isValid() && m_saavnRestUntil > QDateTime::currentDateTimeUtc();
    if (m_saavnEnabled && !track.title.trimmed().isEmpty()) {
        if (saavnVerdict(track).kind == SaavnVerdict::Unknown && !resting)
            startSaavnLookup(track);
    }
    // YouTube's link too, even where JioSaavn's is known: it is what the song
    // falls back to at once should mpv refuse JioSaavn's.
    prefetch(track.videoId);
}

void StreamResolver::report(const QString &videoId, const QString &url, int tier, bool fromCache)
{
    const auto race = m_races.find(videoId);
    if (race == m_races.end()) {
        emitResolved(videoId, url, tier, fromCache);
        return;
    }
    race->youtubeReady = true;
    race->url = url;
    race->tier = tier;
    race->fromCache = fromCache;
    const qint64 left = kSaavnGraceMs - race->clock.elapsed();
    if (left <= 0) {
        qInfo("jiosaavn: %s has not answered; YouTube plays", qPrintable(videoId));
        youtubeWins(videoId);
        return;
    }
    const int generation = race->generation;
    QTimer::singleShot(int(left), this, [this, videoId, generation]() {
        const auto race = m_races.constFind(videoId);
        if (race == m_races.constEnd() || race->generation != generation)
            return;
        qInfo("jiosaavn: %s did not answer within %d ms; YouTube plays, and a later answer is kept",
              qPrintable(videoId), kSaavnGraceMs);
        youtubeWins(videoId);
    });
}

// The ladder found nothing. With a race on, JioSaavn is all that is left and
// is waited for — its own timeouts bound the wait — before the failure is
// told.
void StreamResolver::reportFailure(const QString &videoId, const QString &reason)
{
    const auto race = m_races.find(videoId);
    if (race == m_races.end()) {
        emitFailed(videoId, reason);
        return;
    }
    race->youtubeFailed = true;
    race->failure = reason;
}

void StreamResolver::youtubeWins(const QString &videoId)
{
    const Race race = m_races.take(videoId);
    // JioSaavn still to answer: what it says is for the song already
    // playing, which may move over to it (saavnLateMatch).
    if (m_saavnAsking.contains(videoId))
        m_saavnLate.insert(videoId);
    emitResolved(videoId, race.url, race.tier, race.fromCache);
}

void StreamResolver::startSaavnLookup(const Saavn::Target &track)
{
    if (m_saavnAsking.contains(track.videoId))
        return;   // already on its way; its answer serves this too
    m_saavnAsking.insert(track.videoId);
    const QString videoId = track.videoId;
    const QString signature = Saavn::signature(track);
    const int delay = m_saavnTestDelayMs;
    m_saavn.lookup(track, [this, videoId, signature, delay](const JioSaavn::Result &result) {
        if (delay <= 0) {
            saavnAnswered(videoId, signature, result);
            return;
        }
        qInfo("jiosaavn: %s answered; held back %d ms for the self-test (--saavn-late)", qPrintable(videoId), delay);
        QTimer::singleShot(delay, this, [this, videoId, signature, result]() {
            saavnAnswered(videoId, signature, result);
        });
    });
}

void StreamResolver::findSaavn(const Saavn::Target &track, QObject *context, SaavnCopyCallback done)
{
    const QPointer<QObject> guard(context);
    const auto answer = [this, guard, done](const SaavnCopy &copy) {
        // Always later, so the caller hears back the same way whatever the
        // answer, and never while it is still asking. A copy only while
        // JioSaavn is still on by then.
        QMetaObject::invokeMethod(this, [this, guard, done, copy]() {
            if (!guard)
                return;
            if (!copy.url.isEmpty() && !m_saavnEnabled) {
                SaavnCopy off;
                off.reason = QStringLiteral("JioSaavn is off (Standard sound quality)");
                done(off);
                return;
            }
            done(copy);
        }, Qt::QueuedConnection);
    };
    SaavnCopy none;
    if (!m_saavnEnabled) {
        none.reason = QStringLiteral("JioSaavn is off (Standard sound quality)");
        answer(none);
        return;
    }
    if (track.videoId.isEmpty() || track.title.trimmed().isEmpty()) {
        none.reason = QStringLiteral("no title to look for");
        answer(none);
        return;
    }
    const SaavnVerdict verdict = saavnVerdict(track);
    switch (verdict.kind) {
    case SaavnVerdict::Match: {
        SaavnCopy copy;
        copy.url = verdict.url;
        copy.kbps = verdict.kbps;
        copy.saavnId = verdict.saavnId;
        answer(copy);
        return;
    }
    case SaavnVerdict::NoMatch:
        none.reason = QStringLiteral("JioSaavn does not have this recording");
        answer(none);
        return;
    case SaavnVerdict::Refused:
        none.reason = QStringLiteral("JioSaavn's link was refused a few minutes ago");
        answer(none);
        return;
    case SaavnVerdict::Unknown:
        break;
    }
    if (m_saavnRestUntil.isValid() && m_saavnRestUntil > QDateTime::currentDateTimeUtc()) {
        none.reason = QStringLiteral("JioSaavn is being left alone for a few minutes");
        answer(none);
        return;
    }
    // Asked, or already being asked: the answer comes through saavnAnswered.
    m_saavnWaiters[track.videoId].append([guard, done](const SaavnCopy &copy) {
        if (guard)
            done(copy);
    });
    startSaavnLookup(track);
}

void StreamResolver::saavnAnswered(const QString &videoId, const QString &signature,
                                   const JioSaavn::Result &result)
{
    m_saavnAsking.remove(videoId);
    const QDateTime now = QDateTime::currentDateTimeUtc();

    switch (result.kind) {
    case JioSaavn::Result::Match: {
        m_saavnFailuresInARow = 0;
        SaavnVerdict verdict;
        verdict.kind = SaavnVerdict::Match;
        verdict.url = result.url;
        verdict.kbps = result.kbps;
        verdict.saavnId = result.saavnId;
        verdict.signature = signature;
        verdict.expires = now.addDays(kSaavnMatchDays);
        rememberSaavn(videoId, verdict);
        // The host and the bitrate: the link itself stays out of the log.
        qInfo("jiosaavn: %s is \"%s\" by %s (%s, %d s) at %d kbps from %s, found in %lld ms", qPrintable(videoId),
              qPrintable(result.title), qPrintable(result.artists), qPrintable(result.saavnId), result.durationSec,
              result.kbps, qPrintable(QUrl(result.url).host()), static_cast<long long>(result.elapsedMs));
        break;
    }
    case JioSaavn::Result::NoMatch: {
        m_saavnFailuresInARow = 0;
        SaavnVerdict verdict;
        verdict.kind = SaavnVerdict::NoMatch;
        verdict.signature = signature;
        verdict.expires = now.addSecs(kSaavnNoMatchHours * 3600);
        rememberSaavn(videoId, verdict);
        qInfo("jiosaavn: %s not taken from JioSaavn: %s (%d rows, %lld ms)", qPrintable(videoId),
              qPrintable(result.reason), result.rows, static_cast<long long>(result.elapsedMs));
        // The first few reasons, so a song that should have matched can be
        // looked into from the log.
        for (qsizetype i = 0; i < result.refusals.size() && i < 3; ++i)
            qInfo("jiosaavn:   %s", qPrintable(result.refusals.at(i)));
        break;
    }
    case JioSaavn::Result::Failed:
        // No verdict: JioSaavn was not heard from, so nothing is remembered
        // about the song and it is asked again next time.
        qWarning("jiosaavn: %s could not be looked up: %s", qPrintable(videoId), qPrintable(result.reason));
        if (++m_saavnFailuresInARow >= kSaavnFailuresBeforeRest) {
            m_saavnFailuresInARow = 0;
            m_saavnRestUntil = now.addSecs(kSaavnRestMinutes * 60);
            qWarning("jiosaavn: %d lookups failed in a row; not asking for %d minutes",
                     kSaavnFailuresBeforeRest, kSaavnRestMinutes);
        }
        break;
    }

    // Standard sound quality was picked while JioSaavn was being asked: what
    // it said is kept, above, for when High is picked again, and used for
    // nothing now. Not a download's copy, not the song playing, not a race.
    const bool usable = m_saavnEnabled;
    if (!usable)
        qInfo("jiosaavn: %s: answered after JioSaavn was turned off (Standard sound quality); not used",
              qPrintable(videoId));

    // Downloads that were waiting on this lookup (findSaavn).
    if (const QList<SaavnCopyCallback> waiters = m_saavnWaiters.take(videoId); !waiters.isEmpty()) {
        SaavnCopy copy;
        if (!usable) {
            copy.reason = QStringLiteral("JioSaavn is off (Standard sound quality)");
        } else if (result.kind == JioSaavn::Result::Match) {
            copy.url = result.url;
            copy.kbps = result.kbps;
            copy.saavnId = result.saavnId;
            copy.album = result.album;
            copy.durationSec = result.durationSec;
        } else {
            copy.reason = result.kind == JioSaavn::Result::NoMatch
                              ? QStringLiteral("JioSaavn does not have this recording (%1)").arg(result.reason)
                              : QStringLiteral("JioSaavn could not be asked: %1").arg(result.reason);
        }
        for (const SaavnCopyCallback &waiter : waiters)
            waiter(copy);
    }

    // The answer to a race YouTube has already won: the song playing may
    // move over to it (QT7), which is the player's to decide.
    if (m_saavnLate.remove(videoId) && usable && result.kind == JioSaavn::Result::Match) {
        qInfo("jiosaavn: %s: the match came after YouTube had begun (%lld ms after it was asked); "
              "the song may move over to it", qPrintable(videoId), static_cast<long long>(result.elapsedMs));
        Q_EMIT saavnLateMatch(videoId, handOverSaavn(videoId, result.url), result.kbps, result.durationSec);
    }

    const auto race = m_races.find(videoId);
    if (race == m_races.end())
        return;   // a prefetch, or a song the race already gave to YouTube
    if (usable && result.kind == JioSaavn::Result::Match) {
        m_races.erase(race);
        // YouTube's leg carries on unheard, into the cache: should mpv refuse
        // JioSaavn's link, the way back is already there.
        carryOnUnheard(videoId);
        emitResolved(videoId, handOverSaavn(videoId, result.url), TierJioSaavn, /*fromCache=*/false);
        return;
    }
    if (race->youtubeReady) {
        youtubeWins(videoId);
        return;
    }
    if (race->youtubeFailed) {
        const QString reason = race->failure;
        m_races.erase(race);
        emitFailed(videoId, reason);
        return;
    }
    // YouTube is still on its way, and is now simply the answer.
    m_races.erase(race);
}

void StreamResolver::carryOnUnheard(const QString &videoId)
{
    if (Job *job = m_jobs.value(videoId)) {
        job->prefetch = true;
        foregroundChanged();
    }
}

StreamResolver::SaavnVerdict StreamResolver::saavnVerdict(const Saavn::Target &track)
{
    const QString &videoId = track.videoId;
    const QString signature = Saavn::signature(track);
    const QDateTime now = QDateTime::currentDateTimeUtc();
    const auto known = m_saavnVerdicts.constFind(videoId);
    if (known != m_saavnVerdicts.constEnd()) {
        if (known->expires <= now) {
            m_saavnVerdicts.remove(videoId);
            return {};   // the table's copy expired with it
        }
        // Asked about under another name or length: that answer was for a
        // different question.
        if (known->kind != SaavnVerdict::Refused && known->signature != signature)
            return {};
        return *known;
    }

    purgeExpiredSaavn();
    QSqlDatabase db = AppDatabase::connection();
    if (!db.isOpen())
        return {};
    QSqlQuery query(db);
    query.prepare(QStringLiteral(
        "SELECT matched, saavn_id, url, kbps, expires_at, signature, matcher FROM saavn_matches"
        " WHERE video_id = ?"));
    query.addBindValue(videoId);
    if (!query.exec() || !query.next())
        return {};
    SaavnVerdict verdict;
    verdict.expires = QDateTime::fromSecsSinceEpoch(query.value(4).toLongLong(), QTimeZone::UTC);
    verdict.signature = query.value(5).toString();
    // Past its time, judged by an older matcher, or about the song under
    // another name or length: asked afresh, and the new answer replaces it.
    if (verdict.expires <= now || query.value(6).toInt() != Saavn::matcherVersion()
        || verdict.signature != signature)
        return {};
    verdict.kind = query.value(0).toInt() == 1 ? SaavnVerdict::Match : SaavnVerdict::NoMatch;
    verdict.saavnId = query.value(1).toString();
    verdict.url = query.value(2).toString();
    verdict.kbps = query.value(3).toInt();
    // The link goes to mpv as it is: JioSaavn's CDN over HTTPS, or nothing.
    if (verdict.kind == SaavnVerdict::Match && !Saavn::isCdnLink(verdict.url))
        return {};
    m_saavnVerdicts.insert(videoId, verdict);
    return verdict;
}

void StreamResolver::purgeExpiredSaavn()
{
    if (m_saavnPurged)
        return;
    QSqlDatabase db = AppDatabase::connection();
    if (!db.isOpen())
        return;
    m_saavnPurged = true;
    QSqlQuery query(db);
    query.prepare(QStringLiteral("DELETE FROM saavn_matches WHERE expires_at <= ?"));
    query.addBindValue(QDateTime::currentSecsSinceEpoch());
    query.exec();
}

void StreamResolver::rememberSaavn(const QString &videoId, const SaavnVerdict &verdict)
{
    m_saavnVerdicts.insert(videoId, verdict);
    if (verdict.kind != SaavnVerdict::Match && verdict.kind != SaavnVerdict::NoMatch)
        return;   // a refusal is for this session only
    QSqlDatabase db = AppDatabase::connection();
    if (!db.isOpen())
        return;
    // When it was asked is not kept, only when the answer runs out, which is
    // all anything reads; Clear history empties the table (Library).
    QSqlQuery query(db);
    query.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO saavn_matches (video_id, matched, saavn_id, url, kbps, expires_at, signature, matcher)"
        " VALUES (?, ?, ?, ?, ?, ?, ?, ?)"));
    query.addBindValue(videoId);
    query.addBindValue(verdict.kind == SaavnVerdict::Match ? 1 : 0);
    query.addBindValue(AppDatabase::text(verdict.saavnId));
    query.addBindValue(AppDatabase::text(verdict.url));
    query.addBindValue(verdict.kbps);
    query.addBindValue(verdict.expires.toSecsSinceEpoch());
    query.addBindValue(AppDatabase::text(verdict.signature));
    query.addBindValue(Saavn::matcherVersion());
    if (!query.exec())
        qWarning("jiosaavn: could not remember the answer for %s", qPrintable(videoId));
}

void StreamResolver::refuseSaavn(const QString &videoId)
{
    SaavnVerdict verdict;
    verdict.kind = SaavnVerdict::Refused;
    verdict.expires = QDateTime::currentDateTimeUtc().addSecs(kSaavnRefusedMinutes * 60);
    m_saavnVerdicts.insert(videoId, verdict);
    QSqlDatabase db = AppDatabase::connection();
    if (db.isOpen()) {
        QSqlQuery query(db);
        query.prepare(QStringLiteral("DELETE FROM saavn_matches WHERE video_id = ?"));
        query.addBindValue(videoId);
        query.exec();
    }
}

void StreamResolver::passOverSaavn(const QString &videoId, const QString &why)
{
    const auto known = m_saavnVerdicts.constFind(videoId);
    if (known == m_saavnVerdicts.constEnd() || known->kind != SaavnVerdict::Match)
        return;
    const QString saavnId = known->saavnId;
    SaavnVerdict verdict;
    verdict.kind = SaavnVerdict::NoMatch;
    verdict.signature = known->signature;
    verdict.expires = QDateTime::currentDateTimeUtc().addDays(kSaavnMatchDays);
    rememberSaavn(videoId, verdict);
    qInfo("jiosaavn: %s: its match (%s) is passed over for %d days, by the race and by downloads: %s",
          qPrintable(videoId), qPrintable(saavnId), kSaavnMatchDays, qUtf8Printable(why));
}

QString StreamResolver::handOverSaavn(const QString &videoId, const QString &url)
{
    if (videoId != m_spoilSaavn)
        return url;
    m_spoilSaavn.clear();
    // A file the CDN does not have, on the host it does serve: refused the
    // way a moved or withdrawn file would be.
    QUrl spoiled(url);
    spoiled.setPath(QStringLiteral("/000/monolist-selftest-missing_320.mp4"));
    qInfo("jiosaavn: %s handed over spoiled, for the self-test", qPrintable(videoId));
    return spoiled.toString(QUrl::FullyEncoded);
}

int StreamResolver::saavnKbps(const QString &videoId) const
{
    const auto known = m_saavnVerdicts.constFind(videoId);
    return known != m_saavnVerdicts.constEnd() && known->kind == SaavnVerdict::Match ? known->kbps : 0;
}

void StreamResolver::setSaavnIndiaHeaders(bool on, bool forgetNoMatches)
{
    if (on == m_saavn.indiaHeaders())
        return;
    m_saavn.setIndiaHeaders(on);
    if (!forgetNoMatches)
        return;
    for (auto it = m_saavnVerdicts.begin(); it != m_saavnVerdicts.end();) {
        if (it->kind == SaavnVerdict::NoMatch)
            it = m_saavnVerdicts.erase(it);
        else
            ++it;
    }
    QSqlDatabase db = AppDatabase::connection();
    if (db.isOpen()) {
        QSqlQuery query(db);
        query.exec(QStringLiteral("DELETE FROM saavn_matches WHERE matched = 0"));
    }
}

