#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantList>

#include "soundchain.h"

class Library;
class MpvEngine;

// The sound effects as the listener sets them: Slowed + reverb, Nightcore,
// 8D audio, High bass and the equaliser. Kept in the settings table (the
// sound.* keys), handed to the engine (MpvEngine::setEffects) the moment they
// change, and shown to QML as the singleton Sound.
//
// Slowed + reverb and Nightcore both set the speed, so turning one on turns
// the other off. Every switch is written at once; a band of the equaliser,
// dragged, is written 400 ms after it settles, and at once as Monolist
// closes. What is read back is checked: a value out of range is the default
// again, and the equaliser's gains are the preset's unless it is "custom"
// (sound.eq_gains is only kept, and only read, for a custom curve).
class SoundEffects : public QObject
{
    Q_OBJECT
    // Slowed + reverb: every song slower and lower (speed 0.80, 0.85 or 0.90,
    // pitch with it) in a hall (reverb 0 off, 1 light, 2 medium, 3 heavy).
    Q_PROPERTY(bool slowedReverb READ slowedReverb WRITE setSlowedReverb NOTIFY effectsChanged)
    Q_PROPERTY(double slowedSpeed READ slowedSpeed WRITE setSlowedSpeed NOTIFY effectsChanged)
    Q_PROPERTY(int reverbLevel READ reverbLevel WRITE setReverbLevel NOTIFY effectsChanged)
    // Nightcore: faster and higher (1.20, 1.25 or 1.30).
    Q_PROPERTY(bool nightcore READ nightcore WRITE setNightcore NOTIFY effectsChanged)
    Q_PROPERTY(double nightcoreSpeed READ nightcoreSpeed WRITE setNightcoreSpeed NOTIFY effectsChanged)
    // 8D audio: the song carried round the head, with a light room. Best
    // with headphones. It has no strength to choose.
    Q_PROPERTY(bool eightD READ eightD WRITE setEightD NOTIFY effectsChanged)
    // High bass: a shelf under 100 Hz, +3, +6, +9 or +12 dB.
    Q_PROPERTY(bool highBass READ highBass WRITE setHighBass NOTIFY effectsChanged)
    Q_PROPERTY(int highBassDb READ highBassDb WRITE setHighBassDb NOTIFY effectsChanged)
    // The equaliser: ten bands, 31 Hz to 16 kHz, +-12 dB in 0.5 dB steps.
    Q_PROPERTY(bool equaliser READ equaliser WRITE setEqualiser NOTIFY effectsChanged)
    // The preset whose curve the bands hold (an id from `presets`), or
    // "custom" once a band has been moved off every preset.
    Q_PROPERTY(QString preset READ preset WRITE applyPreset NOTIFY effectsChanged)
    // The ten bands' gains, in dB, 31 Hz first.
    Q_PROPERTY(QVariantList bandGains READ bandGains NOTIFY effectsChanged)
    // "31", "62", "125" ... "8K", "16K".
    Q_PROPERTY(QStringList bandLabels READ bandLabels CONSTANT)
    // [{ id, name }], Flat first; "custom" is not among them.
    Q_PROPERTY(QVariantList presets READ presets CONSTANT)
    // The strengths each effect comes in, for its chips.
    Q_PROPERTY(QVariantList slowedSpeeds READ slowedSpeeds CONSTANT)       // 0.8, 0.85, 0.9
    Q_PROPERTY(QStringList reverbLevels READ reverbLevels CONSTANT)        // "Off" ... "Heavy", by level
    Q_PROPERTY(QVariantList nightcoreSpeeds READ nightcoreSpeeds CONSTANT) // 1.2, 1.25, 1.3
    Q_PROPERTY(QVariantList highBassSteps READ highBassSteps CONSTANT)     // 3, 6, 9, 12
    // Any of the five switched on: the player bar's glyph is red.
    Q_PROPERTY(bool active READ active NOTIFY effectsChanged)
    // What is on, in a line: "Slowed 0.85× + reverb · 8D · High bass +6 dB ·
    // EQ Bass boost"; empty with nothing on.
    Q_PROPERTY(QString summary READ summary NOTIFY effectsChanged)
    // How far the sound is turned down so the boosts do not clip, in dB
    // (0 or more): "Turned down 3.6 dB so the boost does not clip".
    Q_PROPERTY(double headroomDb READ headroomDb NOTIFY effectsChanged)
    // The effects would not start this session (MpvEngine::effectsFailed),
    // and are off until Monolist restarts; the switches keep what was chosen.
    Q_PROPERTY(bool failed READ failed NOTIFY effectsChanged)
public:
    // Either may be null: without an engine (--sound-test) nothing is played,
    // without a library nothing is kept.
    SoundEffects(MpvEngine *engine, Library *library, QObject *parent = nullptr);
    ~SoundEffects() override;

    // The effects as the last launch left them, handed to the engine. Called
    // once, before the first song is loaded, so it plays with them from its
    // first second.
    void restore();
    // A band drag still waiting to be written, written now (the app closing).
    void flush();

    bool slowedReverb() const { return m_settings.slowedReverb; }
    void setSlowedReverb(bool on);
    double slowedSpeed() const { return m_settings.slowedSpeed; }
    void setSlowedSpeed(double speed);
    int reverbLevel() const { return m_settings.reverbLevel; }
    void setReverbLevel(int level);
    bool nightcore() const { return m_settings.nightcore; }
    void setNightcore(bool on);
    double nightcoreSpeed() const { return m_settings.nightcoreSpeed; }
    void setNightcoreSpeed(double speed);
    bool eightD() const { return m_settings.eightD; }
    void setEightD(bool on);
    bool highBass() const { return m_settings.highBass; }
    void setHighBass(bool on);
    int highBassDb() const { return m_settings.highBassDb; }
    void setHighBassDb(int db);
    bool equaliser() const { return m_settings.eq; }
    void setEqualiser(bool on);
    QString preset() const { return m_settings.preset; }
    QVariantList bandGains() const;
    QStringList bandLabels() const;
    QVariantList presets() const;
    QVariantList slowedSpeeds() const;
    QStringList reverbLevels() const;
    QVariantList nightcoreSpeeds() const;
    QVariantList highBassSteps() const;
    bool active() const;
    QString summary() const;
    double headroomDb() const;
    bool failed() const;

    SoundChain::Settings settings() const { return m_settings; }

    // One band to `db` (clamped to +-12, to the nearest 0.5): the preset
    // becomes "custom", or the preset whose curve this now is.
    Q_INVOKABLE void setBandGain(int band, double db);
    // A preset's curve on the bands, by id; "custom" and unknown ids change
    // nothing.
    Q_INVOKABLE void applyPreset(const QString &id);
    // Every band to 0: the Flat preset.
    Q_INVOKABLE void resetBands();
    // The curve the bands and High bass make together, in dB, at `points`
    // frequencies from 31 Hz to 16 kHz, evenly spaced on a log scale (the
    // sliders' row): the bands as they stand, whether the equaliser is on or
    // not, and the shelf while High bass is on.
    Q_INVOKABLE QVariantList responseCurve(int points) const;
    // Every effect off; strengths and the curve are kept for next time.
    Q_INVOKABLE void turnAllOff();

Q_SIGNALS:
    void effectsChanged();
    // Something to tell the listener, for the toast.
    void notice(const QString &text);

private:
    // The settings changed: sanitized, handed to the engine, the log line
    // scheduled, and effectsChanged.
    void apply();
    void save(const QString &key, const QString &value);
    // The preset, and the curve where it is a custom one.
    void saveGains();
    QString describe() const;

    MpvEngine *m_engine;
    Library *m_library;
    SoundChain::Settings m_settings;
    // A band drag's write, 400 ms after it settles.
    QTimer m_gainsSave;
    // The log line, once a run of changes has settled.
    QTimer m_logLine;
};
