#include "winmediasession.h"
#include "playbackcontroller.h"

#include <QEvent>
#include <QKeyEvent>
#include <QUrl>
#include <QVariantMap>
#include <QWindow>

#include <atomic>
#include <functional>
#include <mutex>

#include <windows.h>
#include <eventtoken.h>
#include <inspectable.h>
#include <roapi.h>
#include <systemmediatransportcontrolsinterop.h>
#include <winstring.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

// A namespace with a name, not an anonymous one: nothing in this file
// implements these interfaces (Windows does), and GCC, seeing a type no other
// file could name, would take a call through one for a call that cannot
// happen, and optimise it into a jump to nowhere. The Release build crashed
// so; the Debug build, not optimised, did not.
namespace MonolistWinRT {

// The parts of Windows.Media, Windows.Foundation and Windows.Storage.Streams
// used here, as mingw-w64's newer headers declare them (this toolchain's do
// not have them yet): each interface's methods in their order, and its IID.
// Enums travel as 32-bit ints, booleans as one byte, times as 100 ns ticks.
// The two event handlers' IIDs are the ones Windows derives for them from
// their type names (the ButtonPressed one checked against the SDK's).

constexpr int kStatusClosed = 0;
constexpr int kStatusPlaying = 3;
constexpr int kStatusPaused = 4;
constexpr int kTypeMusic = 1;
constexpr int kButtonPlay = 0;
constexpr int kButtonPause = 1;
constexpr int kButtonStop = 2;
constexpr int kButtonNext = 6;
constexpr int kButtonPrevious = 7;

struct TimeSpan
{
    INT64 Duration;
};

struct IDisplayUpdater;
struct IButtonHandler;
struct IPositionHandler;
struct ITimeline;

// Windows.Media.ISystemMediaTransportControls
struct IControls : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE get_PlaybackStatus(int *value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_PlaybackStatus(int value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_DisplayUpdater(IDisplayUpdater **value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_SoundLevel(int *value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsEnabled(boolean *value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_IsEnabled(boolean value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsPlayEnabled(boolean *value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_IsPlayEnabled(boolean value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsStopEnabled(boolean *value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_IsStopEnabled(boolean value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsPauseEnabled(boolean *value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_IsPauseEnabled(boolean value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsRecordEnabled(boolean *value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_IsRecordEnabled(boolean value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsFastForwardEnabled(boolean *value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_IsFastForwardEnabled(boolean value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsRewindEnabled(boolean *value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_IsRewindEnabled(boolean value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsPreviousEnabled(boolean *value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_IsPreviousEnabled(boolean value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsNextEnabled(boolean *value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_IsNextEnabled(boolean value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsChannelUpEnabled(boolean *value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_IsChannelUpEnabled(boolean value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsChannelDownEnabled(boolean *value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_IsChannelDownEnabled(boolean value) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_ButtonPressed(IButtonHandler *handler, EventRegistrationToken *token) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_ButtonPressed(EventRegistrationToken token) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_PropertyChanged(IUnknown *handler, EventRegistrationToken *token) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_PropertyChanged(EventRegistrationToken token) = 0;
};
const IID kControlsIid = { 0x99fa3ff4, 0x1742, 0x42a6, { 0x90, 0x2e, 0x08, 0x7d, 0x41, 0xf9, 0x65, 0xec } };

// Windows.Media.ISystemMediaTransportControls2: the timeline, Windows 10 1607 on.
struct IControls2 : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE get_AutoRepeatMode(int *value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_AutoRepeatMode(int value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_ShuffleEnabled(boolean *value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_ShuffleEnabled(boolean value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_PlaybackRate(DOUBLE *value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_PlaybackRate(DOUBLE value) = 0;
    virtual HRESULT STDMETHODCALLTYPE UpdateTimelineProperties(ITimeline *timeline) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_PlaybackPositionChangeRequested(IPositionHandler *handler,
                                                                          EventRegistrationToken *token) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_PlaybackPositionChangeRequested(EventRegistrationToken token) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_PlaybackRateChangeRequested(IUnknown *handler, EventRegistrationToken *token) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_PlaybackRateChangeRequested(EventRegistrationToken token) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_ShuffleEnabledChangeRequested(IUnknown *handler, EventRegistrationToken *token) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_ShuffleEnabledChangeRequested(EventRegistrationToken token) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_AutoRepeatModeChangeRequested(IUnknown *handler, EventRegistrationToken *token) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_AutoRepeatModeChangeRequested(EventRegistrationToken token) = 0;
};
const IID kControls2Iid = { 0xea98d2f6, 0x7f3c, 0x4af2, { 0xa5, 0x86, 0x72, 0x88, 0x98, 0x08, 0xef, 0xb1 } };

struct IMusicProperties;

// Windows.Media.ISystemMediaTransportControlsDisplayUpdater
struct IDisplayUpdater : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE get_Type(int *value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_Type(int value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_AppMediaId(HSTRING *value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_AppMediaId(HSTRING value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Thumbnail(IInspectable **value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_Thumbnail(IInspectable *value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_MusicProperties(IMusicProperties **value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_VideoProperties(IInspectable **value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_ImageProperties(IInspectable **value) = 0;
    virtual HRESULT STDMETHODCALLTYPE CopyFromFileAsync(int type, IInspectable *source, IInspectable **operation) = 0;
    virtual HRESULT STDMETHODCALLTYPE ClearAll() = 0;
    virtual HRESULT STDMETHODCALLTYPE Update() = 0;
};

// Windows.Media.IMusicDisplayProperties
struct IMusicProperties : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE get_Title(HSTRING *value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_Title(HSTRING value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_AlbumArtist(HSTRING *value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_AlbumArtist(HSTRING value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Artist(HSTRING *value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_Artist(HSTRING value) = 0;
};

// Windows.Media.ISystemMediaTransportControlsTimelineProperties
struct ITimeline : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE get_StartTime(TimeSpan *value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_StartTime(TimeSpan value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_EndTime(TimeSpan *value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_EndTime(TimeSpan value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_MinSeekTime(TimeSpan *value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_MinSeekTime(TimeSpan value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_MaxSeekTime(TimeSpan *value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_MaxSeekTime(TimeSpan value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Position(TimeSpan *value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_Position(TimeSpan value) = 0;
};
const IID kTimelineIid = { 0x5125316a, 0xc3a2, 0x475b, { 0x85, 0x07, 0x93, 0x53, 0x4d, 0xc8, 0x8f, 0x15 } };

// Windows.Media.ISystemMediaTransportControlsButtonPressedEventArgs
struct IButtonArgs : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE get_Button(int *value) = 0;
};

// Windows.Media.IPlaybackPositionChangeRequestedEventArgs
struct IPositionArgs : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE get_RequestedPlaybackPosition(TimeSpan *value) = 0;
};

// TypedEventHandler<SystemMediaTransportControls, ...ButtonPressedEventArgs>
struct IButtonHandler : IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE Invoke(IControls *sender, IButtonArgs *args) = 0;
};
const IID kButtonHandlerIid = { 0x0557e996, 0x7b23, 0x5bae, { 0xaa, 0x81, 0xea, 0x0d, 0x67, 0x11, 0x43, 0xa4 } };

// TypedEventHandler<SystemMediaTransportControls, PlaybackPositionChangeRequestedEventArgs>
struct IPositionHandler : IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE Invoke(IControls *sender, IPositionArgs *args) = 0;
};
const IID kPositionHandlerIid = { 0x44e34f15, 0xbdc0, 0x50a7, { 0xac, 0xe4, 0x39, 0xe9, 0x1f, 0xb7, 0x53, 0xf1 } };

// Windows.Foundation.IUriRuntimeClassFactory
struct IUriFactory : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE CreateUri(HSTRING uri, IInspectable **instance) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateWithRelativeUri(HSTRING base, HSTRING relative, IInspectable **instance) = 0;
};
const IID kUriFactoryIid = { 0x44a9796f, 0x723e, 0x4fdf, { 0xa2, 0x18, 0x03, 0x3e, 0x75, 0xb0, 0xc0, 0x84 } };

// Windows.Storage.Streams.IRandomAccessStreamReferenceStatics
struct IStreamReferences : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE CreateFromFile(IInspectable *file, IInspectable **reference) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateFromUri(IInspectable *uri, IInspectable **reference) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateFromStream(IInspectable *stream, IInspectable **reference) = 0;
};
const IID kStreamReferencesIid = { 0x857309dc, 0x3fbf, 0x4e7d, { 0x98, 0x6f, 0xef, 0x3b, 0x1a, 0x07, 0xa9, 0x64 } };

const IID kInteropIid = { 0xddb0472d, 0xc911, 0x4a1f, { 0x86, 0xd9, 0xdc, 0x3d, 0x71, 0xa9, 0x5f, 0x5a } };
const IID kUnknownIid = { 0x00000000, 0x0000, 0x0000, { 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };
const IID kAgileObjectIid = { 0x94ea2b94, 0xe9cc, 0x49e0, { 0xc0, 0xff, 0xee, 0x64, 0xca, 0x8f, 0x5b, 0x90 } };

} // namespace MonolistWinRT

using namespace MonolistWinRT;

namespace {

// The position Windows shows may drift this far from the player's before it
// is told again; anything further is a seek. And while playing, it is told
// at least this often anyway.
constexpr qint64 kDriftMs = 1500;
constexpr qint64 kRefreshMs = 5000;

// A string for the Windows Runtime, for as long as the call it is made for.
class HString
{
public:
    explicit HString(const QString &text)
    {
        WindowsCreateString(reinterpret_cast<LPCWSTR>(text.utf16()), UINT32(text.size()), &m_value);
    }
    ~HString()
    {
        if (m_value)
            WindowsDeleteString(m_value);
    }
    HString(const HString &) = delete;
    HString &operator=(const HString &) = delete;
    HSTRING get() const { return m_value; }

private:
    HSTRING m_value = nullptr;
};

// Where Windows' events go. They arrive on a thread of Windows' own, so the
// player is only ever reached through its event loop, and the session takes
// the player away (under the lock) before it goes: an event already on its
// way then does nothing.
struct Relay
{
    std::mutex lock;
    PlaybackController *player = nullptr;
};

// An event handler Windows can call from any thread (it says it is agile,
// so Windows calls it where the event happens, as it is).
template <typename Interface, typename Args>
class Handler final : public Interface
{
public:
    Handler(const IID &iid, std::function<void(Args *)> call) : m_iid(iid), m_call(std::move(call)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **object) override
    {
        if (!object)
            return E_POINTER;
        if (IsEqualIID(riid, kUnknownIid) || IsEqualIID(riid, kAgileObjectIid) || IsEqualIID(riid, m_iid)) {
            *object = static_cast<Interface *>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_refs; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG left = --m_refs;
        if (left == 0)
            delete this;
        return left;
    }
    HRESULT STDMETHODCALLTYPE Invoke(IControls *, Args *args) override
    {
        if (args)
            m_call(args);
        return S_OK;
    }

private:
    std::atomic<ULONG> m_refs { 1 };
    const IID m_iid;
    const std::function<void(Args *)> m_call;
};

QString field(const QVariantMap &track, const char *key)
{
    return track.value(QLatin1String(key)).toString().trimmed();
}

} // namespace

struct WinMediaSession::Native
{
    bool uninitialize = false;   // RoInitialize was ours to undo
    ComPtr<IControls> controls;
    ComPtr<IControls2> controls2;
    ComPtr<IDisplayUpdater> updater;
    ComPtr<ITimeline> timeline;
    ComPtr<IUriFactory> uris;
    ComPtr<IStreamReferences> references;
    EventRegistrationToken buttonToken {};
    EventRegistrationToken positionToken {};
    bool buttonAdded = false;
    bool positionAdded = false;
    std::shared_ptr<Relay> relay;
};

WinMediaSession::WinMediaSession(PlaybackController *player, QObject *parent)
    : QObject(parent)
    , m_player(player)
{
}

WinMediaSession::~WinMediaSession()
{
    if (!m_native)
        return;
    Native &w = *m_native;
    {
        std::lock_guard<std::mutex> hold(w.relay->lock);
        w.relay->player = nullptr;
    }
    if (w.buttonAdded)
        w.controls->remove_ButtonPressed(w.buttonToken);
    if (w.positionAdded)
        w.controls2->remove_PlaybackPositionChangeRequested(w.positionToken);
    // Gone from the flyout with the app, rather than left there paused.
    w.controls->put_PlaybackStatus(kStatusClosed);
    w.updater->ClearAll();
    w.updater->Update();
    w.controls->put_IsEnabled(false);
    const bool uninitialize = w.uninitialize;
    m_native.reset();
    if (uninitialize)
        RoUninitialize();
}

bool WinMediaSession::attach(QWindow *window)
{
    if (m_native || !window || !m_player)
        return active();

    auto w = std::make_unique<Native>();
    // The Windows Runtime on this thread. Qt has usually started COM here
    // already, for drag and drop, and either way suits: S_FALSE and a
    // different apartment both still let the calls below be made.
    const HRESULT init = RoInitialize(RO_INIT_SINGLETHREADED);
    w->uninitialize = SUCCEEDED(init);

    const auto fail = [&](const char *what, HRESULT hr) {
        qWarning("media keys: %s (0x%08lx): the media keys work only while the window has the focus",
                 what, static_cast<unsigned long>(hr));
        if (w->uninitialize)
            RoUninitialize();
        // What the keys do without Windows' controls: Qt hands them to the
        // window with the focus as keys.
        window->installEventFilter(this);
        return false;
    };

    ComPtr<ISystemMediaTransportControlsInterop> interop;
    HRESULT hr = RoGetActivationFactory(HString(QStringLiteral("Windows.Media.SystemMediaTransportControls")).get(),
                                        kInteropIid, reinterpret_cast<void **>(interop.GetAddressOf()));
    if (FAILED(hr))
        return fail("Windows has no media controls to give", hr);
    hr = interop->GetForWindow(reinterpret_cast<HWND>(window->winId()), kControlsIid,
                               reinterpret_cast<void **>(w->controls.GetAddressOf()));
    if (FAILED(hr) || !w->controls)
        return fail("the window's media controls could not be had", hr);
    hr = w->controls->get_DisplayUpdater(w->updater.GetAddressOf());
    if (FAILED(hr) || !w->updater)
        return fail("the media controls have nothing to show a song with", hr);

    // The timeline, where Windows has it; the rest works without.
    if (SUCCEEDED(w->controls->QueryInterface(kControls2Iid, reinterpret_cast<void **>(w->controls2.GetAddressOf())))) {
        ComPtr<IInspectable> timeline;
        if (SUCCEEDED(RoActivateInstance(
                HString(QStringLiteral("Windows.Media.SystemMediaTransportControlsTimelineProperties")).get(),
                timeline.GetAddressOf())) && timeline) {
            timeline->QueryInterface(kTimelineIid, reinterpret_cast<void **>(w->timeline.GetAddressOf()));
        }
    }
    // For the cover, which Windows fetches itself from its address.
    RoGetActivationFactory(HString(QStringLiteral("Windows.Foundation.Uri")).get(), kUriFactoryIid,
                           reinterpret_cast<void **>(w->uris.GetAddressOf()));
    RoGetActivationFactory(HString(QStringLiteral("Windows.Storage.Streams.RandomAccessStreamReference")).get(),
                           kStreamReferencesIid, reinterpret_cast<void **>(w->references.GetAddressOf()));

    w->controls->put_IsEnabled(true);
    w->controls->put_IsPlayEnabled(true);
    w->controls->put_IsPauseEnabled(true);
    w->controls->put_IsNextEnabled(true);
    w->controls->put_IsPreviousEnabled(true);
    w->controls->put_IsStopEnabled(true);

    w->relay = std::make_shared<Relay>();
    w->relay->player = m_player;
    const std::shared_ptr<Relay> relay = w->relay;

    auto *buttons = new Handler<IButtonHandler, IButtonArgs>(kButtonHandlerIid, [relay](IButtonArgs *args) {
        int button = -1;
        if (FAILED(args->get_Button(&button)))
            return;
        std::lock_guard<std::mutex> hold(relay->lock);
        PlaybackController *player = relay->player;
        if (!player)
            return;
        switch (button) {
        case kButtonPlay:
            QMetaObject::invokeMethod(player, &PlaybackController::play, Qt::QueuedConnection);
            break;
        // Stop is a pause here, as the player bar has no stop: the song
        // stays where it is.
        case kButtonPause:
        case kButtonStop:
            QMetaObject::invokeMethod(player, &PlaybackController::pause, Qt::QueuedConnection);
            break;
        case kButtonNext:
            QMetaObject::invokeMethod(player, &PlaybackController::next, Qt::QueuedConnection);
            break;
        case kButtonPrevious:
            QMetaObject::invokeMethod(player, &PlaybackController::previous, Qt::QueuedConnection);
            break;
        default:
            break;
        }
    });
    w->buttonAdded = SUCCEEDED(w->controls->add_ButtonPressed(buttons, &w->buttonToken));
    buttons->Release();

    if (w->controls2) {
        auto *seeks = new Handler<IPositionHandler, IPositionArgs>(kPositionHandlerIid, [relay](IPositionArgs *args) {
            TimeSpan at {};
            if (FAILED(args->get_RequestedPlaybackPosition(&at)))
                return;
            const qint64 ms = at.Duration / 10000;
            std::lock_guard<std::mutex> hold(relay->lock);
            PlaybackController *player = relay->player;
            if (player)
                QMetaObject::invokeMethod(player, [player, ms]() { player->setPosition(ms); }, Qt::QueuedConnection);
        });
        w->positionAdded = SUCCEEDED(w->controls2->add_PlaybackPositionChangeRequested(seeks, &w->positionToken));
        seeks->Release();
    }

    if (!w->buttonAdded)
        return fail("the media keys could not be listened to", E_FAIL);

    m_native = std::move(w);
    qInfo("media keys: Windows' media controls are Monolist's%s",
          m_native->timeline ? ", with the timeline" : "");

    connect(m_player, &PlaybackController::currentTrackChanged, this, &WinMediaSession::trackChanged);
    connect(m_player, &PlaybackController::playingChanged, this, &WinMediaSession::publish);
    connect(m_player, &PlaybackController::durationChanged, this, &WinMediaSession::publish);
    connect(m_player, &PlaybackController::positionChanged, this, &WinMediaSession::positionMoved);
    trackChanged();
    return true;
}

// Without Windows' controls: the keys as Qt passes them to the window with
// the focus.
bool WinMediaSession::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::KeyPress && !m_native && m_player) {
        switch (static_cast<QKeyEvent *>(event)->key()) {
        case Qt::Key_MediaTogglePlayPause:
            m_player->togglePlay();
            return true;
        case Qt::Key_MediaPlay:
            m_player->play();
            return true;
        case Qt::Key_MediaPause:
        case Qt::Key_MediaStop:
            m_player->pause();
            return true;
        case Qt::Key_MediaNext:
            m_player->next();
            return true;
        case Qt::Key_MediaPrevious:
            m_player->previous();
            return true;
        default:
            break;
        }
    }
    return QObject::eventFilter(watched, event);
}

void WinMediaSession::trackChanged()
{
    if (!m_native)
        return;
    Native &w = *m_native;
    const QVariantMap track = m_player->currentTrack();
    if (!track.isEmpty()) {
        w.updater->put_Type(kTypeMusic);
        ComPtr<IMusicProperties> music;
        if (SUCCEEDED(w.updater->get_MusicProperties(music.GetAddressOf())) && music) {
            music->put_Title(HString(field(track, "title")).get());
            music->put_Artist(HString(field(track, "artist")).get());
        }
        const QString cover = field(track, "artwork");
        if (cover != m_coverSource) {
            m_coverSource = cover;
            setCover(cover);
        }
        w.updater->Update();
    }
    publish();
}

// A cover on the web, which Windows fetches itself. Only those: Windows reads
// no address a desktop app's file would have, and a song without one shows
// Monolist's icon instead.
void WinMediaSession::setCover(const QString &source)
{
    Native &w = *m_native;
    const QUrl url(source);
    ComPtr<IInspectable> reference;
    if (w.uris && w.references && (url.scheme() == QLatin1String("https") || url.scheme() == QLatin1String("http"))) {
        ComPtr<IInspectable> uri;
        if (SUCCEEDED(w.uris->CreateUri(HString(source).get(), uri.GetAddressOf())) && uri)
            w.references->CreateFromUri(uri.Get(), reference.GetAddressOf());
    }
    w.updater->put_Thumbnail(reference.Get());
}

void WinMediaSession::positionMoved()
{
    if (!m_native || !m_publishedAt.isValid())
        return;
    const qint64 expected = m_publishedPosition + (m_publishedPlaying ? m_publishedAt.elapsed() : 0);
    if (qAbs(m_player->position() - expected) > kDriftMs
        || (m_publishedPlaying && m_publishedAt.elapsed() > kRefreshMs))
        publishTimeline();
}

void WinMediaSession::publish()
{
    if (!m_native)
        return;
    Native &w = *m_native;
    if (m_player->currentTrack().isEmpty()) {
        w.controls->put_PlaybackStatus(kStatusClosed);
        w.updater->ClearAll();
        w.updater->Update();
        m_coverSource.clear();
        m_publishedAt.invalidate();
        return;
    }
    w.controls->put_PlaybackStatus(m_player->playing() ? kStatusPlaying : kStatusPaused);
    publishTimeline();
}

void WinMediaSession::publishTimeline()
{
    Native &w = *m_native;
    const qint64 duration = qMax<qint64>(0, m_player->duration());
    qint64 position = qMax<qint64>(0, m_player->position());
    if (duration > 0)
        position = qMin(position, duration);
    m_publishedPosition = position;
    m_publishedPlaying = m_player->playing();
    m_publishedAt.start();
    if (!w.controls2 || !w.timeline)
        return;
    const auto ticks = [](qint64 ms) { return TimeSpan { ms * 10000 }; };
    w.timeline->put_StartTime(ticks(0));
    w.timeline->put_EndTime(ticks(duration));
    w.timeline->put_MinSeekTime(ticks(0));
    w.timeline->put_MaxSeekTime(ticks(duration));
    w.timeline->put_Position(ticks(position));
    w.controls2->UpdateTimelineProperties(w.timeline.Get());
}
