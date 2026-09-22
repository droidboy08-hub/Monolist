#include "streamresolver.h"
#include "ytdlp.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimeZone>
#include <QUrlQuery>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>

#include <memory>
#include <utility>

namespace {

constexpr int kPipedTimeoutMs = 8000;
constexpr int kInvidiousTimeoutMs = 6000;

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

QStringList StreamResolver::defaultPipedInstances()
{
    return {
        QStringLiteral("pipedapi.kavin.rocks"),
        QStringLiteral("pipedapi.moomoo.me"),
        QStringLiteral("piped-api.garudalinux.org"),
        QStringLiteral("api.piped.projectsegfau.lt"),
        QStringLiteral("piped.privacydev.net")
    };
}

QStringList StreamResolver::defaultInvidiousInstances()
{
    return {
        QStringLiteral("yewtu.be"),
        QStringLiteral("invidious.projectsegfau.lt"),
        QStringLiteral("iv.ggtyler.dev"),
        QStringLiteral("inv.nadeko.net"),
        QStringLiteral("invidious.nerdvpn.de"),
        QStringLiteral("invidious.privacydev.net"),
        QStringLiteral("yt.artemislena.eu"),
        QStringLiteral("invidious.fdn.fr"),
        QStringLiteral("invidious.slipfox.xyz"),
        QStringLiteral("invidious.lunar.icu"),
        QStringLiteral("invidious.vps.sh"),
        QStringLiteral("inv.tux.pizza"),
        QStringLiteral("invidious.io.lol")
    };
}

StreamResolver::StreamResolver(QObject *parent)
    : QObject(parent)
    , m_network(new QNetworkAccessManager(this))
    , m_piped(defaultPipedInstances())
    , m_invidious(defaultInvidiousInstances())
{
}

StreamResolver::~StreamResolver()
{
    cancelAll();
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
        Q_EMIT failed(videoId, QStringLiteral("Empty video id."));
        return;
    }

    if (firstTier <= TierYtDlp) {
        // Still-valid link from earlier or from a prefetch: answer at once,
        // but queued, so the caller sees the same order of events as always.
        const auto cached = m_cache.constFind(videoId);
        if (cached != m_cache.constEnd() && cached->expires > QDateTime::currentDateTimeUtc()) {
            const CacheEntry entry = *cached;
            QMetaObject::invokeMethod(this, [this, videoId, entry]() {
                Q_EMIT resolved(videoId, entry.url, entry.tier, /*fromCache=*/true);
            }, Qt::QueuedConnection);
            return;
        }
        // A prefetch already on its way: let it finish, and report this time.
        if (Job *running = m_jobs.value(videoId); running && running->prefetch) {
            running->prefetch = false;
            return;
        }
    }

    cancel(videoId);

    auto *job = new Job;
    job->videoId = videoId;
    m_jobs.insert(videoId, job);
    startTier(job, qBound(int(TierYtDlp), firstTier, int(TierExhausted)));
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
            Q_EMIT videoResolved(videoId, links.video, links.audio);
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
                // Two streams: yt-dlp lists them as it would merge them.
                const QJsonArray parts = root.value(QStringLiteral("requested_formats")).toArray();
                if (parts.size() >= 2) {
                    const QJsonObject first = parts.at(0).toObject();
                    const QJsonObject second = parts.at(1).toObject();
                    const bool firstIsVideo = first.value(QStringLiteral("vcodec")).toString()
                                              != QLatin1String("none");
                    video = (firstIsVideo ? first : second).value(QStringLiteral("url")).toString();
                    audio = (firstIsVideo ? second : first).value(QStringLiteral("url")).toString();
                }
                if (video.isEmpty()) {
                    Q_EMIT videoFailed(videoId, QStringLiteral("No picture is published for this track."));
                    return;
                }
                m_videoCache.insert(videoId, { video, audio, expiryOf(video) });
                Q_EMIT videoResolved(videoId, video, audio);
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
    const auto cached = m_cache.constFind(videoId);
    if (cached != m_cache.constEnd() && cached->expires > QDateTime::currentDateTimeUtc())
        return;

    auto *job = new Job;
    job->videoId = videoId;
    job->prefetch = true;
    m_jobs.insert(videoId, job);
    startTier(job, TierYtDlp);
}

void StreamResolver::invalidate(const QString &videoId)
{
    m_cache.remove(videoId);
    m_videoCache.remove(videoId);
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

    switch (tier) {
    case TierYtDlp:      startYtDlp(job);         break;
    case TierPiped:      startPipedRace(job);     break;
    case TierInvidious:  startInvidiousRace(job); break;
    default: {
        const QString reason = job->errors.isEmpty()
            ? QStringLiteral("No source could resolve this track.")
            : job->errors.join(QStringLiteral(" | "));
        const QString id = job->videoId;
        const bool silent = job->prefetch;
        discard(job);
        if (!silent)
            Q_EMIT failed(id, reason);
        break;
    }
    }
}

void StreamResolver::startYtDlp(Job *job)
{
    if (!YtDlp::isAvailable()) {
        tierExhausted(job, QStringLiteral("yt-dlp not installed"));
        return;
    }

    YtDlpRequest *request = YtDlp::resolveAudio(job->videoId, this);
    job->ytdlp = request;
    const QString videoId = job->videoId;
    const int generation = job->generation;

    connect(request, &YtDlpRequest::succeededJson, this,
            [this, videoId, generation](const QJsonDocument &document) {
                Job *job = m_jobs.value(videoId);
                if (!job || job->settled || job->generation != generation)
                    return;
                const QString url = document.object().value(QStringLiteral("url")).toString();
                if (url.isEmpty())
                    tierExhausted(job, QStringLiteral("yt-dlp returned no audio url"));
                else
                    succeed(job, url);
            });

    connect(request, &YtDlpRequest::failed, this,
            [this, videoId, generation](const QString &reason) {
                Job *job = m_jobs.value(videoId);
                if (job && !job->settled && job->generation == generation)
                    tierExhausted(job, QStringLiteral("yt-dlp: %1").arg(reason));
            });
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

void StreamResolver::succeed(Job *job, const QString &url)
{
    if (job->settled)
        return;
    job->settled = true;

    const QString videoId = job->videoId;
    const int tier = job->tier;
    const bool silent = job->prefetch;
    m_cache.insert(videoId, { url, tier, expiryOf(url) });

    abortPending(job);
    discard(job);

    if (!silent)
        Q_EMIT resolved(videoId, url, tier, /*fromCache=*/false);
}

void StreamResolver::tierExhausted(Job *job, const QString &reason)
{
    if (job->settled)
        return;
    job->errors.append(reason);
    abortPending(job);
    startTier(job, job->tier + 1);
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
}

void StreamResolver::discard(Job *job)
{
    m_jobs.remove(job->videoId);
    delete job;
}

void StreamResolver::cancel(const QString &videoId)
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
    const QStringList ids = m_jobs.keys();
    for (const QString &id : ids)
        cancel(id);
}
