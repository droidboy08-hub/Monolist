#pragma once

#include <QObject>
#include <QString>
#include <QVariantMap>

class TrackModel;
class MpvEngine;
class StreamResolver;
class DownloadManager;

// Playback facade for the UI.
//
// The QML-visible API is unchanged from the interface prototype — every
// property and slot the components bind to still exists with the same name and
// meaning. What changed is underneath: position and duration now come from
// libmpv instead of a QTimer, and loading a track runs the source ladder that
// Melody called its playback protocol.
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
    Q_PROPERTY(int currentIndex READ currentIndex NOTIFY currentTrackChanged)
    Q_PROPERTY(qint64 position READ position WRITE setPosition NOTIFY positionChanged)
    Q_PROPERTY(qint64 duration READ duration NOTIFY durationChanged)
    Q_PROPERTY(qreal progress READ progress NOTIFY positionChanged)
    Q_PROPERTY(QString positionText READ positionText NOTIFY positionChanged)
    Q_PROPERTY(QString durationText READ durationText NOTIFY durationChanged)
    Q_PROPERTY(qreal volume READ volume WRITE setVolume NOTIFY volumeChanged)
    Q_PROPERTY(bool shuffle READ shuffle WRITE setShuffle NOTIFY shuffleChanged)
    Q_PROPERTY(int repeatMode READ repeatMode NOTIFY repeatModeChanged)
    Q_PROPERTY(bool favourite READ favourite NOTIFY favouriteChanged)
    // — added for the real backend —
    Q_PROPERTY(bool buffering READ buffering NOTIFY bufferingChanged)
    Q_PROPERTY(bool resolving READ resolving NOTIFY statusChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusChanged)
    Q_PROPERTY(QString sourceLabel READ sourceLabel NOTIFY statusChanged)
    Q_PROPERTY(bool engineAvailable READ engineAvailable CONSTANT)
public:
    enum RepeatMode { RepeatOff = 0, RepeatAll = 1, RepeatOne = 2 };
    Q_ENUM(RepeatMode)

    explicit PlaybackController(MpvEngine *engine,
                                StreamResolver *resolver,
                                DownloadManager *downloads,
                                QObject *parent = nullptr);

    void setQueue(TrackModel *model);

    bool playing() const { return m_playing; }
    QVariantMap currentTrack() const { return m_currentTrack; }
    int currentIndex() const { return m_index; }
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
    QString sourceLabel() const { return m_sourceLabel; }
    bool engineAvailable() const;

public Q_SLOTS:
    void play();
    void pause();
    void togglePlay();
    void next();
    void previous();
    void loadIndex(int index);
    void playIndex(int index);
    void setPosition(qint64 ms);
    void seekFraction(qreal fraction);
    void setVolume(qreal volume);
    void setShuffle(bool shuffle);
    void cycleRepeat();
    void toggleFavourite();

    // Plays an ad-hoc search result that is not in the library yet.
    void playSource(const QString &videoId, const QString &title, const QString &artist);

Q_SIGNALS:
    void playingChanged();
    void currentTrackChanged();
    void positionChanged();
    void durationChanged();
    void volumeChanged();
    void shuffleChanged();
    void repeatModeChanged();
    void favouriteChanged();
    void bufferingChanged();
    void statusChanged();
    void playbackError(const QString &reason);

private:
    void beginTrack(const QVariantMap &track, bool autoPlay);
    void handleResolved(const QString &videoId, const QString &url, int tier);
    void handleResolveFailed(const QString &videoId, const QString &reason);
    void handleEndOfFile();
    void setStatus(const QString &text, const QString &source, bool resolving);
    void setDuration(qint64 ms);
    void setPlayingFlag(bool playing);
    void recordHistory(int trackId);

    MpvEngine *m_engine = nullptr;
    StreamResolver *m_resolver = nullptr;
    DownloadManager *m_downloads = nullptr;
    TrackModel *m_queue = nullptr;

    QVariantMap m_currentTrack;
    QString m_pendingVideoId;      // resolution in flight for this id
    QString m_statusText;
    QString m_sourceLabel;

    int m_index = -1;
    bool m_playing = false;
    bool m_buffering = false;
    bool m_resolving = false;
    bool m_autoPlayAfterResolve = true;
    qint64 m_position = 0;
    qint64 m_duration = 0;
    qreal m_volume = 0.65;
    bool m_shuffle = false;
    int m_repeatMode = RepeatOff;
    bool m_favourite = false;
};
