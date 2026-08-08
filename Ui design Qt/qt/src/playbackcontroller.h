#pragma once

#include <QObject>
#include <QTimer>
#include <QVariantMap>

class TrackModel;

// Playback facade for the UI.
//
// Today it advances a simulated clock so the interface is fully operable with
// mock data. The real engine (MPV via libmpv, FFmpeg decoding) plugs in behind
// the same API: replace the body of play/pause/seek/loadIndex and drive
// setPosition/setDuration from the engine's event loop. No QML change needed.
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
public:
    enum RepeatMode { RepeatOff = 0, RepeatAll = 1, RepeatOne = 2 };
    Q_ENUM(RepeatMode)

    explicit PlaybackController(QObject *parent = nullptr);

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

Q_SIGNALS:
    void playingChanged();
    void currentTrackChanged();
    void positionChanged();
    void durationChanged();
    void volumeChanged();
    void shuffleChanged();
    void repeatModeChanged();
    void favouriteChanged();

private:
    void tick();
    void setDuration(qint64 ms);

    TrackModel *m_queue = nullptr;
    QTimer m_clock;
    QVariantMap m_currentTrack;
    int m_index = -1;
    bool m_playing = false;
    qint64 m_position = 0;
    qint64 m_duration = 0;
    qreal m_volume = 0.65;
    bool m_shuffle = false;
    int m_repeatMode = RepeatOff;
    bool m_favourite = false;
};
