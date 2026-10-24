#include "googlesignin.h"

#include "appdatabase.h"
#include "cookieimport.h"
#include "ytmsession.h"

#include <QDateTime>
#include <QDesktopServices>
#include <QGuiApplication>
#include <QLocale>
#include <QMap>
#include <QRegularExpression>
#include <QSqlQuery>
#include <QUrl>
#include <QWindow>

#import <AppKit/AppKit.h>
#import <WebKit/WebKit.h>

// The Mac's sign-in window: Google's page in WebKit, the engine every Mac
// carries, under the rules googlesignin.h sets out for Windows' WebView2.
// The engine as it comes: its own user agent (no customUserAgent, no
// applicationNameForUserAgent), no script added to any page, no request
// changed, the Web Inspector off. Its website data lives in memory alone
// (a non-persistent data store, new for each sign-in, as a private window in
// Safari is), so nothing of Google's or YouTube's is ever written to disk
// and there is no profile to delete afterwards. Only YouTube's cookies are
// read, once YouTube Music is signed in, and Google's refusal is final.

@interface MonolistSignInDelegate : NSObject <WKNavigationDelegate, WKUIDelegate, NSWindowDelegate>
@property (nonatomic, assign) GoogleSignIn::Native *native;
@property (nonatomic) quint64 run;
@end

namespace {

const QString kWindowTitle = QStringLiteral("Sign in to YouTube Music");

// Where YouTube Music's own Sign in button goes, as googlesignin_win.cpp's:
// Google's page, then YouTube's sign-in handler, which sets the session on
// youtube.com, then YouTube Music. Nothing added to it.
const char kSignInUrl[] =
    "https://accounts.google.com/ServiceLogin?ltmpl=music&service=youtube&uilel=3&passive=true"
    "&continue=https%3A%2F%2Fwww.youtube.com%2Fsignin%3Faction_handle_signin%3Dtrue%26app%3Ddesktop"
    "%26hl%3Den%26next%3Dhttps%253A%252F%252Fmusic.youtube.com%252F&hl=en";

// Google refused to sign in here: when, so the window is not offered again
// for a while, and the import is offered instead. The same setting as on
// Windows: a library is one computer's.
const QString kRefusedAtKey = QStringLiteral("signin.refused_at");
constexpr qint64 kRefusalRestDays = 7;

bool isYouTube(const QString &domain)
{
    const QString host = domain.startsWith(QLatin1Char('.')) ? domain.mid(1).toLower() : domain.toLower();
    return host == QLatin1String("youtube.com") || host.endsWith(QLatin1String(".youtube.com"));
}

// Where the window may go: Google's sign-in and account pages (in any of
// Google's country domains), and YouTube's, over https.
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

// YouTube's cookies of the window's store, as cookies.txt text; Google's own
// are never read out. No value is ever logged.
QByteArray youTubeCookies(NSArray<NSHTTPCookie *> *cookies)
{
    QMap<QString, QByteArray> lines;
    for (NSHTTPCookie *cookie in cookies) {
        const QString domain = QString::fromNSString(cookie.domain);
        const QString name = QString::fromNSString(cookie.name);
        if (name.isEmpty() || !isYouTube(domain))
            continue;
        QString path = QString::fromNSString(cookie.path);
        if (path.isEmpty())
            path = QStringLiteral("/");
        QByteArray line;
        if (cookie.HTTPOnly)
            line += "#HttpOnly_";
        line += domain.toUtf8();
        line += domain.startsWith(QLatin1Char('.')) ? "\tTRUE\t" : "\tFALSE\t";
        line += path.toUtf8();
        line += cookie.secure ? "\tTRUE\t" : "\tFALSE\t";
        line += QByteArray::number(cookie.sessionOnly || !cookie.expiresDate
                                       ? qint64(0)
                                       : qint64(cookie.expiresDate.timeIntervalSince1970));
        line += '\t';
        line += name.toUtf8();
        line += '\t';
        line += QByteArray(cookie.value.UTF8String);
        line += '\n';
        lines.insert(name + QLatin1Char('\t') + domain + QLatin1Char('\t') + path, line);
    }
    QByteArray text = "# Netscape HTTP Cookie File\n";
    for (QByteArray &line : lines) {
        text += line;
        line.fill('\0');
    }
    return text;
}

} // namespace

struct GoogleSignIn::Native
{
    GoogleSignIn *q = nullptr;
    NSWindow *window = nil;
    WKWebView *web = nil;
    MonolistSignInDelegate *delegate = nil;
    // Each window is a run: what an earlier one's engine says late is not
    // this one's.
    quint64 run = 0;
    bool reading = false;

    bool current(quint64 forRun) const { return forRun == run && web; }

    // The window's title says whose page it is showing, as an address bar
    // would.
    void showAddress(const QUrl &url)
    {
        const QString host = url.host().toLower();
        window.title = (host.isEmpty() ? kWindowTitle : kWindowTitle + QStringLiteral(" — ") + host).toNSString();
    }

    // Each page the window lands on: Google's refusal ends it; YouTube's own
    // pages are where a finished sign-in arrives, so its cookies are read
    // there.
    void landed(quint64 forRun)
    {
        if (!current(forRun))
            return;
        const QUrl url = QUrl::fromNSURL(web.URL);
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

    // YouTube's cookies, once they are a session the import itself would
    // take.
    void readCookies(quint64 forRun)
    {
        if (reading || !web)
            return;
        reading = true;
        WKHTTPCookieStore *store = web.configuration.websiteDataStore.httpCookieStore;
        [store getAllCookies:^(NSArray<NSHTTPCookie *> *cookies) {
            if (!current(forRun))
                return;
            reading = false;
            QByteArray text = youTubeCookies(cookies);
            // Not yet: Google's pages, or YouTube Music still signed out.
            if (CookieImport::parse(text).ok())
                q->finish(text);
            text.fill('\0');
        }];
    }

    // What the engine says that ends a window: GoogleSignIn's own calls,
    // which only this, its friend, may make.
    void stopped()
    {
        q->fail(QStringLiteral("failed"), QStringLiteral("The sign-in window stopped."), true);
    }
    void closedByUser() { q->cancel(); }

    // The window closed and its engine let go of, with all it held: the
    // store was in memory only, so nothing is left anywhere.
    void retire()
    {
        ++run;
        reading = false;
        WKWebView *leavingWeb = web;
        NSWindow *leavingWindow = window;
        MonolistSignInDelegate *leavingDelegate = delegate;
        web = nil;
        window = nil;
        delegate = nil;
        leavingDelegate.native = nullptr;
        if (leavingWeb) {
            [leavingWeb removeObserver:leavingDelegate forKeyPath:@"URL"];
            [leavingWeb stopLoading];
            leavingWeb.navigationDelegate = nil;
            leavingWeb.UIDelegate = nil;
        }
        if (leavingWindow) {
            leavingWindow.delegate = nil;
            [leavingWindow orderOut:nil];
            // Closed after this turn of the event loop: never from inside
            // one of the engine's own callbacks.
            dispatch_async(dispatch_get_main_queue(), ^{
                [leavingWindow close];
                (void)leavingWeb;
                (void)leavingDelegate;
            });
        }
    }
};

@implementation MonolistSignInDelegate

// Only Google's and YouTube's pages, over https, in the window itself; a
// link anywhere else goes to the user's own browser when they clicked it,
// and is dropped when they did not. Frames inside a page are the page's.
- (void)webView:(WKWebView *)webView
    decidePolicyForNavigationAction:(WKNavigationAction *)action
                    decisionHandler:(void (^)(WKNavigationActionPolicy))decide
{
    (void)webView;
    const QUrl url = QUrl::fromNSURL(action.request.URL);
    if (!action.targetFrame.isMainFrame || allowedPage(url)
        || url.toString() == QLatin1String("about:blank")) {
        decide(WKNavigationActionPolicyAllow);
        return;
    }
    decide(WKNavigationActionPolicyCancel);
    if (action.navigationType == WKNavigationTypeLinkActivated && url.scheme() == QLatin1String("https"))
        QDesktopServices::openUrl(url);
}

- (void)webView:(WKWebView *)webView didFinishNavigation:(WKNavigation *)navigation
{
    (void)webView;
    (void)navigation;
    if (self.native)
        self.native->landed(self.run);
}

// A page that changes its address without loading another (Google's
// sign-in moves between its steps so) is looked at too.
- (void)observeValueForKeyPath:(NSString *)keyPath
                      ofObject:(id)object
                        change:(NSDictionary<NSKeyValueChangeKey, id> *)change
                       context:(void *)context
{
    (void)keyPath;
    (void)object;
    (void)change;
    (void)context;
    if (self.native)
        self.native->landed(self.run);
}

// A page asking for a window of its own (a help link) gets the user's own
// browser instead, if they clicked it: this window is for signing in.
- (WKWebView *)webView:(WKWebView *)webView
    createWebViewWithConfiguration:(WKWebViewConfiguration *)configuration
               forNavigationAction:(WKNavigationAction *)action
                    windowFeatures:(WKWindowFeatures *)features
{
    (void)webView;
    (void)configuration;
    (void)features;
    const QUrl url = QUrl::fromNSURL(action.request.URL);
    if (action.navigationType == WKNavigationTypeLinkActivated && url.scheme() == QLatin1String("https"))
        QDesktopServices::openUrl(url);
    return nil;
}

- (void)webViewWebContentProcessDidTerminate:(WKWebView *)webView
{
    (void)webView;
    if (self.native)
        self.native->stopped();
}

// Its own close button: nothing kept.
- (void)windowWillClose:(NSNotification *)note
{
    (void)note;
    if (self.native)
        self.native->closedByUser();
}

@end

// ------------------------------------------------------------------ public

GoogleSignIn::GoogleSignIn(YtmSession *session, QObject *parent)
    : QObject(parent), m_native(std::make_unique<Native>()), m_session(session)
{
    m_native->q = this;
    const qint64 refusedAt = setting(kRefusedAtKey).toLongLong();
    if (refusedAt > 0 && QDateTime::currentSecsSinceEpoch() - refusedAt < kRefusalRestDays * 24 * 3600)
        m_refusedAt = refusedAt;
}

GoogleSignIn::~GoogleSignIn()
{
    m_native->retire();
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

void GoogleSignIn::setOwner(QWindow *window)
{
    m_owner = window;
}

// WebKit is every Mac's; on Qt's Cocoa platform only, where there are
// AppKit windows to show it in.
bool GoogleSignIn::available() const
{
    return QGuiApplication::platformName() == QLatin1String("cocoa");
}

void GoogleSignIn::start()
{
    Native &n = *m_native;
    if (!available()) {
        fail(QStringLiteral("failed"), QString(), true);
        return;
    }
    if (busy() && n.window) {
        [n.window makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
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
    // A store of its own, in memory: nothing shared with another window,
    // with Safari, or with the next sign-in.
    WKWebViewConfiguration *configuration = [WKWebViewConfiguration new];
    configuration.websiteDataStore = [WKWebsiteDataStore nonPersistentDataStore];

    // 480 by 700, over the middle of the app's window, or of the screen.
    const NSSize size = NSMakeSize(480, 700);
    NSRect area = NSScreen.mainScreen.visibleFrame;
    if (m_owner && m_owner->isVisible()) {
        const QRect frame = m_owner->frameGeometry();
        const CGFloat screenTop = NSMaxY(NSScreen.screens.firstObject.frame);
        area = NSMakeRect(frame.x(), screenTop - frame.y() - frame.height(), frame.width(), frame.height());
    }
    const NSRect rect = NSMakeRect(NSMidX(area) - size.width / 2, NSMidY(area) - size.height / 2, size.width,
                                   size.height);
    NSWindow *window = [[NSWindow alloc]
        initWithContentRect:rect
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    window.releasedWhenClosed = NO;
    window.title = kWindowTitle.toNSString();

    WKWebView *web = [[WKWebView alloc] initWithFrame:window.contentView.bounds configuration:configuration];
    web.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    if (@available(macOS 13.3, *))
        web.inspectable = NO;

    MonolistSignInDelegate *delegate = [MonolistSignInDelegate new];
    delegate.native = &n;
    delegate.run = run;
    web.navigationDelegate = delegate;
    web.UIDelegate = delegate;
    window.delegate = delegate;
    [web addObserver:delegate forKeyPath:@"URL" options:NSKeyValueObservingOptionNew context:nullptr];
    [window.contentView addSubview:web];

    n.window = window;
    n.web = web;
    n.delegate = delegate;

    setState(QStringLiteral("opening"));
    qInfo("signin: opening Google's sign-in page");
    [web loadRequest:[NSURLRequest requestWithURL:[NSURL URLWithString:@(kSignInUrl)]]];
    [window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    setState(QStringLiteral("open"));
}

void GoogleSignIn::cancel()
{
    if (!busy())
        return;
    qInfo("signin: closed before signing in");
    m_native->retire();
    setState(QStringLiteral("idle"));
}

// Nothing to clear: the window's store was in memory only.
void GoogleSignIn::removeLeftovers()
{
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
