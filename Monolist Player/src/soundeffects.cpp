#include "soundeffects.h"

#include "library.h"
#include "mpvengine.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QVariantMap>

#include <cmath>

namespace {

// The settings table's keys.
const char kSlowedKey[] = "sound.slowed_reverb";
const char kSlowedSpeedKey[] = "sound.slowed_speed";
const char kReverbKey[] = "sound.reverb";
const char kNightcoreKey[] = "sound.nightcore";
const char kNightcoreSpeedKey[] = "sound.nightcore_speed";
const char kEightDKey[] = "sound.8d";
const char kHighBassKey[] = "sound.high_bass";
const char kHighBassDbKey[] = "sound.high_bass_db";
const char kEqKey[] = "sound.eq";
const char kPresetKey[] = "sound.eq_preset";
const char kGainsKey[] = "sound.eq_gains";

// A band dragged is written this long after it settles, as the volume is.
constexpr int kGainsSaveDelayMs = 400;
// The log line waits for a run of changes (a drag, chips clicked through)
// to settle, so there is one line for it, not one a step.
constexpr int kLogDelayMs = 800;

const char kCustom[] = "custom";

QString flag(bool on)
{
    return on ? QStringLiteral("1") : QStringLiteral("0");
}

// One of `steps`, or `fallback` for anything else: a stored 7 is not a speed.
template <typename T, size_t N>
T stepOf(const QString &text, const std::array<T, N> &steps, T fallback)
{
    bool ok = false;
    const double value = text.trimmed().toDouble(&ok);
    if (!ok)
        return fallback;
    for (T step : steps) {
        if (std::fabs(double(step) - value) < 0.001)
            return step;
    }
    return fallback;
}

QString speedText(double speed)
{
    return QString::number(speed, 'f', 2);
}

} // namespace

SoundEffects::SoundEffects(MpvEngine *engine, Library *library, QObject *parent)
    : QObject(parent)
    , m_engine(engine)
    , m_library(library)
{
    m_gainsSave.setSingleShot(true);
    m_gainsSave.setInterval(kGainsSaveDelayMs);
    connect(&m_gainsSave, &QTimer::timeout, this, &SoundEffects::saveGains);
    m_logLine.setSingleShot(true);
    m_logLine.setInterval(kLogDelayMs);
    connect(&m_logLine, &QTimer::timeout, this, [this]() {
        qInfo("sound: %s", qUtf8Printable(describe()));
    });
    // A drag less than 400 ms before closing is written as it closes.
    if (QCoreApplication *app = QCoreApplication::instance())
        connect(app, &QCoreApplication::aboutToQuit, this, &SoundEffects::flush);
    if (m_engine) {
        connect(m_engine, &MpvEngine::effectsFailed, this, [this]() {
            Q_EMIT notice(QStringLiteral("Sound effects would not start, so they are off until Monolist restarts."));
            Q_EMIT effectsChanged();
        });
    }
}

SoundEffects::~SoundEffects()
{
    flush();
}

void SoundEffects::restore()
{
    const auto value = [this](const char *key) {
        return m_library ? m_library->settingValue(QString::fromLatin1(key)) : QString();
    };
    SoundChain::Settings s;
    s.slowedReverb = value(kSlowedKey) == QLatin1String("1");
    s.slowedSpeed = stepOf(value(kSlowedSpeedKey), SoundChain::kSlowedSpeeds, 0.85);
    {
        bool ok = false;
        const int level = value(kReverbKey).toInt(&ok);
        s.reverbLevel = ok && level >= 0 && level <= 3 ? level : 2;
    }
    s.nightcore = value(kNightcoreKey) == QLatin1String("1");
    s.nightcoreSpeed = stepOf(value(kNightcoreSpeedKey), SoundChain::kNightcoreSpeeds, 1.25);
    s.eightD = value(kEightDKey) == QLatin1String("1");
    s.highBass = value(kHighBassKey) == QLatin1String("1");
    s.highBassDb = stepOf(value(kHighBassDbKey), SoundChain::kHighBassSteps, 6);
    s.eq = value(kEqKey) == QLatin1String("1");

    // The preset's own curve, unless it is a custom one, which is the only
    // time the stored gains are read.
    const QString id = value(kPresetKey);
    if (id == QLatin1String(kCustom)) {
        const QJsonDocument json = QJsonDocument::fromJson(value(kGainsKey).toUtf8());
        const QJsonArray gains = json.array();
        bool readable = json.isArray() && gains.size() == SoundChain::kBands;
        for (const QJsonValue &gain : gains)
            readable = readable && gain.isDouble() && std::isfinite(gain.toDouble());
        if (readable) {
            for (int i = 0; i < SoundChain::kBands; ++i)
                s.gains[size_t(i)] = gains.at(i).toDouble();   // clamped by sanitized
        }
        s.gains = SoundChain::sanitized(s).gains;
        s.preset = SoundChain::presetFor(s.gains);   // Flat for gains that did not read
    } else if (const SoundChain::Preset *known = SoundChain::preset(id)) {
        s.gains = known->gains;
        s.preset = known->id;
    } else {
        s.preset = QStringLiteral("flat");
    }

    // Slowed wins where both say they were on, which only a damaged setting
    // can make.
    m_settings = SoundChain::sanitized(s);
    if (m_engine)
        m_engine->setEffects(m_settings);
    if (active())
        qInfo("sound: as the last launch left them: %s", qUtf8Printable(describe()));
    Q_EMIT effectsChanged();
}

void SoundEffects::flush()
{
    if (m_gainsSave.isActive()) {
        m_gainsSave.stop();
        saveGains();
    }
}

void SoundEffects::save(const QString &key, const QString &value)
{
    if (m_library)
        m_library->setSetting(key, value);
}

void SoundEffects::saveGains()
{
    save(QString::fromLatin1(kPresetKey), m_settings.preset);
    if (m_settings.preset != QLatin1String(kCustom))
        return;
    QJsonArray gains;
    for (double gain : m_settings.gains)
        gains.append(gain);
    save(QString::fromLatin1(kGainsKey), QString::fromUtf8(QJsonDocument(gains).toJson(QJsonDocument::Compact)));
}

void SoundEffects::apply()
{
    m_settings = SoundChain::sanitized(m_settings);
    if (m_engine)
        m_engine->setEffects(m_settings);
    m_logLine.start();
    Q_EMIT effectsChanged();
}

void SoundEffects::setSlowedReverb(bool on)
{
    if (on == m_settings.slowedReverb)
        return;
    m_settings.slowedReverb = on;
    save(QString::fromLatin1(kSlowedKey), flag(on));
    // Both set the speed: one at a time.
    if (on && m_settings.nightcore) {
        m_settings.nightcore = false;
        save(QString::fromLatin1(kNightcoreKey), flag(false));
    }
    apply();
}

void SoundEffects::setSlowedSpeed(double speed)
{
    SoundChain::Settings next = m_settings;
    next.slowedSpeed = speed;
    next = SoundChain::sanitized(next);
    if (next.slowedSpeed == m_settings.slowedSpeed)
        return;
    m_settings.slowedSpeed = next.slowedSpeed;
    save(QString::fromLatin1(kSlowedSpeedKey), speedText(m_settings.slowedSpeed));
    apply();
}

void SoundEffects::setReverbLevel(int level)
{
    level = qBound(0, level, 3);
    if (level == m_settings.reverbLevel)
        return;
    m_settings.reverbLevel = level;
    save(QString::fromLatin1(kReverbKey), QString::number(level));
    apply();
}

void SoundEffects::setNightcore(bool on)
{
    if (on == m_settings.nightcore)
        return;
    m_settings.nightcore = on;
    save(QString::fromLatin1(kNightcoreKey), flag(on));
    if (on && m_settings.slowedReverb) {
        m_settings.slowedReverb = false;
        save(QString::fromLatin1(kSlowedKey), flag(false));
    }
    apply();
}

void SoundEffects::setNightcoreSpeed(double speed)
{
    SoundChain::Settings next = m_settings;
    next.nightcoreSpeed = speed;
    next = SoundChain::sanitized(next);
    if (next.nightcoreSpeed == m_settings.nightcoreSpeed)
        return;
    m_settings.nightcoreSpeed = next.nightcoreSpeed;
    save(QString::fromLatin1(kNightcoreSpeedKey), speedText(m_settings.nightcoreSpeed));
    apply();
}

void SoundEffects::setEightD(bool on)
{
    if (on == m_settings.eightD)
        return;
    m_settings.eightD = on;
    save(QString::fromLatin1(kEightDKey), flag(on));
    apply();
}

void SoundEffects::setHighBass(bool on)
{
    if (on == m_settings.highBass)
        return;
    m_settings.highBass = on;
    save(QString::fromLatin1(kHighBassKey), flag(on));
    apply();
}

void SoundEffects::setHighBassDb(int db)
{
    SoundChain::Settings next = m_settings;
    next.highBassDb = db;
    next = SoundChain::sanitized(next);
    if (next.highBassDb == m_settings.highBassDb)
        return;
    m_settings.highBassDb = next.highBassDb;
    save(QString::fromLatin1(kHighBassDbKey), QString::number(m_settings.highBassDb));
    apply();
}

void SoundEffects::setEqualiser(bool on)
{
    if (on == m_settings.eq)
        return;
    m_settings.eq = on;
    save(QString::fromLatin1(kEqKey), flag(on));
    apply();
}

void SoundEffects::setBandGain(int band, double db)
{
    if (band < 0 || band >= SoundChain::kBands || !std::isfinite(db))
        return;
    SoundChain::Settings next = m_settings;
    next.gains[size_t(band)] = db;
    next = SoundChain::sanitized(next);
    if (next.gains[size_t(band)] == m_settings.gains[size_t(band)])
        return;
    m_settings.gains = next.gains;
    m_settings.preset = SoundChain::presetFor(m_settings.gains);
    // Written once the drag has settled; heard at once.
    m_gainsSave.start();
    apply();
}

void SoundEffects::applyPreset(const QString &id)
{
    const SoundChain::Preset *known = SoundChain::preset(id);
    if (!known)
        return;
    // A drag still to be written would put its own curve back.
    m_gainsSave.stop();
    const bool same = m_settings.preset == known->id && m_settings.gains == known->gains;
    m_settings.gains = known->gains;
    m_settings.preset = known->id;
    save(QString::fromLatin1(kPresetKey), known->id);
    if (!same)
        apply();
}

void SoundEffects::resetBands()
{
    applyPreset(QStringLiteral("flat"));
}

void SoundEffects::turnAllOff()
{
    if (!active())
        return;
    m_settings.slowedReverb = false;
    m_settings.nightcore = false;
    m_settings.eightD = false;
    m_settings.highBass = false;
    m_settings.eq = false;
    for (const char *key : { kSlowedKey, kNightcoreKey, kEightDKey, kHighBassKey, kEqKey })
        save(QString::fromLatin1(key), flag(false));
    apply();
}

QVariantList SoundEffects::bandGains() const
{
    QVariantList gains;
    for (double gain : m_settings.gains)
        gains.append(gain);
    return gains;
}

QStringList SoundEffects::bandLabels() const
{
    return SoundChain::bandLabels();
}

QVariantList SoundEffects::presets() const
{
    QVariantList list;
    for (const SoundChain::Preset &p : SoundChain::presets())
        list.append(QVariantMap{ { QStringLiteral("id"), p.id }, { QStringLiteral("name"), p.name } });
    return list;
}

QVariantList SoundEffects::slowedSpeeds() const
{
    QVariantList list;
    for (double speed : SoundChain::kSlowedSpeeds)
        list.append(speed);
    return list;
}

QStringList SoundEffects::reverbLevels() const
{
    return { QStringLiteral("Off"), QStringLiteral("Light"), QStringLiteral("Medium"), QStringLiteral("Heavy") };
}

QVariantList SoundEffects::nightcoreSpeeds() const
{
    QVariantList list;
    for (double speed : SoundChain::kNightcoreSpeeds)
        list.append(speed);
    return list;
}

QVariantList SoundEffects::highBassSteps() const
{
    QVariantList list;
    for (int db : SoundChain::kHighBassSteps)
        list.append(db);
    return list;
}

bool SoundEffects::active() const
{
    return m_settings.slowedReverb || m_settings.nightcore || m_settings.eightD || m_settings.highBass
           || m_settings.eq;
}

QString SoundEffects::summary() const
{
    QStringList parts;
    if (m_settings.slowedReverb) {
        // At 1× nothing is slowed: what is heard is the hall.
        if (m_settings.slowedSpeed >= 0.995 && m_settings.reverbLevel > 0)
            parts << QStringLiteral("Reverb");
        else
            parts << QStringLiteral("Slowed %1×").arg(speedText(m_settings.slowedSpeed))
                         + (m_settings.reverbLevel > 0 ? QStringLiteral(" + reverb") : QString());
    }
    if (m_settings.nightcore)
        parts << QStringLiteral("Nightcore %1×").arg(speedText(m_settings.nightcoreSpeed));
    if (m_settings.eightD)
        parts << QStringLiteral("8D");
    if (m_settings.highBass)
        parts << QStringLiteral("High bass +%1 dB").arg(m_settings.highBassDb);
    if (m_settings.eq) {
        const SoundChain::Preset *known = SoundChain::preset(m_settings.preset);
        parts << QStringLiteral("EQ ") + (known ? known->name : QStringLiteral("Custom"));
    }
    return parts.join(QStringLiteral(" · "));
}

double SoundEffects::headroomDb() const
{
    const double pre = SoundChain::paramsFor(m_settings).preDb;
    return pre < 0.0 ? -pre : 0.0;
}

bool SoundEffects::failed() const
{
    return m_engine && m_engine->effectsBroken();
}

QVariantList SoundEffects::responseCurve(int points) const
{
    QVariantList curve;
    const double shelf = m_settings.highBass ? double(m_settings.highBassDb) : 0.0;
    for (double db : SoundChain::responseCurve(m_settings.gains, shelf, qBound(2, points, 2000)))
        curve.append(db);
    return curve;
}

// For the log: "slowed 0.85 + reverb medium, 8d, high bass +6 dB, eq
// bass_boost (pre -5.0 dB, limiter -1 dBFS)".
QString SoundEffects::describe() const
{
    static const char *const kReverbNames[] = { "no reverb", "reverb light", "reverb medium", "reverb heavy" };
    QStringList parts;
    if (m_settings.slowedReverb) {
        parts << QStringLiteral("slowed %1 + %2")
                     .arg(speedText(m_settings.slowedSpeed),
                          QLatin1String(kReverbNames[qBound(0, m_settings.reverbLevel, 3)]));
    }
    if (m_settings.nightcore)
        parts << QStringLiteral("nightcore %1").arg(speedText(m_settings.nightcoreSpeed));
    if (m_settings.eightD)
        parts << QStringLiteral("8d");
    if (m_settings.highBass)
        parts << QStringLiteral("high bass +%1 dB").arg(m_settings.highBassDb);
    if (m_settings.eq) {
        QString eq = QStringLiteral("eq ") + m_settings.preset;
        if (m_settings.preset == QLatin1String(kCustom)) {
            QStringList gains;
            for (double gain : m_settings.gains)
                gains << SoundChain::number(gain, 1);
            eq += QStringLiteral(" [") + gains.join(QLatin1Char(' ')) + QLatin1Char(']');
        }
        parts << eq;
    }
    if (parts.isEmpty())
        return QStringLiteral("all effects off");
    const SoundChain::Params p = SoundChain::paramsFor(m_settings);
    return parts.join(QStringLiteral(", "))
           + QStringLiteral(" (pre %1 dB, %2)")
                 .arg(SoundChain::number(p.preDb, 1),
                      p.limit < 1.0 ? QStringLiteral("limiter -1 dBFS") : QStringLiteral("no limiter"))
           + (failed() ? QStringLiteral("; not started this session") : QString());
}
