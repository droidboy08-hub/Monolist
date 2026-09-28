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

#include <functional>

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

    // JioSaavn's copy of a song being downloaded (DownloadManager): the
    // match already known where it still holds, otherwise asked for now, as
    // playback would ask. Never while JioSaavn is off (Standard sound
    // quality): then `done` hears "none" without JioSaavn being contacted.
    // `done` runs once, on this object's thread, after findSaavn returns,
    // and not at all once `context` has gone.
    struct SaavnCopy {
        QString url;          // empty: none to use, and `reason` says why
        int kbps = 0;         // 0 when the link does not say
        QString saavnId;
        QString album;        // JioSaavn's name for it, when asked just now
        int durationSec = 0;  // likewise
        QString reason;
    };
    using SaavnCopyCallback = std::function<void(const SaavnCopy &)>;
    void findSaavn(const Saavn::Target &track, QObject *context, SaavnCopyCallback done);
    // For --play --saavn-late <ms>: every JioSaavn answer is held back this
    // long before it is heard, so the race is lost to YouTube on a real song
    // and the mid-song upgrade (saavnLateMatch) can be watched.
    void setSaavnTestDelay(int ms) { m_saavnTestDelayMs = qMax(0, ms); }
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
    //
    // `homeTier` is the rung the song was playing from before the refusal.
    // A link found on any other rung below InnerTube is a rescue link: the
    // muxed stream's itag 18 is about 96 kbps AAC, and kept as the song's
    // link it was what every replay played for up to five hours. So it is
    // kept apart, for this play alone (dropRescueLinks), and resolve() never
    // answers with it; a resolveVia that starts on its rung does, at once,
    // so the same play can go back to it. -1 keeps every answer as the
    // song's link, as a plain walk down the ladder does.
    void resolveVia(const QString &videoId, const QList<int> &tiers, int homeTier = -1);
    // Where to look next once the player has refused a link from `tier`,
    // best first; the caller leaves out what this track has already had
    // refused. The refused tier itself is not in it: asking it again (a
    // remembered link gone stale, a fresh InnerTube link refused) is the
    // caller's to decide, since how often depends on the track.
    static QList<int> afterRefusal(int tier);
    // What `url`, a link this resolver answered for the track with, must be
    // fetched with, where its tier says so (the muxed stream's, which yt-dlp
    // gets as a client that checks); empty otherwise.
    QVariantMap headersFor(const QString &videoId, const QString &url) const;
    // The rescue links are for the play they rescued: every one is forgotten
    // as a song begins, the same song again included, so its next play starts
    // from the top of the ladder and is Opus again.
    void dropRescueLinks() { m_rescueLinks.clear(); }
    // playback.rescue_link=keep, the switch back: a rescue link is the
    // song's link until it expires, as before.
    void setKeepRescueLinks(bool keep) { m_keepRescueLinks = keep; }
    bool keepsRescueLinks() const { return m_keepRescueLinks; }
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

    // For --play --spoil [n]: the next `count` InnerTube links for this track
    // are handed over spoiled, so the CDN refuses them the way it now and
    // then refuses a real one, and the recovery can be watched on a real
    // track. Two spoil the fresh link asked for after the first as well, so
    // the rescue below it runs.
    void spoilNextStream(const QString &videoId, int count = 1)
    {
        m_spoil = count > 0 ? videoId : QString();
        m_spoilsLeft = count;
    }
    // For --recovery-test: `tier` answers this track with `url` at once,
    // asking no one, or fails at once where `url` is empty. Each answer
    // carries a g=<n> of its own, so a stand-in server can tell a fresh link
    // from an old one, and an expire= that --spoil can spoil.
    void setTestAnswer(const QString &videoId, int tier, const QString &url)
    {
        m_testAnswers[videoId].insert(tier, url);
    }
    void clearTestAnswers() { m_testAnswers.clear(); }
    // For --play --spoil-saavn: the same for the next JioSaavn link for this
    // track, which then points at a file the CDN does not have.
    void spoilNextSaavn(const QString &videoId) { m_spoilSaavn = videoId; }

    // A song someone is waiting for has 20 s, from resolve(), resolveVia()
    // or resolveTrack() to its answer, JioSaavn's part included: past it the
    // walk stops and failed() says so, and the player moves on. Walked to the
    // end, the ladder could take ~49 s before (a visitor id, /player and its
    // retry at 8 s each, then yt-dlp and the muxed stream at 12 s each).
    // Prefetches have no such limit; each rung has its own.
    // playback.resolve_deadline=off is the switch back.
    void setDeadline(bool on) { m_deadlineOn = on; }
    bool deadline() const { return m_deadlineOn; }
    // One yt-dlp lookup at a time for resolves (the sound's and the muxed
    // stream's): two at once each take several times as long under
    // emulation or on a small machine. A song someone is waiting for goes
    // first: a prefetch's lookup is stopped for it and waits, still on its
    // rung, for its turn. While such a song resolves no new download starts
    // (YtDlp::playbackResolving). ytdlp.resolves=parallel is the switch
    // back: every lookup at once, as before, and downloads never wait.
    void setYtDlpOneAtATime(bool on);
    bool ytdlpOneAtATime() const { return m_ytdlpOneAtATime; }

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
    // JioSaavn's answer to a race YouTube won by default — JioSaavn had not
    // answered within the moment it is given — was a match after all: the
    // same recording, at `kbps`, `durationSec` long, at `url`. What the
    // player may now move to mid-song (QT7). Not for a prefetch's answer,
    // nor one that came after the song was asked for again.
    void saavnLateMatch(const QString &videoId, const QString &url, int kbps, int durationSec);

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
        // After a refusal, the rung the song played from (resolveVia);
        // answers from another rung below InnerTube are rescue links.
        int homeTier = -1;
        // Since it asked for yt-dlp and found another lookup running.
        QElapsedTimer waitingForYtDlp;
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

    void start(const QString &videoId, QList<int> tiers, int homeTier = -1);
    // The ladder's own job for this song, without the race around it.
    void cancelJob(const QString &videoId);
    void startTier(Job *job, int tier);
    void startInnerTube(Job *job);
    void startYtDlp(Job *job);
    void startMuxed(Job *job);
    // One yt-dlp at a time (setYtDlpOneAtATime): a job on the yt-dlp or the
    // muxed rung asks for the turn, and its lookup starts once it has it.
    void queueYtDlp(Job *job);
    void launchYtDlp(Job *job);
    void pumpYtDlp();
    // The prefetch holding the turn stops, and waits for it again.
    void preemptYtDlp();
    // `job` no longer runs or waits for a lookup (it moved on, or went).
    void releaseYtDlp(Job *job);
    // Whether a song someone waits for is resolving, for the downloads.
    void foregroundChanged();

    // The 20 s a resolve has (setDeadline), by song. A new resolve of the
    // same song starts its own.
    void armDeadline(const QString &videoId);
    void deadlinePassed(const QString &videoId);
    // What every answer and failure goes out through, so the song's
    // deadline ends with it.
    void emitResolved(const QString &videoId, const QString &url, int tier, bool fromCache);
    void emitFailed(const QString &videoId, const QString &reason);
    void startPipedRace(Job *job);
    void startInvidiousRace(Job *job);

    void succeed(Job *job, const QString &url, const QVariantMap &headers = QVariantMap());
    // InnerTube's link as it is handed over: spoiled, for --spoil.
    QString handOverInnerTube(Job *job, const QString &url);
    // setTestAnswer's answer for the tier the job is on.
    void answerForTest(Job *job, const QString &url);
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
    bool m_deadlineOn = true;
    QHash<QString, int> m_deadlines;   // song -> the generation of its timer
    int m_deadlineGeneration = 0;
    bool m_ytdlpOneAtATime = true;
    Job *m_ytdlpHolder = nullptr;      // whose lookup runs
    QList<Job *> m_ytdlpWaiting;       // in the order they asked
    bool m_foreground = false;         // last said to YtDlp::setPlaybackResolving
    QHash<QString, CacheEntry> m_cache;
    // The rescue links of the play under way (see resolveVia), apart from
    // the songs' own links above.
    QHash<QString, CacheEntry> m_rescueLinks;
    bool m_keepRescueLinks = false;
    QHash<QString, VideoLinks> m_videoCache;
    QHash<QString, QPointer<YtDlpRequest>> m_videoJobs;
    QStringList m_piped;
    QStringList m_invidious;
    QString m_spoil;   // see spoilNextStream
    int m_spoilsLeft = 0;
    QString m_spoilSaavn;   // see spoilNextSaavn
    QHash<QString, QHash<int, QString>> m_testAnswers;   // see setTestAnswer
    int m_testAnswersGiven = 0;
    QString handOverSaavn(const QString &videoId, const QString &url);

    bool m_saavnEnabled = false;   // off until the listener picks High sound quality
    QHash<QString, Race> m_races;
    int m_raceGeneration = 0;
    QHash<QString, SaavnVerdict> m_saavnVerdicts;
    QSet<QString> m_saavnAsking;     // lookups on their way
    // Songs whose race YouTube won while JioSaavn was still being asked:
    // a match from that lookup is a late one (saavnLateMatch).
    QSet<QString> m_saavnLate;
    // Downloads waiting on a lookup already on its way (findSaavn).
    QHash<QString, QList<SaavnCopyCallback>> m_saavnWaiters;
    int m_saavnTestDelayMs = 0;
    // A JioSaavn that cannot be reached is left alone for a while, rather
    // than asked, and waited for, at every song.
    int m_saavnFailuresInARow = 0;
    QDateTime m_saavnRestUntil;
    // Last, so it goes first: its answers arrive into everything above.
    JioSaavn m_saavn;
};

