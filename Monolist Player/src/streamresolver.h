#pragma once

#include <QDateTime>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QVariantMap>

class QNetworkAccessManager;
class QNetworkReply;
class YtDlpRequest;

// Turns a video id into a playable audio URL, trying progressively less
// reliable sources until one answers.
//
// This is the Melody streaming engine carried over from the React app, with one
// structural change: Melody raced Piped and Invidious because it had no better
// option — a browser cannot run yt-dlp, and every request had to clear CORS via
// a public proxy. A native app has neither limit, so yt-dlp becomes tier 0 and
// the instance racing is demoted to a fallback for when yt-dlp is missing,
// rate-limited, or broken by an upstream change.
//
// The racing itself is worth keeping: public instances fail constantly and
// unpredictably, and "ask twelve, take the first answer" degrades far better
// than any single endpoint. It is the C++ equivalent of Melody's Promise.any.
class StreamResolver : public QObject
{
    Q_OBJECT
public:
    enum Tier {
        TierYtDlp = 0,
        TierPiped,
        TierInvidious,
        TierExhausted
    };
    Q_ENUM(Tier)

    explicit StreamResolver(QObject *parent = nullptr);
    ~StreamResolver() override;

    // Starts at `firstTier`: a URL from one tier that the player could not open
    // is retried from the next. From the first tier, a still-valid cached URL
    // answers at once.
    void resolve(const QString &videoId, int firstTier = TierYtDlp);
    // Resolves into the cache without reporting, so that the track after the
    // current one starts without waiting. A resolve() for the same id while it
    // runs takes it over.
    void prefetch(const QString &videoId);
    // The same track with its picture, for the video view. yt-dlp only: the
    // fallback instances answer with sound. Two URLs when YouTube keeps the
    // picture and the sound apart, one when it does not.
    void resolveVideo(const QString &videoId, int maxHeight = 720);
    void cancelVideo(const QString &videoId);

    // Forgets a cached URL, for when the player could not open it.
    void invalidate(const QString &videoId);
    void cancel(const QString &videoId);
    void cancelAll();

    // Instance lists rot — hosts disappear every few months. They are settable
    // so a config update can fix playback without shipping a new binary.
    void setPipedInstances(const QStringList &hosts);
    void setInvidiousInstances(const QStringList &hosts);
    QStringList pipedInstances() const { return m_piped; }
    QStringList invidiousInstances() const { return m_invidious; }

    static QStringList defaultPipedInstances();
    static QStringList defaultInvidiousInstances();

Q_SIGNALS:
    void resolved(const QString &videoId, const QString &url, int tier, bool fromCache);
    void failed(const QString &videoId, const QString &reason);
    void tierChanged(const QString &videoId, int tier);
    // `headers` are what the links must be fetched with; see MpvEngine::load.
    void videoResolved(const QString &videoId, const QString &videoUrl, const QString &audioUrl,
                       const QVariantMap &headers);
    void videoFailed(const QString &videoId, const QString &reason);

private:
    struct Job {
        QString videoId;
        int tier = TierYtDlp;
        // Bumped on every tier change. Callbacks from an abandoned tier still
        // arrive — aborting a reply fires its finished() handler — and are
        // ignored by comparing against the generation they were created in.
        int generation = 0;
        QPointer<YtDlpRequest> ytdlp;
        QList<QPointer<QNetworkReply>> pending;
        QStringList errors;
        bool settled = false;
        bool prefetch = false;   // resolve into the cache only, report nothing
    };

    // A resolved URL stays valid for hours (YouTube signs an expiry into it),
    // so a replayed or prefetched track starts without going to yt-dlp again.
    struct CacheEntry {
        QString url;
        int tier = TierYtDlp;
        QDateTime expires;
    };
    static QDateTime expiryOf(const QString &url);

    void startTier(Job *job, int tier);
    void startYtDlp(Job *job);
    void startPipedRace(Job *job);
    void startInvidiousRace(Job *job);

    void succeed(Job *job, const QString &url);
    void tierExhausted(Job *job, const QString &reason);
    void abortPending(Job *job);
    void discard(Job *job);

    // The picture's links, kept apart from the sound's: the same track can be
    // remembered both ways.
    struct VideoLinks {
        QString video;
        QString audio;      // empty when the one stream carries both
        QVariantMap headers;
        QDateTime expires;
    };

    QNetworkAccessManager *m_network;
    QHash<QString, Job *> m_jobs;
    QHash<QString, CacheEntry> m_cache;
    QHash<QString, VideoLinks> m_videoCache;
    QHash<QString, QPointer<YtDlpRequest>> m_videoJobs;
    QStringList m_piped;
    QStringList m_invidious;
};
