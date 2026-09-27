#pragma once

#include <QObject>
#include <QSize>
#include <QString>
#include <QVariantMap>

struct mpv_handle;

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
    void load(const QString &urlOrPath, bool startPlaying = true,
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
    // A load is on its way and has not yet opened, failed or been stopped.
    bool isLoadingFile() const { return m_loadingFile; }
    void setPaused(bool paused);
    void seekAbsolute(qint64 ms);
    void setVolume(qreal volume);        // 0.0 – 1.0
    void setSpeed(qreal speed);
    void setReplayGainEnabled(bool enabled);

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

Q_SIGNALS:
    void positionChanged(qint64 ms);
    void durationChanged(qint64 ms);
    void pausedChanged(bool paused);
    void bufferingChanged(bool buffering);
    void endOfFile();                    // natural end, not a manual stop
    void loadFailed(const QString &reason);
    void videoAddFailed(const QString &reason);
    // The latest load's file is open: its tracks are known, and a picture
    // can join it.
    void fileLoaded();
    void metadataChanged(const QString &title, const QString &artist);
    // Empty until the file being played turns out to have a picture.
    void videoSizeChanged(const QSize &size);
    void audioDevicesChanged();

private Q_SLOTS:
    void drainEvents();

private:
    static void onWakeup(void *ctx);
    void observeProperties();
    void applyBaseOptions();
    void setOption(const char *name, const char *value);
    // The link's own headers, for whatever is opened next.
    void applyHeaders(const QVariantMap &headers);
    // True once the file of the latest load has started: what mpv reports
    // from then on is about it.
    bool currentFileStarted() const { return m_currentEntry > 0 && m_startedEntry == m_currentEntry; }

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
    QSize m_videoSize;
    qint64 m_duration = 0;
    QVariantList m_audioDevices;
};
