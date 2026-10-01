#include "googlesignin.h"

#include "appdatabase.h"
#include "cookieimport.h"
#include "ytmsession.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QMap>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSqlQuery>
#include <QTimer>
#include <QUrl>
#include <QWindow>

#include <windows.h>
#include <objbase.h>

#include "WebView2.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <utility>

// A namespace with a name, not an anonymous one: the callbacks below are
// called by WebView2, never by this file, and GCC, seeing types no other file
// could name, would take a call through one for a call that cannot happen and
// optimise it away (see winmediasession.cpp, where a Release build crashed).
namespace MonolistWebView {

constexpr wchar_t kWindowClass[] = L"MonolistGoogleSignIn";
const QString kWindowTitle = QStringLiteral("Sign in to YouTube Music");

// Where YouTube Music's own Sign in button goes: Google's page, then
// YouTube's sign-in handler, which sets the session on youtube.com, then
// YouTube Music. Nothing added to it.
constexpr wchar_t kSignInUrl[] =
    L"https://accounts.google.com/ServiceLogin?ltmpl=music&service=youtube&uilel=3&passive=true"
    L"&continue=https%3A%2F%2Fwww.youtube.com%2Fsignin%3Faction_handle_signin%3Dtrue%26app%3Ddesktop"
    L"%26hl%3Den%26next%3Dhttps%253A%252F%252Fmusic.youtube.com%252F&hl=en";

// Google refused to sign in here: when, so the window is not offered again
// for a while (kRefusalRestDays), and the import is offered instead.
const QString kRefusedAtKey = QStringLiteral("signin.refused_at");
constexpr qint64 kRefusalRestDays = 7;

// Variables that would change how the engine starts for this process: extra
// command-line switches (a debugging port among them), another profile or
// another engine. The window is the engine as it comes, so none of them.
const char *const kEngineVariables[] = {
    "WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS", "WEBVIEW2_USER_DATA_FOLDER",
    "WEBVIEW2_BROWSER_EXECUTABLE_FOLDER", "WEBVIEW2_RELEASE_CHANNEL_PREFERENCE",
    "WEBVIEW2_RELEASE_CHANNELS", "WEBVIEW2_CHANNEL_SEARCH_KIND",
    "WEBVIEW2_PIPE_FOR_SCRIPT_DEBUGGER", "WEBVIEW2_WAIT_FOR_SCRIPT_DEBUGGER",
};

// Cleared in this process, and in the environment the engine's processes
// inherit. Names only are logged, never values.
void clearEngineVariables()
{
    for (const char *name : kEngineVariables) {
        if (!qEnvironmentVariableIsSet(name))
            continue;
        qInfo("signin: ignoring %s", name);
        qunsetenv(name);
        SetEnvironmentVariableA(name, nullptr);
    }
}

// Whether the process with this id is running (or cannot be asked, which
// means it is there): its sign-in window's profile is left alone.
bool processRunning(DWORD pid)
{
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process)
        return GetLastError() == ERROR_ACCESS_DENIED;
    DWORD code = 0;
    const bool running = GetExitCodeProcess(process, &code) && code == STILL_ACTIVE;
    CloseHandle(process);
    return running;
}

QString profilePrefix()
{
    return QStringLiteral("monolist-signin-");
}

using CreateEnvironment = HRESULT(STDAPICALLTYPE *)(PCWSTR, PCWSTR, ICoreWebView2EnvironmentOptions *,
                                                   ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *);
using BrowserVersion = HRESULT(STDAPICALLTYPE *)(PCWSTR, LPWSTR *);
using DpiForWindow = UINT(WINAPI *)(HWND);

// One WebView2 callback: its interface, and Invoke as a function. Made with
// one reference, which whoever made it gives up once it has been handed over.
template <typename Interface, typename... Args>
class Callback final : public Interface
{
public:
    Callback(const IID &iid, std::function<HRESULT(Args...)> call) : m_iid(iid), m_call(std::move(call)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **object) override
    {
        if (!object)
            return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, m_iid)) {
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
    HRESULT STDMETHODCALLTYPE Invoke(Args... args) override { return m_call(args...); }

private:
    std::atomic<ULONG> m_refs { 1 };
    const IID m_iid;
    const std::function<HRESULT(Args...)> m_call;
};

// A string WebView2 handed over, as a QString, and given back.
QString take(LPWSTR text)
{
    if (!text)
        return QString();
    const QString result = QString::fromWCharArray(text);
    CoTaskMemFree(text);
    return result;
}

bool isYouTube(const QString &domain)
{
    const QString host = domain.startsWith(QLatin1Char('.')) ? domain.mid(1).toLower() : domain.toLower();
    return host == QLatin1String("youtube.com") || host.endsWith(QLatin1String(".youtube.com"));
}

// Where the window may go: Google's sign-in and account pages (in any of
// Google's country domains), and YouTube's, over https. Anything else a page
// links to opens in the user's own browser, if they clicked it.
bool allowedPage(const QUrl &url)
{
    if (url.scheme() != QLatin1String("https"))
        return false;
    static const QRegularExpression hosts(QStringLiteral(
        R"(^(?:[a-z0-9-]+\.)*(?:google\.(?:[a-z]{2,3}|co\.[a-z]{2}|com\.[a-z]{2})|youtube\.com)$)"));
    return hosts.match(url.host().toLower()).hasMatch();
}

// Google's "Couldn't sign you in / This browser or app may not be secure"
// page, under whichever version of its sign-in.
bool isRefusal(const QUrl &url)
{
    const QString host = url.host().toLower();
    if (!host.startsWith(QLatin1String("accounts.google.")))
        return false;
    const QString path = url.path().toLower();
    return path.contains(QLatin1String("rejected")) || path.contains(QLatin1String("deniedsignin"));
}

QString setting(const QString &key)
{
    QSqlQuery query(AppDatabase::connection());
    query.prepare(QStringLiteral("SELECT value FROM settings WHERE key = ?"));
    query.addBindValue(key);
    return query.exec() && query.next() ? query.value(0).toString() : QString();
}

void setSetting(const QString &key, const QString &value)
{
    QSqlQuery query(AppDatabase::connection());
    query.prepare(QStringLiteral("INSERT OR REPLACE INTO settings (key, value) VALUES (?, ?)"));
    query.addBindValue(key);
    query.addBindValue(AppDatabase::text(value));
    query.exec();
}

// The YouTube cookies of one list, as cookies.txt lines, keyed so the same
// cookie read from two addresses is kept once. No value is ever logged.
void collectCookies(ICoreWebView2CookieList *list, QMap<QString, QByteArray> *lines)
{
    UINT count = 0;
    list->get_Count(&count);
    for (UINT i = 0; i < count; ++i) {
        ICoreWebView2Cookie *cookie = nullptr;
        if (FAILED(list->GetValueAtIndex(i, &cookie)) || !cookie)
            continue;
        LPWSTR name = nullptr, value = nullptr, domain = nullptr, path = nullptr;
        cookie->get_Name(&name);
        cookie->get_Value(&value);
        cookie->get_Domain(&domain);
        cookie->get_Path(&path);
        double expires = 0;
        BOOL httpOnly = FALSE, secure = FALSE, session = FALSE;
        cookie->get_Expires(&expires);
        cookie->get_IsHttpOnly(&httpOnly);
        cookie->get_IsSecure(&secure);
        cookie->get_IsSession(&session);
        cookie->Release();
        const QString cookieName = take(name);
        QString cookieValue = take(value);
        const QString cookieDomain = take(domain);
        QString cookiePath = take(path);
        if (cookiePath.isEmpty())
            cookiePath = QStringLiteral("/");
        if (!cookieName.isEmpty() && isYouTube(cookieDomain)) {
            QByteArray line;
            if (httpOnly)
                line += "#HttpOnly_";
            line += cookieDomain.toUtf8();
            line += cookieDomain.startsWith(QLatin1Char('.')) ? "\tTRUE\t" : "\tFALSE\t";
            line += cookiePath.toUtf8();
            line += secure ? "\tTRUE\t" : "\tFALSE\t";
            line += QByteArray::number(session || expires <= 0 ? qint64(0) : qint64(expires));
            line += '\t';
            line += cookieName.toUtf8();
            line += '\t';
            line += cookieValue.toUtf8();
            line += '\n';
            lines->insert(cookieName + QLatin1Char('\t') + cookieDomain + QLatin1Char('\t') + cookiePath, line);
        }
        cookieValue.fill(QLatin1Char('\0'));
    }
}

// The profile folder, deleted once the engine's processes have let go of it;
// a few tries, and removeLeftovers() at the next launch after that.
void removeProfile(const QString &path, int tries)
{
    if (path.isEmpty() || !QDir(path).exists())
        return;
    if (QDir(path).removeRecursively() || tries <= 0)
        return;
    QTimer::singleShot(500, QCoreApplication::instance(), [path, tries]() { removeProfile(path, tries - 1); });
}

// One window's engine, let go of as a whole: what retire() hands on.
struct Engine
{
    HWND window = nullptr;
    ICoreWebView2Environment *environment = nullptr;
    ICoreWebView2Controller *controller = nullptr;
    ICoreWebView2 *web = nullptr;
    EventRegistrationToken navigated {};
    EventRegistrationToken moved {};
    EventRegistrationToken starting {};
    EventRegistrationToken popups {};
    QString profile;
};

// Closes the engine, then its window; and the profile goes once the engine's
// processes have exited (BrowserProcessExited), or after a while if that
// never comes. Touches nothing of GoogleSignIn's.
void releaseEngine(Engine engine)
{
    if (engine.web) {
        engine.web->remove_NavigationCompleted(engine.navigated);
        engine.web->remove_SourceChanged(engine.moved);
        engine.web->remove_NavigationStarting(engine.starting);
        engine.web->remove_NewWindowRequested(engine.popups);
        engine.web->Release();
    }
    if (engine.controller) {
        engine.controller->Close();
        engine.controller->Release();
    }
    if (engine.window) {
        SetWindowLongPtrW(engine.window, GWLP_USERDATA, 0);
        DestroyWindow(engine.window);
    }

    struct Ending {
        ICoreWebView2Environment *environment = nullptr;
        QString profile;
        bool done = false;
    };
    auto ending = std::make_shared<Ending>();
    ending->environment = engine.environment;
    ending->profile = engine.profile;
    const auto finish = [ending]() {
        if (ending->done)
            return;
        ending->done = true;
        removeProfile(ending->profile, 20);
        if (ending->environment) {
            ending->environment->Release();
            ending->environment = nullptr;
        }
    };
    ICoreWebView2Environment5 *environment5 = nullptr;
    if (engine.environment
        && SUCCEEDED(engine.environment->QueryInterface(IID_ICoreWebView2Environment5,
                                                        reinterpret_cast<void **>(&environment5)))
        && environment5) {
        auto *onExited = new Callback<ICoreWebView2BrowserProcessExitedEventHandler, ICoreWebView2Environment *,
                                      ICoreWebView2BrowserProcessExitedEventArgs *>(
            IID_ICoreWebView2BrowserProcessExitedEventHandler,
            [finish](ICoreWebView2Environment *, ICoreWebView2BrowserProcessExitedEventArgs *) -> HRESULT {
                // Not from inside the engine's own event.
                QTimer::singleShot(0, QCoreApplication::instance(), finish);
                return S_OK;
            });
        EventRegistrationToken token {};
        environment5->add_BrowserProcessExited(onExited, &token);
        onExited->Release();
        environment5->Release();
    }
    QTimer::singleShot(10000, QCoreApplication::instance(), finish);
}

LRESULT CALLBACK hostProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

} // namespace MonolistWebView

using namespace MonolistWebView;

struct GoogleSignIn::Native
{
    GoogleSignIn *q = nullptr;
    HMODULE loader = nullptr;
    CreateEnvironment createEnvironment = nullptr;
    bool runtime = false;
    // Held while this lives: a callback holding a weak copy can tell whether
    // the object it would call into is still there.
    std::shared_ptr<char> alive = std::make_shared<char>(0);

    Engine engine;
    // Which window is open: what arrives for an earlier one is dropped.
    quint64 run = 0;
    bool reading = false;

    bool current(const std::weak_ptr<char> &life, quint64 forRun) const
    {
        return !life.expired() && forRun == run;
    }
    void attach(quint64 forRun, ICoreWebView2Controller *made);
    void landed(quint64 forRun);
    void readCookies(quint64 forRun);
    // Stops listening at once and hides the window; the engine, the window
    // and the profile are let go of a moment later, outside WebView2's own
    // callbacks, whatever is opened meanwhile.
    void retire();
    void resize();
    void showAddress(const QUrl &url);
};

namespace MonolistWebView {

LRESULT CALLBACK hostProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto *native = reinterpret_cast<GoogleSignIn::Native *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (message) {
    case WM_SIZE:
        if (native)
            native->resize();
        return 0;
    case WM_SETFOCUS:
        if (native && native->engine.controller)
            native->engine.controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
        return 0;
    case WM_CLOSE:
        // Closed by the user: nothing kept. The window goes when the engine does.
        if (native) {
            GoogleSignIn *q = native->q;
            QMetaObject::invokeMethod(q, [q]() { q->cancel(); }, Qt::QueuedConnection);
            return 0;
        }
        break;
    case WM_NCDESTROY:
        // Gone with its owner, the app's window, at the end.
        if (native && native->engine.window == hwnd)
            native->engine.window = nullptr;
        break;
    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

HWND createHostWindow(QWindow *owner, GoogleSignIn::Native *native)
{
    HINSTANCE instance = GetModuleHandleW(nullptr);
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW cls {};
        cls.cbSize = sizeof cls;
        cls.lpfnWndProc = hostProc;
        cls.hInstance = instance;
        cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        // The app's own icon (monolist.rc.in), as Qt's windows have.
        cls.hIcon = LoadIconW(instance, L"IDI_ICON1");
        cls.lpszClassName = kWindowClass;
        registered = RegisterClassExW(&cls) != 0;
        if (!registered)
            return nullptr;
    }
    HWND ownerWindow = owner ? reinterpret_cast<HWND>(owner->winId()) : nullptr;
    UINT dpi = 96;
    if (auto forWindow = reinterpret_cast<DpiForWindow>(
            reinterpret_cast<void *>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow")));
        forWindow && ownerWindow)
        dpi = forWindow(ownerWindow);
    const int width = MulDiv(480, int(dpi), 96);
    const int height = MulDiv(700, int(dpi), 96);
    // Over the middle of the app's window, or of the screen.
    RECT area {};
    if (!ownerWindow || !GetWindowRect(ownerWindow, &area))
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &area, 0);
    const int x = area.left + qMax(0, int(area.right - area.left - width) / 2);
    const int y = area.top + qMax(0, int(area.bottom - area.top - height) / 2);
    HWND window = CreateWindowExW(0, kWindowClass, reinterpret_cast<LPCWSTR>(kWindowTitle.utf16()),
                                  WS_OVERLAPPEDWINDOW, x, y, width, height, ownerWindow, nullptr, instance, nullptr);
    if (!window)
        return nullptr;
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(native));
    ShowWindow(window, SW_SHOWNORMAL);
    UpdateWindow(window);
    SetForegroundWindow(window);
    return window;
}

} // namespace MonolistWebView

void GoogleSignIn::Native::resize()
{
    if (!engine.controller || !engine.window)
        return;
    RECT bounds {};
    GetClientRect(engine.window, &bounds);
    engine.controller->put_Bounds(bounds);
}

// The window's title says whose page it is showing, as an address bar would.
void GoogleSignIn::Native::showAddress(const QUrl &url)
{
    if (!engine.window)
        return;
    const QString host = url.host().toLower();
    const QString title = host.isEmpty() ? kWindowTitle : kWindowTitle + QStringLiteral(" — ") + host;
    SetWindowTextW(engine.window, reinterpret_cast<LPCWSTR>(title.utf16()));
}

void GoogleSignIn::Native::attach(quint64 forRun, ICoreWebView2Controller *made)
{
    made->AddRef();
    engine.controller = made;
    if (FAILED(engine.controller->get_CoreWebView2(&engine.web)) || !engine.web) {
        q->fail(QStringLiteral("failed"), QStringLiteral("The sign-in window could not start."), true);
        return;
    }
    ICoreWebView2 *web = engine.web;
    // The developer tools off and the status line hidden; nothing else of
    // the engine's is changed — not its user agent, not what it sends.
    ICoreWebView2Settings *settings = nullptr;
    if (SUCCEEDED(web->get_Settings(&settings)) && settings) {
        settings->put_AreDevToolsEnabled(FALSE);
        settings->put_IsStatusBarEnabled(FALSE);
        settings->Release();
    }
    const std::weak_ptr<char> life = alive;

    // Only Google's and YouTube's pages, over https; a link anywhere else
    // goes to the user's own browser when they clicked it, and is dropped
    // when they did not.
    auto *onStarting = new Callback<ICoreWebView2NavigationStartingEventHandler, ICoreWebView2 *,
                                    ICoreWebView2NavigationStartingEventArgs *>(
        IID_ICoreWebView2NavigationStartingEventHandler,
        [](ICoreWebView2 *, ICoreWebView2NavigationStartingEventArgs *args) -> HRESULT {
            if (!args)
                return S_OK;
            LPWSTR uri = nullptr;
            args->get_Uri(&uri);
            const QUrl url(take(uri));
            if (allowedPage(url) || url.toString() == QLatin1String("about:blank"))
                return S_OK;
            BOOL clicked = FALSE;
            args->get_IsUserInitiated(&clicked);
            args->put_Cancel(TRUE);
            if (clicked && url.scheme() == QLatin1String("https"))
                QDesktopServices::openUrl(url);
            return S_OK;
        });
    web->add_NavigationStarting(onStarting, &engine.starting);
    onStarting->Release();

    auto *onNavigated = new Callback<ICoreWebView2NavigationCompletedEventHandler, ICoreWebView2 *,
                                     ICoreWebView2NavigationCompletedEventArgs *>(
        IID_ICoreWebView2NavigationCompletedEventHandler,
        [this, life, forRun](ICoreWebView2 *, ICoreWebView2NavigationCompletedEventArgs *) -> HRESULT {
            if (!life.expired() && current(life, forRun))
                landed(forRun);
            return S_OK;
        });
    web->add_NavigationCompleted(onNavigated, &engine.navigated);
    onNavigated->Release();

    // A page that changes its address without loading another (Google's
    // sign-in moves between its steps so) is looked at too.
    auto *onMoved = new Callback<ICoreWebView2SourceChangedEventHandler, ICoreWebView2 *,
                                 ICoreWebView2SourceChangedEventArgs *>(
        IID_ICoreWebView2SourceChangedEventHandler,
        [this, life, forRun](ICoreWebView2 *, ICoreWebView2SourceChangedEventArgs *) -> HRESULT {
            if (!life.expired() && current(life, forRun))
                landed(forRun);
            return S_OK;
        });
    web->add_SourceChanged(onMoved, &engine.moved);
    onMoved->Release();

    // A page asking for a window of its own (a help link) gets the user's
    // own browser instead, if they clicked it: this window is for signing in.
    auto *onPopup = new Callback<ICoreWebView2NewWindowRequestedEventHandler, ICoreWebView2 *,
                                 ICoreWebView2NewWindowRequestedEventArgs *>(
        IID_ICoreWebView2NewWindowRequestedEventHandler,
        [](ICoreWebView2 *, ICoreWebView2NewWindowRequestedEventArgs *args) -> HRESULT {
            if (!args)
                return S_OK;
            LPWSTR uri = nullptr;
            args->get_Uri(&uri);
            const QUrl url(take(uri));
            BOOL clicked = FALSE;
            args->get_IsUserInitiated(&clicked);
            args->put_Handled(TRUE);
            if (clicked && url.scheme() == QLatin1String("https"))
                QDesktopServices::openUrl(url);
            return S_OK;
        });
    web->add_NewWindowRequested(onPopup, &engine.popups);
    onPopup->Release();

    resize();
    engine.controller->put_IsVisible(TRUE);
    engine.controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
    web->Navigate(kSignInUrl);
    q->setState(QStringLiteral("open"));
}

// Each page the window lands on: Google's refusal ends it; YouTube's own
// pages are where a finished sign-in arrives, so its cookies are read there.
void GoogleSignIn::Native::landed(quint64 forRun)
{
    if (forRun != run || !engine.web)
        return;
    LPWSTR source = nullptr;
    engine.web->get_Source(&source);
    const QUrl url(take(source));
    showAddress(url);
    if (isRefusal(url)) {
        qInfo("signin: Google refused to sign in from this window");
        q->m_refusedAt = QDateTime::currentSecsSinceEpoch();
        setSetting(kRefusedAtKey, QString::number(q->m_refusedAt));
        q->fail(QStringLiteral("refused"),
                QStringLiteral("Google wouldn't sign in from inside Monolist. You can import a sign-in from "
                               "your browser instead."),
                true);
        return;
    }
    const QString host = url.host().toLower();
    if (host == CookieImport::kMusicHost || host == CookieImport::kWwwHost)
        readCookies(forRun);
}

// YouTube's cookies, as music.youtube.com and www.youtube.com would be sent
// them (never Google's own); finished once they are a session the import
// itself would take.
void GoogleSignIn::Native::readCookies(quint64 forRun)
{
    if (reading || !engine.web)
        return;
    ICoreWebView2_2 *web2 = nullptr;
    if (FAILED(engine.web->QueryInterface(IID_ICoreWebView2_2, reinterpret_cast<void **>(&web2))) || !web2)
        return;
    ICoreWebView2CookieManager *manager = nullptr;
    web2->get_CookieManager(&manager);
    web2->Release();
    if (!manager)
        return;
    reading = true;
    const std::weak_ptr<char> life = alive;
    auto lines = std::make_shared<QMap<QString, QByteArray>>();
    const auto done = [this, life, forRun, lines]() {
        if (life.expired() || !current(life, forRun))
            return;
        reading = false;
        QByteArray text = "# Netscape HTTP Cookie File\n";
        for (QByteArray &line : *lines) {
            text += line;
            line.fill('\0');
        }
        lines->clear();
        // Not yet: Google's pages, or YouTube Music still signed out.
        if (CookieImport::parse(text).ok())
            q->finish(text);
        text.fill('\0');
    };
    auto *onWww = new Callback<ICoreWebView2GetCookiesCompletedHandler, HRESULT, ICoreWebView2CookieList *>(
        IID_ICoreWebView2GetCookiesCompletedHandler,
        [life, lines, done](HRESULT result, ICoreWebView2CookieList *list) -> HRESULT {
            if (life.expired())
                return S_OK;
            if (SUCCEEDED(result) && list)
                collectCookies(list, lines.get());
            done();
            return S_OK;
        });
    auto *onMusic = new Callback<ICoreWebView2GetCookiesCompletedHandler, HRESULT, ICoreWebView2CookieList *>(
        IID_ICoreWebView2GetCookiesCompletedHandler,
        [this, life, forRun, lines, manager, onWww](HRESULT result, ICoreWebView2CookieList *list) -> HRESULT {
            const bool live = !life.expired() && current(life, forRun);
            if (live && SUCCEEDED(result) && list)
                collectCookies(list, lines.get());
            // Then www.youtube.com's, with the same manager.
            if (live && FAILED(manager->GetCookies(L"https://www.youtube.com", onWww)))
                reading = false;
            onWww->Release();
            manager->Release();
            return S_OK;
        });
    if (FAILED(manager->GetCookies(L"https://music.youtube.com", onMusic))) {
        reading = false;
        onWww->Release();
        manager->Release();
    }
    onMusic->Release();
}

void GoogleSignIn::Native::retire()
{
    ++run;
    reading = false;
    if (engine.window)
        ShowWindow(engine.window, SW_HIDE);
    Engine leaving = std::exchange(engine, Engine());
    if (!leaving.window && !leaving.controller && !leaving.environment && leaving.profile.isEmpty())
        return;
    QTimer::singleShot(0, QCoreApplication::instance(), [leaving]() { releaseEngine(leaving); });
}

// ------------------------------------------------------------------ public

GoogleSignIn::GoogleSignIn(YtmSession *session, QObject *parent)
    : QObject(parent), m_native(std::make_unique<Native>()), m_session(session)
{
    Native &n = *m_native;
    n.q = this;
    // The loader beside Monolist (packaged with it), and the engine
    // installed with Windows; both, or this way is not offered.
    const QString loaderPath = QDir::toNativeSeparators(QCoreApplication::applicationDirPath()
                                                        + QStringLiteral("/WebView2Loader.dll"));
    n.loader = LoadLibraryExW(reinterpret_cast<LPCWSTR>(loaderPath.utf16()), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!n.loader)
        return;
    n.createEnvironment = reinterpret_cast<CreateEnvironment>(
        reinterpret_cast<void *>(GetProcAddress(n.loader, "CreateCoreWebView2EnvironmentWithOptions")));
    const auto version = reinterpret_cast<BrowserVersion>(
        reinterpret_cast<void *>(GetProcAddress(n.loader, "GetAvailableCoreWebView2BrowserVersionString")));
    if (n.createEnvironment && version) {
        clearEngineVariables();
        LPWSTR found = nullptr;
        n.runtime = SUCCEEDED(version(nullptr, &found)) && found && *found;
        const QString engine = take(found);
        qInfo("signin: WebView2 %s", n.runtime ? qPrintable(engine) : "not installed");
    }
    const qint64 refusedAt = setting(kRefusedAtKey).toLongLong();
    if (refusedAt > 0 && QDateTime::currentSecsSinceEpoch() - refusedAt < kRefusalRestDays * 24 * 3600)
        m_refusedAt = refusedAt;
}

bool GoogleSignIn::refused() const
{
    return m_refusedAt > 0 && QDateTime::currentSecsSinceEpoch() - m_refusedAt < kRefusalRestDays * 24 * 3600;
}

void GoogleSignIn::forgetRefusal()
{
    m_refusedAt = 0;
    setSetting(kRefusedAtKey, QString());
    Q_EMIT stateChanged();
}

// At the end, with the window still open: the engine is closed now, and its
// profile goes as soon as it can, or at the next launch (removeLeftovers).
GoogleSignIn::~GoogleSignIn()
{
    Native &n = *m_native;
    ++n.run;
    n.alive.reset();
    Engine leaving = std::exchange(n.engine, Engine());
    const QString profile = leaving.profile;
    leaving.profile.clear();
    releaseEngine(leaving);
    removeProfile(profile, 0);
}

void GoogleSignIn::setOwner(QWindow *window)
{
    m_owner = window;
}

bool GoogleSignIn::available() const
{
    return m_native->runtime;
}

void GoogleSignIn::start()
{
    Native &n = *m_native;
    if (!available()) {
        fail(QStringLiteral("failed"), QString(), true);
        return;
    }
    if (busy() && n.engine.window) {
        ShowWindow(n.engine.window, SW_SHOWNORMAL);
        SetForegroundWindow(n.engine.window);
        return;
    }
    // Refused lately: Google's answer stands for a while, and is not asked
    // again at every click.
    if (refused()) {
        const QString when = QLocale().toString(QDateTime::fromSecsSinceEpoch(m_refusedAt).date(),
                                                QLocale::LongFormat);
        fail(QStringLiteral("refused"),
             QStringLiteral("Google wouldn't sign in from inside Monolist on %1. Import a sign-in from your "
                            "browser instead.").arg(when),
             true);
        return;
    }

    const quint64 run = ++n.run;
    // Named after this process, so another Monolist starting meanwhile
    // leaves it alone (removeLeftovers).
    n.engine.profile = QDir(QDir::tempPath()).filePath(
        profilePrefix() + QString::number(QCoreApplication::applicationPid()) + QLatin1Char('-')
        + QString::number(QRandomGenerator::global()->generate64(), 16));
    if (!QDir().mkpath(n.engine.profile)) {
        n.engine.profile.clear();
        fail(QStringLiteral("failed"), QStringLiteral("The sign-in window could not start."), true);
        return;
    }
    n.engine.window = createHostWindow(m_owner, &n);
    if (!n.engine.window) {
        removeProfile(n.engine.profile, 0);
        n.engine.profile.clear();
        fail(QStringLiteral("failed"), QStringLiteral("The sign-in window could not start."), true);
        return;
    }
    setState(QStringLiteral("opening"));
    qInfo("signin: opening Google's sign-in page");

    const std::weak_ptr<char> life = n.alive;
    auto *onEnvironment = new Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler, HRESULT,
                                       ICoreWebView2Environment *>(
        IID_ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler,
        [this, life, run](HRESULT result, ICoreWebView2Environment *environment) -> HRESULT {
            if (life.expired() || !m_native->current(life, run))
                return S_OK;
            Native &n = *m_native;
            if (FAILED(result) || !environment) {
                fail(QStringLiteral("failed"), QStringLiteral("The sign-in window could not start."), true);
                return S_OK;
            }
            environment->AddRef();
            n.engine.environment = environment;
            auto *onController = new Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler, HRESULT,
                                              ICoreWebView2Controller *>(
                IID_ICoreWebView2CreateCoreWebView2ControllerCompletedHandler,
                [this, life, run](HRESULT made, ICoreWebView2Controller *controller) -> HRESULT {
                    if (life.expired() || !m_native->current(life, run))
                        return S_OK;
                    if (FAILED(made) || !controller) {
                        fail(QStringLiteral("failed"), QStringLiteral("The sign-in window could not start."), true);
                        return S_OK;
                    }
                    m_native->attach(run, controller);
                    return S_OK;
                });
            // InPrivate, as a private window in a browser is: Google's and
            // YouTube's cookies live in memory and are never written to the
            // profile folder, however the app ends. No ordinary window
            // instead, where InPrivate cannot be had.
            HRESULT asked = E_NOINTERFACE;
            ICoreWebView2Environment10 *environment10 = nullptr;
            ICoreWebView2ControllerOptions *options = nullptr;
            if (SUCCEEDED(environment->QueryInterface(IID_ICoreWebView2Environment10,
                                                      reinterpret_cast<void **>(&environment10)))
                && environment10 && SUCCEEDED(environment10->CreateCoreWebView2ControllerOptions(&options))
                && options && SUCCEEDED(options->put_IsInPrivateModeEnabled(TRUE)))
                asked = environment10->CreateCoreWebView2ControllerWithOptions(n.engine.window, options, onController);
            if (options)
                options->Release();
            if (environment10)
                environment10->Release();
            if (FAILED(asked))
                fail(QStringLiteral("failed"), QStringLiteral("The sign-in window could not start."), true);
            onController->Release();
            return S_OK;
        });
    // The engine as it comes: no options, and none of the variables that
    // would add switches to it or move its profile.
    clearEngineVariables();
    const std::wstring folder = QDir::toNativeSeparators(n.engine.profile).toStdWString();
    if (FAILED(n.createEnvironment(nullptr, folder.c_str(), nullptr, onEnvironment)))
        fail(QStringLiteral("failed"), QStringLiteral("The sign-in window could not start."), true);
    onEnvironment->Release();
}

void GoogleSignIn::cancel()
{
    if (!busy())
        return;
    qInfo("signin: closed before signing in");
    m_native->retire();
    setState(QStringLiteral("idle"));
}

// Earlier windows' profiles, but none an engine still holds: Chromium keeps
// its lockfile open while it runs, so one that cannot be removed belongs to
// a window still open (another Monolist's).
void GoogleSignIn::removeLeftovers()
{
    const QDir temp(QDir::tempPath());
    const QStringList found = temp.entryList({ profilePrefix() + QLatin1Char('*') }, QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &name : found) {
        QDir profile(temp.filePath(name));
        // "<pid>-<random>": another Monolist still running keeps its own.
        const QString rest = name.mid(profilePrefix().size());
        const int dash = rest.indexOf(QLatin1Char('-'));
        bool numeric = false;
        const DWORD pid = dash > 0 ? rest.left(dash).toULong(&numeric) : 0;
        if (numeric && pid != DWORD(QCoreApplication::applicationPid()) && processRunning(pid))
            continue;
        const QString lock = profile.filePath(QStringLiteral("EBWebView/lockfile"));
        if (QFileInfo::exists(lock) && !QFile::remove(lock))
            continue;
        profile.removeRecursively();
    }
}

void GoogleSignIn::setState(const QString &state)
{
    if (state == m_state)
        return;
    m_state = state;
    Q_EMIT stateChanged();
}

void GoogleSignIn::finish(const QByteArray &cookies)
{
    m_native->retire();
    setState(QStringLiteral("reading"));
    const bool taken = m_session && m_session->importSignIn(cookies);
    if (!taken) {
        const QString why = m_session ? m_session->importError() : QString();
        fail(QStringLiteral("failed"),
             why.isEmpty() ? QStringLiteral("The sign-in could not be kept.") : why, true);
        return;
    }
    // Signed in: an old refusal no longer stands.
    setSetting(kRefusedAtKey, QString());
    qInfo("signin: the window's session was handed to the account");
    setState(QStringLiteral("done"));
    Q_EMIT notice(QStringLiteral("Signed in. Checking with YouTube Music…"));
}

void GoogleSignIn::fail(const QString &state, const QString &text, bool fallBack)
{
    m_native->retire();
    setState(state);
    if (!text.isEmpty())
        Q_EMIT notice(text);
    if (fallBack)
        Q_EMIT fallbackRequested();
}
