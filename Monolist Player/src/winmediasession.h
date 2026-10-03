#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QString>

#include <memory>

class PlaybackController;
class QWindow;

// Windows' own media controls (System Media Transport Controls), which is how
// Windows routes the media keys.
//
// The play, next and previous keys on a keyboard or a headset, and the media
// flyout Windows shows beside its volume and in Quick Settings, act on
// whichever app last said it was playing. Monolist says so here, and shows
// the song, its cover and how far into it it is. Without this the keys reach
// Monolist only while its window has the focus, and the play key starts some
// other player instead.
//
// The Windows Runtime's types are kept in the .cpp; main.cpp sees none of
// them. macOS has the same in macos/mediasession.
class WinMediaSession : public QObject
{
    Q_OBJECT
public:
    explicit WinMediaSession(PlaybackController *player, QObject *parent = nullptr);
    ~WinMediaSession() override;

    // The controls belong to a window, so they are asked for once it exists.
    // False when Windows has none to give (an N edition without its media
    // features, say): the media keys then still work while the window has
    // the focus.
    bool attach(QWindow *window);
    bool active() const { return m_native != nullptr; }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void trackChanged();
    void positionMoved();
    // Playing or paused, and where, now.
    void publish();
    void publishTimeline();
    void setCover(const QString &source);

    struct Native;
    std::unique_ptr<Native> m_native;
    PlaybackController *m_player;
    QString m_coverSource;
    // What Windows was last told. It runs the clock on from there by itself,
    // so the place is told again only when it jumps, and every few seconds.
    qint64 m_publishedPosition = 0;
    bool m_publishedPlaying = false;
    double m_publishedRate = 1.0;   // the song's time against the wall's
    QElapsedTimer m_publishedAt;
};
