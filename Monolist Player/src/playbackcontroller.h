#pragma once

#include <QObject>
#include <QSet>
#include <QString>
#include <QTimer>
#include <QVariantMap>

#include "innertube.h"
#include "queuemodel.h"

class QAbstractItemModel;
class Library;
class MpvEngine;
class StreamResolver;
class DownloadManager;

// Playback facade for the UI.
//
// It owns the play queue. Playing from a list — the library, downloads, an
// album, a playlist — queues that list; a search result or a suggestion plays
// on its own; Play next and Add to queue edit the queue; and when it runs out,
// autoplay continues with YouTube Music's radio for the last song, the way
// YouTube Music itself does.
//
// Where the audio for a track comes from is the source ladder Melody called
// its playback protocol:
//
//   local file present  -> play from disk                      (Melody: LOCAL)
//   otherwise           -> StreamResolver, then play that URL   (Melody: STEALTH)
//
// Melody's third rung, the hidden YouTube iframe, has no equivalent here and
// needs none: it existed only because a browser cannot play an arbitrary audio
// URL that fails CORS. mpv has no such restriction, so a resolved URL either
// plays or the resolver moves to the next source.
class PlaybackController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool playing READ playing NOTIFY playingChanged)
    Q_PROPERTY(QVariantMap currentTrack READ currentTrack NOTIFY currentTrackChanged)
    Q_PROPERTY(QString currentSourceId READ currentSourceId NOTIFY currentTrackChanged)
    Q_PROPERTY(int currentIndex READ currentIndex NOTIFY currentTrackChanged)   // in the queue
    Q_PROPERTY(QueueModel *queue READ queue CONSTANT)
    Q_PROPERTY(bool autoplay READ autoplay WRITE setAutoplay NOTIFY autoplayChanged)
    Q_PROPERTY(qint64 position READ position WRITE setPosition NOTIFY positionChanged)
    Q_PROPERTY(qint64 duration READ duration NOTIFY durationChanged)
    Q_PROPERTY(qreal progress READ progress NOTIFY positionChanged)
    Q_PROPERTY(QString positionText READ positionText NOTIFY positionChanged)
    Q_PROPERTY(QString durationText READ durationText NOTIFY durationChanged)
    Q_PROPERTY(qreal volume READ volume WRITE setVolume NOTIFY volumeChanged)
    Q_PROPERTY(bool shuffle READ shuffle WRITE setShuffle NOTIFY shuffleChanged)
    Q_PROPERTY(int repeatMode READ repeatMode NOTIFY repeatModeChanged)
    Q_PROPERTY(bool favourite READ favourite NOTIFY favouriteChanged)
    Q_PROPERTY(bool buffering READ buffering NOTIFY bufferingChanged)
    Q_PROPERTY(bool resolving READ resolving NOTIFY statusChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusChanged)
    // Whether `statusText` is reporting a failure rather than progress. The
    // player bar shows the status line while a track is resolving; without
    // this it has no reason to keep showing it once resolving stops, which is
    // exactly when a failure has something to say.
    Q_PROPERTY(bool statusError READ statusError NOTIFY statusChanged)
    Q_PROPERTY(QString sourceLabel READ sourceLabel NOTIFY statusChanged)
    Q_PROPERTY(bool engineAvailable READ engineAvailable CONSTANT)
    // The picture. `videoAvailable` is whether this track has one at all
    // (YouTube Music's own songs are a still image, so they do not);
    // `videoWanted` is the switch, which lasts for this track only;
    // `videoPlaying` is whether the picture is actually loaded.
    Q_PROPERTY(bool videoAvailable READ videoAvailable NOTIFY currentTrackChanged)
    Q_PROPERTY(bool videoWanted READ videoWanted WRITE setVideoWanted NOTIFY videoChanged)
    Q_PROPERTY(bool videoPlaying READ videoPlaying NOTIFY videoChanged)
    Q_PROPERTY(int videoHeight READ videoHeight WRITE setVideoHeight NOTIFY videoChanged)
public:
    enum RepeatMode { RepeatOff = 0, RepeatAll = 1, RepeatOne = 2 };
    Q_ENUM(RepeatMode)

    explicit PlaybackController(MpvEngine *engine,
                                StreamResolver *resolver,
                                DownloadManager *downloads,
                                QObject *parent = nullptr);

    // The library, for likes and for recording plays against library rows,
    // and for the settings table the player's own choices are kept in.
    void setLibrary(Library *library);
    // Volume, shuffle, repeat and autoplay as they were left. Called once the
    // library is set, before a queue is loaded or the interface reads them;
    // each is written back whenever it changes.
    void restoreSettings();

    bool playing() const { return m_playing; }
    QVariantMap currentTrack() const { return m_currentTrack; }
    QString currentSourceId() const;
    int currentIndex() const { return m_queue.currentIndex(); }
    QueueModel *queue() { return &m_queue; }
    bool autoplay() const { return m_autoplay; }
    qint64 position() const { return m_position; }
    qint64 duration() const { return m_duration; }
    qreal progress() const;
    QString positionText() const;
    QString durationText() const;
    qreal volume() const { return m_volume; }
    bool shuffle() const { return m_shuffle; }
    int repeatMode() const { return m_repeatMode; }
    bool favourite() const { return m_favourite; }
    bool buffering() const { return m_buffering; }
    bool resolving() const { return m_resolving; }
    QString statusText() const { return m_statusText; }
    bool statusError() const { return m_statusError; }
    QString sourceLabel() const { return m_sourceLabel; }
    bool engineAvailable() const;
    bool videoAvailable() const;
    bool videoWanted() const { return m_videoWanted; }
    bool videoPlaying() const { return m_videoPlaying; }
    int videoHeight() const { return m_videoHeight; }
    void setVideoHeight(int height);

public Q_SLOTS:
    void play();
    void pause();
    void togglePlay();
    void next();
    void previous();

    // Queue positions.
    void loadIndex(int index);
    void playIndex(int index);

    // Queue every row of a list model that uses the app's track roles and
    // start at `row`: the library, downloads, an album. (Search results are
    // not a list to play through: Search plays the one picked, with
    // playTracks, and the radio follows it.)
    //
    // `origin` is which surface asked — "search", "home", "playlist",
    // "library", "queue". It is not for display: the recommender weights what
    // it learns by where a play came from, because a song picked out of a
    // search is a stronger statement than the fourth track of an album that
    // happened to keep going. Left empty it weighs as neutral.
    void playModel(QAbstractItemModel *model, int row, const QString &origin = QString());
    void loadModel(QAbstractItemModel *model, int row);
    void playTracks(const QVariantList &tracks, int start, const QString &origin = QString());

    void playNext(const QVariantMap &track);
    void addToQueue(const QVariantMap &track);
    void removeFromQueue(int index);
    void clearUpcoming();

    void setPosition(qint64 ms);
    void seekFraction(qreal fraction);
    void setVolume(qreal volume);
    void setShuffle(bool shuffle);
    void cycleRepeat();
    void toggleFavourite();
    void setAutoplay(bool autoplay);
    // Swaps what is playing for the same track with, or without, its picture,
    // carrying on from the same second.
    void setVideoWanted(bool wanted);

    // Plays one track on its own; autoplay carries on from it.
    void playSource(const QString &videoId,
                    const QString &title,
                    const QString &artist,
                    const QString &artwork = QString(),
                    qint64 durationMs = 0,
                    const QString &album = QString(),
                    bool isVideo = false,
                    const QString &origin = QString());

    // Every YouTube video has a thumbnail at a predictable URL, so a track with
    // a source id never has to show a blank plate even when no artwork field
    // was stored. Exposed to QML so lists can use it for their own rows.
    Q_INVOKABLE static QString artworkForSource(const QString &videoId);

Q_SIGNALS:
    void playingChanged();
    void currentTrackChanged();
    void autoplayChanged();
    void positionChanged();
    void durationChanged();
    void volumeChanged();
    void shuffleChanged();
    void repeatModeChanged();
    void favouriteChanged();
    void bufferingChanged();
    void statusChanged();
    void videoChanged();
    void playbackError(const QString &reason);
    // Something the user should be told, in their words, for the toast.
    void notice(const QString &text);
    // A listen was written to the history and to Recently played.
    void playRecorded();

private:
    void startQueue(QList<QueueTrack> tracks, int start, bool autoPlay);
    void beginCurrent(bool autoPlay);
    void beginTrack(const QVariantMap &track, bool autoPlay);
    void handleResolved(const QString &videoId, const QString &url, int tier, bool fromCache);
    void handleResolveFailed(const QString &videoId, const QString &reason);
    void handleVideoResolved(const QString &videoId, const QString &videoUrl, const QString &audioUrl,
                             const QVariantMap &headers);
    void playWithVideo(bool video);
    // A picture that will not play must not cost the song: back to the sound,
    // from the same second, with a word about it.
    bool abandonVideo(const QString &reason);
    void handleEndOfFile();
    void prefetchUpcoming();
    void advance(bool keepPlaying);
    // One row per listen, finalised with the playhead at the moment the track
    // is left. What the recommender is built on.
    void startListening();   // history and the play event, once per track
    void openPlayEvent(const QVariantMap &track);
    void closePlayEvent();
    bool extendWithRadio();   // false when there is nothing to seed a radio from
    void refreshFavourite();
    void setStatus(const QString &text, const QString &source, bool resolving, bool error = false);
    void setDuration(qint64 ms);
    void setPlayingFlag(bool playing);
    void haltPlayback();   // the flag cleared and mpv paused, together
    void recordHistory(const QVariantMap &track);
    void saveSetting(const QString &key, const QString &value);
    void saveVolume();

    MpvEngine *m_engine = nullptr;
    StreamResolver *m_resolver = nullptr;
    DownloadManager *m_downloads = nullptr;
    Library *m_library = nullptr;

    QueueModel m_queue;
    InnerTube m_innerTube;   // for the radio

    QVariantMap m_currentTrack;
    QString m_pendingVideoId;        // resolution in flight for this id
    QString m_streamVideoId;         // the resolved stream now loaded, if any,
    int m_streamTier = -1;           // the tier it came from (-1 for files),
    bool m_streamFromCache = false;  // and whether it was a remembered link
    QString m_statusText;
    QString m_sourceLabel;
    bool m_statusError = false;
    // The open play event, and the songs already finalised while this app has
    // been running — "played again" is only knowable in memory.
    qint64 m_playEventId = 0;
    QString m_playEventKey;
    QSet<QString> m_finalisedThisSession;
    // The surface the current queue was started from. It lasts as long as the
    // queue: the fourth track of an album still came from wherever the album
    // did. Only the radio's own additions override it.
    QString m_source;
    // Consecutive tracks that would not resolve. A queue is skipped past one
    // bad track, but a machine that is offline — or a YouTube-wide block —
    // must not race the whole queue, spawning a resolve for every row.
    int m_consecutiveFailures = 0;

    QString m_radioSeed;             // the song the radio request in flight is for
    bool m_waitingForRadio = false;  // the queue ran out and is waiting on it

    bool m_playing = false;
    bool m_buffering = false;
    bool m_resolving = false;
    // Whether the listener is waiting for sound from this track: set by
    // starting it with Play or by pressing Play, cleared by Pause.
    bool m_autoPlayAfterResolve = true;
    // Nothing is recorded for the current track yet: it was only loaded, or
    // its sound has not arrived. It is recorded once, when it does play.
    bool m_listenPending = false;
    // Previous restarted the current song. Its listen was closed then; the
    // next begins once it plays past the restart point again. Rewinding
    // until mpv reports the jump back, Replaying from then on.
    enum class Replay { None, Rewinding, Replaying };
    Replay m_replay = Replay::None;
    bool m_autoplay = true;
    qint64 m_position = 0;
    qint64 m_duration = 0;
    qreal m_volume = 0.65;
    // A drag of the volume slider is dozens of changes a second: the value is
    // written once it settles, not for each.
    QTimer m_volumeSave;
    bool m_shuffle = false;
    int m_repeatMode = RepeatOff;
    bool m_favourite = false;
    bool m_videoWanted = false;
    bool m_videoPlaying = false;
    // True from asking mpv to play the picture until a frame proves it can:
    // anything that ends the file in between is the video's fault, not the
    // song's, and must not move the queue on.
    bool m_videoUnproven = false;
    qint64 m_resumeAt = 0;         // where the next load should begin
    int m_videoHeight = 720;
    QString m_videoPendingId;      // a picture being resolved for this track
};
