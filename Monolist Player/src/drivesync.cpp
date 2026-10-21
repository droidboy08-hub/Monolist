#include "drivesync.h"

#include "appdatabase.h"
#include "library.h"
#include "recommender.h"
#include "windowchrome.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QDate>
#include <QFileDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QNetworkAccessManager>
#include <QNetworkInformation>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTimeZone>
#include <QTimer>
#include <QVariant>
#include <QWindow>

#include <algorithm>
#include <utility>

const QString DriveSync::kFileName = QStringLiteral("monolist-library.json");

namespace {

const QString kDrive = QStringLiteral("https://www.googleapis.com");
// What the last sync left, kept between launches. Never a token, and nothing
// of the YouTube Music sign-in's.
const QString kFileIdKey = QStringLiteral("backup.file_id");
const QString kRemoteKey = QStringLiteral("backup.remote_modified");
const QString kHashKey = QStringLiteral("backup.last_hash");
const QString kSyncedKey = QStringLiteral("backup.last_synced_at");
const QString kHeldKey = QStringLiteral("backup.held");
// A backoff that Drive asks to be longer is never longer than this.
constexpr qint64 kMaxRetryAfterMs = 60 * 60 * 1000;

QString countOf(qsizetype count, const char *one, const char *many)
{
    return QStringLiteral("%1 %2").arg(QLocale().toString(qint64(count)),
                                       QString::fromUtf8(count == 1 ? one : many));
}

// What a sync brought in, for the toast after signing in or SYNC NOW;
// empty when it brought nothing anyone would see. What was taken out is
// named as well as what was added (DESIGN.md), History's rows among them,
// and a playlist that only changed is not called a new one.
QString broughtIn(const LibraryBackup::Counts &counts)
{
    QStringList added;
    if (counts.playlistsAdded > 0)
        added << countOf(counts.playlistsAdded, "playlist", "playlists");
    if (counts.likesAdded > 0)
        added << countOf(counts.likesAdded, "liked song", "liked songs");
    if (counts.savedAdded > 0)
        added << countOf(counts.savedAdded, "saved album or playlist", "saved albums and playlists");
    if (counts.playsAdded > 0)
        added << countOf(counts.playsAdded, "play", "plays");
    QStringList removed;
    if (counts.playlistsRemoved > 0)
        removed << countOf(counts.playlistsRemoved, "playlist", "playlists");
    if (counts.likesRemoved > 0)
        removed << countOf(counts.likesRemoved, "liked song", "liked songs");
    if (counts.savedRemoved > 0)
        removed << countOf(counts.savedRemoved, "saved album or playlist", "saved albums and playlists");
    if (counts.historyRemoved > 0)
        removed << countOf(counts.historyRemoved, "song from History", "songs from History");
    QStringList parts;
    if (!added.isEmpty())
        parts << added.join(QStringLiteral(", ")) + QStringLiteral(" added");
    if (counts.playlistsUpdated > 0)
        parts << countOf(counts.playlistsUpdated, "playlist", "playlists") + QStringLiteral(" changed");
    if (!removed.isEmpty())
        parts << removed.join(QStringLiteral(", ")) + QStringLiteral(" removed, as they were deleted on another computer");
    return parts.isEmpty() ? QString() : QStringLiteral("From your Google Drive: ") + parts.join(QStringLiteral("; "));
}

// An HTTP Date ("Tue, 20 Oct 2026 10:00:00 GMT", RFC 7231's form, always
// in GMT), as UTC; invalid for anything else.
QDateTime httpDate(const QByteArray &value)
{
    QString text = QString::fromLatin1(value).trimmed();
    if (!text.endsWith(QLatin1String(" GMT")))
        return {};
    text.chop(4);
    QDateTime when = QLocale::c().toDateTime(text, QStringLiteral("ddd, dd MMM yyyy HH:mm:ss"));
    if (!when.isValid())
        return {};
    when.setTimeZone(QTimeZone(QTimeZone::UTC));
    return when;
}

void addCounts(LibraryBackup::Counts &into, const LibraryBackup::Counts &from)
{
    into.likesAdded += from.likesAdded;
    into.likesRemoved += from.likesRemoved;
    into.playlistsAdded += from.playlistsAdded;
    into.playlistsUpdated += from.playlistsUpdated;
    into.playlistsRemoved += from.playlistsRemoved;
    into.savedAdded += from.savedAdded;
    into.savedRemoved += from.savedRemoved;
    into.playsAdded += from.playsAdded;
    into.eventsAdded += from.eventsAdded;
    into.playsUpdated += from.playsUpdated;
    into.turnDownsRemoved += from.turnDownsRemoved;
    into.detailsUpdated += from.detailsUpdated;
    into.tombstonesAdded += from.tombstonesAdded;
    into.historyRemoved += from.historyRemoved;
    into.historyAdded += from.historyAdded;
}

// Drive's file ids are letters, digits, '-' and '_'; anything else in an
// answer is not put into an address.
bool plainId(const QString &id)
{
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9_-]{1,256}$"));
    return pattern.match(id).hasMatch();
}

QString userAgent()
{
    return QStringLiteral("Monolist/%1 (+https://github.com/droidboy08-hub/Monolist)")
        .arg(QCoreApplication::applicationVersion().isEmpty() ? QStringLiteral("0.1")
                                                              : QCoreApplication::applicationVersion());
}

const char *problemName(DriveSync::Problem problem)
{
    switch (problem) {
    case DriveSync::Problem::None:       return "none";
    case DriveSync::Problem::Offline:    return "no answer";
    case DriveSync::Problem::Slowed:     return "asked to slow down";
    case DriveSync::Problem::Server:     return "a server error";
    case DriveSync::Problem::Full:       return "the Drive is full";
    case DriveSync::Problem::Refused:    return "the client refused";
    case DriveSync::Problem::Odd:        return "an answer not followed";
    case DriveSync::Problem::Database:   return "the merge refused";
    case DriveSync::Problem::Unreadable: return "the file cannot be read";
    case DriveSync::Problem::Newer:      return "the file is from a newer Monolist";
    case DriveSync::Problem::Clock:      return "this computer's clock is wrong";
    }
    return "?";
}

} // namespace

// One sync, from finding the file to the upload.
struct DriveSync::Run {
    quint64 serial = 0;
    bool forced = false;     // SYNC NOW, or right after signing in
    bool periodic = false;   // the half-hourly one is owed: it takes the plays waiting
    bool refound = false;    // a file vanished under it once, and was looked for again
    QList<Remote> files;     // as found, the one kept first
    int next = 0;            // the file being read
    bool keptRead = false;
    QByteArray keptHash;     // what the kept file held, as LibraryBackup hashes it
    bool snapshotted = false; // the library was read and written out (or found the same)
    QByteArray hash;         // what the library held when it was written out
    quint64 editSerial = 0;  // the edits it holds
    QString fileId;          // the kept file, after the sync
    QString modified;
    int removing = 0;        // the extra copies deleted so far
};

DriveSync::DriveSync(Library *library, QObject *parent)
    : QObject(parent)
    , m_library(library)
    , m_backup(library)
    , m_auth(new DriveAuth(library, this))
    , m_network(new QNetworkAccessManager(this))
{
    m_clock.start();
    m_syncTimer.setSingleShot(true);
    m_syncTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_syncTimer, &QTimer::timeout, this, [this]() { startRun(/*forced=*/false); });
    m_editTimer.setSingleShot(true);
    connect(&m_editTimer, &QTimer::timeout, this, [this]() {
        m_firstEditMs = -1;
        requestSync();
    });
    connect(&m_periodTimer, &QTimer::timeout, this, [this]() {
        m_periodDue = true;
        requestSync();
    });
    connect(&m_tickTimer, &QTimer::timeout, this, [this]() {
        if (signedIn() && m_lastSyncedAt.isValid())
            Q_EMIT stateChanged();
    });
    connect(m_auth, &DriveAuth::stateChanged, this, &DriveSync::authChanged);
    connect(m_auth, &DriveAuth::signedIn, this, &DriveSync::signedInNow);
    // Back from the browser: Monolist's window in front again, the full one,
    // as the tray's Open Monolist brings it (Main.qml's bringBack). The
    // browser has the foreground then, and Windows would only flash the
    // taskbar button for a window that asks for it from behind, so it is
    // taken (WindowChrome::takeForeground), once QML has shown the window.
    connect(m_auth, &DriveAuth::returned, this, [this]() {
        if (!m_owner)
            return;
        QMetaObject::invokeMethod(m_owner.data(), "bringBack", Q_ARG(QVariant, true));
        QTimer::singleShot(0, this, [this]() {
            if (m_owner)
                WindowChrome::takeForeground(m_owner);
        });
    });
}

DriveSync::~DriveSync()
{
    stopRun();
}

void DriveSync::setRecommender(Recommender *recommender)
{
    m_backup.setRecommender(recommender);
}

void DriveSync::setOwner(QWindow *window)
{
    m_owner = window;
}

void DriveSync::setFilePicker(FilePicker picker)
{
    m_picker = std::move(picker);
}

void DriveSync::setTiming(const Timing &timing)
{
    m_timing = timing;
    if (m_timing.backoffMs.isEmpty())
        m_timing.backoffMs = Timing().backoffMs;
    // A self-test shortening the half hour while signed in: the timer that
    // runs takes the new length now.
    if (m_periodTimer.isActive())
        m_periodTimer.start(m_timing.periodMs);
}

void DriveSync::setTestDrive(const QUrl &base)
{
    m_testDrive = base;
}

QUrl DriveSync::driveBase() const
{
    return m_auth->testing() && DriveAuth::isLoopback(m_testDrive) ? m_testDrive : QUrl(kDrive);
}

// The token goes to Drive and nowhere else: www.googleapis.com over https,
// or a test client's stand-in on this computer, never anything a redirect
// or an answer names.
bool DriveSync::mayCarryToken(const QUrl &url) const
{
    if (url.scheme() == QLatin1String("https") && url.host() == QLatin1String("www.googleapis.com")
        && url.port(443) == 443)
        return true;
    if (!m_auth->testing() || !DriveAuth::isLoopback(m_testDrive))
        return false;
    return url.scheme() == m_testDrive.scheme() && url.host() == m_testDrive.host()
           && url.port(-1) == m_testDrive.port(-1);
}

QString DriveSync::setting(const QString &key) const
{
    return m_library ? m_library->settingValue(key) : QString();
}

void DriveSync::setSetting(const QString &key, const QString &value)
{
    if (m_library)
        m_library->setSetting(key, value);
}

void DriveSync::forgetSyncState()
{
    QSqlQuery remove(AppDatabase::connection());
    remove.prepare(QStringLiteral("DELETE FROM settings WHERE key IN (?, ?, ?, ?, ?)"));
    for (const QString &key : { kFileIdKey, kRemoteKey, kHashKey, kSyncedKey, kHeldKey })
        remove.addBindValue(key);
    remove.exec();
    m_lastSyncedAt = QDateTime();
    m_problem = Problem::None;
    m_failures = 0;
    m_retryAt = QDateTime();
    m_notBeforeMs = 0;
    // Another account's file has never had this library: the next sync
    // reads all of it.
    m_syncedThisLaunch = false;
    m_clockSkewSecs = 0;
}

// ------------------------------------------------------------------ state

void DriveSync::start()
{
    if (m_started)
        return;
    m_started = true;
    if (m_demo)
        return;
    m_auth->start();
    m_lastSyncedAt = QDateTime::fromString(setting(kSyncedKey), Qt::ISODate);
    const QString heldAs = setting(kHeldKey);
    if (heldAs == QLatin1String("newer"))
        m_problem = Problem::Newer;
    else if (heldAs == QLatin1String("unreadable"))
        m_problem = Problem::Unreadable;
    if (signedIn()) {
        m_periodTimer.start(m_timing.periodMs);
        m_tickTimer.start(m_timing.tickMs);
        QTimer::singleShot(m_timing.launchMs, this, &DriveSync::requestSync);
        if (held())
            qInfo("backup: syncing waits for SYNC NOW: %s", problemName(m_problem));
    }
    // A self-test's network is its stand-ins, and it has no window; the
    // system's comings and goings would only make its timings uncertain.
    if (!m_auth->testing()) {
        watchNetwork();
        watchWindow();
    }
    Q_EMIT stateChanged();
}

// Monolist left in the tray for an hour, then brought to the front: the
// other computer may have changed a playlist since, and an edit made here
// before the next sync would be the newer copy, its name, pin and order
// winning over that change. Coming to the front syncs first, when the last
// sync is a few minutes old; one with nothing new is a list of the files.
void DriveSync::watchWindow()
{
    if (m_watchingWindow)
        return;
    m_watchingWindow = true;
    auto *app = qobject_cast<QGuiApplication *>(QCoreApplication::instance());
    if (!app)
        return;
    connect(app, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
        if (state != Qt::ApplicationActive || m_run)
            return;
        // Not before the launch's own sync (15 s in), which the window's
        // first coming to the front would otherwise bring forward.
        if (m_lastStartMs < 0 || m_clock.elapsed() - m_lastStartMs < m_timing.staleMs)
            return;
        requestSync();
    });
}

// A lost connection is the ordinary reason a sync backs off. When the system
// says it is back, the sync goes then rather than at the end of the wait.
void DriveSync::watchNetwork()
{
    if (m_watchingNetwork)
        return;
    m_watchingNetwork = true;
    if (!QNetworkInformation::loadBackendByFeatures(QNetworkInformation::Feature::Reachability))
        return;
    connect(QNetworkInformation::instance(), &QNetworkInformation::reachabilityChanged, this,
            [this](QNetworkInformation::Reachability reachability) {
                if (reachability != QNetworkInformation::Reachability::Online)
                    return;
                if (m_problem != Problem::Offline && m_problem != Problem::Server)
                    return;
                m_notBeforeMs = 0;
                m_syncTimer.stop();
                requestSync();
            });
}

void DriveSync::authChanged()
{
    if (!signedIn()) {
        stopRun();
        m_syncTimer.stop();
        m_editTimer.stop();
        m_periodTimer.stop();
        m_tickTimer.stop();
    }
    if (m_auth->state() == DriveAuth::State::Waiting)
        m_justSignedOut = false;
    Q_EMIT stateChanged();
}

void DriveSync::signedInNow()
{
    // Another sign-in may be another account, with its own file: nothing
    // the last one's syncs left is taken for this one's.
    forgetSyncState();
    m_justSignedOut = false;
    m_firstAfterSignIn = true;
    m_forcedPending = false;
    m_pendingCounts = LibraryBackup::Counts();
    m_periodTimer.start(m_timing.periodMs);
    m_tickTimer.start(m_timing.tickMs);
    startRun(/*forced=*/true);
}

QString DriveSync::state() const
{
    switch (m_auth->state()) {
    case DriveAuth::State::Unavailable: return QStringLiteral("unavailable");
    case DriveAuth::State::SignedOut:   return QStringLiteral("off");
    case DriveAuth::State::Waiting:
    case DriveAuth::State::Finishing:   return QStringLiteral("waiting");
    case DriveAuth::State::SignedIn:    return QStringLiteral("connected");
    case DriveAuth::State::Expired:     return QStringLiteral("expired");
    case DriveAuth::State::Failed:      return QStringLiteral("error");
    }
    return QStringLiteral("off");
}

QString DriveSync::email() const
{
    const QString email = m_auth->email();
    if (email.isEmpty() && signedIn())
        return QStringLiteral("your Google account");
    return email;
}

bool DriveSync::busy() const
{
    return m_demoBusy || m_run || m_auth->state() == DriveAuth::State::Finishing;
}

QString DriveSync::retryText() const
{
    const QDateTime at = m_retryAt.toLocalTime();
    const QString time = QLocale::system().toString(at.time(), QLocale::ShortFormat);
    if (at.date() == QDate::currentDate())
        return time;
    return QLocale::system().toString(at.date(), QLocale::ShortFormat) + QLatin1Char(' ') + time;
}

QString DriveSync::error() const
{
    switch (m_auth->state()) {
    case DriveAuth::State::Expired:
    case DriveAuth::State::Failed:
        return m_auth->why();
    case DriveAuth::State::SignedIn:
        break;
    default:
        return QString();
    }
    const QString when = retryText();
    switch (m_problem) {
    case Problem::None:
        return QString();
    case Problem::Offline:
        return QStringLiteral("Couldn't reach Google Drive. Trying again at %1").arg(when);
    case Problem::Slowed:
        return QStringLiteral("Google Drive asked Monolist to slow down. Trying again at %1").arg(when);
    case Problem::Server:
        return QStringLiteral("Google Drive had a problem. Trying again at %1").arg(when);
    case Problem::Full:
        return QStringLiteral("Your Google Drive is full, so the backup couldn't be saved. Trying again at %1").arg(when);
    case Problem::Refused:
        return QStringLiteral("Google refused this copy of Monolist's sign-in. Trying again at %1").arg(when);
    case Problem::Odd:
        return QStringLiteral("Google Drive answered in a way Monolist doesn't follow. Trying again at %1").arg(when);
    case Problem::Database:
        return QStringLiteral("Monolist couldn't merge the backup into your library, so nothing was changed. "
                              "Trying again at %1").arg(when);
    case Problem::Unreadable:
        // What to do comes with it: nothing syncs until the user asks, and
        // asking again reads the same file, so what ends it is said too.
        return QStringLiteral("The backup in your Google Drive could not be read; it was left as it is. SYNC NOW "
                              "tries it again. To start a new backup from this computer instead, delete the old "
                              "one (open Drive's Settings, then Manage apps, Monolist, Delete hidden app data), "
                              "then press SYNC NOW");
    case Problem::Newer:
        return QStringLiteral("The backup in your Google Drive was saved by a newer Monolist; it was left as it "
                              "is. Update Monolist to sync again");
    case Problem::Clock:
        return QStringLiteral("This computer's clock is %1, so syncing has stopped until it is set right. Trying "
                              "again at %2").arg(skewText(), when);
    }
    return QString();
}

QString DriveSync::skewText() const
{
    const qint64 seconds = std::abs(m_clockSkewSecs);
    QString amount;
    if (seconds < 90 * 60) {
        const qint64 minutes = std::max<qint64>(1, (seconds + 30) / 60);
        amount = countOf(minutes, "minute", "minutes");
    } else if (seconds < 36 * 3600) {
        amount = countOf((seconds + 1800) / 3600, "hour", "hours");
    } else {
        amount = countOf((seconds + 43200) / 86400, "day", "days");
    }
    return amount + (m_clockSkewSecs > 0 ? QStringLiteral(" fast") : QStringLiteral(" slow"));
}

// Every line here may be shown as styled text: what came from outside is
// escaped where it is put in (DriveAuth's sentences already are).
QString DriveSync::status() const
{
    const QString signedOut = QStringLiteral("Signed out. Your backup is still in your Google Drive; to delete it, "
                                             "open Drive's Settings, then Manage apps, Monolist, Delete hidden app "
                                             "data");
    switch (m_auth->state()) {
    case DriveAuth::State::Unavailable:
        if (storedSignIn()) {
            return unavailableReason()
                   + QStringLiteral(", so it can't sync your backup. The sign-in an earlier copy made is still kept "
                                    "here: SIGN OUT withdraws it");
        }
        return m_justSignedOut ? signedOut : unavailableReason();
    case DriveAuth::State::SignedOut:
        return m_justSignedOut ? signedOut : QString();
    case DriveAuth::State::Waiting:
        if (m_auth->browserOpened())
            return QStringLiteral("Waiting for you in the browser");
        // The link is not offered as one: it would open by the same means
        // that just failed. COPY LINK puts it on the clipboard instead.
        return QStringLiteral("Monolist couldn't open your browser. Press COPY LINK, then paste it into your "
                              "browser's address bar and sign in there");
    case DriveAuth::State::Finishing:
        return QStringLiteral("Finishing signing in…");
    case DriveAuth::State::Expired:
    case DriveAuth::State::Failed:
        return m_auth->why();
    case DriveAuth::State::SignedIn:
        break;
    }
    if (busy())
        return QStringLiteral("Syncing…");
    if (m_problem != Problem::None)
        return error();
    if (!m_lastSyncedAt.isValid())
        return QStringLiteral("Not synced yet");

    const qint64 seconds = std::max<qint64>(0, m_lastSyncedAt.secsTo(QDateTime::currentDateTimeUtc()));
    if (seconds < 60)
        return QStringLiteral("Synced just now");
    if (seconds < 60 * 60) {
        const qint64 minutes = seconds / 60;
        return minutes == 1 ? QStringLiteral("Synced a minute ago")
                            : QStringLiteral("Synced %1 minutes ago").arg(minutes);
    }
    const QDateTime at = m_lastSyncedAt.toLocalTime();
    const QString time = QLocale::system().toString(at.time(), QLocale::ShortFormat);
    if (at.date() == QDate::currentDate())
        return QStringLiteral("Synced today at %1").arg(time);
    if (at.date() == QDate::currentDate().addDays(-1))
        return QStringLiteral("Synced yesterday at %1").arg(time);
    return QStringLiteral("Synced %1 at %2").arg(QLocale::system().toString(at.date(), QLocale::ShortFormat), time);
}

bool DriveSync::showDemo(const QString &name)
{
    using State = DriveAuth::State;
    const QString email = QStringLiteral("your.name@gmail.com");
    const QString ended = QStringLiteral("Your Google sign-in has ended. Sign in again to go on syncing");
    m_problem = Problem::None;
    m_demoBusy = false;
    m_justSignedOut = false;
    m_lastSyncedAt = QDateTime::currentDateTimeUtc().addSecs(-2 * 60);
    m_retryAt = QDateTime::currentDateTimeUtc().addSecs(5 * 60);
    if (name == QLatin1String("off")) {
        m_auth->showDemo(State::SignedOut, QString(), QString());
    } else if (name == QLatin1String("signedout")) {
        m_auth->showDemo(State::SignedOut, QString(), QString());
        m_justSignedOut = true;
    } else if (name == QLatin1String("waiting")) {
        m_auth->showDemo(State::Waiting, QString(), QString());
    } else if (name == QLatin1String("nobrowser")) {
        m_auth->showDemo(State::Waiting, QString(), QString(), true, true, /*opened=*/false);
    } else if (name == QLatin1String("finishing")) {
        m_auth->showDemo(State::Finishing, QString(), QString());
    } else if (name == QLatin1String("synced") || name == QLatin1String("syncing")
               || name == QLatin1String("offline") || name == QLatin1String("slowed")
               || name == QLatin1String("unreadable") || name == QLatin1String("newer")
               || name == QLatin1String("clock")) {
        m_auth->showDemo(State::SignedIn, email, QString());
        m_demoBusy = name == QLatin1String("syncing");
        if (name == QLatin1String("offline"))
            m_problem = Problem::Offline;
        else if (name == QLatin1String("slowed"))
            m_problem = Problem::Slowed;
        else if (name == QLatin1String("unreadable"))
            m_problem = Problem::Unreadable;
        else if (name == QLatin1String("newer"))
            m_problem = Problem::Newer;
        else if (name == QLatin1String("clock")) {
            m_problem = Problem::Clock;
            m_clockSkewSecs = 5 * 3600 + 30 * 60;   // a clock kept in local time, five and a half hours ahead
        }
    } else if (name == QLatin1String("expired")) {
        m_auth->showDemo(State::Expired, email, ended);
    } else if (name == QLatin1String("denied")) {
        m_auth->showDemo(State::Failed, QString(),
                         QStringLiteral("You didn't allow it, so Monolist isn't signed in to Google Drive"));
    } else if (name == QLatin1String("noscope")) {
        m_auth->showDemo(State::Failed, QString(),
                         QStringLiteral("Monolist needs its folder in your Google Drive. Press TRY AGAIN and, on "
                                        "Google's page, tick the box for Monolist's folder before Continue"));
    } else if (name == QLatin1String("nostore")) {
        m_auth->showDemo(State::Unavailable, QString(), QString(), true, /*store=*/false);
    } else if (name == QLatin1String("nobuild")) {
        m_auth->showDemo(State::Unavailable, QString(), QString(), /*client=*/false);
    } else if (name == QLatin1String("nobuildsignedin")) {
        m_auth->showDemo(State::Unavailable, email, QString(), /*client=*/false, true, true, /*stored=*/true);
    } else {
        return false;
    }
    m_demo = true;
    m_started = true;
    qInfo("backup: showing the Google Drive row as \"%s\", with nothing stored or sent", qPrintable(name));
    Q_EMIT stateChanged();
    return true;
}

// -------------------------------------------------------------- the buttons

void DriveSync::signIn()
{
    if (m_demo)
        return;
    m_justSignedOut = false;
    m_auth->signIn();
}

void DriveSync::cancelSignIn()
{
    m_auth->cancel();
}

void DriveSync::copySignInLink()
{
    const QUrl page = m_auth->signInPage();
    QClipboard *clipboard = QGuiApplication::clipboard();
    if (!browserFailed() || page.isEmpty() || !clipboard)
        return;
    // Never logged: it carries the client and the sign-in's challenge.
    clipboard->setText(page.toString(QUrl::FullyEncoded));
    Q_EMIT notice(QStringLiteral("Copied. Paste it into your browser's address bar"));
}

void DriveSync::signOut()
{
    if (m_demo)
        return;
    stopRun();
    m_syncTimer.stop();
    m_editTimer.stop();
    m_periodTimer.stop();
    m_tickTimer.stop();
    // A sign-in that never finished, put away from its red line, leaves no
    // backup behind: "Signed out, your backup is still in your Drive" is
    // said only after an account that was signed in.
    const bool hadAccount = signedIn() || m_auth->state() == DriveAuth::State::Expired
                            || !m_auth->email().isEmpty() || storedSignIn();
    if (m_auth->signOut()) {
        forgetSyncState();
        m_justSignedOut = hadAccount;
        m_firstAfterSignIn = false;
        m_forcedPending = false;
        m_pendingCounts = LibraryBackup::Counts();
    }
    Q_EMIT stateChanged();
}

void DriveSync::syncNow()
{
    if (m_demo || !signedIn())
        return;
    if (m_run) {
        m_againForced = true;
        return;
    }
    startRun(/*forced=*/true);
}

void DriveSync::noteLibraryEdited()
{
    m_editedSinceSync = true;
    ++m_editSerial;
    Q_EMIT libraryEdited();
    if (!m_started || m_demo || !signedIn() || held())
        return;
    // A minute after the last edit, so a run of them (a playlist being put
    // together) goes up once; but never more than five minutes into the run.
    const qint64 now = m_clock.elapsed();
    if (m_firstEditMs < 0)
        m_firstEditMs = now;
    const qint64 wait = std::min<qint64>(m_timing.editMs, m_firstEditMs + m_timing.editMaxMs - now);
    m_editTimer.start(int(std::max<qint64>(0, wait)));
}

// A song every three or four minutes, each its own minute-later sync, would
// send the whole file (megabytes, with a long history) once a song: so a
// play only waits for the next sync that uploads, the half-hourly one at
// the latest.
void DriveSync::notePlayed()
{
    m_playedSinceSync = true;
    ++m_editSerial;
}

void DriveSync::requestSync()
{
    if (!m_started || m_demo || !signedIn() || held())
        return;
    if (m_run) {
        m_again = true;
        return;
    }
    const qint64 now = m_clock.elapsed();
    qint64 at = now;
    if (m_lastStartMs >= 0)
        at = std::max(at, m_lastStartMs + m_timing.gapMs);
    at = std::max(at, m_notBeforeMs);
    const int wait = int(at - now);
    if (!m_syncTimer.isActive() || m_syncTimer.remainingTime() > wait)
        m_syncTimer.start(wait);
}

// ------------------------------------------------------------------ a sync

void DriveSync::startRun(bool forced)
{
    if (m_run || m_demo || !signedIn())
        return;
    if (held()) {
        if (!forced)
            return;
        // SYNC NOW is the user saying to look at the file again.
        m_problem = Problem::None;
        setSetting(kHeldKey, QString());
    }
    m_syncTimer.stop();
    m_again = false;
    m_againForced = false;
    m_run = std::make_unique<Run>();
    m_run->serial = ++m_runSerial;
    m_run->forced = forced;
    // Owed until a sync gets through: the half-hourly one's plays, and what
    // SYNC NOW is to say (failRun gives the first back).
    m_run->periodic = std::exchange(m_periodDue, false);
    if (forced)
        m_forcedPending = true;
    m_lastStartMs = m_clock.elapsed();
    qInfo("backup: sync started (%s)", m_firstAfterSignIn ? "after signing in" : forced ? "asked for" : "on its own");
    Q_EMIT stateChanged();
    find();
}

void DriveSync::find()
{
    m_run->files.clear();
    m_run->next = 0;
    m_run->keptRead = false;
    m_run->keptHash.clear();
    QUrl url = driveBase();
    url.setPath(QStringLiteral("/drive/v3/files"));
    const QString query = QStringLiteral("name='%1' and trashed=false").arg(kFileName);
    url.setQuery(QStringLiteral("spaces=appDataFolder&q=%1&fields=%2")
                     .arg(QString::fromLatin1(QUrl::toPercentEncoding(query)),
                          QString::fromLatin1(QUrl::toPercentEncoding(QStringLiteral("files(id,modifiedTime,size)")))));
    send("GET", url, QByteArray(), QByteArray(), [this](const Answer &answer) { found(answer); });
}

void DriveSync::found(const Answer &answer)
{
    if (answer.status != 200) {
        failRun(problemOf(answer), answer);
        return;
    }
    // Before anything is read or written: a clock far from Google's would
    // put times in the file that win, or lose, every comparison they meet.
    if (answer.serverTime.isValid()) {
        m_clockSkewSecs = answer.serverTime.secsTo(QDateTime::currentDateTimeUtc());
        if (std::abs(m_clockSkewSecs) > kClockToleranceSecs) {
            failRun(Problem::Clock, answer);
            return;
        }
    }
    QList<Remote> files;
    const QJsonArray list = QJsonDocument::fromJson(answer.body).object().value(QStringLiteral("files")).toArray();
    for (const QJsonValue &value : list) {
        const QJsonObject file = value.toObject();
        const QString id = file.value(QStringLiteral("id")).toString();
        if (!plainId(id))
            continue;
        const QString size = file.value(QStringLiteral("size")).toVariant().toString();
        files.append({ id, file.value(QStringLiteral("modifiedTime")).toString() + QLatin1Char('|') + size });
    }
    // Two computers that each made the file at once: every computer keeps
    // the same one (the first by id), merges the other in and deletes it.
    std::sort(files.begin(), files.end(), [](const Remote &a, const Remote &b) { return a.id < b.id; });
    m_run->files = files;
    qInfo("backup: sync: %d %s in Google Drive", int(files.size()),
          files.size() == 1 ? "backup" : "backups");

    if (files.isEmpty()) {
        mergeAndUpload();
        return;
    }
    // Unchanged since this computer last wrote or read it: what it holds is
    // in the library already, so it is not read again (it can be large).
    // SYNC NOW reads it whatever.
    if (!m_run->forced && files.size() == 1 && files.first().id == setting(kFileIdKey)
        && files.first().modified == setting(kRemoteKey) && !setting(kHashKey).isEmpty()) {
        qInfo("backup: sync: the backup has not changed since this computer last saw it");
        mergeAndUpload();
        return;
    }
    downloadNext();
}

void DriveSync::downloadNext()
{
    if (m_run->next >= m_run->files.size()) {
        mergeAndUpload();
        return;
    }
    QUrl url = driveBase();
    url.setPath(QStringLiteral("/drive/v3/files/") + m_run->files.at(m_run->next).id);
    url.setQuery(QStringLiteral("alt=media"));
    send("GET", url, QByteArray(), QByteArray(), [this](const Answer &answer) { downloaded(answer); });
}

void DriveSync::downloaded(const Answer &answer)
{
    if (answer.status == 404 && !m_run->refound) {
        // Deleted between the list and the read (the user deleting the
        // hidden data, or another computer's copy going): look again.
        m_run->refound = true;
        qInfo("backup: sync: the backup went while it was being read; looking again");
        find();
        return;
    }
    if (answer.tooLarge) {
        failRun(Problem::Unreadable, answer);
        return;
    }
    if (answer.status != 200) {
        failRun(problemOf(answer), answer);
        return;
    }
    LibraryBackup::Data data;
    QString why;
    if (!LibraryBackup::parse(answer.body, &data, &why)) {
        const double version = QJsonDocument::fromJson(answer.body).object().value(QStringLiteral("version")).toDouble();
        failRun(version > LibraryBackup::kVersion ? Problem::Newer : Problem::Unreadable, answer);
        return;
    }
    qInfo("backup: sync: read %lld bytes from Google Drive", qint64(answer.body.size()));
    if (data.leftOut > 0) {
        qInfo("backup: sync: %d things in it were dated later than this computer's clock allows; left out until due",
              data.leftOut);
    }
    LibraryBackup::Counts counts;
    if (!m_backup.merge(data, &counts, &why)) {
        failRun(Problem::Database, answer);
        return;
    }
    // Kept past this run: what it merged is in the library now, even if the
    // upload that follows fails and a retry has to finish the sync.
    addCounts(m_pendingCounts, counts);
    if (m_run->next == 0) {
        m_run->keptRead = true;
        m_run->keptHash = LibraryBackup::contentHash(data);
    }
    ++m_run->next;
    downloadNext();
}

void DriveSync::mergeAndUpload()
{
    // Nothing to send and nothing new: the file is as this computer last
    // left it (not read, unchanged), nothing was edited here since, and the
    // plays waiting, if any, go with the half-hourly sync. The library is not
    // even read then, which with a long history is the most this costs. Not
    // before a sync has finished since launch: the last run of Monolist may
    // have quit with edits it had not sent.
    if (!m_run->forced && !m_run->keptRead && m_run->files.size() == 1 && m_syncedThisLaunch && !m_editedSinceSync
        && (!m_playedSinceSync || !m_run->periodic) && !setting(kHashKey).isEmpty()) {
        qInfo("backup: sync: nothing to upload%s", m_playedSinceSync ? "; plays wait for the half-hourly sync" : "");
        m_run->hash = setting(kHashKey).toLatin1();
        m_run->fileId = m_run->files.first().id;
        m_run->modified = m_run->files.first().modified;
        removeExtras();
        return;
    }
    const LibraryBackup::Data snapshot = m_backup.snapshot();
    m_run->snapshotted = true;
    m_run->hash = LibraryBackup::contentHash(snapshot);
    m_run->editSerial = m_editSerial;
    if (!m_run->files.isEmpty()) {
        const Remote &kept = m_run->files.first();
        const bool same = m_run->keptRead ? m_run->hash == m_run->keptHash
                                          : m_run->hash == setting(kHashKey).toLatin1();
        if (same) {
            qInfo("backup: sync: the backup already holds the library; nothing to upload");
            m_run->fileId = kept.id;
            m_run->modified = kept.modified;
            removeExtras();
            return;
        }
    }
    upload(LibraryBackup::toJson(snapshot));
}

void DriveSync::upload(const QByteArray &content)
{
    QUrl url = driveBase();
    if (m_run->files.isEmpty()) {
        // The first backup: made in the app's own folder, which nothing but
        // Monolist can see (and the user can only delete as a whole).
        url.setPath(QStringLiteral("/upload/drive/v3/files"));
        url.setQuery(QStringLiteral("uploadType=multipart&fields=id,modifiedTime,size"));
        QByteArray boundary;
        do {
            boundary = "monolist-" + QByteArray::number(QRandomGenerator::global()->generate64(), 16)
                       + QByteArray::number(QRandomGenerator::global()->generate64(), 16);
        } while (content.contains(boundary));
        const QJsonObject metadata{ { QStringLiteral("name"), kFileName },
                                    { QStringLiteral("parents"), QJsonArray{ QStringLiteral("appDataFolder") } },
                                    { QStringLiteral("mimeType"), QStringLiteral("application/json") } };
        const QByteArray body = "--" + boundary + "\r\nContent-Type: application/json; charset=UTF-8\r\n\r\n"
                                + QJsonDocument(metadata).toJson(QJsonDocument::Compact) + "\r\n--" + boundary
                                + "\r\nContent-Type: application/json\r\n\r\n" + content + "\r\n--" + boundary + "--\r\n";
        qInfo("backup: sync: making the backup in Google Drive (%lld bytes)", qint64(content.size()));
        send("POST", url, body, "multipart/related; boundary=" + boundary,
             [this](const Answer &answer) { uploaded(answer); });
        return;
    }
    url.setPath(QStringLiteral("/upload/drive/v3/files/") + m_run->files.first().id);
    url.setQuery(QStringLiteral("uploadType=media&fields=id,modifiedTime,size"));
    qInfo("backup: sync: updating the backup in Google Drive (%lld bytes)", qint64(content.size()));
    send("PATCH", url, content, "application/json", [this](const Answer &answer) { uploaded(answer); });
}

void DriveSync::uploaded(const Answer &answer)
{
    if (answer.status == 404 && !m_run->refound) {
        m_run->refound = true;
        qInfo("backup: sync: the backup went before it could be updated; looking again");
        find();
        return;
    }
    if (answer.status != 200) {
        failRun(problemOf(answer), answer);
        return;
    }
    const QJsonObject file = QJsonDocument::fromJson(answer.body).object();
    const QString id = file.value(QStringLiteral("id")).toString();
    m_run->fileId = plainId(id) ? id : QString();
    m_run->modified = file.value(QStringLiteral("modifiedTime")).toString() + QLatin1Char('|')
                      + file.value(QStringLiteral("size")).toVariant().toString();
    if (m_run->fileId.isEmpty())
        m_run->modified.clear();   // read again next time, rather than trust it
    removeExtras();
}

void DriveSync::removeExtras()
{
    const int extra = 1 + m_run->removing;
    if (m_run->files.size() <= extra || !m_run->keptRead) {
        finishRun();
        return;
    }
    // Merged in above, and held by the one kept now: this copy is only a
    // second file every computer would read.
    QUrl url = driveBase();
    url.setPath(QStringLiteral("/drive/v3/files/") + m_run->files.at(extra).id);
    send("DELETE", url, QByteArray(), QByteArray(), [this](const Answer &answer) {
        qInfo("backup: sync: a second copy of the backup %s",
              answer.status == 204 || answer.status == 200 || answer.status == 404 ? "was deleted"
                                                                                    : "could not be deleted now");
        ++m_run->removing;
        removeExtras();
    });
}

void DriveSync::finishRun()
{
    Run &run = *m_run;
    setSetting(kFileIdKey, run.fileId);
    setSetting(kRemoteKey, run.modified);
    setSetting(kHashKey, QString::fromLatin1(run.hash));
    m_lastSyncedAt = QDateTime::currentDateTimeUtc();
    setSetting(kSyncedKey, m_lastSyncedAt.toString(Qt::ISODate));
    setSetting(kHeldKey, QString());
    m_failures = 0;
    m_problem = Problem::None;
    m_retryAt = QDateTime();
    m_notBeforeMs = 0;
    m_syncedThisLaunch = true;
    // Nothing edited while it ran: what is in Drive is the library. A sync
    // that did not read the library sent nothing, and the plays wait on.
    if (run.snapshotted && run.editSerial == m_editSerial) {
        m_editedSinceSync = false;
        m_playedSinceSync = false;
        m_editTimer.stop();
        m_firstEditMs = -1;
    }
    qInfo("backup: synced with Google Drive");
    // What the first sync after signing in brought, or a SYNC NOW, said now
    // that one has got through, with everything the runs before it merged.
    const bool first = std::exchange(m_firstAfterSignIn, false);
    const bool asked = std::exchange(m_forcedPending, false);
    const QString brought = broughtIn(std::exchange(m_pendingCounts, LibraryBackup::Counts()));
    if (first) {
        // Sticky: it may come while the window is still behind the browser
        // the sign-in was finished in, and must not leave unread.
        Q_EMIT notice(brought.isEmpty() ? QStringLiteral("Your library is backed up in your Google Drive") : brought,
                      /*sticky=*/true);
    } else if (asked && !brought.isEmpty()) {
        Q_EMIT notice(brought);
    }
    endRun();
}

DriveSync::Problem DriveSync::problemOf(const Answer &answer) const
{
    if (answer.redirectRefused)
        return Problem::Odd;
    if (answer.status == 0)
        return Problem::Offline;
    if (answer.status == 429)
        return Problem::Slowed;
    if (answer.status == 403) {
        if (answer.reason == QLatin1String("rateLimitExceeded") || answer.reason == QLatin1String("userRateLimitExceeded"))
            return Problem::Slowed;
        if (answer.reason == QLatin1String("storageQuotaExceeded"))
            return Problem::Full;
        return Problem::Odd;
    }
    if (answer.status >= 500)
        return Problem::Server;
    return Problem::Odd;
}

void DriveSync::failRun(Problem problem)
{
    failRun(problem, Answer());
}

void DriveSync::failRun(Problem problem, const Answer &answer)
{
    m_problem = problem;
    // The half-hourly sync's plays are still owed to the next.
    if (m_run && m_run->periodic)
        m_periodDue = true;
    if (problem == Problem::Clock) {
        // Not a failure of Drive's, and not backed off: looked at again every
        // few minutes (one list of the files each time), and synced as soon
        // as the clock is right.
        const qint64 waitMs = m_timing.clockRetryMs;
        m_notBeforeMs = m_clock.elapsed() + waitMs;
        m_retryAt = QDateTime::currentDateTimeUtc().addMSecs(waitMs);
        qWarning("backup: sync stopped: this computer's clock is %lld s %s Google's; nothing read or written,"
                 " looked at again in %lld s",
                 qint64(std::abs(m_clockSkewSecs)), m_clockSkewSecs > 0 ? "ahead of" : "behind", waitMs / 1000);
        m_syncTimer.start(int(waitMs));
        endRun();
        return;
    }
    if (problem == Problem::Unreadable || problem == Problem::Newer) {
        // Never overwritten, and not read again on a timer: the file may be
        // the only copy of something, and SYNC NOW is the user's to press.
        setSetting(kHeldKey, problem == Problem::Newer ? QStringLiteral("newer") : QStringLiteral("unreadable"));
        qWarning("backup: sync stopped: %s; the backup is left as it is until SYNC NOW", problemName(problem));
        endRun();
        return;
    }
    ++m_failures;
    qint64 waitMs = m_timing.backoffMs.at(std::min<qsizetype>(m_failures, m_timing.backoffMs.size()) - 1);
    if (answer.retryAfterMs > waitMs)
        waitMs = std::min(answer.retryAfterMs, kMaxRetryAfterMs);
    m_notBeforeMs = m_clock.elapsed() + waitMs;
    m_retryAt = QDateTime::currentDateTimeUtc().addMSecs(waitMs);
    // Drive's reason is one of its fixed words (accessNotConfigured: the
    // Drive API is not turned on in the client's project), never anything
    // of the user's; letters only, all the same.
    const QString reason = QString(answer.reason).remove(QRegularExpression(QStringLiteral("[^A-Za-z]"))).left(48);
    if (answer.status > 0)
        qWarning("backup: sync stopped: %s (HTTP %d%s); trying again in %lld s", problemName(problem), answer.status,
                 reason.isEmpty() ? "" : qPrintable(QStringLiteral(", ") + reason), waitMs / 1000);
    else
        qWarning("backup: sync stopped: %s; trying again in %lld s", problemName(problem), waitMs / 1000);
    m_syncTimer.start(int(waitMs));
    endRun();
}

void DriveSync::endRun()
{
    m_run.reset();
    Q_EMIT stateChanged();
    if (std::exchange(m_againForced, false)) {
        QTimer::singleShot(0, this, [this]() { startRun(/*forced=*/true); });
        m_again = false;
    } else if (std::exchange(m_again, false)) {
        requestSync();
    }
}

void DriveSync::stopRun()
{
    if (m_reply) {
        QNetworkReply *reply = m_reply;
        m_reply.clear();
        disconnect(reply, nullptr, this, nullptr);
        reply->abort();
        reply->deleteLater();
    }
    m_run.reset();
    m_again = false;
    m_againForced = false;
}

// ------------------------------------------------------------ the requests

void DriveSync::send(const QByteArray &verb, const QUrl &url, const QByteArray &body, const QByteArray &type, Done done,
                     bool retried)
{
    if (!m_run)
        return;
    const quint64 serial = m_run->serial;
    m_auth->accessToken([this, serial, verb, url, body, type, done, retried](DriveAuth::Token result,
                                                                           const QByteArray &token) {
        if (!m_run || m_run->serial != serial)
            return;
        switch (result) {
        case DriveAuth::Token::Ok:
            break;
        case DriveAuth::Token::Offline:
            failRun(Problem::Offline);
            return;
        case DriveAuth::Token::Refused:
            failRun(Problem::Refused);
            return;
        case DriveAuth::Token::Ended:
            qInfo("backup: sync stopped: the Google sign-in has ended");
            endRun();
            return;
        }
        if (!mayCarryToken(url)) {
            qWarning("backup: a request to somewhere other than Google Drive was not sent");
            failRun(Problem::Odd);
            return;
        }
        QNetworkRequest request(url);
        request.setHeader(QNetworkRequest::UserAgentHeader, userAgent());
        request.setRawHeader("Authorization", "Bearer " + token);
        if (!type.isEmpty())
            request.setHeader(QNetworkRequest::ContentTypeHeader, type);
        // No cookies either way: nothing is sent, nothing is kept.
        request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
        request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
        // A redirect carries the request's headers with it, the token among
        // them: one is followed only on the same host, over https.
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::UserVerifiedRedirectPolicy);
        request.setTransferTimeout(m_timing.timeoutMs);

        QNetworkReply *reply = nullptr;
        if (verb == "GET")
            reply = m_network->get(request);
        else if (verb == "DELETE")
            reply = m_network->deleteResource(request);
        else if (verb == "POST")
            reply = m_network->post(request, body);
        else
            reply = m_network->sendCustomRequest(request, verb, body);
        m_reply = reply;
        connect(reply, &QNetworkReply::redirected, reply, [this, reply, url](const QUrl &to) {
            const QUrl next = url.resolved(to);
            if (next.scheme() == QLatin1String("https") && next.host().compare(url.host(), Qt::CaseInsensitive) == 0
                && mayCarryToken(next)) {
                Q_EMIT reply->redirectAllowed();
                return;
            }
            qWarning("backup: Google Drive's answer pointed somewhere else; not followed");
            reply->setProperty("monolist.refused", true);
            reply->abort();
        });
        // The file is never larger than LibraryBackup reads; anything that
        // is, is not the backup, and is not held in memory to find out.
        connect(reply, &QNetworkReply::downloadProgress, reply, [reply](qint64 received, qint64) {
            if (received > LibraryBackup::kMaxBytes && !reply->property("monolist.tooLarge").toBool()) {
                reply->setProperty("monolist.tooLarge", true);
                reply->abort();
            }
        });
        connect(reply, &QNetworkReply::finished, this, [this, serial, reply, verb, url, body, type, done, retried]() {
            reply->deleteLater();
            if (m_reply == reply)
                m_reply.clear();
            if (!m_run || m_run->serial != serial)
                return;
            Answer answer;
            answer.redirectRefused = reply->property("monolist.refused").toBool();
            answer.tooLarge = reply->property("monolist.tooLarge").toBool();
            answer.status = answer.redirectRefused || answer.tooLarge
                                ? 0 : reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            // A success whose transfer broke off (the connection dropped, the
            // transfer timeout) is no answer: its body is part of one, which
            // read as the file would be taken for a damaged backup and held
            // until the user asked. An error status is an answer, whole or not.
            const bool cutOff = answer.status >= 200 && answer.status < 300 && reply->error() != QNetworkReply::NoError;
            if (cutOff)
                answer.status = 0;
            if (answer.status != 0)
                answer.body = reply->readAll();
            else if (!answer.redirectRefused && !answer.tooLarge)
                qInfo("backup: Google Drive %s (network error %d)", cutOff ? "stopped answering part way" : "did not answer",
                      int(reply->error()));
            // Google's own time, for this computer's clock to be checked against.
            answer.serverTime = httpDate(reply->rawHeader("Date"));
            bool seconds = false;
            const qint64 retryAfter = reply->rawHeader("Retry-After").trimmed().toLongLong(&seconds);
            if (seconds && retryAfter > 0)
                answer.retryAfterMs = retryAfter * 1000;
            if (answer.status >= 400) {
                answer.reason = QJsonDocument::fromJson(answer.body).object().value(QStringLiteral("error")).toObject()
                                    .value(QStringLiteral("errors")).toArray().at(0).toObject()
                                    .value(QStringLiteral("reason")).toString();
            }
            if (answer.status == 401 && !retried) {
                // The token was refused before its time (revoked, or the
                // clock off): renewed once and the request sent again, once.
                qInfo("backup: Google Drive refused the access token; renewing it once");
                m_auth->forgetAccessToken();
                send(verb, url, body, type, done, /*retried=*/true);
                return;
            }
            done(answer);
        });
    });
}

// --------------------------------------------------------- Export, Import

// The dialog waits for the user in an event loop of its own, so it opens
// after the click that asked for it has returned: QML's handler, and the
// button it belongs to, are never left half-way through a nested loop.
void DriveSync::exportLibrary()
{
    if (m_fileBusy)
        return;
    m_fileBusy = true;
    QTimer::singleShot(0, this, &DriveSync::runExport);
}

void DriveSync::importLibrary()
{
    if (m_fileBusy)
        return;
    m_fileBusy = true;
    QTimer::singleShot(0, this, &DriveSync::runImport);
}

void DriveSync::runExport()
{
    const QString suggested = QStringLiteral("Monolist library %1.json")
                                  .arg(QDate::currentDate().toString(Qt::ISODate));
    const QString path = pickFile(/*save=*/true, suggested);
    if (path.isEmpty()) {
        m_fileBusy = false;
        return;
    }
    LibraryBackup::Data written;
    QString error;
    const bool ok = m_backup.exportTo(path, &written, &error);
    m_fileBusy = false;
    if (!ok) {
        Q_EMIT notice(error);
        return;
    }
    const auto plays = std::count_if(written.plays.cbegin(), written.plays.cend(),
                                     [](const LibraryBackup::Play &play) { return play.kind == QLatin1String("play"); });
    Q_EMIT notice(QStringLiteral("Saved your library: %1, %2, %3")
                      .arg(countOf(written.playlists.size(), "playlist", "playlists"),
                           countOf(written.likes.size(), "liked song", "liked songs"),
                           countOf(plays, "play", "plays")));
}

void DriveSync::runImport()
{
    const QString path = pickFile(/*save=*/false, QString());
    if (path.isEmpty()) {
        m_fileBusy = false;
        return;
    }
    LibraryBackup::Counts counts;
    QString error;
    const bool ok = m_backup.importFrom(path, &counts, &error);
    m_fileBusy = false;
    if (!ok) {
        Q_EMIT notice(error);
        return;
    }
    // What came in is this library's now, and goes to the Drive backup with
    // the rest of it.
    if (counts.changed())
        noteLibraryEdited();
    Q_EMIT notice(counts.summary());
}

QString DriveSync::pickFile(bool save, const QString &suggested)
{
    if (m_picker)
        return m_picker(save, suggested);

    QString folder = m_folder;
    if (folder.isEmpty() || !QFileInfo(folder).isDir())
        folder = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    // The system's own dialog (Qt Widgets', which the tray already links):
    // QML's FileDialog would bring every Controls style with it when
    // deployed (CLAUDE.md).
    QFileDialog dialog(nullptr, save ? QStringLiteral("Save your library") : QStringLiteral("Bring in a library"),
                       folder);
    dialog.setAcceptMode(save ? QFileDialog::AcceptSave : QFileDialog::AcceptOpen);
    dialog.setFileMode(save ? QFileDialog::AnyFile : QFileDialog::ExistingFile);
    dialog.setNameFilters({ QStringLiteral("Monolist library (*.json)"), QStringLiteral("All files (*)") });
    if (save) {
        dialog.setDefaultSuffix(QStringLiteral("json"));
        dialog.selectFile(suggested);
    }
    // A Widgets dialog in front of QML's window and kept with it: its own
    // window made first, then told whose it is, which the system's dialog
    // takes as its owner.
    if (m_owner) {
        dialog.winId();
        if (QWindow *handle = dialog.windowHandle())
            handle->setTransientParent(m_owner);
    }
    if (dialog.exec() != QDialog::Accepted)
        return {};
    const QString path = dialog.selectedFiles().value(0);
    if (!path.isEmpty())
        m_folder = QFileInfo(path).absolutePath();
    return path;
}
