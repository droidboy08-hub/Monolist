#include "accountguard.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

const QString kStoreKey = QStringLiteral("ytmusic.guard");
constexpr qint64 kHourMs = 60LL * 60 * 1000;
constexpr qint64 kDayMs = 24 * kHourMs;
// How often a call waiting on another's turn asks again.
constexpr qint64 kTurnPollMs = 400;

std::function<qint64()> &guardClock()
{
    static std::function<qint64()> c;
    return c;
}

int randomBelow(int bound)
{
    return bound > 0 ? int(QRandomGenerator::global()->bounded(bound)) : 0;
}

QString clockText(qint64 at)
{
    return QDateTime::fromMSecsSinceEpoch(at).toString(QStringLiteral("HH:mm"));
}

}

AccountGuard::AccountGuard(QObject *parent)
    : QObject(parent)
{
    m_resumeTimer.setSingleShot(true);
    // Precise: a coarse timer of an hour may fire a second early, and a rest
    // that ends before its time is not over.
    m_resumeTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_resumeTimer, &QTimer::timeout, this, &AccountGuard::resumed);
    m_saveTimer.setSingleShot(true);
    connect(&m_saveTimer, &QTimer::timeout, this, &AccountGuard::save);
    m_tokens = m_limits.burst;
    m_refilledAt = now();
    m_nextJitter = randomBelow(m_limits.jitterMs);
}

AccountGuard::~AccountGuard()
{
    if (m_saveTimer.isActive())
        save();
}

void AccountGuard::setClock(std::function<qint64()> c)
{
    guardClock() = std::move(c);
}

qint64 AccountGuard::now()
{
    return guardClock() ? guardClock()() : QDateTime::currentMSecsSinceEpoch();
}

const char *AccountGuard::name(Kind kind)
{
    switch (kind) {
    case Kind::Check: return "check";
    case Kind::Browse: return "browse";
    case Kind::Listen: return "listen report";
    case Kind::YtDlp: return "yt-dlp lookup";
    }
    return "call";
}

void AccountGuard::setLimits(const Limits &limits)
{
    m_limits = limits;
    m_tokens = std::min<double>(m_tokens, limits.burst);
    m_nextJitter = randomBelow(limits.jitterMs);
}

void AccountGuard::setStore(Store store)
{
    m_store = std::move(store);
    load();
}

int AccountGuard::weightOf(Kind kind) const
{
    return kind == Kind::YtDlp ? std::max(1, m_limits.ytdlpWeight) : 1;
}

void AccountGuard::prune()
{
    const qint64 dayAgo = now() - kDayMs;
    while (!m_uses.isEmpty() && m_uses.first().at <= dayAgo)
        m_uses.removeFirst();
    const qint64 forgotten = now() - m_limits.tripMemoryMs;
    while (!m_trips.isEmpty() && m_trips.first() <= forgotten)
        m_trips.removeFirst();
}

int AccountGuard::sum(qint64 since, bool anyKind, Kind kind) const
{
    int total = 0;
    for (const Use &use : m_uses) {
        if (use.at > since && (anyKind || use.kind == kind))
            total += anyKind ? use.weight : 1;
    }
    return total;
}

int AccountGuard::usedLastHour() const { return sum(now() - kHourMs, true, Kind::Browse); }
int AccountGuard::usedLastDay() const { return sum(now() - kDayMs, true, Kind::Browse); }
int AccountGuard::countLastHour(Kind kind) const { return sum(now() - kHourMs, false, kind); }
int AccountGuard::countLastDay(Kind kind) const { return sum(now() - kDayMs, false, kind); }

bool AccountGuard::paused() const
{
    return m_pausedUntil > now();
}

bool AccountGuard::busy() const
{
    return m_inFlight && now() - m_inFlightSince < m_limits.holdMs;
}

void AccountGuard::refill()
{
    const qint64 t = now();
    if (m_limits.refillMs > 0 && t > m_refilledAt)
        m_tokens = std::min<double>(m_limits.burst, m_tokens + double(t - m_refilledAt) / m_limits.refillMs);
    m_refilledAt = t;
}

qint64 AccountGuard::admit(Kind kind, QString *why)
{
    const auto refuse = [why](const QString &text) -> qint64 {
        if (why)
            *why = text;
        return -1;
    };
    const qint64 t = now();
    if (m_pausedUntil > 0 && m_pausedUntil <= t)
        resumed();
    if (paused()) {
        return refuse(QStringLiteral("the account is resting until %1: %2")
                          .arg(clockText(m_pausedUntil), m_pauseWhy));
    }
    prune();
    const int weight = weightOf(kind);
    if (usedLastHour() + weight > m_limits.perHour)
        return refuse(QStringLiteral("the account has made its %1 calls for this hour").arg(m_limits.perHour));
    if (usedLastDay() + weight > m_limits.perDay)
        return refuse(QStringLiteral("the account has made its %1 calls for today").arg(m_limits.perDay));
    if (kind == Kind::Listen) {
        if (countLastHour(kind) >= m_limits.listensPerHour)
            return refuse(QStringLiteral("%1 listens were reported in the last hour, the most that may be")
                              .arg(m_limits.listensPerHour));
        if (countLastDay(kind) >= m_limits.listensPerDay)
            return refuse(QStringLiteral("%1 listens were reported today, the most that may be")
                              .arg(m_limits.listensPerDay));
    }
    if (kind == Kind::YtDlp) {
        if (countLastHour(kind) >= m_limits.ytdlpPerHour)
            return refuse(QStringLiteral("%1 songs were asked for with the account in the last hour, the most that "
                                         "may be").arg(m_limits.ytdlpPerHour));
        if (countLastDay(kind) >= m_limits.ytdlpPerDay)
            return refuse(QStringLiteral("%1 songs were asked for with the account today, the most that may be")
                              .arg(m_limits.ytdlpPerDay));
        // Refused rather than kept waiting: a song is not held up for the
        // account, it goes on down the signed-out ladder instead.
        if (m_lastYtDlp > 0 && t - m_lastYtDlp < m_limits.ytdlpGapMs)
            return refuse(QStringLiteral("the account was asked for a song %1 s ago").arg((t - m_lastYtDlp) / 1000));
    }
    // Another call with the account is out: its turn first.
    if (busy()) {
        if (why)
            *why = QStringLiteral("another call with the account is out (%1)").arg(QLatin1String(name(m_inFlightKind)));
        return kTurnPollMs;
    }
    refill();
    qint64 wait = 0;
    if (m_tokens < 1.0)
        wait = qint64(std::ceil((1.0 - m_tokens) * m_limits.refillMs));
    if (m_lastStart > 0)
        wait = std::max(wait, m_lastStart + m_limits.gapMs + m_nextJitter - t);
    if (wait > 0) {
        if (why)
            *why = QStringLiteral("spacing the account's calls");
        return wait;
    }
    return 0;
}

quint64 AccountGuard::started(Kind kind)
{
    const qint64 t = now();
    refill();
    m_tokens = std::max(0.0, m_tokens - 1.0);
    m_inFlight = true;
    const quint64 ticket = ++m_turn;
    m_inFlightSince = t;
    m_inFlightKind = kind;
    m_lastStart = t;
    m_nextJitter = randomBelow(m_limits.jitterMs);
    if (kind == Kind::YtDlp)
        m_lastYtDlp = t;
    m_uses.append({ t, kind, weightOf(kind) });
    saveSoon();
    return ticket;
}

void AccountGuard::finished(Kind kind, int status, qint64 retryAfterSecs, quint64 ticket)
{
    Q_UNUSED(kind)
    if (ticket == 0 || ticket == m_turn)
        m_inFlight = false;
    if (status == 429) {
        trip(QStringLiteral("YouTube answered 429, too many requests"),
             retryAfterSecs > 0 ? retryAfterSecs * 1000 : 0);
    }
}

void AccountGuard::trip(const QString &why, qint64 retryAfterMs)
{
    const qint64 t = now();
    prune();
    // Several calls refused together are one refusal, not three in a row.
    if (paused() && !m_trips.isEmpty() && t - m_trips.last() < 60 * 1000) {
        if (retryAfterMs > 0 && t + retryAfterMs > m_pausedUntil) {
            m_pausedUntil = t + retryAfterMs;
            armResume();
            save();
        }
        return;
    }
    m_trips.append(t);
    const int count = int(m_trips.size());
    qint64 pause = count <= 1 ? m_limits.firstPauseMs : count == 2 ? m_limits.secondPauseMs : m_limits.thirdPauseMs;
    pause = std::max(pause, retryAfterMs);
    m_pausedUntil = std::max(m_pausedUntil, t + pause);
    m_pauseWhy = why;
    m_inFlight = false;
    qWarning("account: YouTube asked to slow down (%s); nothing more is asked with the account until %s "
             "(pause %d of the last day)",
             qPrintable(why), qPrintable(clockText(m_pausedUntil)), count);
    armResume();
    save();
    Q_EMIT pausedChanged();
}

void AccountGuard::forgive()
{
    const bool wasPaused = paused();
    m_uses.clear();
    m_trips.clear();
    m_pausedUntil = 0;
    m_pauseWhy.clear();
    m_inFlight = false;
    m_lastStart = 0;
    m_lastYtDlp = 0;
    m_tokens = m_limits.burst;
    m_resumeTimer.stop();
    save();
    if (wasPaused)
        Q_EMIT pausedChanged();
}

void AccountGuard::armResume()
{
    const qint64 left = m_pausedUntil - now();
    if (left <= 0) {
        m_resumeTimer.stop();
        return;
    }
    m_resumeTimer.start(int(std::min<qint64>(left, std::numeric_limits<int>::max())));
}

void AccountGuard::resumed()
{
    if (m_pausedUntil == 0)
        return;
    // Woken early (a long wait capped, a timer's slack): not over yet.
    if (m_pausedUntil > now()) {
        armResume();
        return;
    }
    m_pausedUntil = 0;
    m_pauseWhy.clear();
    m_resumeTimer.stop();
    qInfo("account: the pause is over; the account may be used again");
    save();
    Q_EMIT pausedChanged();
}

// ---------------------------------------------------------------- keeping

void AccountGuard::load()
{
    if (!m_store.read)
        return;
    const QJsonObject kept = QJsonDocument::fromJson(m_store.read(kStoreKey).toUtf8()).object();
    m_uses.clear();
    m_trips.clear();
    for (const QJsonValue &value : kept.value(QStringLiteral("uses")).toArray()) {
        const QJsonArray use = value.toArray();
        const int kind = use.at(1).toInt();
        if (use.size() < 3 || kind < 0 || kind > int(Kind::YtDlp))
            continue;
        m_uses.append({ qint64(use.at(0).toDouble()), Kind(kind), std::max(1, use.at(2).toInt()) });
    }
    std::sort(m_uses.begin(), m_uses.end(), [](const Use &a, const Use &b) { return a.at < b.at; });
    for (const QJsonValue &value : kept.value(QStringLiteral("trips")).toArray())
        m_trips.append(qint64(value.toDouble()));
    std::sort(m_trips.begin(), m_trips.end());
    m_pausedUntil = qint64(kept.value(QStringLiteral("pausedUntil")).toDouble());
    m_pauseWhy = kept.value(QStringLiteral("why")).toString();
    for (const Use &use : std::as_const(m_uses)) {
        if (use.kind == Kind::YtDlp)
            m_lastYtDlp = std::max(m_lastYtDlp, use.at);
        m_lastStart = std::max(m_lastStart, use.at);
    }
    prune();
    if (paused()) {
        qInfo("account: resting until %s, as an earlier run was told (%s)", qPrintable(clockText(m_pausedUntil)),
              qPrintable(m_pauseWhy));
        armResume();
    } else {
        m_pausedUntil = 0;
        m_pauseWhy.clear();
    }
}

void AccountGuard::saveSoon()
{
    if (!m_saveTimer.isActive())
        m_saveTimer.start(5000);
}

void AccountGuard::save()
{
    m_saveTimer.stop();
    if (!m_store.write)
        return;
    prune();
    QJsonArray uses;
    for (const Use &use : std::as_const(m_uses))
        uses.append(QJsonArray{ double(use.at), int(use.kind), use.weight });
    QJsonArray trips;
    for (qint64 at : std::as_const(m_trips))
        trips.append(double(at));
    QJsonObject kept{ { QStringLiteral("uses"), uses }, { QStringLiteral("trips"), trips } };
    if (paused()) {
        kept.insert(QStringLiteral("pausedUntil"), double(m_pausedUntil));
        kept.insert(QStringLiteral("why"), m_pauseWhy);
    }
    m_store.write(kStoreKey, QString::fromUtf8(QJsonDocument(kept).toJson(QJsonDocument::Compact)));
}
