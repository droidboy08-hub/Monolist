#pragma once

#include <QDateTime>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QVariantMap>

#include "innertube.h"

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
//
// Since then the ladder has grown a rung above yt-dlp. Asking YouTube's own
// /player endpoint in this process answers in well under a second where
// starting yt-dlp takes three and a half, because what yt-dlp mostly costs is
// not the process but the round trips it makes on its way to the same answer.
// It is deliberately a tier and not a replacement: it cannot do age-gated or
// made-for-kids tracks or live streams, and the arrangement it depends on is
// one YouTube can withdraw — it already withdrew the equivalent for the iOS
// client between two yt-dlp releases. When it fails, yt-dlp still gets the
// track, and the only thing lost is the speed.
class StreamResolver : public QObject
{
    Q_OBJECT
public:
    // In the order a resolve walks them.
    enum Tier {
        TierInnerTube = 0,
        TierYtDlp,
        // The track's muxed stream (itag 18, the sound with a small picture in
        // one file), from other clients than the two above. Never a first
        // choice — the picture's bytes are fetched and thrown away — but the
        // last try for sound before a track is given up on, and the first
        // after the player refuses InnerTube's sound-only stream, since the
        // sound yt-dlp would fetch next comes from that same client.
        TierMuxed,
        TierPiped,
        TierInvidious,
        TierExhausted
    };
    Q_ENUM(Tier)

    explicit StreamResolver(QObject *parent = nullptr);
    ~StreamResolver() override;

    // Walks the tiers from `firstTier` down until one answers. From the first
    // tier, a still-valid cached URL answers at once.
    void resolve(const QString &videoId, int firstTier = TierInnerTube);
    // Walks exactly these tiers, in this order: how the player retries a
    // track whose stream it could not open (see afterRefusal).
    void resolveVia(const QString &videoId, const QList<int> &tiers);
    // Where to look next once the player has refused a link from `tier`,
    // best first; the caller leaves out what this track has already had
    // refused.
    static QList<int> afterRefusal(int tier);
    // What a link must be fetched with, where its tier says so (the muxed
    // stream's, which yt-dlp gets as a client that checks); empty otherwise.
    QVariantMap headersFor(const QString &videoId) const;
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

    // For --play --spoil: the next InnerTube link for this track is handed
    // over spoiled, so the CDN refuses it the way it now and then refuses a
    // real one, and the recovery can be watched on a real track.
    void spoilNextStream(const QString &videoId) { m_spoil = videoId; }

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
        int tier = TierInnerTube;
        QList<int> next;   // the tiers still to try, in order
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
        int tier = TierInnerTube;
        QDateTime expires;
        QVariantMap headers;
    };
    static QDateTime expiryOf(const QString &url);

    void start(const QString &videoId, QList<int> tiers);
    void startTier(Job *job, int tier);
    void startInnerTube(Job *job);
    void startYtDlp(Job *job);
    void startMuxed(Job *job);
    void startPipedRace(Job *job);
    void startInvidiousRace(Job *job);

    void succeed(Job *job, const QString &url, const QVariantMap &headers = QVariantMap());
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
    // The fast tier's own client, separate from the one PlaybackController
    // uses for search and radio, so a resolve and a search never wait on each
    // other's connection.
    InnerTube m_innerTube;
    QHash<QString, Job *> m_jobs;
    QHash<QString, CacheEntry> m_cache;
    QHash<QString, VideoLinks> m_videoCache;
    QHash<QString, QPointer<YtDlpRequest>> m_videoJobs;
    QStringList m_piped;
    QStringList m_invidious;
    QString m_spoil;   // see spoilNextStream
};

