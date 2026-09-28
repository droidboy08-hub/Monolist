#pragma once

#include <QDateTime>
#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QStringList>
#include <QVariantMap>

#include "innertube.h"
#include "jiosaavn.h"

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
//
// Beside the ladder there is now another catalogue. JioSaavn has much of the
// same music as AAC at up to 320 kbps, where YouTube gives anyone not paying
// it about 128. resolveTrack asks both at once (the race in
// BITCHORD_ENGINE_RESEARCH.md 4.3, turned towards quality): JioSaavn's link
// plays if it is the same recording and arrives within a moment of
// YouTube's; otherwise YouTube's plays as it always did, and JioSaavn's
// late answer is kept for the next time the song is played. The ladder is
// untouched underneath, and is where a JioSaavn link mpv refuses falls back
// to.
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
        TierExhausted,
        // Not a rung of the ladder: the other catalogue, raced against it by
        // resolveTrack. Kept out of the numbering the ladder walks.
        TierJioSaavn = 100
    };
    Q_ENUM(Tier)

    explicit StreamResolver(QObject *parent = nullptr);
    ~StreamResolver() override;

    // Walks the tiers from `firstTier` down until one answers. From the first
    // tier, a still-valid cached URL answers at once.
    void resolve(const QString &videoId, int firstTier = TierInnerTube);
    // A song from the queue: the ladder, and while JioSaavn is on, JioSaavn
    // raced against it. Answers with resolved() exactly as resolve() does,
    // with TierJioSaavn when JioSaavn's link won. A song JioSaavn is known to
    // have plays from it at once; one it is known not to have goes straight
    // to the ladder.
    void resolveTrack(const Saavn::Target &track);
    // prefetch() for a queued song, which asks JioSaavn ahead too.
    void prefetchTrack(const Saavn::Target &track);
    // mpv could not play JioSaavn's link for this song. The match is
    // forgotten and JioSaavn left out for this song for a while; the next
    // look after that asks again, in case the link had only moved.
    void refuseSaavn(const QString &videoId);
    // The bitrate of the JioSaavn link known for this song, 0 when the link
    // does not say or there is none.
    int saavnKbps(const QString &videoId) const;

    void setSaavnEnabled(bool on) { m_saavnEnabled = on; }
    bool saavnEnabled() const { return m_saavnEnabled; }
    // Changing it forgets every "JioSaavn does not have it", which may have
    // been only because of where the request seemed to come from. Not when
    // the setting is only being restored at launch: those answers were given
    // under it.
    void setSaavnIndiaHeaders(bool on, bool forgetNoMatches = true);
    bool saavnIndiaHeaders() const { return m_saavn.indiaHeaders(); }
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
    // Stops resolving this song, race and all. A JioSaavn lookup already
    // asked is let finish: it is one small request, and its answer is kept.
    void cancel(const QString &videoId);
    void cancelAll();

    // For --play --spoil: the next InnerTube link for this track is handed
    // over spoiled, so the CDN refuses it the way it now and then refuses a
    // real one, and the recovery can be watched on a real track.
    void spoilNextStream(const QString &videoId) { m_spoil = videoId; }
    // For --play --spoil-saavn: the same for the next JioSaavn link for this
    // track, which then points at a file the CDN does not have.
    void spoilNextSaavn(const QString &videoId) { m_spoilSaavn = videoId; }

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
    // The ladder's own job for this song, without the race around it.
    void cancelJob(const QString &videoId);
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

    // Where the ladder's answers go: straight out, or into the race for the
    // song when there is one.
    void report(const QString &videoId, const QString &url, int tier, bool fromCache);
    void reportFailure(const QString &videoId, const QString &reason);

    // The race for one song, from the moment both were asked. What YouTube
    // answered is held here while JioSaavn has its moment.
    struct Race {
        QElapsedTimer clock;
        int generation = 0;
        bool youtubeReady = false;
        QString url;
        int tier = TierInnerTube;
        bool fromCache = false;
        bool youtubeFailed = false;
        QString failure;
    };
    void youtubeWins(const QString &videoId);

    // What JioSaavn said about a song, remembered in memory and in the
    // saavn_matches table. A Match or NoMatch holds only for the song as it
    // was asked about (`signature`, Saavn::signature) and the matcher that
    // judged it; a Refused holds for the video whatever its name.
    struct SaavnVerdict {
        enum Kind { Unknown, Match, NoMatch, Refused };
        Kind kind = Unknown;
        QString url;
        int kbps = 0;
        QString saavnId;
        QString signature;
        QDateTime expires;
    };
    SaavnVerdict saavnVerdict(const Saavn::Target &track);
    void startSaavnLookup(const Saavn::Target &track);
    void saavnAnswered(const QString &videoId, const QString &signature, const JioSaavn::Result &result);
    void rememberSaavn(const QString &videoId, const SaavnVerdict &verdict);
    // Once a session: answers past their time are deleted from the table,
    // not only passed over.
    void purgeExpiredSaavn();
    bool m_saavnPurged = false;

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
    QString m_spoilSaavn;   // see spoilNextSaavn
    QString handOverSaavn(const QString &videoId, const QString &url);

    bool m_saavnEnabled = false;   // off until the listener picks High sound quality
    QHash<QString, Race> m_races;
    int m_raceGeneration = 0;
    QHash<QString, SaavnVerdict> m_saavnVerdicts;
    QSet<QString> m_saavnAsking;     // lookups on their way
    // A JioSaavn that cannot be reached is left alone for a while, rather
    // than asked, and waited for, at every song.
    int m_saavnFailuresInARow = 0;
    QDateTime m_saavnRestUntil;
    // Last, so it goes first: its answers arrive into everything above.
    JioSaavn m_saavn;
};

