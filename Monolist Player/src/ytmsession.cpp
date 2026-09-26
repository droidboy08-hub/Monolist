#include "ytmsession.h"

#include "library.h"
#include "secretstore.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QNetworkCookie>
#include <QNetworkInformation>

#include <algorithm>
#include <limits>
#include <memory>

namespace {

// The jar's name in SecretStore, and the one setting: the account's name.
// Nothing secret is ever written to settings.
const QString kSecretName = QStringLiteral("ytmusic.cookies");
const QString kNameKey = QStringLiteral("ytmusic.account_name");

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

QString joinedNames(const QList<Cookie> &jar)
{
    return CookieImport::names(jar).join(QStringLiteral(", "));
}

}

const QByteArray YtmSession::kOrigin = QByteArrayLiteral("https://music.youtube.com");

// What one check heard back: the account menu and the home feed, asked side
// by side, decided on together once both have answered.
struct YtmSession::CheckAnswers {
    quint64 run = 0;
    int answered = 0;
    QString name;
    QString loggedIn;
    QString menuError;
    QString homeError;
};

YtmSession::YtmSession(Library *library, QObject *parent)
    : QObject(parent)
    , m_library(library)
{
    m_checkTimer.setSingleShot(true);
    connect(&m_checkTimer, &QTimer::timeout, this, &YtmSession::check);
    m_saveTimer.setSingleShot(true);
    connect(&m_saveTimer, &QTimer::timeout, this, &YtmSession::save);
    updateStatus();
}

YtmSession::~YtmSession()
{
    // Cookies rotated in the last half-minute are kept, not lost with the
    // process.
    if (m_dirty)
        save();
    if (m_hooked)
        InnerTube::setAccountHook({});
    // Before the members its answers would reach.
    delete m_innerTube.data();
}

void YtmSession::installHook()
{
    const QPointer<YtmSession> self(this);
    InnerTube::AccountHook hook;
    hook.headers = [self](InnerTube::Auth auth, const QByteArray &origin, QByteArray *cookie,
                          QByteArray *authorization) -> quint64 {
        return self ? self->authHeaders(auth, origin, cookie, authorization) : 0;
    };
    hook.rejected = [self](quint64 session, int status) {
        if (self)
            self->reportRejected(session, status);
    };
    hook.cookies = [self](quint64 session, const QList<QNetworkCookie> &cookies) {
        if (self)
            self->absorbCookies(session, cookies);
    };
    InnerTube::setAccountHook(std::move(hook));
    m_hooked = true;
}

void YtmSession::setTiming(const Timing &timing)
{
    m_timing = timing;
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
    const QString remembered = m_library ? m_library->settingValue(kNameKey) : QString();
    QByteArray json;
    QString error;
    switch (SecretStore::read(kSecretName, &json, &error)) {
    case SecretStore::Status::Ok: {
        QList<Cookie> jar;
        m_name = remembered;
        if (!CookieImport::fromJson(json, &jar) || !CookieImport::missingRequired(jar).isEmpty()) {
            qWarning("ytmusic: the stored session is not one Monolist can use; it is deleted");
            SecretStore::remove(kSecretName);
            m_rejectReason = QStringLiteral("the stored copy could not be read");
            setState(State::Rejected);
            break;
        }
        m_jar = jar;
        setState(State::Checking);
        qInfo("ytmusic: restored a session of %d cookies (%s); checking it in %d s", int(m_jar.size()),
              qPrintable(joinedNames(m_jar)), m_timing.launchCheckMs / 1000);
        scheduleCheck(m_timing.launchCheckMs);
        break;
    }
    case SecretStore::Status::NotFound:
        // A name with no session: it was refused, and the row says so until
        // the user imports again or signs out.
        m_name = remembered;
        setState(remembered.isEmpty() ? State::SignedOut : State::Rejected);
        break;
    case SecretStore::Status::Corrupt:
        // Monolist's own copy, and it will never open again.
        qWarning("ytmusic: the stored session does not open (%s); it is deleted", qPrintable(error));
        SecretStore::remove(kSecretName);
        m_name = remembered;
        m_rejectReason = QStringLiteral("the stored copy does not open on this computer");
        setState(State::Rejected);
        break;
    case SecretStore::Status::Unavailable:
        // Nothing was ever kept, so no name either.
        if (!remembered.isEmpty() && m_library)
            m_library->setSetting(kNameKey, QString());
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

// Styled text: what came from outside (the account's name, an error) is
// escaped where it is put in.
void YtmSession::updateStatus()
{
    const QString who = m_name.isEmpty() ? QString() : QStringLiteral(" for %1").arg(m_name.toHtmlEscaped());
    QString line;
    switch (m_state) {
    case State::SignedOut:
        line = m_notice;
        break;
    case State::Checking:
        line = QStringLiteral("Checking the sign-in%1 with YouTube Music…").arg(who);
        break;
    case State::Active:
        line = QStringLiteral("YouTube Music confirms the sign-in. Nothing uses it yet: your own Home and "
                              "library come next, and playback, search, lyrics and radio always stay signed out.");
        if (m_memoryOnly) {
            line += QStringLiteral(" It is forgotten when Monolist closes: %1")
                        .arg(SecretStore::unavailableReason().toHtmlEscaped());
        }
        break;
    case State::Unreachable:
        line = QStringLiteral("YouTube Music could not be reached to check the sign-in%1, so it is not used "
                              "yet. Monolist asks again later, and plays signed out meanwhile.").arg(who);
        break;
    case State::Rejected:
        line = QStringLiteral("Your YouTube Music session%1 has ended%2. Import it again to sign back in; "
                              "Monolist plays on signed out.")
                   .arg(who, m_rejectReason.isEmpty() ? QString()
                                                      : QStringLiteral(": ") + m_rejectReason.toHtmlEscaped());
        break;
    }
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
        m_importError = QStringLiteral("%1 is far too large to be a cookies file.").arg(name);
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

    const QByteArray json = CookieImport::toJson(result.cookies);
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
    m_checking = false;
    m_checkTimer.stop();
    m_saveTimer.stop();
    m_dirty = false;
    m_jar = result.cookies;
    m_memoryOnly = stored == SecretStore::Status::Unavailable;
    m_importError.clear();
    m_notice.clear();
    m_rejectReason.clear();
    m_zeroes = 0;
    m_failures = 0;
    m_confirmed = false;
    // Whose session this is, YouTube Music says; a name kept from before
    // may be another account's.
    m_name.clear();
    if (m_library)
        m_library->setSetting(kNameKey, QString());
    qInfo("ytmusic: imported %s, from %s: %s; %s, %d bytes", qPrintable(result.summary()), qPrintable(source),
          qPrintable(joinedNames(m_jar)),
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
    m_checking = false;
    m_checkTimer.stop();
    m_saveTimer.stop();
    m_dirty = false;
    const int held = int(m_jar.size());
    m_jar.clear();
    QString error;
    const SecretStore::Status removed = SecretStore::remove(kSecretName, &error);
    m_name.clear();
    if (m_library)
        m_library->setSetting(kNameKey, QString());
    m_rejectReason.clear();
    m_importError.clear();
    m_zeroes = 0;
    m_failures = 0;
    m_confirmed = false;
    m_memoryOnly = false;
    if (removed == SecretStore::Status::Ok || removed == SecretStore::Status::Unavailable) {
        qInfo("ytmusic: signed out; Monolist's copy of the session is deleted (%d cookies)", held);
        m_notice = QStringLiteral("Signed out, and Monolist's encrypted copy is deleted. The session itself "
                                  "lasts at Google until it expires; to end it now, sign it out under "
                                  "<a href=\"https://myaccount.google.com/device-activity\">your Google "
                                  "Account's devices</a>.");
    } else {
        qWarning("ytmusic: signed out, but the stored session could not be deleted: %s", qPrintable(error));
        m_notice = QStringLiteral("Signed out, but Monolist could not delete its encrypted copy: %1")
                       .arg(error.toHtmlEscaped());
    }
    setState(State::SignedOut);
    Q_EMIT sessionChanged();
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
    tube->accountMenu(InnerTube::Auth::Checking, [answers, finish](const QJsonObject &root, const QString &error) {
        answers->menuError = error;
        answers->name = InnerTube::parseAccountName(root);
        finish();
    });
    tube->browse(QStringLiteral("FEmusic_home"), [answers, finish](const QJsonObject &root, const QString &error) {
        answers->homeError = error;
        answers->loggedIn = InnerTube::parseLoggedIn(root);
        finish();
    }, InnerTube::Auth::Checking);
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
        if (!answers.name.isEmpty() && answers.name != m_name) {
            m_name = answers.name;
            if (m_library)
                m_library->setSetting(kNameKey, m_name);
        }
        const bool news = m_state != State::Active;
        m_confirmed = true;
        qInfo("ytmusic: YouTube Music confirms the session (%s)%s",
              answers.loggedIn == QLatin1String("1") ? "logged_in=1" : "the account menu names it",
              m_name.isEmpty() ? "" : qPrintable(QStringLiteral(" as ") + m_name));
        setState(State::Active);
        if (news)
            Q_EMIT sessionChanged();
        scheduleCheck(m_timing.periodicMs);
        Q_EMIT checked(QStringLiteral("active"));
        return;
    }

    if (answers.loggedIn == QLatin1String("0")) {
        // Once could be a hiccup on their side; twice in a row is an answer.
        if (++m_zeroes >= 2) {
            qWarning("ytmusic: YouTube Music answered logged_in=0 twice: the session is over");
            reject(QStringLiteral("YouTube Music answered as if signed out"));
            Q_EMIT checked(QStringLiteral("rejected"));
            return;
        }
        qInfo("ytmusic: YouTube Music answered logged_in=0; asking once more in %d s before believing it",
              m_timing.recheckMs / 1000);
        scheduleCheck(m_timing.recheckMs);
        Q_EMIT checked(QStringLiteral("again"));
        return;
    }

    // No verdict: no answer, or one that says nothing. Proof of nothing, so
    // the session is kept and asked about again later.
    ++m_failures;
    const int doublings = qBound(0, m_failures - 1, 16);
    const qint64 wait = std::min<qint64>(qint64(m_timing.retryMinMs) << doublings, m_timing.retryMaxMs);
    const QString why = !answers.homeError.isEmpty() ? answers.homeError
                      : !answers.menuError.isEmpty() ? answers.menuError
                                                     : QStringLiteral("the answers did not say");
    qInfo("ytmusic: could not check the session (%s); asking again in %lld s", qPrintable(why),
          (long long)(wait / 1000));
    // One confirmed earlier in this run stays in use through a blip.
    if (!m_confirmed || m_state != State::Active)
        setState(State::Unreachable);
    scheduleCheck(wait);
    watchNetwork();
    Q_EMIT checked(QStringLiteral("unreachable"));
}

void YtmSession::reject(const QString &reason)
{
    ++m_generation;
    ++m_checkRun;
    m_checking = false;
    m_checkTimer.stop();
    m_saveTimer.stop();
    m_dirty = false;
    m_jar.clear();
    QString error;
    const SecretStore::Status removed = SecretStore::remove(kSecretName, &error);
    if (removed != SecretStore::Status::Ok && removed != SecretStore::Status::Unavailable)
        qWarning("ytmusic: could not delete the refused session: %s", qPrintable(error));
    m_rejectReason = reason;
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

QByteArray YtmSession::authorization(const QList<Cookie> &jar, qint64 timestamp, const QByteArray &origin)
{
    const QByteArray sapisid = CookieImport::value(jar, "SAPISID");
    const QByteArray firstParty = CookieImport::value(jar, "__Secure-1PAPISID");
    const QByteArray thirdParty = CookieImport::value(jar, "__Secure-3PAPISID");
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

quint64 YtmSession::authHeaders(InnerTube::Auth auth, const QByteArray &origin, QByteArray *cookie,
                                QByteArray *authorization)
{
    if (m_jar.isEmpty())
        return 0;
    switch (auth) {
    case InnerTube::Auth::Anonymous:
        return 0;
    case InnerTube::Auth::IfSignedIn:
        if (m_state != State::Active)
            return 0;
        break;
    case InnerTube::Auth::Checking:
        break;   // any session held: the check is what decides
    }
    const qint64 now = nowSecs();
    *cookie = CookieImport::header(m_jar, now);
    *authorization = YtmSession::authorization(m_jar, now, origin);
    return cookie->isEmpty() ? 0 : m_generation;
}

void YtmSession::reportRejected(quint64 session, int httpStatus)
{
    if (session == 0 || session != m_generation || m_jar.isEmpty())
        return;
    const bool wasChecking = m_checking;
    qWarning("ytmusic: YouTube Music refused the session (HTTP %d); its cookies are no longer sent", httpStatus);
    reject(QStringLiteral("YouTube Music refused it (HTTP %1)").arg(httpStatus));
    if (wasChecking)
        Q_EMIT checked(QStringLiteral("rejected"));
}

void YtmSession::absorbCookies(quint64 session, const QList<QNetworkCookie> &cookies)
{
    if (session == 0 || session != m_generation || m_jar.isEmpty())
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
            domain = QString::fromLatin1(kOrigin.mid(int(qstrlen("https://"))));
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
        if (cookie.name.isEmpty() || !CookieImport::isYouTubeDomain(cookie.domain)
            || !CookieImport::sentToMusic(cookie))
            continue;

        int index = -1;
        for (int i = 0; i < m_jar.size(); ++i) {
            if (m_jar.at(i).name == cookie.name) {
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
    qInfo("ytmusic: YouTube Music rotated the session: %d updated (%s), %d removed%s; saving within %d s",
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
    const QByteArray json = CookieImport::toJson(m_jar);
    QString error;
    if (SecretStore::write(kSecretName, json, &error) == SecretStore::Status::Ok)
        qInfo("ytmusic: saved the rotated session (%d cookies, %d bytes)", int(m_jar.size()), int(json.size()));
    else
        qWarning("ytmusic: could not save the rotated session: %s", qPrintable(error));
}

// ---------------------------------------------------------------- demonstration

void YtmSession::showDemo(const QString &demo)
{
    ++m_generation;
    ++m_checkRun;
    m_checking = false;
    m_checkTimer.stop();
    m_jar.clear();   // nothing to send, so nothing ever is
    QString which = demo;
    if (which.endsWith(QLatin1String("+file"))) {
        which.chop(int(qstrlen("+file")));
        // Not a path: deleting it only says it was a demonstration.
        m_importedFile = QStringLiteral("demo:cookies-music.youtube.com.txt");
    }
    m_name = QStringLiteral("Demo Listener");
    m_confirmed = which == QLatin1String("active");
    if (which == QLatin1String("active")) {
        setState(State::Active);
    } else if (which == QLatin1String("checking")) {
        setState(State::Checking);
    } else if (which == QLatin1String("unreachable")) {
        setState(State::Unreachable);
    } else if (which == QLatin1String("rejected")) {
        m_rejectReason = QStringLiteral("YouTube Music refused it (HTTP 403)");
        setState(State::Rejected);
    } else {
        m_name.clear();
        setState(State::SignedOut);
    }
    qInfo("ytmusic: showing a demonstration: %s (no cookies held, nothing sent)", qPrintable(state()));
}
