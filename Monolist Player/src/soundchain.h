#pragma once

#include <QList>
#include <QString>
#include <QStringList>

#include <array>

// The sound effects (Slowed + reverb, Nightcore, 8D audio, High bass and the
// equaliser) as mpv is given them: one chain of audio filters, labelled @fx,
// written only from the fixed templates below and never from anything a user
// typed; and the numbers behind it. Pure: nothing here talks to mpv
// (MpvEngine does) or keeps settings (SoundEffects does).
//
// The chain, in order (graph() writes it):
//   aformat stereo   so a 5.1 or mono source reaches every filter as two
//                    channels (afir crashed on 5.1 without it);
//   volume@pre       turned down by as much as the boosts below need
//                    (preGainDb), so they do not clip;
//   equalizer@b0..9  the ten bands, 31 Hz to 16 kHz, Q 1.41;
//   lowshelf@hb      High bass: a shelf at 100 Hz, Q 0.707. Not FFmpeg's
//                    "bass", which is a different shape than the maths here;
//   [8D]             only while 8D is in this song's graph: the song folded
//                    to the middle and walked round the head every 10 s of
//                    song time (an aeval panner), mixed in by amix@pan;
//   reverb           a 2.5 s hall made inside the graph from noise (no file
//                    to ship), mixed in by amix@mix;
//   alimiter@lim     whatever is left over is caught at -1 dBFS.
// Every value that moves is one af-command away (commands()), so effects
// change mid-song without a gap; the shape of the graph changes only when
// mpv is given a new chain.
//
// Slowed and Nightcore are not in the chain at all: they are mpv's speed,
// with pitch correction off (speedFactor), which keeps the clock, seeks and
// the song's length right where resampling inside the chain broke them.
namespace SoundChain {

constexpr int kBands = 10;
// The bands' centres, in Hz, and their width.
constexpr std::array<int, kBands> kBandHz = { 31, 62, 125, 250, 500, 1000, 2000, 4000, 8000, 16000 };
constexpr double kBandQ = 1.41;
// High bass's shelf.
constexpr double kShelfHz = 100.0;
constexpr double kShelfQ = 0.707;
// How far a band may go, up or down, and the step it moves in.
constexpr double kMaxGainDb = 12.0;
constexpr double kGainStepDb = 0.5;
// The limiter's ceiling while any effect is on (-1 dBFS); 1 at neutral.
constexpr double kLimit = 0.891;
// The steps the speeds and strengths come in.
constexpr std::array<double, 3> kSlowedSpeeds = { 0.80, 0.85, 0.90 };
constexpr std::array<double, 3> kNightcoreSpeeds = { 1.20, 1.25, 1.30 };
constexpr std::array<int, 4> kHighBassSteps = { 3, 6, 9, 12 };
// The reverb's weight in the mix for each strength: off, light, medium, heavy.
constexpr std::array<double, 4> kReverbWets = { 0.0, 0.35, 0.70, 1.00 };
// The 8D branch's weight when it is fully in: +3 dB, made up for what the
// panner takes away (it lowers the loudness 3.5-7 LU and never raises a peak).
constexpr double kEightDWeight = 1.41;
// The room 8D brings with it: at least this much reverb while 8D is on.
constexpr double kEightDRoom = 0.30;

// What the listener chose. Out-of-range values are put right by sanitized().
struct Settings {
    bool slowedReverb = false;
    double slowedSpeed = 0.85;      // 0.80, 0.85 or 0.90
    int reverbLevel = 2;            // 0 off, 1 light, 2 medium, 3 heavy
    bool nightcore = false;         // never together with slowedReverb
    double nightcoreSpeed = 1.25;   // 1.20, 1.25 or 1.30
    bool eightD = false;
    bool highBass = false;
    int highBassDb = 6;             // 3, 6, 9 or 12
    bool eq = false;
    QString preset = QStringLiteral("flat");   // a preset's id, or "custom"
    std::array<double, kBands> gains {};       // dB, applied while eq is on

    bool operator==(const Settings &o) const;
    bool operator!=(const Settings &o) const { return !(*this == o); }
};

// What the chain holds: every value an af-command can move, and the one
// thing only a new chain can change, whether the 8D branch is in it. Values
// are kept rounded to what they are written with (dB to 0.1, weights to
// 0.01, the limit to 0.001), so two that print the same are equal.
struct Params {
    double preDb = 0.0;                   // volume@pre
    std::array<double, kBands> gains {};  // equalizer@bN
    double shelfDb = 0.0;                 // lowshelf@hb
    double panWet = 0.0;                  // the 8D branch's weight in amix@pan, 0 to kEightDWeight
    double wet = 0.0;                     // the reverb's weight in amix@mix, 0 to 1
    double limit = 1.0;                   // alimiter@lim
    bool has8D = false;                   // the 8D branch is in the graph

    bool operator==(const Params &o) const;
    bool operator!=(const Params &o) const { return !(*this == o); }
};

// One af-command: `af-command fx <option> <value> <target>`.
struct Command {
    QString option;
    QString value;
    QString target;
    bool operator==(const Command &o) const
    {
        return option == o.option && value == o.value && target == o.target;
    }
};

struct Preset {
    QString id;
    QString name;
    std::array<double, kBands> gains;
};

// Every value inside its range: speeds and strengths to their nearest step,
// gains to +-12 in 0.5 dB steps, and Nightcore off where Slowed is on.
Settings sanitized(const Settings &settings);

// The reverb's weight for each strength: off, light, medium, heavy.
double reverbWet(int level);
// What the chain should hold for these settings. has8D follows eightD.
Params paramsFor(const Settings &settings);
// The chain at rest: it passes the sound through unchanged.
Params neutral();
// Whether these settings need the chain at all. Slowed or Nightcore alone,
// and an equaliser left flat, do not.
bool needsChain(const Settings &settings);
// Whether anything in these values changes the sound.
bool isNeutral(const Params &params);
// mpv's speed for these settings, against 1: Slowed's or Nightcore's.
double speedFactor(const Settings &settings);
// Slowed or Nightcore is on: mpv's pitch correction goes off, so the pitch
// moves with the speed.
bool ownsSpeed(const Settings &settings);

// The chain as libavfilter reads it, and as mpv's af takes it:
// @fx:lavfi=graph=%<UTF-8 bytes>%<graph>.
QString graph(const Params &params);
QString afValue(const Params &params);
QString afValue(const QString &graphText);
// The commands that move a running chain from `from` to `to`, only for the
// values that print differently. The 8D branch's weights only where `from`
// has the branch: commands cannot add it.
QList<Command> commands(const Params &from, const Params &to);
// Every value a chain of this shape holds, as commands: what is sent again
// when the sound output is reopened, in case the graph went with it.
QList<Command> commandsFor(const Params &params);
// The commands a chain reading sound at `sampleRate` (Hz) can be sent. A
// band above half the rate (16 kHz in a 22.05 kHz file, 8 kHz in an 8 kHz
// one) is passed over by FFmpeg as it builds the graph ("Invalid frequency"),
// and a command to it then crashed the shipped libmpv, every time: such a
// band is left out. Every other filter takes commands at any rate. With the
// rate not known (0), every band is left out.
QList<Command> commandsAt(const QList<Command> &commands, int sampleRate);
// One 15 ms step of a ramp from `from` toward `to`: every value moves
// together, so all arrive at once, and none by more than its limit (a band or
// the shelf 1 dB, the pre-gain 0.5 dB, a weight 0.07, the limit 0.01). The
// graph's shape stays from's. A single jump in any of them clicks.
Params stepToward(const Params &from, const Params &to);
// How many steps stepToward takes to get there.
int stepsBetween(const Params &from, const Params &to);

// The combined response of the bands and the shelf at `hz`, in dB, by the
// same RBJ formulas FFmpeg's equalizer and lowshelf use, at 48 kHz.
double responseDb(const std::array<double, kBands> &gains, double shelfDb, double hz);
// Its largest value over 240 points from 20 Hz to 20 kHz: the sliders
// understate it, since neighbouring bands add up.
double peakResponseDb(const std::array<double, kBands> &gains, double shelfDb);
// How far the sound is turned down before the boosts: half of the curve's
// peak up to 12 dB and all of it above (so at most +6 dB reaches the
// limiter), and 2 dB more while any reverb is mixed in. Never positive.
double preGainDb(double peakDb, double wet);
// The curve at `points` frequencies from the first band's centre to the
// last's, evenly spaced on a log scale: what the sliders' row spans.
QList<double> responseCurve(const std::array<double, kBands> &gains, double shelfDb, int points);

// The sixteen presets, Flat first.
const QList<Preset> &presets();
// Null for an id that is not one.
const Preset *preset(const QString &id);
// The preset whose gains these are, or "custom".
QString presetFor(const std::array<double, kBands> &gains);
// "31", "62" ... "8K", "16K".
QStringList bandLabels();

// A number as the chain writes it: `decimals` places, with no trailing
// zeros and never "-0".
QString number(double value, int decimals);

} // namespace SoundChain
