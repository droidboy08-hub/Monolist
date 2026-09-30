#include "ytmsession.h"

#include "library.h"
#include "secretstore.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QNetworkCookie>
#include <QNetworkInformation>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QTimeZone>

#include <algorithm>
#include <limits>
#include <memory>
#include <string>
#include <utility>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX   // std::numeric_limits<int>::max() below
#endif
#include <windows.h>
#endif

namespace {

// The jar's name in SecretStore, and the settings: the account's name and
// public handle, and what the account is used for. Nothing secret is ever
// written to settings.
const QString kSecretName = QStringLiteral("ytmusic.cookies");
const QString kNameKey = QStringLiteral("ytmusic.account_name");
const QString kHandleKey = QStringLiteral("ytmusic.account_handle");
// Where a session is signed out at Google: Google Account → Security → Your
// devices.
const QString kDevicesUrl = QStringLiteral("https://myaccount.google.com/device-activity");
const QString kHomeKey = QStringLiteral("ytmusic.use_for_home");
const QString kPlayKey = QStringLiteral("ytmusic.play_when_needed");
const QString kReportKey = QStringLiteral("ytmusic.report_listens");

// yt-dlp's cookies files: their folder, and their names, which nothing else
// in it has.
const QString kCookieFolderName = QStringLiteral("yt-dlp-cookies");
const QString kCookieFilePattern = QStringLiteral("cookies-*.txt");
// A deleted file still held open (Windows) is tried again this many times,
// a second apart.
constexpr int kDeleteAttempts = 5;
// yt-dlp saying the cookies are no longer valid has the session checked, at
// most this often.
constexpr qint64 kDoubtEveryMs = 10 * 60 * 1000;

// Far above any cookies.txt worth reading; a file this size is something else.
constexpr qint64 kMaxFileBytes = 16 * 1024 * 1024;
// A signed-in session is about twenty cookies. Rotation adds to the jar, and
// this keeps a server that keeps adding from growing it without end.
constexpr int kMaxJar = 80;

using Cookie = CookieImport::Cookie;

qint64 nowSecs()
{
    return QDateTime::currentSecsSinceEpoch();
}

int timerMs(qint64 ms)
{
    return int(std::clamp<qint64>(ms, 0, std::numeric_limits<int>::max()));
}

// `ms` give or take `spread` of it (0.25: from 75 to 125 per cent), so the
// account's checks never come on a clock a server could set its watch by.
qint64 jittered(qint64 ms, double spread)
{
    const double factor = 1.0 + spread * (2.0 * QRandomGenerator::global()->generateDouble() - 1.0);
    return qint64(double(ms) * factor);
}

QString joinedNames(const QList<Cookie> &jar)
{
    return CookieImport::names(jar).join(QStringLiteral(", "));
}

// What is known of a session beside its cookies, for the log: which parts,
// never their values (but for the account's index, a number from 0 to 99).
QString describe(const CookieImport::SessionInfo &info)
{
    if (info.isEmpty())
        return QString();
    QStringList parts;
    if (info.authUser >= 0)
        parts << QStringLiteral("account index %1").arg(info.authUser);
    if (!info.visitorData.isEmpty())
        parts << QStringLiteral("its visitor id");
    if (!info.dataSyncId.isEmpty()) {
        parts << (info.delegatedId().isEmpty() ? QStringLiteral("its DATASYNC_ID (the account's own channel)")
                                               : QStringLiteral("its DATASYNC_ID (a brand channel)"));
    }
    return QStringLiteral(", with ") + parts.join(QStringLiteral(", "));
}

}

const QByteArray YtmSession::kOrigin = QByteArrayLiteral("https://music.youtube.com");

// What one check heard back: the account menu and the home feed, asked side
// by side, decided on together once both have answered.
struct YtmSession::CheckAnswers {
    quint64 run = 0;
    int answered = 0;
    QString name;
    QString handle;
    QString loggedIn;
    QString menuError;
    QString homeError;
    // What the answers' responseContext says of the session (as they come;
    // learn() checks them), the home feed's first and the menu's after.
    QString visitorData;
    QString dataSyncId;
    QJsonObject home;   // the feed's answer, kept for Home once confirmed
};

YtmSession::YtmSession(Library *library, QObject *parent)
    : QObject(parent)
    , m_library(library)
{
    m_checkTimer.setSingleShot(true);
    connect(&m_checkTimer, &QTimer::timeout, this, &YtmSession::check);
    m_saveTimer.setSingleShot(true);
    connect(&m_saveTimer, &QTimer::timeout, this, &YtmSession::save);
    // On until the user turns it off: someone who signs in has asked for
    // what the account knows, and Home is the first of it. The other two
    // are on by default by the owner's choice: a song the account can play
    // is better played than skipped, and the listens are what the account's
    // recommendations learn from.
    m_useForHome = !m_library || m_library->settingValue(kHomeKey) != QLatin1String("0");
    m_playWhenNeeded = !m_library || m_library->settingValue(kPlayKey) != QLatin1String("0");
    m_reportListens = !m_library || m_library->settingValue(kReportKey) != QLatin1String("0");
    // The counts and any rest an earlier run was told to take outlive it:
    // a restart must never be a way round them.
    m_guard.setLimits(m_timing.guard);
    if (m_library) {
        Library *library = m_library;
        m_guard.setStore({ [library](const QString &key) { return library->settingValue(key); },
                           [library](const QString &key, const QString &value) { library->setSetting(key, value); } });
    }
    connect(&m_guard, &AccountGuard::pausedChanged, this, [this]() {
        // Rested: whatever the session's standing, a check says it again
        // before anything else is asked with it (admit holds the rest back
        // until then).
        if (!m_guard.paused() && !m_jar.isEmpty()) {
            m_needsCheck = true;
            m_failures = 0;
            scheduleCheck(m_timing.launchCheckMs
                          + qint64(QRandomGenerator::global()->bounded(std::max(1, 2 * m_timing.launchCheckMs))));
        }
        updateStatus();
        Q_EMIT accountUseChanged();
    });
    updateStatus();
}

QJsonObject YtmSession::takeCheckedHome(qint64 maxAgeMs)
{
    QJsonObject home = std::exchange(m_checkedHome, QJsonObject());
    // Only the feed of this very session: another account's, or the one
    // before a sign-out, is never shown as this one's.
    if (home.isEmpty() || !m_checkedHomeAge.isValid() || m_checkedHomeAge.elapsed() > maxAgeMs
        || m_state != State::Active || m_checkedHomeSession != m_generation || m_jar.isEmpty())
        return {};
    return home;
}

void YtmSession::forgetCheckState()
{
    m_checkedHome = {};
    m_checkedHomeAge.invalidate();
    m_checkedHomeSession = 0;
    m_needsCheck = false;
}

QString YtmSession::accountKey() const
{
    if (m_jar.isEmpty())
        return QString();
    const QString who = !m_info.dataSyncId.isEmpty() ? m_info.dataSyncId : m_handle + QLatin1Char('|') + m_name;
    if (who == QLatin1String("|"))
        return QString();
    return QString::fromLatin1(
        QCryptographicHash::hash("monolist-account:" + who.toUtf8(), QCryptographicHash::Sha256).toHex().left(16));
}

QString YtmSession::restLine() const
{
    if (!m_guard.paused())
        return QString();
    const QDateTime until = QDateTime::fromMSecsSinceEpoch(m_guard.pausedUntil());
    const bool today = until.date() == QDate::currentDate();
    const QString when = today ? QLocale::system().toString(until.time(), QLocale::ShortFormat)
                               : QLocale::system().toString(until, QLocale::ShortFormat);
    return QStringLiteral("YouTube asked Monolist to slow down, so nothing is asked with your account until %1. "
                          "Everything plays signed out meanwhile; your sign-in is kept.").arg(when);
}

void YtmSession::ytDlpSaid(const QString &errorOutput)
{
    static const QRegularExpression slowDown(
        QStringLiteral("not a bot|HTTP Error 429|Too Many Requests|rate[- ]limited|unusual traffic"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = slowDown.match(errorOutput);
    if (match.hasMatch())
        m_guard.trip(QStringLiteral("yt-dlp's lookup with the account was answered \"%1\"").arg(match.captured(0)));
}

void YtmSession::setUseForHome(bool use)
{
    if (use == m_useForHome)
        return;
    m_useForHome = use;
    if (m_library)
        m_library->setSetting(kHomeKey, use ? QStringLiteral("1") : QStringLiteral("0"));
    qInfo("ytmusic: Home %s", use ? "asks as the account while one is signed in" : "stays signed out");
    updateStatus();
    Q_EMIT useForHomeChanged();
}

void YtmSession::setPlayWhenNeeded(bool play)
{
    if (play == m_playWhenNeeded)
        return;
    m_playWhenNeeded = play;
    if (m_library)
        m_library->setSetting(kPlayKey, play ? QStringLiteral("1") : QStringLiteral("0"));
    qInfo("ytmusic: a song YouTube will not play signed out %s",
          play ? "is asked for with the account while one is signed in" : "stays signed out, and is skipped");
    updateStatus();
    Q_EMIT playWhenNeededChanged();
}

void YtmSession::setReportListens(bool report)
{
    if (report == m_reportListens)
        return;
    m_reportListens = report;
    if (m_library)
        m_library->setSetting(kReportKey, report ? QStringLiteral("1") : QStringLiteral("0"));
    qInfo("ytmusic: listens %s", report ? "are reported to the account's YouTube history while one is signed in"
                                        : "are not reported to YouTube");
    updateStatus();
    Q_EMIT reportListensChanged();
}

bool YtmSession::accountForHome() const
{
    // What the hook asks of an IfSignedIn call (authHeaders, admit), so what
    // Home believes it asked as is what it did.
    return m_useForHome && m_state == State::Active && !m_jar.isEmpty() && !m_guard.paused();
}

YtmSession::~YtmSession()
{
    // Cookies rotated in the last half-minute are kept, not lost with the
    // process.
    if (m_dirty)
        save();
    // A cookies file a lookup still had when the app closed: its yt-dlp is
    // being ended with the app, and the file goes now, or with the next
    // launch's sweep should it still be held.
    for (const QString &path : std::as_const(m_cookieFiles))
        QFile::remove(path);
    if (m_hooked) {
        InnerTube::setAccountHook({});
        InnerTube::setSessionVisitorData(QString());
    }
    // Before the members its answers would reach.
    delete m_innerTube.data();
}

void YtmSession::installHook()
{
    const QPointer<YtmSession> self(this);
    InnerTube::AccountHook hook;
    hook.admit = [self](const InnerTube::AccountRequest &request, QString *why) -> qint64 {
        return self ? self->admit(request, why) : 0;
    };
    hook.headers = [self](const InnerTube::AccountRequest &request, InnerTube::AccountHeaders *headers) -> quint64 {
        return self ? self->authHeaders(request, headers) : 0;
    };
    hook.started = [self](quint64, const InnerTube::AccountRequest &request) -> quint64 {
        return self ? self->m_guard.started(kindOf(request)) : 0;
    };
    hook.finished = [self](quint64, const InnerTube::AccountRequest &request, const InnerTube::AccountOutcome &outcome) {
        if (self)
            self->callFinished(request, outcome);
    };
    hook.rejected = [self](quint64 session, int status) {
        if (self)
            self->reportRejected(session, status);
    };
    hook.doubted = [self](quint64 session, int status) {
        if (self && session != 0 && session == self->m_generation)
            self->doubt(QStringLiteral("YouTube Music refused a call with the account (HTTP %1)").arg(status));
    };
    hook.cookies = [self](quint64 session, const QString &host, const QList<QNetworkCookie> &cookies) {
        if (self)
            self->absorbCookies(session, host, cookies);
    };
    InnerTube::setAccountHook(std::move(hook));
    m_hooked = true;
    publishVisitor();
}

void YtmSession::publishVisitor()
{
    // Only while hooked: a YtmSession that never hands out the account (a
    // test's second one) must not take the first one's id away.
    if (m_hooked)
        InnerTube::setSessionVisitorData(m_jar.isEmpty() ? QString() : m_info.visitorData);
}

void YtmSession::setTiming(const Timing &timing)
{
    m_timing = timing;
    m_guard.setLimits(timing.guard);
}

InnerTube *YtmSession::innerTube()
{
    // Made the first time there is something to check, and without the
    // warm-up: nobody signed out pays for it.
    if (!m_innerTube)
        m_innerTube = new InnerTube(this, /*warmUp=*/false);
    return m_innerTube;
}

void YtmSession::start()
{
    installHook();
    // What an earlier run lent yt-dlp and never saw deleted (a crash, a
    // kill mid-lookup): a copy of the session in plain text, so it goes
    // first, whatever the state of the session now.
    sweepCookieFiles();
    const QString remembered = m_library ? m_library->settingValue(kNameKey) : QString();
    const QString handle = m_library ? m_library->settingValue(kHandleKey) : QString();
    QByteArray json;
    QString error;
    switch (SecretStore::read(kSecretName, &json, &error)) {
    case SecretStore::Status::Ok: {
        QList<Cookie> jar;
        CookieImport::SessionInfo info;
        m_name = remembered;
        m_handle = handle;
        if (!CookieImport::fromJson(json, &jar, &info) || !CookieImport::missingRequired(jar).isEmpty()) {
            qWarning("ytmusic: the stored session is not one Monolist can use; it is deleted");
            SecretStore::remove(kSecretName);
            m_ended = Ended::Unreadable;
            setState(State::Rejected);
            break;
        }
        m_jar = jar;
        m_info = info;
        publishVisitor();
        setState(State::Checking);
        {
            // A few seconds in, once Home has asked for its signed-out feed,
            // and never at quite the same moment of a launch.
            const qint64 in = m_timing.launchCheckMs
                              + qint64(QRandomGenerator::global()->bounded(std::max(1, m_timing.launchCheckMs)));
            qInfo("ytmusic: restored a session of %d cookies (%s)%s; checking it in %.1f s", int(m_jar.size()),
                  qPrintable(joinedNames(m_jar)), qPrintable(describe(m_info)), double(in) / 1000);
            scheduleCheck(in);
        }
        break;
    }
    case SecretStore::Status::NotFound:
        // A name with no session: it was refused, and the row says so until
        // the user imports again or signs out.
        m_name = remembered;
        m_handle = remembered.isEmpty() ? QString() : handle;
        m_ended = Ended::Unknown;
        setState(remembered.isEmpty() ? State::SignedOut : State::Rejected);
        break;
    case SecretStore::Status::Corrupt:
        // Monolist's own copy, and it will never open again.
        qWarning("ytmusic: the stored session does not open (%s); it is deleted", qPrintable(error));
        SecretStore::remove(kSecretName);
        m_name = remembered;
        m_handle = handle;
        m_ended = Ended::WontOpen;
        setState(State::Rejected);
        break;
    case SecretStore::Status::Unavailable:
        // Nothing was ever kept, so no name either.
        if (m_library) {
            if (!remembered.isEmpty())
                m_library->setSetting(kNameKey, QString());
            if (!handle.isEmpty())
                m_library->setSetting(kHandleKey, QString());
        }
        setState(State::SignedOut);
        break;
    case SecretStore::Status::Failed:
        m_notice = QStringLiteral("Monolist could not read the stored sign-in: %1").arg(error.toHtmlEscaped());
        qWarning("ytmusic: could not read the stored session: %s", qPrintable(error));
        setState(State::SignedOut);
        break;
    }
    qInfo("ytmusic: YouTube Music %s%s", qPrintable(state()),
          m_name.isEmpty() ? "" : qPrintable(QStringLiteral(" (") + m_name + QLatin1Char(')')));
}

QString YtmSession::state() const
{
    switch (m_state) {
    case State::SignedOut:   return QStringLiteral("signedOut");
    case State::Checking:    return QStringLiteral("checking");
    case State::Active:      return QStringLiteral("active");
    case State::Unreachable: return QStringLiteral("unreachable");
    case State::Rejected:    return QStringLiteral("rejected");
    }
    return QStringLiteral("signedOut");
}

QString YtmSession::importedFileName() const
{
    if (m_importedFile.startsWith(QLatin1String("demo:")))
        return m_importedFile.mid(int(qstrlen("demo:")));
    return m_importedFile.isEmpty() ? QString() : QFileInfo(m_importedFile).fileName();
}

bool YtmSession::remembered() const
{
    return SecretStore::available();
}

void YtmSession::setState(State state)
{
    m_state = state;
    if (state != State::SignedOut)
        m_notice.clear();
    updateStatus();
}

QString YtmSession::signedOutNotice()
{
    return QStringLiteral("Signed out: Monolist's encrypted copy of the session is deleted. At Google the session "
                          "itself lasts until it expires. To end it now, open your Google Account → Security → "
                          "<a href=\"%1\">Your devices</a>, choose the browser you signed in with, and sign it "
                          "out there.").arg(kDevicesUrl);
}

// The status line is styled text: what came from outside (the account's
// name, an error) is escaped where it is put in. The headline is plain.
//
// Each says what happened in the user's terms and what to do next; the
// reasons behind them are in the log.
void YtmSession::updateStatus()
{
    const QString who = m_name.isEmpty() ? QString() : QStringLiteral(" for %1").arg(m_name.toHtmlEscaped());
    QString line;
    QString headline;
    switch (m_state) {
    case State::SignedOut:
        line = m_notice;
        break;
    case State::Checking:
        headline = QStringLiteral("Checking…");
        if (m_zeroes > 0) {
            line = QStringLiteral("YouTube Music answered the sign-in%1 as if signed out. Monolist asks once more "
                                  "before believing it.").arg(who);
        } else if (m_fresh) {
            line = QStringLiteral("Asking YouTube Music whether this sign-in works. Nothing uses it until YouTube "
                                  "Music says so.");
        } else {
            line = QStringLiteral("Asking YouTube Music whether the sign-in%1 still works. Nothing uses it until "
                                  "YouTube Music says so.").arg(who);
        }
        break;
    case State::Active: {
        headline = m_name.isEmpty() ? QStringLiteral("Signed in") : QStringLiteral("Signed in as ") + m_name;
        if (!m_handle.isEmpty())
            headline += QStringLiteral(" · ") + m_handle;
        // Whose feed and history these are: a brand channel's, not the
        // account's own.
        if (!m_info.delegatedId().isEmpty())
            headline += QStringLiteral(" · a brand channel");
        // What the switches under the row let it be used for, and nothing
        // it is not.
        QStringList uses;
        if (m_useForHome)
            uses << QStringLiteral("Home shows your own feed");
        if (m_playWhenNeeded)
            uses << QStringLiteral("a song YouTube will not play signed out is asked for with it");
        if (m_reportListens)
            uses << QStringLiteral("what you play is added to your YouTube history");
        line = QStringLiteral("YouTube Music confirms the sign-in. ");
        if (uses.isEmpty()) {
            line += QStringLiteral("Nothing uses it: Home stays signed out, songs play without it, and what you "
                                   "play is not reported. ");
        } else {
            QString said = uses.join(QStringLiteral("; "));
            said[0] = said.at(0).toUpper();
            line += said + QStringLiteral(". ");
            if (!m_useForHome)
                line += QStringLiteral("Home stays signed out. ");
        }
        line += QStringLiteral("Search, lyrics and radio always stay signed out, and nothing is ever changed in "
                               "your account.");
        if (m_memoryOnly) {
            line += QStringLiteral(" It is forgotten when Monolist closes: %1")
                        .arg(SecretStore::unavailableReason().toHtmlEscaped());
        }
        break;
    }
    case State::Unreachable: {
        headline = QStringLiteral("Could not reach YouTube Music");
        // A time of day rather than "in five minutes", which would go stale
        // on the screen.
        const QString when = m_retryAt.isValid()
            ? QStringLiteral("at %1").arg(QLocale::system().toString(m_retryAt.time(), QLocale::ShortFormat))
            : QStringLiteral("later");
        line = QStringLiteral("The sign-in%1 is kept, but not used until YouTube Music has confirmed it. Monolist "
                              "tries again %2, or as soon as the network is back; until then it plays signed out.")
                   .arg(who, when);
        break;
    }
    case State::Rejected: {
        const QString status = m_endedStatus > 0 ? QStringLiteral(" (HTTP %1)").arg(m_endedStatus) : QString();
        switch (m_ended) {
        case Ended::Refused:
        case Ended::SignedOut:
            if (m_fresh) {
                // Imported, and answered as signed out from the first: the
                // copy was never a working sign-in.
                headline = QStringLiteral("Not signed in");
                line = m_ended == Ended::Refused
                    ? QStringLiteral("YouTube Music refused this sign-in%1: the session had already ended, or "
                                     "was signed out in the browser after it was copied. ").arg(status)
                    : QStringLiteral("YouTube Music answered this sign-in as signed out: it was copied before "
                                     "signing in, or from a window that was signed out afterwards. ");
                line += QStringLiteral("In a new private window, sign in at music.youtube.com (your picture shows "
                                       "at the top right), then export or copy it again.");
            } else {
                headline = QStringLiteral("Session expired");
                line = m_ended == Ended::Refused
                    ? QStringLiteral("YouTube Music no longer accepts the sign-in%1%2: the session has ended. ")
                          .arg(who, status)
                    : QStringLiteral("YouTube Music now answers the sign-in%1 as signed out: the session has "
                                     "ended. ").arg(who);
                line += QStringLiteral("Sessions last days to weeks, and end at once when they are signed out, in "
                                       "the browser or from your Google Account. Import a new one to sign back "
                                       "in; until then Monolist plays signed out.");
            }
            break;
        case Ended::Unreadable:
        case Ended::WontOpen:
            headline = QStringLiteral("Sign-in not restored");
            line = m_ended == Ended::WontOpen
                ? QStringLiteral("Monolist's stored copy of the sign-in%1 does not open here: it is encrypted for "
                                 "one user on one computer. ").arg(who)
                : QStringLiteral("Monolist's stored copy of the sign-in%1 could not be read. ").arg(who);
            line += QStringLiteral("It has been deleted. Import the session again to sign back in; until then "
                                   "Monolist plays signed out.");
            break;
        case Ended::Unknown:
            headline = QStringLiteral("Session expired");
            line = QStringLiteral("The YouTube Music session%1 has ended. Import a new one to sign back in; until "
                                  "then Monolist plays signed out.").arg(who);
            break;
        }
        break;
    }
    }
    // A rest is what matters most while it lasts: whatever the session's
    // standing, nothing is asked with it until the rest is over.
    if (m_guard.paused() && (m_state == State::Active || m_state == State::Checking || m_state == State::Unreachable)) {
        if (m_state != State::Active)
            headline = QStringLiteral("Resting");
        line = restLine().toHtmlEscaped();
    }
    m_headline = headline;
    m_statusLine = line;
    Q_EMIT changed();
}

// ---------------------------------------------------------------- importing

bool YtmSession::importFile(const QUrl &file)
{
    const QString path = file.isLocalFile() ? file.toLocalFile() : file.toString();
    const QString name = QFileInfo(path).fileName();
    QFile input(path);
    if (path.isEmpty() || !input.open(QIODevice::ReadOnly)) {
        m_importError = QStringLiteral("Monolist could not open %1: %2")
                            .arg(name.isEmpty() ? QStringLiteral("that file") : name, input.errorString());
        Q_EMIT changed();
        return false;
    }
    if (input.size() > kMaxFileBytes) {
        m_importError = QStringLiteral("%1 is far too large to be a cookies.txt file, which holds a few kilobytes. "
                                       "Choose the file the extension saved.").arg(name);
        Q_EMIT changed();
        return false;
    }
    const QByteArray bytes = input.readAll();
    input.close();
    if (!importResult(CookieImport::parse(bytes), QStringLiteral("the file ") + name))
        return false;
    // Read and kept; now the file is only a liability, and the user is
    // asked what to do with it. Never deleted without that.
    m_importedFile = QFileInfo(path).absoluteFilePath();
    Q_EMIT changed();
    return true;
}

bool YtmSession::importText(const QString &text)
{
    return importResult(CookieImport::parse(text.toUtf8()), QStringLiteral("pasted text"));
}

void YtmSession::clearImportError()
{
    if (m_importError.isEmpty())
        return;
    m_importError.clear();
    Q_EMIT changed();
}

bool YtmSession::importResult(const CookieImport::Result &result, const QString &source)
{
    if (!result.ok()) {
        m_importError = result.error;
        // The reason never holds a value; the counts are only counts.
        qInfo("ytmusic: %s is not a signed-in session (cookies read: %d, for YouTube Music: %d): %s",
              qPrintable(source), result.read, int(result.read - result.otherSites - result.elsewhere),
              qPrintable(result.error));
        Q_EMIT changed();
        return false;
    }

    const QByteArray json = CookieImport::toJson(result.cookies, result.info);
    QString error;
    const SecretStore::Status stored = SecretStore::write(kSecretName, json, &error);
    if (stored != SecretStore::Status::Ok && stored != SecretStore::Status::Unavailable) {
        m_importError = QStringLiteral("Monolist could not keep the sign-in: %1").arg(error);
        qWarning("ytmusic: could not store the imported session: %s", qPrintable(error));
        Q_EMIT changed();
        return false;
    }

    // A new session: whatever was under way belonged to the old one.
    ++m_generation;
    ++m_checkRun;
    forgetCheckState();
    m_checking = false;
    m_checkTimer.stop();
    m_saveTimer.stop();
    m_dirty = false;
    m_jar = result.cookies;
    // Only what the copy itself said: what an earlier session learned was
    // another session's.
    m_info = result.info;
    publishVisitor();
    m_memoryOnly = stored == SecretStore::Status::Unavailable;
    m_importError.clear();
    m_notice.clear();
    m_ended = Ended::Unknown;
    m_endedStatus = 0;
    m_retryAt = QDateTime();
    m_fresh = true;
    m_zeroes = 0;
    m_failures = 0;
    m_confirmed = false;
    // Doubts about the last session are not this one's.
    m_lastDoubt.invalidate();
    // Whose session this is, YouTube Music says; a name kept from before
    // may be another account's.
    m_name.clear();
    m_handle.clear();
    if (m_library) {
        m_library->setSetting(kNameKey, QString());
        m_library->setSetting(kHandleKey, QString());
    }
    qInfo("ytmusic: imported %s%s, from %s: %s; %s, %d bytes", qPrintable(result.summary()),
          qPrintable(describe(m_info)), qPrintable(source), qPrintable(joinedNames(m_jar)),
          m_memoryOnly ? "held in memory only (no secret store here)"
                       : qPrintable(QStringLiteral("kept encrypted (") + SecretStore::backendName() + QLatin1Char(')')),
          int(json.size()));
    setState(State::Checking);
    Q_EMIT sessionChanged();
    check();
    return true;
}

bool YtmSession::deleteImportedFile()
{
    if (m_importedFile.isEmpty())
        return false;
    const QString name = importedFileName();
    QFile file(m_importedFile);
    if (m_importedFile.startsWith(QLatin1String("demo:"))) {
        m_importedFile.clear();
        Q_EMIT notice(QStringLiteral("A demonstration: nothing was deleted."));
    } else if (!file.exists()) {
        m_importedFile.clear();
        Q_EMIT notice(QStringLiteral("%1 was already gone.").arg(name));
    } else if (file.remove()) {
        m_importedFile.clear();
        qInfo("ytmusic: deleted the exported file %s, as asked", qPrintable(name));
        Q_EMIT notice(QStringLiteral("Deleted %1.").arg(name));
    } else {
        // The offer stays, for a second try once whatever held it lets go.
        qWarning("ytmusic: could not delete the exported file %s: %s", qPrintable(name),
                 qPrintable(file.errorString()));
        Q_EMIT notice(QStringLiteral("Monolist could not delete %1: %2. Delete it yourself.")
                          .arg(name, file.errorString()));
        Q_EMIT changed();
        return false;
    }
    Q_EMIT changed();
    return true;
}

void YtmSession::keepImportedFile()
{
    if (m_importedFile.isEmpty())
        return;
    const QString name = importedFileName();
    m_importedFile.clear();
    Q_EMIT notice(QStringLiteral("Kept %1. It still holds your sign-in, so delete it once you are done with it.")
                      .arg(name));
    Q_EMIT changed();
}

void YtmSession::signOut()
{
    ++m_generation;
    ++m_checkRun;
    forgetCheckState();
    m_checking = false;
    m_checkTimer.stop();
    m_saveTimer.stop();
    m_dirty = false;
    const int held = int(m_jar.size());
    m_jar.clear();
    m_info = {};
    publishVisitor();
    QString error;
    const SecretStore::Status removed = SecretStore::remove(kSecretName, &error);
    const bool named = !m_name.isEmpty();
    m_name.clear();
    m_handle.clear();
    if (m_library) {
        m_library->setSetting(kNameKey, QString());
        m_library->setSetting(kHandleKey, QString());
    }
    m_ended = Ended::Unknown;
    m_endedStatus = 0;
    m_retryAt = QDateTime();
    m_fresh = false;
    m_importError.clear();
    m_zeroes = 0;
    m_failures = 0;
    m_confirmed = false;
    m_memoryOnly = false;
    if (removed == SecretStore::Status::Ok || removed == SecretStore::Status::Unavailable) {
        qInfo("ytmusic: signed out; Monolist's copy of the session is deleted (%d cookies)", held);
        // A session that had already ended has nothing left at Google to
        // sign out: only its name is forgotten. One that never signed
        // anyone in (an import answered as signed out) leaves nothing to say.
        m_notice = held > 0 ? signedOutNotice()
                 : named    ? QStringLiteral("Signed out. Monolist holds no sign-in now.")
                            : QString();
    } else {
        qWarning("ytmusic: signed out, but the stored session could not be deleted: %s", qPrintable(error));
        m_notice = QStringLiteral("Signed out, but Monolist could not delete its encrypted copy: %1")
                       .arg(error.toHtmlEscaped());
    }
    setState(State::SignedOut);
    Q_EMIT sessionChanged();
    Q_EMIT signedOut();
}

// ---------------------------------------------------------------- checking

void YtmSession::checkNow()
{
    if (m_jar.isEmpty())
        return;
    m_failures = 0;
    check();
}

void YtmSession::scheduleCheck(qint64 ms)
{
    m_checkTimer.start(timerMs(ms));
}

void YtmSession::check()
{
    if (m_jar.isEmpty() || m_checking)
        return;
    m_checkTimer.stop();
    // Resting: not even the check is asked. The rest's end starts one
    // (pausedChanged, in the constructor).
    if (m_guard.paused()) {
        qInfo("ytmusic: the account is resting (%s); the session is checked once the rest is over",
              qPrintable(m_guard.pauseReason()));
        // As well as the rest's own end (pausedChanged): a check waits for
        // it whatever wakes up first.
        scheduleCheck(m_guard.pausedUntil() - AccountGuard::now() + m_timing.launchCheckMs
                      + qint64(QRandomGenerator::global()->bounded(std::max(1, m_timing.launchCheckMs))));
        updateStatus();
        return;
    }
    m_checking = true;
    // Only the first check says Checking; a later one runs behind an active
    // session, which stays in use until there is a verdict against it.
    if (m_state != State::Active)
        setState(State::Checking);

    auto answers = std::make_shared<CheckAnswers>();
    answers->run = ++m_checkRun;
    const QPointer<YtmSession> self(this);
    const auto finish = [self, answers]() {
        if (self && ++answers->answered == 2)
            self->checkFinished(*answers);
    };
    qInfo("ytmusic: asking YouTube Music whether the session is signed in (%d cookies)", int(m_jar.size()));
    InnerTube *tube = innerTube();
    // The home feed's word on the session is taken over the menu's, whichever
    // answers first.
    tube->accountMenu(InnerTube::Auth::Checking, [answers, finish](const QJsonObject &root, const QString &error) {
        answers->menuError = error;
        answers->name = InnerTube::parseAccountName(root);
        answers->handle = InnerTube::parseAccountHandle(root);
        if (answers->visitorData.isEmpty())
            answers->visitorData = InnerTube::parseVisitorData(root);
        if (answers->dataSyncId.isEmpty())
            answers->dataSyncId = InnerTube::parseDataSyncId(root);
        finish();
    });
    tube->browse(QStringLiteral("FEmusic_home"), [answers, finish](const QJsonObject &root, const QString &error) {
        answers->homeError = error;
        answers->home = root;
        answers->loggedIn = InnerTube::parseLoggedIn(root);
        const QString visitor = InnerTube::parseVisitorData(root);
        const QString dataSync = InnerTube::parseDataSyncId(root);
        if (!visitor.isEmpty())
            answers->visitorData = visitor;
        if (!dataSync.isEmpty())
            answers->dataSyncId = dataSync;
        finish();
    }, InnerTube::Auth::Checking);
}

void YtmSession::learn(const CheckAnswers &answers)
{
    // Called only once the answers have confirmed the session, so they were
    // made with it (a check is never asked again without it: see
    // InnerTube::send); and only what looks as it should is kept.
    CookieImport::SessionInfo info = m_info;
    const QString visitor = CookieImport::visitorDataFrom(answers.visitorData);
    const QString dataSync = CookieImport::dataSyncIdFrom(answers.dataSyncId);
    if (!visitor.isEmpty())
        info.visitorData = visitor;
    if (!dataSync.isEmpty())
        info.dataSyncId = dataSync;
    if (info == m_info)
        return;
    QStringList what;
    if (info.visitorData != m_info.visitorData)
        what << QStringLiteral("visitor id");
    if (info.dataSyncId != m_info.dataSyncId) {
        what << (info.delegatedId().isEmpty() ? QStringLiteral("DATASYNC_ID (the account's own channel)")
                                              : QStringLiteral("DATASYNC_ID (a brand channel)"));
    }
    m_info = info;
    publishVisitor();
    qInfo("ytmusic: learned the session's %s from YouTube Music's answer",
          qPrintable(what.join(QStringLiteral(" and "))));
    // Kept with the cookies, so the next launch starts with them.
    m_dirty = true;
    if (!m_saveTimer.isActive())
        m_saveTimer.start(m_timing.saveDelayMs);
}

void YtmSession::checkFinished(const CheckAnswers &answers)
{
    // Signed out, refused or replaced while the answers were out.
    if (answers.run != m_checkRun)
        return;
    m_checking = false;
    if (m_jar.isEmpty())
        return;

    // logged_in=1 is the proof. When the feed does not say either way, the
    // account menu naming someone is proof enough: signed out, it only
    // offers to sign in.
    const bool named = answers.menuError.isEmpty() && !answers.name.isEmpty();
    if (answers.loggedIn == QLatin1String("1") || (answers.loggedIn.isEmpty() && named)) {
        m_zeroes = 0;
        m_failures = 0;
        m_retryAt = QDateTime();
        if (!answers.name.isEmpty() && answers.name != m_name) {
            m_name = answers.name;
            if (m_library)
                m_library->setSetting(kNameKey, m_name);
        }
        // The handle goes with the name it came with: a menu that named the
        // account but gave no handle leaves none shown.
        if (!answers.name.isEmpty() && answers.handle != m_handle) {
            m_handle = answers.handle;
            if (m_library)
                m_library->setSetting(kHandleKey, m_handle);
        }
        const bool news = m_state != State::Active;
        m_confirmed = true;
        m_fresh = false;
        qInfo("ytmusic: YouTube Music confirms the session (%s)%s",
              answers.loggedIn == QLatin1String("1") ? "logged_in=1" : "the account menu names it",
              m_name.isEmpty() ? "" : qPrintable(QStringLiteral(" as ") + m_name));
        learn(answers);
        // The feed this check was answered, for Home to take (Catalog)
        // rather than ask the account for it again; this session's alone,
        // and none at all when this check's feed did not say it was.
        if (answers.loggedIn == QLatin1String("1") && !answers.home.isEmpty()) {
            m_checkedHome = answers.home;
            m_checkedHomeAge.start();
            m_checkedHomeSession = m_generation;
        } else {
            m_checkedHome = {};
            m_checkedHomeAge.invalidate();
        }
        const bool gated = std::exchange(m_needsCheck, false);
        setState(State::Active);
        if (news)
            Q_EMIT sessionChanged();
        else if (gated)
            Q_EMIT accountUseChanged();   // usable again, the session unchanged
        scheduleCheck(jittered(m_timing.periodicMs, 1.0 / 12));
        Q_EMIT checked(QStringLiteral("active"));
        return;
    }

    if (answers.loggedIn == QLatin1String("0")) {
        // Once could be a hiccup on their side; twice in a row is an answer.
        if (++m_zeroes >= 2) {
            qWarning("ytmusic: YouTube Music answered logged_in=0 twice: the session is over");
            reject(Ended::SignedOut);
            Q_EMIT checked(QStringLiteral("rejected"));
            return;
        }
        qInfo("ytmusic: YouTube Music answered logged_in=0; asking once more in %d s before believing it",
              m_timing.recheckMs / 1000);
        updateStatus();   // still Checking, and now it says why it asks again
        scheduleCheck(m_timing.recheckMs);
        Q_EMIT checked(QStringLiteral("again"));
        return;
    }

    // No verdict: no answer, or one that says nothing. Proof of nothing, so
    // the session is kept and asked about again later.
    ++m_failures;
    const int doublings = qBound(0, m_failures - 1, 16);
    const qint64 wait =
        jittered(std::min<qint64>(qint64(m_timing.retryMinMs) << doublings, m_timing.retryMaxMs), 0.25);
    const QString why = !answers.homeError.isEmpty() ? answers.homeError
                      : !answers.menuError.isEmpty() ? answers.menuError
                                                     : QStringLiteral("the answers did not say");
    qInfo("ytmusic: could not check the session (%s); asking again in %lld s", qPrintable(why),
          (long long)(wait / 1000));
    // Said on the row, so it is set before the state is.
    m_retryAt = QDateTime::currentDateTime().addMSecs(wait);
    // One confirmed earlier in this run stays in use through a blip.
    if (!m_confirmed || m_state != State::Active)
        setState(State::Unreachable);
    scheduleCheck(wait);
    watchNetwork();
    Q_EMIT checked(QStringLiteral("unreachable"));
}

void YtmSession::reject(Ended why, int httpStatus)
{
    ++m_generation;
    ++m_checkRun;
    forgetCheckState();
    m_checking = false;
    m_checkTimer.stop();
    m_saveTimer.stop();
    m_dirty = false;
    m_jar.clear();
    m_info = {};
    publishVisitor();
    QString error;
    const SecretStore::Status removed = SecretStore::remove(kSecretName, &error);
    if (removed != SecretStore::Status::Ok && removed != SecretStore::Status::Unavailable)
        qWarning("ytmusic: could not delete the refused session: %s", qPrintable(error));
    // m_fresh is left as it was: whether this session had ever been
    // confirmed is what the row's words depend on.
    m_ended = why;
    m_endedStatus = httpStatus;
    m_retryAt = QDateTime();
    m_zeroes = 0;
    m_failures = 0;
    m_confirmed = false;
    setState(State::Rejected);
    Q_EMIT sessionChanged();
}

// When the system says the network is back, a check that could not be made
// is made then, rather than at the end of its wait.
void YtmSession::watchNetwork()
{
    if (m_watchingNetwork)
        return;
    m_watchingNetwork = true;
    if (!QNetworkInformation::loadBackendByFeatures(QNetworkInformation::Feature::Reachability))
        return;
    connect(QNetworkInformation::instance(), &QNetworkInformation::reachabilityChanged, this,
            [this](QNetworkInformation::Reachability reachability) {
                if (reachability == QNetworkInformation::Reachability::Online && m_state == State::Unreachable
                    && !m_jar.isEmpty()) {
                    m_failures = 0;
                    scheduleCheck(2000);
                }
            });
}

// ---------------------------------------------------------------- the hook

QByteArray YtmSession::sidHash(const QByteArray &scheme, qint64 timestamp, const QByteArray &sid,
                               const QByteArray &origin)
{
    const QByteArray stamp = QByteArray::number(timestamp);
    const QByteArray hash =
        QCryptographicHash::hash(stamp + ' ' + sid + ' ' + origin, QCryptographicHash::Sha1).toHex();
    return scheme + ' ' + stamp + '_' + hash;
}

QByteArray YtmSession::authorization(const QList<Cookie> &cookies, qint64 timestamp, const QByteArray &origin)
{
    const QByteArray sapisid = CookieImport::value(cookies, "SAPISID");
    const QByteArray firstParty = CookieImport::value(cookies, "__Secure-1PAPISID");
    const QByteArray thirdParty = CookieImport::value(cookies, "__Secure-3PAPISID");
    // Without SAPISID, YouTube's own pages hash the third-party one in its
    // place (yt-dlp #393).
    const QByteArray primary = sapisid.isEmpty() ? thirdParty : sapisid;
    QList<QByteArray> parts;
    if (!primary.isEmpty())
        parts << sidHash("SAPISIDHASH", timestamp, primary, origin);
    if (!firstParty.isEmpty())
        parts << sidHash("SAPISID1PHASH", timestamp, firstParty, origin);
    if (!thirdParty.isEmpty())
        parts << sidHash("SAPISID3PHASH", timestamp, thirdParty, origin);
    return parts.join(' ');
}

AccountGuard::Kind YtmSession::kindOf(const InnerTube::AccountRequest &request)
{
    if (request.auth == InnerTube::Auth::Checking)
        return AccountGuard::Kind::Check;
    // A listen report is two calls, /player for the address and the report
    // itself; the first is what the listen share counts.
    if (request.path.endsWith(QLatin1String("/player")))
        return AccountGuard::Kind::Listen;
    return AccountGuard::Kind::Browse;
}

qint64 YtmSession::admit(const InnerTube::AccountRequest &request, QString *why)
{
    // A call that would not carry the account (authHeaders) goes as it
    // would have: nothing to wait for, nothing to count.
    if (m_jar.isEmpty() || request.auth == InnerTube::Auth::Anonymous)
        return 0;
    if (request.auth != InnerTube::Auth::Checking && m_state != State::Active)
        return 0;
    // After a rest or a doubt, the check first: until it confirms the
    // session again, nothing else goes with it.
    if (m_needsCheck && request.auth != InnerTube::Auth::Checking) {
        if (why)
            *why = QStringLiteral("the session is checked again first");
        return -1;
    }
    const qint64 wait = m_guard.admit(kindOf(request), why);
    if (wait < 0 && why)
        qInfo("ytmusic: a %s not asked with the account: %s", AccountGuard::name(kindOf(request)), qPrintable(*why));
    return wait;
}

void YtmSession::callFinished(const InnerTube::AccountRequest &request, const InnerTube::AccountOutcome &outcome)
{
    const AccountGuard::Kind kind = kindOf(request);
    m_guard.finished(kind, outcome.status, outcome.retryAfterSecs, outcome.ticket);
    if (outcome.slowDown) {
        m_guard.trip(QStringLiteral("YouTube answered a %1 with the account by asking to confirm it is not a bot")
                         .arg(QLatin1String(AccountGuard::name(kind))),
                     outcome.retryAfterSecs * 1000);
    } else if (outcome.status == 403) {
        // Refused, but not for the credentials (that is 401): the account is
        // rested, and then checked.
        m_guard.trip(QStringLiteral("YouTube refused a %1 with the account (HTTP 403)")
                         .arg(QLatin1String(AccountGuard::name(kind))),
                     outcome.retryAfterSecs * 1000);
    }
}

quint64 YtmSession::authHeaders(const InnerTube::AccountRequest &request, InnerTube::AccountHeaders *headers)
{
    if (m_jar.isEmpty())
        return 0;
    switch (request.auth) {
    case InnerTube::Auth::Anonymous:
        return 0;
    case InnerTube::Auth::IfSignedIn:
    case InnerTube::Auth::Required:
        if (m_state != State::Active)
            return 0;
        break;
    case InnerTube::Auth::Checking:
        break;   // any session held: the check is what decides
    }
    // What a browser would send to that host and path, and nothing else: a
    // cookie for www.youtube.com alone never reaches YouTube Music.
    const qint64 now = nowSecs();
    const QList<Cookie> sent = CookieImport::forRequest(m_jar, request.host, request.path, now);
    if (sent.isEmpty())
        return 0;
    headers->cookie = CookieImport::header(sent, request.host, request.path, now);
    headers->authorization = YtmSession::authorization(sent, now, request.origin);
    headers->authUser = QByteArray::number(m_info.authUser >= 0 ? m_info.authUser : 0);
    headers->onBehalfOfUser = m_info.delegatedId();
    return m_generation;
}

void YtmSession::reportRejected(quint64 session, int httpStatus)
{
    if (session == 0 || session != m_generation || m_jar.isEmpty())
        return;
    const bool wasChecking = m_checking;
    qWarning("ytmusic: YouTube Music refused the session (HTTP %d); its cookies are no longer sent", httpStatus);
    reject(Ended::Refused, httpStatus);
    if (wasChecking)
        Q_EMIT checked(QStringLiteral("rejected"));
}

void YtmSession::absorbCookies(quint64 session, const QString &host, const QList<QNetworkCookie> &cookies,
                               const char *from)
{
    if (session == 0 || session != m_generation || m_jar.isEmpty() || host.isEmpty())
        return;
    const qint64 now = nowSecs();
    QStringList updated;
    QStringList removed;
    for (const QNetworkCookie &received : cookies) {
        Cookie cookie;
        cookie.name = received.name();
        cookie.value = received.value();
        QString domain = received.domain().toLower();
        if (domain.isEmpty()) {
            // No Domain attribute: the host that answered, and only it.
            domain = host;
            cookie.hostOnly = true;
        } else if (domain.startsWith(QLatin1Char('.'))) {
            domain.remove(0, 1);
        }
        cookie.domain = domain;
        cookie.path = received.path().isEmpty() ? QStringLiteral("/") : received.path();
        cookie.secure = received.isSecure();
        cookie.httpOnly = received.isHttpOnly();
        const QDateTime expiry = received.expirationDate();
        cookie.expires = expiry.isValid() ? expiry.toSecsSinceEpoch() : 0;
        // A browser ignores a Domain the answering host does not belong to
        // (RFC 6265 5.3, step 6): music.youtube.com may set a cookie for
        // youtube.com, never for www.youtube.com.
        const bool mayBeSet = cookie.hostOnly || host == domain || host.endsWith(QLatin1Char('.') + domain);
        if (cookie.name.isEmpty() || !mayBeSet || !CookieImport::isYouTubeDomain(cookie.domain)
            || !CookieImport::sentToAccountHosts(cookie))
            continue;

        // The same cookie is the same name, domain and path; another domain
        // or path makes another cookie, kept beside it.
        int index = -1;
        for (int i = 0; i < m_jar.size(); ++i) {
            const Cookie &held = m_jar.at(i);
            if (held.name == cookie.name && held.domain == cookie.domain && held.hostOnly == cookie.hostOnly
                && held.path == cookie.path) {
                index = i;
                break;
            }
        }
        // An expiry in the past is the server deleting the cookie.
        if (expiry.isValid() && cookie.expires <= now) {
            if (index >= 0) {
                m_jar.removeAt(index);
                removed << QString::fromLatin1(cookie.name);
            }
            continue;
        }
        if (index >= 0) {
            const Cookie &old = m_jar.at(index);
            if (old.value == cookie.value && old.expires == cookie.expires && old.domain == cookie.domain
                && old.path == cookie.path)
                continue;
            m_jar[index] = cookie;
        } else if (m_jar.size() < kMaxJar) {
            m_jar.append(cookie);
        } else {
            continue;
        }
        updated << QString::fromLatin1(cookie.name);
    }
    if (updated.isEmpty() && removed.isEmpty())
        return;
    m_dirty = true;
    // At most this long after the first change not yet saved.
    if (!m_saveTimer.isActive())
        m_saveTimer.start(m_timing.saveDelayMs);
    qInfo("ytmusic: %s rotated the session: %d updated (%s), %d removed%s; saving within %d s", from,
          int(updated.size()), qPrintable(updated.join(QStringLiteral(", "))), int(removed.size()),
          removed.isEmpty() ? "" : qPrintable(QStringLiteral(" (") + removed.join(QStringLiteral(", ")) + QLatin1Char(')')),
          m_timing.saveDelayMs / 1000);
}

void YtmSession::save()
{
    m_saveTimer.stop();
    if (!m_dirty || m_jar.isEmpty() || m_memoryOnly) {
        m_dirty = false;
        return;
    }
    m_dirty = false;
    const QByteArray json = CookieImport::toJson(m_jar, m_info);
    QString error;
    if (SecretStore::write(kSecretName, json, &error) == SecretStore::Status::Ok)
        qInfo("ytmusic: saved the session again (%d cookies%s, %d bytes)", int(m_jar.size()),
              qPrintable(describe(m_info)), int(json.size()));
    else
        qWarning("ytmusic: could not save the rotated session: %s", qPrintable(error));
}

// ---------------------------------------------------------------- playing with the account

bool YtmSession::accountForPlayback(QString *why) const
{
    const auto because = [why](const char *reason) {
        if (why)
            *why = QString::fromLatin1(reason);
        return false;
    };
    if (m_jar.isEmpty())
        return because("no YouTube Music session is signed in");
    if (m_state != State::Active)
        return because("the YouTube Music session is not confirmed yet");
    if (!m_playWhenNeeded)
        return because("\"Play with my account when needed\" is off");
    if (m_guard.paused())
        return because("YouTube asked Monolist to slow down, and the account is resting");
    if (m_needsCheck)
        return because("the session is checked again first");
    return true;
}

QString YtmSession::cookieFolder()
{
    // Local, never roaming: a copy of the session has no business being
    // carried to other computers with a Windows profile.
    QString base = qEnvironmentVariable("MONOLIST_DATA_DIR");
    if (base.isEmpty())
        base = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    return base.isEmpty() ? QString() : QDir(base).filePath(kCookieFolderName);
}

bool YtmSession::onNetworkDrive(const QString &path)
{
    if (path.isEmpty())
        return false;
    const QString absolute = QFileInfo(path).absoluteFilePath();
#ifdef Q_OS_WIN
    const QString native = QDir::toNativeSeparators(absolute);
    if (native.startsWith(QLatin1String("\\\\")))
        return true;   // \\server\share, the Parallels share (\\psf) among them
    if (native.size() < 2 || native.at(1) != QLatin1Char(':'))
        return false;
    const std::wstring root = native.left(2).toStdWString() + L"\\";
    return GetDriveTypeW(root.c_str()) == DRIVE_REMOTE;   // a drive letter mapped to a share
#else
    // The volume the folder, or the nearest part of it that exists, is on.
    QString existing = absolute;
    while (!existing.isEmpty() && !QFileInfo::exists(existing)) {
        const QString up = QFileInfo(existing).absolutePath();
        if (up == existing)
            break;
        existing = up;
    }
    const QStorageInfo volume(existing);
    if (!volume.isValid())
        return false;
    static const QStringList remote{ QStringLiteral("nfs"),   QStringLiteral("nfs4"),     QStringLiteral("smbfs"),
                                     QStringLiteral("cifs"),  QStringLiteral("smb3"),     QStringLiteral("afpfs"),
                                     QStringLiteral("webdav"), QStringLiteral("davfs"),   QStringLiteral("fuse.sshfs"),
                                     QStringLiteral("9p"),    QStringLiteral("afs"),      QStringLiteral("ceph"),
                                     QStringLiteral("glusterfs") };
    return remote.contains(QString::fromLatin1(volume.fileSystemType()).toLower());
#endif
}

int YtmSession::sweepCookieFiles(int sparedSecs)
{
    const QString folder = cookieFolder();
    if (folder.isEmpty())
        return 0;
    const QDir dir(folder);
    if (!dir.exists())
        return 0;
    const QDateTime spare = QDateTime::currentDateTimeUtc().addSecs(-qMax(0, sparedSecs));
    int gone = 0;
    int kept = 0;
    for (const QFileInfo &file : dir.entryInfoList({ kCookieFilePattern }, QDir::Files | QDir::Hidden | QDir::System)) {
        if (sparedSecs > 0 && file.lastModified().toUTC() > spare) {
            ++kept;
            continue;
        }
        if (QFile::remove(file.absoluteFilePath()))
            ++gone;
    }
    if (gone > 0 || kept > 0)
        qInfo("ytmusic: deleted %d cookies file(s) an earlier run lent yt-dlp%s", gone,
              kept > 0 ? qPrintable(QStringLiteral("; %1 written in the last %2 s left, which may be another "
                                                   "running copy's").arg(kept).arg(sparedSecs))
                       : "");
    return gone;
}

quint64 YtmSession::openCookieFile(QString *path, QString *error)
{
    path->clear();
    QString why;
    if (!accountForPlayback(&why)) {
        *error = why;
        return 0;
    }
    // The account's turn, as for every call with it; a lookup is several
    // requests, and the guard counts it so. A song is never held up for it:
    // refused or not yet, it goes on down the signed-out ladder.
    {
        QString why;
        const qint64 wait = m_guard.admit(AccountGuard::Kind::YtDlp, &why);
        if (wait != 0) {
            *error = wait < 0 ? why : QStringLiteral("another call with the account is under way");
            return 0;
        }
    }
    int written = 0;
    const QByteArray text = CookieImport::toNetscape(m_jar, 0, &written);
    if (written == 0) {
        *error = QStringLiteral("the session has no youtube.com cookies to lend yt-dlp");
        return 0;
    }
    const QString folder = cookieFolder();
    // Never onto a share: the file is the whole session in plain text, where
    // the copy SecretStore keeps beside it is encrypted.
    if (onNetworkDrive(folder)) {
        *error = QStringLiteral("the data folder is on a network drive, where the session's cookies are not written "
                                "in plain text");
        qWarning("ytmusic: not lending yt-dlp the session: %s is on a network drive",
                 qPrintable(QDir::toNativeSeparators(folder)));
        return 0;
    }
    if (folder.isEmpty() || !QDir().mkpath(folder)) {
        *error = QStringLiteral("there is nowhere to write the cookies for yt-dlp");
        return 0;
    }
#ifdef Q_OS_UNIX
    // This user's alone, the folder as well as the file.
    QFile::setPermissions(folder, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
#endif
    // A name nobody can guess, and a file made new: never one opened over
    // something already there.
    const QString name = QStringLiteral("cookies-%1.txt")
                             .arg(QRandomGenerator::system()->generate64(), 16, 16, QLatin1Char('0'));
    const QString file = QDir(folder).filePath(name);
    QFile out(file);
    if (!out.open(QIODevice::WriteOnly | QIODevice::NewOnly, QFileDevice::ReadOwner | QFileDevice::WriteOwner)
        || out.write(text) != text.size() || !out.flush()) {
        *error = QStringLiteral("could not write the cookies for yt-dlp: %1").arg(out.errorString());
        out.close();
        QFile::remove(file);
        return 0;
    }
    out.close();
    m_cookieFiles.insert(file);
    m_lent.insert(file, m_jar);
    m_lentTickets.insert(file, m_guard.started(AccountGuard::Kind::YtDlp));
    *path = file;
    qInfo("ytmusic: lent yt-dlp the session's %d youtube.com cookies for one lookup (%s)", written, qPrintable(name));
    return m_generation;
}

void YtmSession::closeCookieFile(quint64 session, const QString &path, bool readBack)
{
    if (path.isEmpty())
        return;
    // The lookup is over, and with it the account's turn.
    const QList<Cookie> lent = m_lent.take(path);
    m_guard.finished(AccountGuard::Kind::YtDlp, 0, 0, m_lentTickets.take(path));
    const QString name = QFileInfo(path).fileName();
    QByteArray text;
    if (readBack) {
        QFile in(path);
        if (in.open(QIODevice::ReadOnly) && in.size() <= kMaxFileBytes)
            text = in.readAll();
        else
            qInfo("ytmusic: yt-dlp's cookies file %s could not be read back: %s", qPrintable(name),
                  qPrintable(in.errorString()));
    }
    // Gone first: it is a copy of the session in plain text, and nothing
    // below needs it on disk.
    deleteCookieFile(path);
    if (text.isEmpty())
        return;
    if (session == 0 || session != m_generation || m_jar.isEmpty()) {
        qInfo("ytmusic: yt-dlp's cookies were for a session that has since ended or changed; nothing is taken "
              "from them");
        return;
    }
    const CookieImport::Result back = CookieImport::parse(text);
    if (!back.ok()) {
        // The error names what is missing, never a value.
        qInfo("ytmusic: what yt-dlp wrote back is not a signed-in session (%s); nothing is taken from it",
              qPrintable(back.error));
        if (back.missing.contains(QStringLiteral("LOGIN_INFO")))
            doubt(QStringLiteral("yt-dlp's lookup ended without the session's LOGIN_INFO"));
        return;
    }
    // Only what changed, and only what yt-dlp could have been sent: it talks
    // to www.youtube.com, so a cookie only another host could set is one it
    // was lent, whatever it wrote. A cookie missing from what it wrote is
    // not taken as deleted: the server deleting it is the only reason worth
    // following, and a jar that lost a line some other way must not cost
    // the session a cookie.
    // Compared with what it was lent rather than with the jar now: a cookie
    // another call rotated while yt-dlp ran is newer than the one yt-dlp
    // hands back unchanged, and must not be put back to the old value.
    const QList<Cookie> &before = lent.isEmpty() ? m_jar : lent;
    QList<QNetworkCookie> changed;
    for (const Cookie &cookie : back.cookies) {
        if (cookie.hostOnly && cookie.domain != CookieImport::kWwwHost)
            continue;
        bool same = false;
        for (const Cookie &held : before) {
            if (held.name == cookie.name && held.domain == cookie.domain && held.hostOnly == cookie.hostOnly
                && held.path == cookie.path) {
                same = held.value == cookie.value && held.expires == cookie.expires;
                break;
            }
        }
        if (same)
            continue;
        QNetworkCookie received(cookie.name, cookie.value);
        // As a Set-Cookie would say it: a Domain attribute exactly when the
        // cookie is not the answering host's alone.
        if (!cookie.hostOnly)
            received.setDomain(QLatin1Char('.') + cookie.domain);
        received.setPath(cookie.path);
        received.setSecure(cookie.secure);
        received.setHttpOnly(cookie.httpOnly);
        if (cookie.expires > 0)
            received.setExpirationDate(QDateTime::fromSecsSinceEpoch(cookie.expires, QTimeZone::UTC));
        changed << received;
    }
    qInfo("ytmusic: yt-dlp gave back %d cookies, %d of them new or changed", int(back.cookies.size()),
          int(changed.size()));
    if (!changed.isEmpty())
        absorbCookies(session, CookieImport::kWwwHost, changed, "yt-dlp's lookup");
}

void YtmSession::deleteCookieFile(const QString &path, int attempt)
{
    if (!QFile::exists(path) || QFile::remove(path)) {
        m_cookieFiles.remove(path);
        return;
    }
    if (attempt + 1 >= kDeleteAttempts) {
        qWarning("ytmusic: could not delete yt-dlp's cookies file %s; the next launch will",
                 qPrintable(QFileInfo(path).fileName()));
        return;
    }
    // Most likely yt-dlp, or a process it started, still has it open while
    // it is being ended.
    QTimer::singleShot(1000, this, [this, path, attempt]() { deleteCookieFile(path, attempt + 1); });
}

void YtmSession::doubt(const QString &why)
{
    if (m_jar.isEmpty())
        return;
    if (m_lastDoubt.isValid() && m_lastDoubt.elapsed() < kDoubtEveryMs) {
        qInfo("ytmusic: %s; the session was checked for that %lld s ago, and is not asked again yet", qPrintable(why),
              (long long)(m_lastDoubt.elapsed() / 1000));
        return;
    }
    m_lastDoubt.start();
    qInfo("ytmusic: %s; checking the session now", qPrintable(why));
    // Until the check has answered, nothing else is asked with a session
    // YouTube has just refused something of.
    m_needsCheck = true;
    checkNow();
}

// ---------------------------------------------------------------- listens

void YtmSession::listenQualified(const QVariantMap &track, qint64 startedAt, bool chosenByUser)
{
    Q_UNUSED(startedAt)
    Q_UNUSED(chosenByUser)
    // A YouTube video id, or nothing to report: a file of the user's own.
    static const QRegularExpression videoIdShape(QStringLiteral("^[A-Za-z0-9_-]{11}$"));
    const QString videoId = track.value(QStringLiteral("sourceId")).toString();
    if (!videoIdShape.match(videoId).hasMatch())
        return;
    if (!m_reportListens || m_state != State::Active || m_jar.isEmpty() || m_guard.paused() || m_needsCheck) {
        // Said only where there is a session the listen would have gone to.
        if (m_reportListens && !m_jar.isEmpty()) {
            qInfo("ytmusic: %s not reported to YouTube history: %s", qPrintable(videoId),
                  m_guard.paused() ? "the account is resting" : "the session is not confirmed");
        }
        Q_EMIT listenReported(videoId, QStringLiteral("skipped"));
        return;
    }
    reportListen(videoId);
}

void YtmSession::reportListen(const QString &videoId)
{
    const quint64 session = m_generation;
    const QPointer<YtmSession> self(this);
    QElapsedTimer clock;
    clock.start();
    innerTube()->playbackTracking(videoId, [self, videoId, session, clock](const QString &url, const QString &error) {
        if (!self)
            return;
        if (url.isEmpty()) {
            qInfo("ytmusic: %s not reported to YouTube history: %s", qPrintable(videoId), qPrintable(error));
            Q_EMIT self->listenReported(videoId, QStringLiteral("failed"));
            return;
        }
        // Signed out, or another account, while the address was asked for.
        if (session != self->m_generation || !self->m_reportListens) {
            Q_EMIT self->listenReported(videoId, QStringLiteral("skipped"));
            return;
        }
        // The host, never the address: it names this visit of the account.
        const QString host = QUrl(url).host();
        self->innerTube()->reportPlayback(url, [self, videoId, host, clock](int status, const QString &error) {
            if (!self)
                return;
            if (!error.isEmpty()) {
                qInfo("ytmusic: %s not reported to YouTube history: %s (%s)", qPrintable(videoId), qPrintable(error),
                      qPrintable(host));
                Q_EMIT self->listenReported(videoId, QStringLiteral("failed"));
                return;
            }
            qInfo("ytmusic: %s reported to the account's YouTube history (%s, HTTP %d, %lld ms)", qPrintable(videoId),
                  qPrintable(host), status, (long long)clock.elapsed());
            Q_EMIT self->listenReported(videoId, QStringLiteral("reported"));
        });
    });
}

// ---------------------------------------------------------------- demonstration

void YtmSession::showDemo(const QString &demo)
{
    ++m_generation;
    ++m_checkRun;
    forgetCheckState();
    m_demo = true;
    m_checking = false;
    m_checkTimer.stop();
    m_jar.clear();   // nothing to send, so nothing ever is
    m_info = {};
    publishVisitor();
    QString which = demo;
    if (which.endsWith(QLatin1String("+file"))) {
        which.chop(int(qstrlen("+file")));
        // Not a path: deleting it only says it was a demonstration.
        m_importedFile = QStringLiteral("demo:cookies-music.youtube.com.txt");
    }
    m_name = QStringLiteral("Demo Listener");
    m_handle = QStringLiteral("@demolistener");
    m_confirmed = which == QLatin1String("active");
    m_fresh = false;
    m_ended = Ended::Unknown;
    m_endedStatus = 0;
    m_retryAt = QDateTime();
    m_notice.clear();
    if (which == QLatin1String("active")) {
        setState(State::Active);
    } else if (which == QLatin1String("checking")) {
        setState(State::Checking);
    } else if (which == QLatin1String("unreachable")) {
        m_retryAt = QDateTime::currentDateTime().addMSecs(m_timing.retryMinMs);
        setState(State::Unreachable);
    } else if (which == QLatin1String("rejected")) {
        m_ended = Ended::Refused;
        m_endedStatus = 401;
        setState(State::Rejected);
    } else if (which == QLatin1String("notsignedin")) {
        // Straight after an import: YouTube Music has not named anyone.
        m_name.clear();
        m_handle.clear();
        m_fresh = true;
        m_ended = Ended::SignedOut;
        setState(State::Rejected);
    } else if (which == QLatin1String("unreadable")) {
        m_ended = Ended::WontOpen;
        setState(State::Rejected);
    } else {
        m_name.clear();
        m_handle.clear();
        if (which == QLatin1String("signedout"))
            m_notice = signedOutNotice();
        setState(State::SignedOut);
    }
    // Any session held before is gone, so nothing goes as the account now.
    Q_EMIT sessionChanged();
    qInfo("ytmusic: showing a demonstration: %s (no cookies held, nothing sent)", qPrintable(state()));
}
