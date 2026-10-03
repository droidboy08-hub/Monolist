#pragma once

#include <QElapsedTimer>
#include <QList>
#include <QObject>
#include <QSize>
#include <QString>
#include <QVariantMap>

#include "soundchain.h"

struct mpv_handle;
class QThread;
class QTimer;

// Thin Qt wrapper around libmpv, configured for audio-only playback.
//
// libmpv delivers events on its own thread; the wakeup callback re-enters the
// Qt event loop through a queued invocation, so every signal below is emitted
// on the thread that owns this object. Nothing here touches QML directly.
//
// Replaces the simulated clock the UI prototype ran on: position and duration
// now come from the decoder, not a QTimer.
class MpvEngine : public QObject
{
    Q_OBJECT
public:
    explicit MpvEngine(QObject *parent = nullptr);
    ~MpvEngine() override;

    // False when libmpv could not be initialised. The app still runs — the
    // controller reports the failure instead of pretending to play.
    bool isValid() const { return m_mpv != nullptr; }
    QString lastError() const { return m_lastError; }

    // Accepts a local file path or a direct stream URL. Playback starts paused
    // or playing according to `startPlaying`. YouTube keeps picture and sound
    // apart above 720p, so a second URL can be played alongside the first.
    // `startAt` begins the file part-way in, which is how switching between
    // the sound and the picture of the same track keeps its place: a seek sent
    // after loadfile would arrive before the file is open and be dropped.
    // `headers` are the ones the link was fetched with (yt-dlp reports them
    // per format): YouTube refuses a link fetched as anything else.
    //
    // Only the file most recently asked for speaks: position, duration, the
    // picture's size, its end and its errors are reported for it alone, never
    // for one it replaced, and nothing at all is reported after stop().
    //
    // True when mpv took the request: the file is on its way, and whether it
    // opens is told later (fileLoaded, or loadFailed). False when it was
    // refused outright, loadFailed having already been sent.
    bool load(const QString &urlOrPath, bool startPlaying = true,
              const QString &audioUrl = QString(), qint64 startAt = 0,
              const QVariantMap &headers = QVariantMap());
    void stop();
    // The picture added to the file already playing, as a track of its own
    // (mpv's video-add), rather than the file loaded again with it: the sound
    // plays on without a gap, and mpv keeps the picture in time with it.
    // Fetched with `headers`, as load() does. videoAddFailed if it would not
    // open. Only for a file that has loaded (hasLoadedFile).
    void addVideo(const QString &url, const QVariantMap &headers = QVariantMap());
    // The latest load's file is open (fileLoaded has been sent for it), so a
    // picture can be added to it.
    bool hasLoadedFile() const { return m_fileLoaded; }
    // The length mpv read from the file most recently loaded, in ms; 0 until
    // it has said, and for a stream with no length. Still the ended file's
    // own while its endOfFile is being handled, so an end can be held up
    // against it.
    qint64 duration() const { return m_duration; }
    // A load is on its way and has not yet opened, failed or been stopped.
    bool isLoadingFile() const { return m_loadingFile; }
    // audioStarted has been sent for the latest load.
    bool hasAudioStarted() const { return m_audioStarted; }
    // mpv's name for the container of the file most recently loaded, once it
    // has opened ("mp3", "matroska,webm", "mov,mp4,m4a,3gp,3g2,mj2"); empty
    // before. Kept past its end, as duration() is: whether that length is
    // the file's own or only a guess (an MP3 with no index) depends on it.
    QString fileFormat() const { return m_fileFormat; }
    void setPaused(bool paused);
    void seekAbsolute(qint64 ms);
    void setVolume(qreal volume);        // 0.0 – 1.0
    // The listener's own speed. What mpv plays at is this times the speed
    // effect's (Slowed, Nightcore), kept within 0.25-4.
    void setSpeed(qreal speed);
    // What mpv plays at now: the speed above times the effect's. Song time
    // runs this much faster than the clock on the wall.
    double playbackRate() const { return m_rate; }
    void setReplayGainEnabled(bool enabled);
    // Loudness levelling: on, files tagged with ReplayGain are levelled by
    // their tags (raised 4 dB to YouTube's reference), and any other by
    // `fallbackDb`, what is known of the song playing (Loudness::gainFor);
    // off, nothing is. Takes effect at once, and holds for the files loaded
    // after it until it is set again.
    void setLevelling(bool on, double fallbackDb);
    // The gain applied to a file without tags, for --loudness-test.
    double fallbackGain() const;
    // mpv's replaygain-preamp as it stands ("4", "0"), for --sound-live-test.
    QString levellingPreamp() const;

    // The sound effects (SoundChain): live, with no gap, and held for every
    // file after. Slowed and Nightcore are mpv's speed with pitch correction
    // off; the rest is one chain of filters in mpv's af, which goes in when
    // the first effect that needs it is switched on, at rest, and has its
    // values moved to the new ones in small steps (15 ms apart) where one
    // jump would click. With nothing playing (at launch, between songs) the
    // chain is written as it should be, straight away.
    //
    // The chain is only ever taken out, or rebuilt, where the sound is
    // rebuilt anyway: a load, a seek, a change of device (taking it out
    // mid-song clicks). Turning 8D on mid-song is the one exception: its
    // branch is only in the graph while it is wanted, and putting it in is
    // a rebuild, a small blip on a dramatic change.
    //
    // A chain mpv would not start never costs the song: the song is played
    // again without it, from where it was, effectsFailed says so, and the
    // effects stay off (ignored here) until Monolist restarts. That is for a
    // failure the chain can be behind, only: a link that would not open, or
    // a file mpv cannot read, fails as it would with no effects on (see
    // retryWithoutEffects).
    void setEffects(const SoundChain::Settings &settings);
    SoundChain::Settings effects() const { return m_fx; }
    // What the running chain holds, as far as the engine has moved it.
    SoundChain::Params effectParams() const { return m_fxLive; }
    // The effects would not start this session, and are off.
    bool effectsBroken() const { return m_fxBroken; }
    // mpv's af set to `afValue` with the "af set" command, on this player and
    // the one waiting to take over: false, with the chain before it left in
    // place, where mpv refused it. Never the af property, which takes any
    // chain and then fails the next file with it. ("af set" does not check a
    // chain while nothing plays, either; see setEffects.)
    bool setAudioFilters(const QString &afValue);
    // mpv's af as it stands, for --sound-live-test.
    QString audioFilterProperty() const;
    // mpv's speed and pitch correction as they stand, for the same.
    double speedProperty() const;
    bool pitchCorrection() const;
    // Whether the chain is running: its limiter answers a command now.
    bool effectsAnswer();
    // For --sound-live-test: every value the running chain holds sent to it
    // again, as the ramp sends it (never to a band past half the sound's
    // rate, see SoundChain::commandsAt), each answer waited for; the filters
    // that refused theirs ("amix@pan"). Empty where every one took it, or no
    // chain is in.
    QStringList effectsRefused();
    // Whether the file playing has a sound track: mpv drops it when the
    // chain will not build, and a file with a picture then plays on without
    // it. False while nothing plays.
    bool hasSoundTrack() const;
    // For --sound-live-test: every chain written from now on carries a
    // filter no FFmpeg has, as a Mac's FFmpeg without one of ours would.
    void breakEffectsForTest(bool broken) { m_fxBreakForTest = broken; }

    // The sound devices mpv can play through, as it lists them: maps with a
    // `name`, which setAudioDevice takes, and a `description`, which is what
    // the system calls the device. mpv lists every sound driver it was built
    // with, so the same speakers can appear more than once, and the first
    // entry is always "auto". Empty until mpv has looked, then kept current
    // as devices are plugged in and taken out.
    QVariantList audioDevices() const { return m_audioDevices; }
    // Where the sound goes: a name from audioDevices, or "auto" for the
    // system's default device, followed as that changes. Takes effect at
    // once, part-way through a song too.
    void setAudioDevice(const QString &name);

    // Decoding the picture costs, so it is off until something shows it.
    // Whatever draws the video renders from this handle (see VideoSurface).
    void setVideoEnabled(bool enabled);
    bool videoEnabled() const { return m_video; }
    // Whether anything is on screen to show the picture. A picture nobody can
    // see is not decoded: its track is put aside (vid=no) with the file left
    // as it is, and taken up again, where the file has got to, once something
    // shows it. Separate from setVideoEnabled, which is the listener's choice
    // and reloads the stream; this is only where the picture can go.
    void setVideoWatched(bool watched);
    // Empty when what is playing has no picture. Read on attaching, in case
    // the size was reported before anything was there to draw it.
    QSize videoSize() const { return m_videoSize; }
    mpv_handle *handle() const { return m_mpv; }

    // What mpv says it is playing, for the file most recently loaded: the
    // codec it decodes, what the decoder puts out, what the sound device was
    // opened with, and the bitrate. Every field is read from mpv, none is
    // taken from where the link came from (an itag, a JioSaavn verdict), so
    // it is what arrived rather than what was asked for. Empty or 0 until
    // mpv has said; cleared by load() and stop(); streamInfoChanged as it
    // fills in, which is once the sound has started.
    struct StreamInfo {
        QString codec;            // audio-codec-name: "opus", "aac", "mp3"
        int sampleRate = 0;       // audio-params: the decoder's output
        QString sampleFormat;     // "floatp"
        QString channels;         // "stereo"
        int outputRate = 0;       // audio-out-params: what the device plays
        QString outputFormat;     // "float"
        QString outputChannels;
        QString output;           // current-ao: "wasapi", "coreaudio"
        // For a file holding sound alone, its size over its length: the
        // average of the whole file. For one with a picture in it (a video,
        // a download's cover), whose size is not all sound, the bitrate its
        // container declares for the sound, where it declares one (AAC and
        // MP3 files usually do); otherwise mpv's own measure of the sound
        // packets decoded, as soon as it has one.
        int kbps = 0;
        bool kbpsIsFileAverage = false;
        bool kbpsIsDeclared = false;
        double volume = -1.0;     // mpv's volume, in percent, as applied

        bool operator==(const StreamInfo &o) const
        {
            return codec == o.codec && sampleRate == o.sampleRate && sampleFormat == o.sampleFormat
                   && channels == o.channels && outputRate == o.outputRate
                   && outputFormat == o.outputFormat && outputChannels == o.outputChannels
                   && output == o.output && kbps == o.kbps && kbpsIsFileAverage == o.kbpsIsFileAverage
                   && kbpsIsDeclared == o.kbpsIsDeclared && volume == o.volume;
        }
        bool operator!=(const StreamInfo &o) const { return !(*this == o); }
    };
    StreamInfo streamInfo() const { return m_streamInfo; }

    // The song playing, taken over part-way through from another link to the
    // same recording without a gap in the sound: QT7, the mid-song move to
    // JioSaavn. A second player, silent and paused, opens `url` a few seconds
    // ahead of where the song is and buffers it; only once it can play from
    // there, and the song has reached that point, do the two sound together
    // for a moment, the new one lined up with the old by its own clock and
    // faded in as the old fades out. Then the new player is this one, and
    // everything above is about it. Nothing is heard of an attempt that
    // fails, and none is made, or finished, in the last 15 s of the song.
    // Only for the sound alone (no picture) of a file that has started, and
    // one at a time. False, with nothing begun, when it cannot be tried now;
    // otherwise upgradeFinished says how it ended.
    //
    // `offsetMs` is how much later the music comes in the new file than in
    // the one playing (AudioAlign measures it): the new player is lined up
    // that far from the old one's clock, so the move neither skips nor
    // repeats. `minKbps` is what the new file must average, its size over
    // its length, once it is open: a link's name is not what it serves.
    bool startUpgrade(const QString &url, double offsetMs, int minKbps = 0,
                      const QVariantMap &headers = QVariantMap());
    // Given up, silently where nothing has been heard of it yet: another
    // file loaded, a stop, the picture turned on, a seek once the two
    // players are sounding together, or the caller's own reason.
    void cancelUpgrade(const QString &why);
    bool upgrading() const { return m_upgrade != nullptr; }

Q_SIGNALS:
    void positionChanged(qint64 ms);
    void durationChanged(qint64 ms);
    void pausedChanged(bool paused);
    void bufferingChanged(bool buffering);
    void endOfFile();                    // natural end, not a manual stop
    void loadFailed(const QString &reason);
    // The file most recently loaded has begun to sound: mpv restarted
    // playback for it while not paused, or its clock passed 0. Once a load.
    // What a link that resolved, or a file that opened, cannot say.
    void audioStarted();
    void videoAddFailed(const QString &reason);
    // The latest load's file is open: its tracks are known, and a picture
    // can join it.
    void fileLoaded();
    void metadataChanged(const QString &title, const QString &artist);
    // Empty until the file being played turns out to have a picture.
    void videoSizeChanged(const QSize &size);
    void audioDevicesChanged();
    void streamInfoChanged();
    // How an upgrade ended: `swapped`, the song now plays from the new link
    // (and the gap, the alignment and the timings are in `detail`, for the
    // log), or not, and why. `fileKbps` is what the new file averages, its
    // size over its length, 0 where that was never measured; `otherLength`,
    // that it ended because the new file is not the song's length. Both are
    // about the file itself, which is no better at the song's next start.
    void upgradeFinished(bool swapped, const QString &detail, int fileKbps, bool otherLength);
    // The player mpv renders from is about to be replaced by the upgrade's,
    // and has been: whatever holds a render context on handle() frees it on
    // the first, before the old player goes, and makes it again on the second.
    void handleAboutToChange();
    void handleChanged();
    // The sound effects would not start (see setEffects): they are off until
    // Monolist restarts. Once a session.
    void effectsFailed(const QString &why);
    // playbackRate() changed.
    void playbackRateChanged(double rate);

private Q_SLOTS:
    void drainEvents();

private:
    static void onWakeup(void *ctx);
    static void onUpgradeWakeup(void *ctx);
    void observeProperties();
    void applyBaseOptions();
    void setOption(const char *name, const char *value);

    // The upgrade's steps (see startUpgrade), in mpvengine.cpp.
    struct Upgrade;
    // The next step in `ms`, unless another has been scheduled meanwhile.
    void upgradeAfter(int ms, void (MpvEngine::*step)());
    void drainUpgrade();
    void judgeUpgrade();          // ready yet? then wait for the moment
    void aimUpgrade(qint64 at);   // where the new player waits, from now
    void awaitTakeover();         // until the song reaches that point
    void beginTakeover();         // both sounding, the new one silent
    void alignTick();             // the new one's clock onto the old's
    void fadeTick();              // one step of the crossfade
    void promoteUpgrade();        // the new player becomes this one
    // `otherLength`: see upgradeFinished.
    void endUpgrade(bool swapped, const QString &detail, bool otherLength = false);
    // Destroys a player on a thread of its own: shutting one down waits for
    // its sound output and its network reads to stop, which the window must
    // not. All of them are waited for when the engine goes.
    void retire(mpv_handle *mpv);
    // The link's own headers, for whatever is opened next.
    void applyHeaders(const QVariantMap &headers);
    // True once the file of the latest load has started: what mpv reports
    // from then on is about it.
    bool currentFileStarted() const { return m_currentEntry > 0 && m_startedEntry == m_currentEntry; }
    // Reads StreamInfo from mpv for the current file; streamInfoChanged if
    // anything differs. clearStreamInfo empties it for a file on its way.
    void refreshStreamInfo();
    void clearStreamInfo();
    // The picture's size read from mpv for the current file, and
    // videoSizeChanged where it differs; nothing while it has none.
    void readVideoSize();

    // The sound effects' parts (see setEffects), in mpvengine.cpp.
    // The settings in force: none once the effects are broken.
    SoundChain::Settings effectiveSettings() const;
    // The settings the chain follows: none while the effects are broken, or
    // while a song is being played again without them to see whether they
    // were what stopped it.
    SoundChain::Settings chainSettings() const;
    // The graph of the current file has been built for its latest load or
    // seek: commands can reach it, and changes are ramped rather than written.
    bool chainRunning() const;
    // The chain's text for these values (with the test's broken filter).
    QString chainText(const SoundChain::Params &params) const;
    // Where the ramp is headed: the settings' values, in the graph's shape
    // as it stands (8D's weights back to 0 when its branch is in unwanted).
    SoundChain::Params rampGoal() const;
    // mpv's af written to what the effects want, where the graph is rebuilt
    // anyway: before a load, a seek, a change of device. Only when the text
    // differs; the levelling's preamp follows the chain here too.
    void syncFilterString();
    // A change of effects carried out: the chain put in, or rebuilt for 8D,
    // and the ramp started; or, with nothing playing, the chain written.
    void applyChain();
    // mpv's af set to the chain for `params` while it plays (setAudioFilters),
    // and what it holds noted: false, the effects then off for the session,
    // where mpv refused it.
    bool writeChain(const SoundChain::Params &params);
    // The graph was built afresh (PLAYBACK_RESTART): the live values back to
    // what its text holds, and the ramp and the health check started again.
    void effectsRestarted();
    // The sound output was opened anew (AUDIO_RECONFIG), which need not
    // rebuild the graph: the live values are sent again, not rewound.
    void effectsReconfigured();
    // Whether a file that ended in this error (an mpv_error) can have been
    // failed by the chain: mpv set up its sound for the latest load, and then
    // had nothing to play, which is how a chain that will not build ends a
    // file. A link that would not open, or a format mpv does not know, ends
    // before any sound is set up, and with another error.
    bool chainCouldFail(int error) const;
    // A file that the chain can have failed, the chain never heard from:
    // played again without it, from where it was. `why` is what went wrong,
    // for the log and effectsFailed. False where it is not the chain's to
    // answer for (it answered, or is not in), and loadFailed is the file's.
    bool retryWithoutEffects(const QString &why);
    // The player waiting to take over brought to the values this one's chain
    // holds, once its own graph can take them.
    void mirrorEffects();
    // mpv's speed and pitch correction from the listener's speed and the
    // effect's, in the order that keeps scaletempo out at a speed off 1.
    void applySpeed();
    // replaygain-preamp from the levelling and whether the chain is in.
    void applyPreamp();
    // One 15 ms tick: the limiter asked first (the health check), then one
    // step of the ramp.
    void effectsTick();
    void startEffectsTick();
    // The chain would not run: effects off for the session (effectsFailed).
    void effectsWouldNotStart(const QString &why);
    // Sends `commands` to one player without waiting for its answer.
    static void sendEffects(mpv_handle *mpv, const QList<SoundChain::Command> &commands);

    mpv_handle *m_mpv = nullptr;
    // Which file is the current one. Each load is numbered, and mpv's answer to
    // it names the playlist entry it made; START_FILE and END_FILE name the
    // entry they are about. 0 is "none" (stopped, or the answer is still on
    // its way); -1 is an mpv whose answer named no entry, so the next file to
    // start is taken to be it.
    quint64 m_loadRequest = 0;
    // The latest addVideo, numbered apart from the loads (kAddVideoTag set).
    quint64 m_addVideoRequest = 0;
    quint64 m_addVideoCount = 0;
    qint64 m_currentEntry = 0;
    qint64 m_startedEntry = 0;
    QString m_lastError;
    bool m_paused = true;
    bool m_buffering = false;
    bool m_video = false;
    bool m_watched = true;   // until a surface says otherwise
    bool m_fileLoaded = false;
    bool m_loadingFile = false;
    bool m_audioStarted = false;   // audioStarted sent for the latest load
    QSize m_videoSize;
    qint64 m_duration = 0;
    QString m_fileFormat;          // see fileFormat()
    qreal m_volume = 1.0;          // as last set, 0.0 – 1.0
    QVariantList m_audioDevices;
    StreamInfo m_streamInfo;
    Upgrade *m_upgrade = nullptr;
    int m_upgradesDone = 0;        // this session, for the log
    QList<QThread *> m_retiring;   // players being shut down (retire)

    // The latest load as it was asked, so a song whose effects would not
    // start can be played again without them (and, after a takeover, the
    // file that took over).
    struct LastLoad {
        QString url;
        QString audioUrl;
        qint64 startAt = 0;
        QVariantMap headers;
    };
    LastLoad m_lastLoad;
    qint64 m_lastPositionMs = 0;   // the current file's clock, as last reported

    // The sound effects (setEffects).
    SoundChain::Settings m_fx;           // what the listener wants
    SoundChain::Params m_fxLive;         // what the running chain holds
    QString m_afGiven;                   // mpv's af as last given ("" none)
    SoundChain::Params m_afGivenParams;  // the values that text holds
    bool m_fxBroken = false;             // would not start: off this session
    bool m_fxBreakForTest = false;
    QTimer *m_fxTick = nullptr;
    // The graph has been built for the current load or seek (its
    // PLAYBACK_RESTART came): until then commands cannot land, and failing
    // ones are not counted against it.
    bool m_fxRestarted = false;
    // Its limiter has answered since then.
    bool m_fxVerified = false;
    // mpv has set up the latest load's sound (an AUDIO_RECONFIG came for it):
    // only from then on can the chain be what fails the file.
    bool m_soundSetUp = false;
    QElapsedTimer m_fxFailingSince;      // the limiter has not answered since
    // A song the chain can have failed (it ended with nothing played, or
    // played its picture on without its sound): played again without it,
    // and only if that plays, sound and all, were the effects at fault (see
    // retryWithoutEffects, effectsRestarted).
    struct EffectsRetry {
        bool active = false;
        QString error;                   // the first attempt's
        quint64 loadRequest = 0;         // the retry's own load
    };
    EffectsRetry m_fxRetry;
    bool m_fxReloading = false;          // load() called by the retry itself
    double m_userSpeed = 1.0;
    double m_rate = 1.0;                 // playbackRate()
    bool m_pitchCorrectionOff = false;   // as last set
    // The levelling (setLevelling), kept so the preamp can follow the
    // chain; and whether the preamp was last set for the chain being in.
    bool m_levelOn = false;
    bool m_preampForChain = false;
};
