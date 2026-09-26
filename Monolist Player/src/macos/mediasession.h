#pragma once

#include <QElapsedTimer>
#include <QImage>
#include <QObject>
#include <QPointer>
#include <QString>

class PlaybackController;
class QNetworkAccessManager;
class QNetworkReply;

// The Mac's Now Playing, which is how macOS routes the media keys.
//
// The play, next and previous keys on a Mac keyboard, the Touch Bar,
// headphone and AirPods controls, Control Center and the lock screen all act
// on whichever app last said it was playing. Monolist says so here, and shows
// the song, its cover and how far into it it is. Without this, the play key
// starts Apple Music over the top of it.
//
// Plain C++ here, so main.cpp needs no Objective-C; the MediaPlayer framework
// is in mediasession.mm.
class MacMediaSession : public QObject
{
    Q_OBJECT
public:
    // `network` fetches the cover through the artwork disk cache, so a cover
    // already on screen is not downloaded again.
    MacMediaSession(PlaybackController *player, QNetworkAccessManager *network,
                    QObject *parent = nullptr);
    ~MacMediaSession() override;

private:
    void trackChanged();
    void positionMoved();
    // Tells the system what is playing, and where, now.
    void publish();
    void fetchCover(const QString &source);

    PlaybackController *m_player;
    QNetworkAccessManager *m_network;
    QPointer<QNetworkReply> m_coverReply;
    QString m_coverSource;
    QImage m_cover;
    // What the system was last told. It runs the clock on from there by
    // itself, so the position is only told again when it jumps: a seek.
    qint64 m_publishedPosition = 0;
    bool m_publishedPlaying = false;
    QElapsedTimer m_publishedAt;
};
