#include "mediasession.h"
#include "playbackcontroller.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <QVariantMap>

#import <AppKit/AppKit.h>
#import <MediaPlayer/MediaPlayer.h>

namespace {

// The position the system shows may drift this far from the player's before
// it is corrected; anything further is a seek.
constexpr qint64 kDriftMs = 1500;

NSImage *toNSImage(const QImage &image)
{
    if (image.isNull())
        return nil;
    // toCGImage() shares the pixels rather than copying them, and declines
    // the formats Core Graphics has no equivalent for.
    CGImageRef cgImage = image.convertToFormat(QImage::Format_ARGB32_Premultiplied).toCGImage();
    if (!cgImage)
        return nil;
    NSImage *result = [[NSImage alloc] initWithCGImage:cgImage size:NSZeroSize];
    CGImageRelease(cgImage);
    return result;
}

// One of the track's fields, or nil when it has none, so that the system
// leaves the line out rather than showing it empty.
NSString *field(const QVariantMap &track, const char *key)
{
    const QString value = track.value(QLatin1String(key)).toString().trimmed();
    return value.isEmpty() ? nil : value.toNSString();
}

} // namespace

MacMediaSession::MacMediaSession(PlaybackController *player, QNetworkAccessManager *network,
                                 QObject *parent)
    : QObject(parent)
    , m_player(player)
    , m_network(network)
{
    // The commands arrive on the main thread, which is the player's; invoking
    // through the event loop anyway keeps a command that arrives mid-update
    // from running inside it.
    MPRemoteCommandCenter *commands = MPRemoteCommandCenter.sharedCommandCenter;
    PlaybackController *target = m_player;

    [commands.playCommand addTargetWithHandler:^MPRemoteCommandHandlerStatus(MPRemoteCommandEvent *event) {
        (void)event;
        QMetaObject::invokeMethod(target, &PlaybackController::play, Qt::QueuedConnection);
        return MPRemoteCommandHandlerStatusSuccess;
    }];
    [commands.pauseCommand addTargetWithHandler:^MPRemoteCommandHandlerStatus(MPRemoteCommandEvent *event) {
        (void)event;
        QMetaObject::invokeMethod(target, &PlaybackController::pause, Qt::QueuedConnection);
        return MPRemoteCommandHandlerStatusSuccess;
    }];
    [commands.togglePlayPauseCommand addTargetWithHandler:^MPRemoteCommandHandlerStatus(MPRemoteCommandEvent *event) {
        (void)event;
        QMetaObject::invokeMethod(target, &PlaybackController::togglePlay, Qt::QueuedConnection);
        return MPRemoteCommandHandlerStatusSuccess;
    }];
    [commands.nextTrackCommand addTargetWithHandler:^MPRemoteCommandHandlerStatus(MPRemoteCommandEvent *event) {
        (void)event;
        QMetaObject::invokeMethod(target, &PlaybackController::next, Qt::QueuedConnection);
        return MPRemoteCommandHandlerStatusSuccess;
    }];
    [commands.previousTrackCommand addTargetWithHandler:^MPRemoteCommandHandlerStatus(MPRemoteCommandEvent *event) {
        (void)event;
        QMetaObject::invokeMethod(target, &PlaybackController::previous, Qt::QueuedConnection);
        return MPRemoteCommandHandlerStatusSuccess;
    }];
    // Dragging the position in Control Center or on the Touch Bar.
    [commands.changePlaybackPositionCommand addTargetWithHandler:^MPRemoteCommandHandlerStatus(MPRemoteCommandEvent *event) {
        MPChangePlaybackPositionCommandEvent *change = (MPChangePlaybackPositionCommandEvent *)event;
        const qint64 ms = qint64(change.positionTime * 1000.0);
        QMetaObject::invokeMethod(target, [target, ms]() { target->setPosition(ms); },
                                  Qt::QueuedConnection);
        return MPRemoteCommandHandlerStatusSuccess;
    }];

    connect(m_player, &PlaybackController::currentTrackChanged, this, &MacMediaSession::trackChanged);
    connect(m_player, &PlaybackController::playingChanged, this, &MacMediaSession::publish);
    connect(m_player, &PlaybackController::durationChanged, this, &MacMediaSession::publish);
    connect(m_player, &PlaybackController::positionChanged, this, &MacMediaSession::positionMoved);

    trackChanged();
}

MacMediaSession::~MacMediaSession()
{
    // nil takes every handler off each command: the blocks hold the player,
    // which is about to go.
    MPRemoteCommandCenter *commands = MPRemoteCommandCenter.sharedCommandCenter;
    [commands.playCommand removeTarget:nil];
    [commands.pauseCommand removeTarget:nil];
    [commands.togglePlayPauseCommand removeTarget:nil];
    [commands.nextTrackCommand removeTarget:nil];
    [commands.previousTrackCommand removeTarget:nil];
    [commands.changePlaybackPositionCommand removeTarget:nil];

    MPNowPlayingInfoCenter *center = MPNowPlayingInfoCenter.defaultCenter;
    center.nowPlayingInfo = nil;
    center.playbackState = MPNowPlayingPlaybackStateStopped;

    if (m_coverReply)
        m_coverReply->abort();
}

void MacMediaSession::trackChanged()
{
    const QString source = m_player->currentTrack().value(QStringLiteral("artwork")).toString();
    if (source != m_coverSource) {
        m_coverSource = source;
        m_cover = QImage();
        fetchCover(source);
    }
    publish();
}

void MacMediaSession::positionMoved()
{
    if (!m_publishedAt.isValid())
        return;
    const qint64 expected = m_publishedPosition + (m_publishedPlaying ? m_publishedAt.elapsed() : 0);
    if (qAbs(m_player->position() - expected) > kDriftMs)
        publish();
}

void MacMediaSession::publish()
{
    MPNowPlayingInfoCenter *center = MPNowPlayingInfoCenter.defaultCenter;
    const QVariantMap track = m_player->currentTrack();
    if (track.isEmpty()) {
        center.nowPlayingInfo = nil;
        center.playbackState = MPNowPlayingPlaybackStateStopped;
        m_publishedAt.invalidate();
        return;
    }

    const bool playing = m_player->playing();
    const qint64 position = m_player->position();
    const qint64 duration = m_player->duration();

    // Setting nil through a subscript removes the key, which is what a
    // missing field should do.
    NSMutableDictionary<NSString *, id> *info = [NSMutableDictionary dictionary];
    info[MPMediaItemPropertyTitle] = field(track, "title");
    info[MPMediaItemPropertyArtist] = field(track, "artist");
    info[MPMediaItemPropertyAlbumTitle] = field(track, "album");
    info[MPNowPlayingInfoPropertyMediaType] = @(MPNowPlayingInfoMediaTypeAudio);
    if (duration > 0)
        info[MPMediaItemPropertyPlaybackDuration] = @(double(duration) / 1000.0);
    info[MPNowPlayingInfoPropertyElapsedPlaybackTime] = @(double(position) / 1000.0);
    // The rate is what lets the system run the clock on by itself.
    info[MPNowPlayingInfoPropertyPlaybackRate] = @(playing ? 1.0 : 0.0);

    if (NSImage *image = toNSImage(m_cover)) {
        info[MPMediaItemPropertyArtwork] =
            [[MPMediaItemArtwork alloc] initWithBoundsSize:image.size
                                            requestHandler:^NSImage *(CGSize size) {
                                                (void)size;
                                                return image;
                                            }];
    }

    center.nowPlayingInfo = info;
    center.playbackState = playing ? MPNowPlayingPlaybackStatePlaying
                                   : MPNowPlayingPlaybackStatePaused;

    m_publishedPosition = position;
    m_publishedPlaying = playing;
    m_publishedAt.start();
}

void MacMediaSession::fetchCover(const QString &source)
{
    if (m_coverReply) {
        m_coverReply->abort();
        m_coverReply = nullptr;
    }
    if (source.isEmpty())
        return;

    const QUrl url(source);
    if (!url.isValid() || url.isLocalFile() || url.scheme().isEmpty()) {
        m_cover = QImage(url.isLocalFile() ? url.toLocalFile() : source);
        return;   // trackChanged() publishes next, with it
    }
    if (!m_network)
        return;

    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::PreferCache);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Monolist/0.1"));
    request.setTransferTimeout(10000);
    QNetworkReply *reply = m_network->get(request);
    m_coverReply = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply, source]() {
        reply->deleteLater();
        if (m_coverReply == reply)
            m_coverReply = nullptr;
        // A cover for a song that is no longer playing is not wanted.
        if (reply->error() != QNetworkReply::NoError || source != m_coverSource)
            return;
        const QImage image = QImage::fromData(reply->readAll());
        if (image.isNull())
            return;
        m_cover = image;
        publish();
    });
}
