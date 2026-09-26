#include "scrobbler.h"

#include "appdatabase.h"
#include "library.h"
#include "playbackcontroller.h"
#include "secretstore.h"

#include <QDateTime>
#include <QDesktopServices>
#include <QGuiApplication>
#include <QNetworkInformation>
#include <QPointer>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSqlError>
#include <QSqlQuery>
#include <QStringList>
#include <QTimeZone>
#include <QVariant>

#include <algorithm>
#include <limits>
#include <utility>

namespace {

// The session key's name in SecretStore, and the two settings: the user name
// and the Scrobble switch. Nothing secret is ever written to settings.
const QString kSessionSecret = QStringLiteral("lastfm.session");
const QString kUserKey = QStringLiteral("lastfm.user");
const QString kEnabledKey = QStringLiteral("lastfm.enabled");

qint64 nowMs()
{
    return QDateTime::currentMSecsSinceEpoch();
}

// A delay as QTimer takes it, in int milliseconds.
int timerMs(qint64 ms)
{
    return int(std::clamp<qint64>(ms, 0, std::numeric_limits<int>::max()));
}

// The start of the next day in UTC, when Last.fm's daily count starts again,
// and a minute more for the clocks to agree.
qint64 nextUtcDayMs()
{
    const QDate tomorrow = QDateTime::currentDateTimeUtc().date().addDays(1);
    return QDateTime(tomorrow, QTime(0, 1), QTimeZone::UTC).toMSecsSinceEpoch();
}

QString idList(const QList<qint64> &ids)
{
    QStringList numbers;
    numbers.reserve(ids.size());
    for (qint64 id : ids)
        numbers << QString::number(id);
    return numbers.join(QLatin1Char(','));
}

QString scrobbles(int count)
{
    return count == 1 ? QStringLiteral("1 scrobble") : QStringLiteral("%1 scrobbles").arg(count);
}

// The errors that are about this build's key rather than anything sent.
bool keyRefused(int error)
{
    return error == 10 || error == 13 || error == 26;
}

}

Scrobbler::Scrobbler(LastFmApi *api, Library *library, QObject *parent)
    : QObject(parent)
    , m_api(api)
    , m_library(library)
{
    m_pollTimer.setSingleShot(false);
    connect(&m_pollTimer, &QTimer::timeout, this, [this]() {
        // Ten minutes of asking every few seconds is enough; after that only
        // coming back to the window, or the button, asks again.
        if (nowMs() - m_tokenAt >= m_timing.pollForMs) {
            m_pollTimer.stop();
            m_wait = Wait::StillWaiting;
            updateStatus();
            return;
        }
        pollSession(false);
    });

    m_flushTimer.setSingleShot(true);
    connect(&m_flushTimer, &QTimer::timeout, this, &Scrobbler::flush);

    // Back from the browser: the likeliest moment the approval has happened.
    if (auto *app = qobject_cast<QGuiApplication *>(QCoreApplication::instance())) {
        connect(app, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
            if (state == Qt::ApplicationActive && m_state == State::Waiting)
                pollSession(false);
        });
    }
}

void Scrobbler::setPlayer(PlaybackController *player)
{
    if (!player)
        return;
    connect(player, &PlaybackController::listenStarted, this, &Scrobbler::listenStarted);
    connect(player, &PlaybackController::listenQualified, this, &Scrobbler::listenQualified);
    connect(player, &PlaybackController::listenResumed, this, &Scrobbler::listenResumed);
}

void Scrobbler::setTiming(const Timing &timing)
{
    m_timing = timing;
}

void Scrobbler::setUrlOpener(std::function<bool(const QUrl &)> opener)
{
    m_openUrl = std::move(opener);
}

void Scrobbler::useTestSession(const QString &user, const QByteArray &key)
{
    m_user = user;
    m_sessionKey = key;
    ++m_sendGeneration;
    m_inFlight = false;
    setState(m_api->available() ? State::Connected : State::Unavailable);
    refreshPending();
    updateStatus();
}

void Scrobbler::start()
{
    if (m_library)
        m_enabled = m_library->settingValue(kEnabledKey) != QLatin1String("0");
    if (!m_api->available()) {
        setState(State::Unavailable);
        return;
    }

    m_user = m_library ? m_library->settingValue(kUserKey) : QString();
    if (m_user.isEmpty()) {
        setState(State::Off);
    } else {
        QByteArray key;
        QString error;
        switch (SecretStore::read(kSessionSecret, &key, &error)) {
        case SecretStore::Status::Ok:
            m_sessionKey = key;
            setState(State::Connected);
            break;
        case SecretStore::Status::NotFound:
            // The key went (a revoked session is deleted) but the account is
            // remembered: what is queued for it waits for a reconnect.
            setState(State::Expired);
            break;
        case SecretStore::Status::Corrupt:
            qWarning("scrobbler: the stored Last.fm sign-in does not open (%s); it needs reconnecting",
                     qPrintable(error));
            setState(State::Expired);
            break;
        default:
            m_error = QStringLiteral("Monolist could not read the stored sign-in: %1").arg(error.toHtmlEscaped());
            setState(State::Error);
            break;
        }
    }
    refreshPending();
    updateStatus();
    qInfo("scrobbler: Last.fm %s%s, %d waiting", qPrintable(state()),
          m_user.isEmpty() ? "" : qPrintable(QStringLiteral(" as ") + m_user), m_pending);
    scheduleFlush(m_timing.afterLaunchMs);
    watchNetwork();
}

// A lost connection is the ordinary reason sends back off. When the system
// says it is back, what was waiting goes then rather than at the end of a
// half-hour backoff.
void Scrobbler::watchNetwork()
{
    if (m_watchingNetwork)
        return;
    m_watchingNetwork = true;
    if (!QNetworkInformation::loadBackendByFeatures(QNetworkInformation::Feature::Reachability)) {
        qInfo("scrobbler: no network status on this system; sending on a timer only");
        return;
    }
    connect(QNetworkInformation::instance(), &QNetworkInformation::reachabilityChanged, this,
            [this](QNetworkInformation::Reachability reachability) {
                if (reachability != QNetworkInformation::Reachability::Online)
                    return;
                if (m_pause == Pause::Backoff) {
                    m_pause = Pause::None;
                    m_failures = 0;
                    updateStatus();
                }
                scheduleFlush(1000);
            });
}

QString Scrobbler::state() const
{
    switch (m_state) {
    case State::Unavailable: return QStringLiteral("unavailable");
    case State::Off:         return QStringLiteral("off");
    case State::Waiting:     return QStringLiteral("waiting");
    case State::Connected:   return QStringLiteral("connected");
    case State::Expired:     return QStringLiteral("expired");
    case State::Error:       return QStringLiteral("error");
    }
    return QStringLiteral("off");
}

QString Scrobbler::pauseName(Pause pause)
{
    switch (pause) {
    case Pause::None:       return QStringLiteral("none");
    case Pause::Backoff:    return QStringLiteral("backoff");
    case Pause::RateLimit:  return QStringLiteral("rate-limit");
    case Pause::DailyLimit: return QStringLiteral("daily-limit");
    case Pause::Hold:       return QStringLiteral("hold");
    }
    return QStringLiteral("none");
}

void Scrobbler::setState(State state)
{
    m_state = state;
    if (state != State::Off)
        m_notice.clear();
    if (state != State::Error)
        m_disconnectFailed = false;
    updateStatus();
}

Scrobbler::State Scrobbler::restingState() const
{
    if (!m_api->available())
        return State::Unavailable;
    if (m_user.isEmpty())
        return State::Off;
    return m_sessionKey.isEmpty() ? State::Expired : State::Connected;
}

void Scrobbler::setEnabled(bool enabled)
{
    if (enabled == m_enabled)
        return;
    m_enabled = enabled;
    if (m_library)
        m_library->setSetting(kEnabledKey, enabled ? QStringLiteral("1") : QStringLiteral("0"));
    qInfo("scrobbler: scrobbling turned %s", enabled ? "on" : "off");
    updateStatus();
    if (enabled)
        scheduleFlush(1000);
}

void Scrobbler::refreshPending()
{
    int count = 0;
    if (!m_user.isEmpty()) {
        QSqlQuery q(AppDatabase::connection());
        q.prepare(QStringLiteral("SELECT COUNT(*) FROM scrobble_queue WHERE account = ?"));
        q.addBindValue(m_user);
        if (q.exec() && q.next())
            count = q.value(0).toInt();
    }
    m_pending = count;
}

// Every string here may be shown as styled text, so what came from outside
// (a user name, Last.fm's own message) is escaped where it is put in.
void Scrobbler::updateStatus()
{
    QString line;
    switch (m_state) {
    case State::Unavailable:
        line = m_api->unavailableReason().toHtmlEscaped();
        break;
    case State::Off:
        line = m_notice;
        break;
    case State::Waiting:
        switch (m_wait) {
        case Wait::Asking:
            line = QStringLiteral("Asking Last.fm for a sign-in page…");
            break;
        case Wait::Approve:
            line = QStringLiteral("Approve Monolist on the Last.fm page that opened in your browser, "
                                  "then come back here.");
            break;
        case Wait::NotYet:
            line = QStringLiteral("Last.fm has not had your approval yet. Approve Monolist on the page in "
                                  "your browser, then press I've approved it.");
            break;
        case Wait::StillWaiting:
            line = QStringLiteral("Still waiting. Once you have approved Monolist on the Last.fm page, "
                                  "come back here or press I've approved it.");
            break;
        case Wait::NoBrowser:
            line = QStringLiteral("Monolist could not open your browser. Open "
                                  "<a href=\"%1\">the Last.fm approval page</a> yourself, approve Monolist "
                                  "there, then come back here.")
                       .arg(m_authUrl.toString(QUrl::FullyEncoded).toHtmlEscaped());
            break;
        }
        break;
    case State::Connected:
        if (!m_enabled) {
            line = m_pending > 0 ? QStringLiteral("Scrobbling is off. %1 from before are kept for when it is on again.")
                                       .arg(scrobbles(m_pending))
                                 : QStringLiteral("Scrobbling is off: nothing is sent.");
        } else if (m_pending > 0) {
            line = QStringLiteral("%1 waiting").arg(scrobbles(m_pending));
            switch (m_pause) {
            case Pause::None:
                break;
            case Pause::Backoff:
                line += QStringLiteral(": Last.fm is not answering, so Monolist keeps trying.");
                break;
            case Pause::RateLimit:
                line += QStringLiteral(": Last.fm asked for a pause; sending again in a quarter of an hour.");
                break;
            case Pause::DailyLimit:
                line += QStringLiteral(": today's Last.fm limit is reached; sending again tomorrow.");
                break;
            case Pause::Hold:
                line += keyRefused(m_holdError)
                            ? QStringLiteral(": Last.fm refused this build's key (error %1). They are kept "
                                             "until it is accepted again.").arg(m_holdError)
                            : QStringLiteral(": Last.fm refused them (error %1). They are kept, and sent "
                                             "again in half an hour.").arg(m_holdError);
                break;
            }
        }
        break;
    case State::Expired:
        line = m_pending > 0
                   ? QStringLiteral("Last.fm no longer accepts Monolist's sign-in for %1. Reconnect to send "
                                    "the %2 waiting.").arg(m_user.toHtmlEscaped(), scrobbles(m_pending))
                   : QStringLiteral("Last.fm no longer accepts Monolist's sign-in for %1. Reconnect to "
                                    "scrobble again.").arg(m_user.toHtmlEscaped());
        break;
    case State::Error:
        line = m_error;
        break;
    }
    m_statusLine = line;
    Q_EMIT changed();
}

// ---------------------------------------------------------------- signing in

void Scrobbler::connectAccount()
{
    if (!m_api->available() || m_state == State::Waiting)
        return;
    const quint64 generation = ++m_connectGeneration;
    m_token.clear();
    m_authUrl.clear();
    m_sessionInFlight = false;
    m_manualCheck = false;
    m_error.clear();
    m_wait = Wait::Asking;
    setState(State::Waiting);
    qInfo("scrobbler: asking Last.fm for a sign-in token");
    m_api->getToken([self = QPointer<Scrobbler>(this), generation](const LastFmApi::Reply &reply) {
        if (!self || generation != self->m_connectGeneration || self->m_state != State::Waiting)
            return;
        self->tokenArrived(reply);
    });
}

void Scrobbler::tokenArrived(const LastFmApi::Reply &reply)
{
    const QString token = LastFmApi::token(reply);
    if (token.isEmpty()) {
        failConnecting(connectError(reply));
        return;
    }
    m_token = token;
    m_tokenAt = nowMs();
    m_authUrl = m_api->authPageUrl(token);
    const bool opened = m_openUrl ? m_openUrl(m_authUrl) : QDesktopServices::openUrl(m_authUrl);
    m_wait = opened ? Wait::Approve : Wait::NoBrowser;
    // Never the address itself: it holds the key and the token.
    qInfo("scrobbler: the approval page %s; waiting for it",
          opened ? "is open in the browser" : "could not be opened");
    m_pollTimer.start(m_timing.pollMs);
    updateStatus();
}

void Scrobbler::checkApproval()
{
    if (m_state != State::Waiting)
        return;
    pollSession(true);
}

void Scrobbler::pollSession(bool manual)
{
    if (m_state != State::Waiting || m_token.isEmpty())
        return;
    if (nowMs() - m_tokenAt > m_timing.tokenLifeMs) {
        failConnecting(QStringLiteral("That approval page has expired. Connect again for a new one."));
        return;
    }
    if (manual)
        m_manualCheck = true;
    // One question at a time; a press while one is out is answered by it.
    if (m_sessionInFlight)
        return;
    m_sessionInFlight = true;
    const quint64 generation = m_connectGeneration;
    m_api->getSession(m_token, [self = QPointer<Scrobbler>(this), generation](const LastFmApi::Reply &reply) {
        if (!self || generation != self->m_connectGeneration)
            return;
        self->m_sessionInFlight = false;
        const bool manual = std::exchange(self->m_manualCheck, false);
        if (self->m_state == State::Waiting)
            self->sessionAnswered(reply, manual);
    });
}

void Scrobbler::sessionAnswered(const LastFmApi::Reply &reply, bool manual)
{
    QString user;
    QByteArray key;
    if (LastFmApi::session(reply, &user, &key)) {
        finishConnecting(user, key);
        return;
    }
    switch (reply.outcome) {
    case LastFmApi::Outcome::KeepWaiting:
        // Not approved yet: the usual answer, and only worth saying to
        // someone who pressed the button to ask.
        if (manual) {
            m_wait = Wait::NotYet;
            updateStatus();
        }
        return;
    case LastFmApi::Outcome::RestartSignIn:
        failConnecting(QStringLiteral("That approval page expired before Monolist heard back. "
                                      "Connect again for a new one."));
        return;
    case LastFmApi::Outcome::Retry:
    case LastFmApi::Outcome::RateLimited:
        return;   // asked again on the next poll
    default:
        failConnecting(connectError(reply));
        return;
    }
}

void Scrobbler::finishConnecting(const QString &user, const QByteArray &key)
{
    QString error;
    const SecretStore::Status stored = SecretStore::write(kSessionSecret, key, &error);
    if (stored != SecretStore::Status::Ok) {
        failConnecting(QStringLiteral("Monolist could not keep the sign-in: %1").arg(error.toHtmlEscaped()));
        return;
    }
    stopWaiting();
    if (m_library)
        m_library->setSetting(kUserKey, user);
    m_user = user;
    m_sessionKey = key;
    // A new session: whatever held the sending back belonged to the old one.
    ++m_sendGeneration;
    m_inFlight = false;
    m_pause = Pause::None;
    m_failures = 0;
    m_singles.clear();
    setState(State::Connected);
    refreshPending();
    updateStatus();
    qInfo("scrobbler: connected to Last.fm as %s, %d waiting", qPrintable(m_user), m_pending);
    scheduleFlush(m_timing.afterEnqueueMs);
}

void Scrobbler::failConnecting(const QString &message)
{
    stopWaiting();
    ++m_connectGeneration;
    m_error = message;
    m_disconnectFailed = false;
    qWarning("scrobbler: sign-in did not complete: %s", qPrintable(message));
    setState(State::Error);
}

void Scrobbler::stopWaiting()
{
    m_pollTimer.stop();
    m_token.clear();
    m_authUrl.clear();
    m_sessionInFlight = false;
    m_manualCheck = false;
}

void Scrobbler::cancelConnect()
{
    if (m_state != State::Waiting)
        return;
    stopWaiting();
    ++m_connectGeneration;
    qInfo("scrobbler: sign-in cancelled");
    setState(restingState());
}

void Scrobbler::disconnectAccount()
{
    stopWaiting();
    ++m_connectGeneration;
    QString error;
    const SecretStore::Status removed = SecretStore::remove(kSessionSecret, &error);
    if (removed != SecretStore::Status::Ok) {
        m_error = QStringLiteral("Monolist could not delete the stored sign-in: %1").arg(error.toHtmlEscaped());
        m_disconnectFailed = true;
        qWarning("scrobbler: could not delete the session key: %s", qPrintable(error));
        setState(State::Error);
        return;
    }
    ++m_sendGeneration;
    m_inFlight = false;
    m_flushTimer.stop();
    m_pause = Pause::None;
    m_failures = 0;
    m_singles.clear();
    if (m_library)
        m_library->setSetting(kUserKey, QString());
    qInfo("scrobbler: disconnected from Last.fm; the session key is deleted");
    m_user.clear();
    m_sessionKey.clear();
    refreshPending();
    setState(State::Off);
    m_notice = QStringLiteral("Disconnected, and the key is deleted from this computer. To withdraw "
                              "Monolist's access at Last.fm as well, remove it from "
                              "<a href=\"https://www.last.fm/settings/applications\">your Last.fm "
                              "applications</a>.");
    updateStatus();
}

QString Scrobbler::connectError(const LastFmApi::Reply &reply)
{
    switch (reply.outcome) {
    case LastFmApi::Outcome::Retry:
        return reply.httpStatus == 0
                   ? QStringLiteral("Last.fm did not answer. Check the connection, then try again.")
                   : QStringLiteral("Last.fm is not answering properly (%1). Try again in a while.")
                         .arg(reply.message.toHtmlEscaped());
    case LastFmApi::Outcome::RateLimited:
        return QStringLiteral("Last.fm asked Monolist to slow down. Try again in a few minutes.");
    case LastFmApi::Outcome::Hold:
        return QStringLiteral("Last.fm refused this build's API key (error %1).").arg(reply.error);
    case LastFmApi::Outcome::Ok:
        return QStringLiteral("Last.fm answered with nothing Monolist could use. Try again.");
    default:
        return reply.error > 0 ? QStringLiteral("Last.fm said no: %1 (error %2).")
                                     .arg(reply.message.toHtmlEscaped()).arg(reply.error)
                               : reply.message.toHtmlEscaped();
    }
}

// ---------------------------------------------------------------- listening

QString Scrobbler::scrobbleArtist(const QVariantMap &track)
{
    static const QRegularExpression topic(QStringLiteral(R"(\s+-\s+Topic$)"));
    QString artist = track.value(QStringLiteral("primaryArtist")).toString().trimmed();
    if (artist.isEmpty())
        artist = track.value(QStringLiteral("artist")).toString().trimmed();
    // A line of YouTube's joined with bullets ("Song • Artist", "Channel •
    // 1.6B views") is not an artist, and sent as one it would make a new,
    // wrong artist on the user's public profile. Better nothing at all.
    if (artist.contains(QStringLiteral(" • ")))
        return QString();
    artist.remove(topic);
    return artist.trimmed();
}

bool Scrobbler::recording() const
{
    return m_api->available() && m_enabled && !m_user.isEmpty();
}

bool Scrobbler::canSend() const
{
    return m_api->available() && m_enabled && m_state == State::Connected && !m_sessionKey.isEmpty();
}

void Scrobbler::listenStarted(const QVariantMap &track, qint64, bool chosenByUser)
{
    qInfo("scrobbler: listen started: \"%s\" by %s%s",
          qPrintable(track.value(QStringLiteral("title")).toString()), qPrintable(scrobbleArtist(track)),
          chosenByUser ? "" : " (autoplay)");
    sendNowPlaying(track);
}

void Scrobbler::listenResumed(const QVariantMap &track, qint64, bool)
{
    // Last.fm's "now playing" lapses on its own; after a long pause it has.
    sendNowPlaying(track);
}

void Scrobbler::listenQualified(const QVariantMap &track, qint64 startedAt, bool chosenByUser)
{
    const QString title = track.value(QStringLiteral("title")).toString();
    if (!recording()) {
        qInfo("scrobbler: \"%s\" counts as played; not kept (%s)", qPrintable(title),
              !m_api->available() ? "no Last.fm here" : !m_enabled ? "scrobbling is off" : "not connected");
        return;
    }
    enqueue(track, startedAt, chosenByUser);
}

void Scrobbler::enqueue(const QVariantMap &track, qint64 startedAt, bool chosenByUser)
{
    const QString artist = scrobbleArtist(track);
    const QString title = track.value(QStringLiteral("title")).toString().trimmed();
    if (artist.isEmpty() || title.isEmpty()) {
        qInfo("scrobbler: not scrobbled: the track has no %s", artist.isEmpty() ? "artist to send" : "title");
        return;
    }

    QSqlQuery q(AppDatabase::connection());
    q.prepare(QStringLiteral(
        "INSERT INTO scrobble_queue (account, artist, track, album, album_artist, duration_s, started_at,"
        " chosen_by_user, video_id, attempts, last_error, queued_at)"
        " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, 0, '', ?)"));
    q.addBindValue(m_user);
    q.addBindValue(artist);
    q.addBindValue(title);
    q.addBindValue(AppDatabase::text(track.value(QStringLiteral("album")).toString().trimmed()));
    q.addBindValue(AppDatabase::text(track.value(QStringLiteral("albumArtist")).toString().trimmed()));
    q.addBindValue(track.value(QStringLiteral("durationMs")).toLongLong() / 1000);
    q.addBindValue(startedAt);
    q.addBindValue(chosenByUser ? 1 : 0);
    q.addBindValue(AppDatabase::text(track.value(QStringLiteral("sourceId")).toString()));
    q.addBindValue(nowMs() / 1000);
    if (!q.exec()) {
        qWarning("scrobbler: could not queue a scrobble: %s", qPrintable(q.lastError().text()));
        return;
    }

    QSqlQuery trim(AppDatabase::connection());
    if (trim.exec(QStringLiteral("DELETE FROM scrobble_queue WHERE id IN (SELECT id FROM scrobble_queue"
                                 " ORDER BY id DESC LIMIT -1 OFFSET %1)").arg(kMaxQueued))
        && trim.numRowsAffected() > 0) {
        qWarning("scrobbler: the queue is full; the %d oldest scrobbles were dropped", trim.numRowsAffected());
    }

    refreshPending();
    qInfo("scrobbler: queued \"%s\" by %s, started %s%s; %d waiting", qPrintable(title),
          qPrintable(artist),
          qPrintable(QDateTime::fromSecsSinceEpoch(startedAt, QTimeZone::UTC).toString(Qt::ISODate)),
          chosenByUser ? "" : ", chosen by autoplay", m_pending);
    updateStatus();
    scheduleFlush(m_timing.afterEnqueueMs);
}

void Scrobbler::sendNowPlaying(const QVariantMap &track)
{
    if (!canSend())
        return;
    // Held back or rate-limited: a now-playing would only be refused too.
    if ((m_pause == Pause::RateLimit || m_pause == Pause::Hold) && nowMs() < m_pausedUntil)
        return;
    LastFmApi::Scrobble item;
    item.artist = scrobbleArtist(track);
    item.track = track.value(QStringLiteral("title")).toString().trimmed();
    item.album = track.value(QStringLiteral("album")).toString().trimmed();
    item.durationS = int(track.value(QStringLiteral("durationMs")).toLongLong() / 1000);
    if (item.artist.isEmpty() || item.track.isEmpty())
        return;
    const quint64 generation = m_sendGeneration;
    m_api->updateNowPlaying(m_sessionKey, item,
                            [self = QPointer<Scrobbler>(this), generation](const LastFmApi::Reply &reply) {
        if (!self || generation != self->m_sendGeneration)
            return;
        const QString outcome = LastFmApi::outcomeName(reply.outcome);
        if (reply.outcome != LastFmApi::Outcome::Ok)
            qInfo("scrobbler: now playing not taken: %s (%s)", qPrintable(outcome), qPrintable(reply.message));
        if (reply.outcome == LastFmApi::Outcome::Reauthenticate)
            self->expire();
        Q_EMIT self->nowPlayingAnswered(outcome);
    });
}

// ---------------------------------------------------------------- sending

void Scrobbler::scheduleFlush(qint64 ms)
{
    const int delay = timerMs(ms);
    if (!m_flushTimer.isActive() || m_flushTimer.remainingTime() > delay)
        m_flushTimer.start(delay);
}

void Scrobbler::pauseFor(Pause pause, qint64 ms)
{
    m_pause = pause;
    m_pausedUntil = nowMs() + ms;
    m_flushTimer.start(timerMs(ms));
}

// 30 seconds, doubling with each failure in a row to half an hour, and
// spread by a quarter either way so that many copies of the app coming back
// online together do not all ask at the same second.
qint64 Scrobbler::backoffMs() const
{
    const int doublings = qBound(0, m_failures - 1, 20);
    const qint64 base = std::min<qint64>(qint64(m_timing.backoffMinMs) << doublings, m_timing.backoffMaxMs);
    const double spread = 0.75 + 0.5 * QRandomGenerator::global()->generateDouble();
    return std::clamp<qint64>(qint64(double(base) * spread), m_timing.backoffMinMs, m_timing.backoffMaxMs);
}

void Scrobbler::flush()
{
    if (m_inFlight || !canSend())
        return;
    if (m_pause != Pause::None) {
        const qint64 left = m_pausedUntil - nowMs();
        if (left > 0) {
            m_flushTimer.start(timerMs(left));
            return;
        }
        m_pause = Pause::None;
        updateStatus();
    }

    // After a batch refused as a whole, its items go one at a time.
    QSqlQuery q(AppDatabase::connection());
    const QString columns = QStringLiteral(
        "SELECT id, artist, track, album, album_artist, duration_s, started_at, chosen_by_user"
        " FROM scrobble_queue WHERE account = ?");
    bool single = false;
    while (!m_singles.isEmpty()) {
        q.prepare(columns + QStringLiteral(" AND id = ?"));
        q.addBindValue(m_user);
        q.addBindValue(m_singles.first());
        if (q.exec() && q.next()) {
            single = true;
            break;
        }
        m_singles.removeFirst();
    }
    if (!single) {
        q.prepare(columns + QStringLiteral(" ORDER BY id LIMIT %1").arg(LastFmApi::kMaxBatch));
        q.addBindValue(m_user);
        if (!q.exec()) {
            qWarning("scrobbler: could not read the queue: %s", qPrintable(q.lastError().text()));
            return;
        }
        if (!q.next())
            return;   // nothing waiting
    }

    QList<qint64> ids;
    QList<LastFmApi::Scrobble> items;
    do {
        LastFmApi::Scrobble item;
        ids << q.value(0).toLongLong();
        item.artist = q.value(1).toString();
        item.track = q.value(2).toString();
        item.album = q.value(3).toString();
        item.albumArtist = q.value(4).toString();
        item.durationS = q.value(5).toInt();
        item.timestamp = q.value(6).toLongLong();
        item.chosenByUser = q.value(7).toInt() != 0;
        items << item;
    } while (!single && q.next());

    m_inFlight = true;
    const quint64 generation = m_sendGeneration;
    m_api->scrobble(m_sessionKey, items,
                    [self = QPointer<Scrobbler>(this), generation, ids](const LastFmApi::Reply &reply) {
        if (!self || generation != self->m_sendGeneration)
            return;
        self->handleBatch(ids, reply);
    });
}

void Scrobbler::handleBatch(const QList<qint64> &ids, const LastFmApi::Reply &reply)
{
    m_inFlight = false;
    const int count = int(ids.size());
    const QString outcome = LastFmApi::outcomeName(reply.outcome);
    const QString error = reply.error > 0 ? QStringLiteral("error %1: %2").arg(reply.error).arg(reply.message)
                                          : reply.message;
    bool more = false;

    switch (reply.outcome) {
    case LastFmApi::Outcome::Ok: {
        QList<int> codes = reply.ignoredCodes;
        // Each item's fate is in the answer, in the order sent. An answer
        // that does not account for every item is not trusted with any.
        if (codes.size() != count && reply.ignored == 0 && reply.accepted == count)
            codes = QList<int>(count, 0);
        if (codes.size() != count) {
            ++m_failures;
            markAttempt(ids, QStringLiteral("an answer that did not list each scrobble"));
            qWarning("scrobbler: Last.fm's answer listed %d of %d scrobbles; all kept, trying later",
                     int(codes.size()), count);
            pauseFor(Pause::Backoff, backoffMs());
            break;
        }
        QList<qint64> done;
        QList<qint64> tomorrow;
        int accepted = 0;
        int dropped = 0;
        for (int i = 0; i < count; ++i) {
            switch (LastFmApi::itemOutcome(codes.at(i))) {
            case LastFmApi::ItemOutcome::Accepted:
                done << ids.at(i);
                ++accepted;
                break;
            case LastFmApi::ItemOutcome::Dropped:
                done << ids.at(i);
                ++dropped;
                break;
            case LastFmApi::ItemOutcome::TryTomorrow:
                tomorrow << ids.at(i);
                break;
            }
        }
        removeRows(done);
        if (!tomorrow.isEmpty())
            markAttempt(tomorrow, QStringLiteral("the daily scrobble limit"));
        for (qint64 id : ids)
            m_singles.removeAll(id);
        m_failures = 0;
        refreshPending();
        // Counts only: a refused scrobble's details stay in the database.
        qInfo("scrobbler: sent %d: %d accepted, %d ignored for good, %d kept for tomorrow; %d left",
              count, accepted, dropped, int(tomorrow.size()), m_pending);
        if (!tomorrow.isEmpty())
            pauseFor(Pause::DailyLimit, nextUtcDayMs() - nowMs());
        else
            more = true;
        break;
    }
    case LastFmApi::Outcome::Retry:
    case LastFmApi::Outcome::KeepWaiting:
    case LastFmApi::Outcome::RestartSignIn:
        ++m_failures;
        markAttempt(ids, error);
        pauseFor(Pause::Backoff, backoffMs());
        qInfo("scrobbler: %d not sent (%s); trying again in %lld s", count, qPrintable(error),
              (long long)((m_pausedUntil - nowMs()) / 1000));
        break;
    case LastFmApi::Outcome::RateLimited:
        markAttempt(ids, error);
        pauseFor(Pause::RateLimit, m_timing.rateLimitMs);
        qInfo("scrobbler: Last.fm asked for a pause (error 29); sending again in %d min",
              m_timing.rateLimitMs / 60000);
        break;
    case LastFmApi::Outcome::Reauthenticate:
        markAttempt(ids, error);
        expire();
        break;
    case LastFmApi::Outcome::Hold:
        m_holdError = reply.error;
        markAttempt(ids, error);
        pauseFor(Pause::Hold, m_timing.holdMs);
        qWarning("scrobbler: Last.fm refused the key (error %d); all %d waiting are kept, trying again in %d min",
                 reply.error, m_pending, m_timing.holdMs / 60000);
        break;
    case LastFmApi::Outcome::Rejected:
        if (count > 1) {
            // Something in the batch was wrong, and the answer does not say
            // what: each item goes again on its own, once.
            m_singles = ids;
            qInfo("scrobbler: Last.fm refused a batch of %d (%s); sending them one at a time", count,
                  qPrintable(error));
            more = true;
        } else if (reply.error == 6) {
            // Invalid parameters, for this item alone: it will never be
            // taken, so it goes.
            removeRows(ids);
            for (qint64 id : ids)
                m_singles.removeAll(id);
            qWarning("scrobbler: Last.fm refused a scrobble on its own (%s); it is dropped", qPrintable(error));
            more = true;
        } else {
            // Any other refusal says nothing about the item, and the same
            // answer to every item would empty the queue one by one. Kept,
            // like everything behind it, and asked again later.
            m_holdError = reply.error;
            markAttempt(ids, error);
            pauseFor(Pause::Hold, m_timing.holdMs);
            qWarning("scrobbler: Last.fm refused a scrobble on its own (%s); all %d waiting are kept, "
                     "trying again in %d min", qPrintable(error), m_pending, m_timing.holdMs / 60000);
        }
        break;
    }

    refreshPending();
    updateStatus();
    Q_EMIT batchAnswered(count, outcome);
    if (more)
        QTimer::singleShot(0, this, &Scrobbler::flush);
}

void Scrobbler::markAttempt(const QList<qint64> &ids, const QString &error)
{
    if (ids.isEmpty())
        return;
    QSqlQuery q(AppDatabase::connection());
    q.prepare(QStringLiteral("UPDATE scrobble_queue SET attempts = attempts + 1, last_error = ? WHERE id IN (%1)")
                  .arg(idList(ids)));
    q.addBindValue(AppDatabase::text(error.left(200)));
    q.exec();
}

void Scrobbler::removeRows(const QList<qint64> &ids)
{
    if (ids.isEmpty())
        return;
    QSqlQuery q(AppDatabase::connection());
    if (!q.exec(QStringLiteral("DELETE FROM scrobble_queue WHERE id IN (%1)").arg(idList(ids))))
        qWarning("scrobbler: could not remove sent scrobbles: %s", qPrintable(q.lastError().text()));
}

// Last.fm no longer accepts the session (error 9: revoked at last.fm, most
// likely). The key is useless now, so it goes; the queue stays, and keeps
// growing, until the user reconnects as the same account.
void Scrobbler::expire()
{
    QString error;
    if (SecretStore::remove(kSessionSecret, &error) != SecretStore::Status::Ok)
        qWarning("scrobbler: could not delete the revoked session key: %s", qPrintable(error));
    m_sessionKey.clear();
    ++m_sendGeneration;
    m_inFlight = false;
    m_flushTimer.stop();
    m_pause = Pause::None;
    m_singles.clear();
    refreshPending();
    qWarning("scrobbler: Last.fm no longer accepts the session (error 9); %d scrobbles kept for a reconnect",
             m_pending);
    setState(State::Expired);
}
