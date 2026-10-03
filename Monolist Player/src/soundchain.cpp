#include "soundchain.h"

#include <QtGlobal>

#include <algorithm>
#include <cmath>
#include <complex>

namespace {

constexpr double kPi = 3.14159265358979323846;
// The rate the response is worked out at. The filters' own rate is the
// file's, which moves the curve a little at the top for a 44.1 kHz file; the
// headroom does not need more than this.
constexpr double kModelRate = 48000.0;

// How far one 15 ms step may move each value (stepToward). Measured on pure
// tones: steps this size move the level smoothly, where one jump clicked.
constexpr double kGainStepLimit = 1.0;
constexpr double kPreStepLimit = 0.5;
constexpr double kWeightStepLimit = 0.07;
constexpr double kLimitStepLimit = 0.01;

// The 8D panner (research r6, "binaural-lite"): the song folded to the
// middle and carried round the head once every 10 s of song time, 80 % of
// the way to each side (a 15.7 dB swing between the ears), with the far ear
// darkened above 2 kHz (head shadow, and both ears when it is behind) and
// up to 0.6 ms late (a first-order all-pass), at constant power. Its clock is
// the song's (t), so a seek lands where the turn would be and the two players
// of a takeover turn together. aeval takes no commands: its period and depth
// are fixed here. In single quotes, so the graph reads its commas as one
// argument.
const char kEightD[] =
    "aeval=exprs='"
    "st(3,2*PI*t/10);st(4,0.8*sin(ld(3)));st(5,max(0,-cos(ld(3))));st(6,(val(0)+val(1))/2);"
    "st(0,ld(0)+(1-exp(-2*PI*2000/s))*(ld(6)-ld(0)));st(7,ld(6)+min(1,0.875*max(0,ld(4))+0.5*ld(5))*(ld(0)-ld(6)));"
    "st(8,0.5+0.00075*s*max(0,ld(4)));st(8,(1-ld(8))/(1+ld(8)));st(9,ld(8)*ld(7)+ld(1)-ld(8)*ld(2));"
    "st(1,ld(7));st(2,ld(9));ld(9)*cos(PI/4*(1+ld(4)))"
    "|"
    "st(3,2*PI*t/10);st(4,0.8*sin(ld(3)));st(5,max(0,-cos(ld(3))));st(6,(val(0)+val(1))/2);"
    "st(0,ld(0)+(1-exp(-2*PI*2000/s))*(ld(6)-ld(0)));st(7,ld(6)+min(1,0.875*max(0,-ld(4))+0.5*ld(5))*(ld(0)-ld(6)));"
    "st(8,0.5+0.00075*s*max(0,-ld(4)));st(8,(1-ld(8))/(1+ld(8)));st(9,ld(8)*ld(7)+ld(1)-ld(8)*ld(2));"
    "st(1,ld(7));st(2,ld(9));ld(9)*sin(PI/4*(1+ld(4)))"
    "':c=stereo";

// The reverb's impulse response, made inside the graph each time it is
// built (about 7 ms): 2.5 s of decorrelated white noise per ear, decaying
// 60 dB in 2.2 s after 20 ms of pre-delay, kept to 150 Hz-6 kHz. irnorm=2 on
// afir is what makes it audible at all (the default left it at -42 dBFS);
// minp=1024 is what lost no samples as it went in.
const char kRoomIr[] =
    "anoisesrc=d=2.5:c=white:r=48000:a=1:seed=11[nl];anoisesrc=d=2.5:c=white:r=48000:a=1:seed=22[nr];"
    "[nl][nr]join=inputs=2:channel_layout=stereo:map=0.0-FL|1.0-FR,afade=t=out:st=0:d=3.67:curve=exp,"
    "afade=t=in:st=0:d=0.004,adelay=delays=20:all=1,atrim=end=2.5,highpass=f=150,lowpass=f=6000,"
    "afade=t=out:st=2.3:d=0.2[ir]";

double roundTo(double value, int decimals)
{
    const double scale = std::pow(10.0, decimals);
    const double rounded = std::round(value * scale) / scale;
    return rounded == 0.0 ? 0.0 : rounded;   // never -0
}

bool same(double a, double b)
{
    return std::fabs(a - b) < 1e-9;
}

template <size_t N>
double nearest(const std::array<double, N> &steps, double value)
{
    double best = steps[0];
    for (double step : steps) {
        if (std::fabs(step - value) < std::fabs(best - value))
            best = step;
    }
    return best;
}

// One biquad, as RBJ's cookbook (and FFmpeg's af_biquads.c) writes it.
struct Biquad {
    double b0, b1, b2, a0, a1, a2;
    std::complex<double> at(double hz) const
    {
        const std::complex<double> z = std::polar(1.0, -2.0 * kPi * hz / kModelRate);
        return (b0 + b1 * z + b2 * z * z) / (a0 + a1 * z + a2 * z * z);
    }
};

Biquad peaking(double hz, double gainDb, double q)
{
    const double a = std::pow(10.0, gainDb / 40.0);
    const double w = 2.0 * kPi * hz / kModelRate;
    const double alpha = std::sin(w) / (2.0 * q);
    const double c = std::cos(w);
    return { 1.0 + alpha * a, -2.0 * c, 1.0 - alpha * a, 1.0 + alpha / a, -2.0 * c, 1.0 - alpha / a };
}

Biquad lowShelf(double hz, double gainDb, double q)
{
    const double a = std::pow(10.0, gainDb / 40.0);
    const double w = 2.0 * kPi * hz / kModelRate;
    const double alpha = std::sin(w) / (2.0 * q);
    const double c = std::cos(w);
    const double s = 2.0 * std::sqrt(a) * alpha;
    return { a * ((a + 1.0) - (a - 1.0) * c + s), 2.0 * a * ((a - 1.0) - (a + 1.0) * c),
             a * ((a + 1.0) - (a - 1.0) * c - s), (a + 1.0) + (a - 1.0) * c + s,
             -2.0 * ((a - 1.0) + (a + 1.0) * c), (a + 1.0) + (a - 1.0) * c - s };
}

bool anyEffect(const SoundChain::Params &p)
{
    if (!same(p.shelfDb, 0.0) || p.panWet > 0.0 || p.wet > 0.0)
        return true;
    return std::any_of(p.gains.cbegin(), p.gains.cend(), [](double g) { return !same(g, 0.0); });
}

// The two weights amix@pan is given: the dry chain's, and the 8D branch's.
QString panWeights(double panWet)
{
    const double dry = roundTo(1.0 - panWet / SoundChain::kEightDWeight, 2);
    return SoundChain::number(dry, 2) + QLatin1Char(' ') + SoundChain::number(panWet, 2);
}

// One value a ramp moves: where it is, where it goes, how far one step may
// take it and how finely it is written.
struct Lane {
    double *value;
    double to;
    double limit;
    int decimals;
};

QList<Lane> lanesOf(SoundChain::Params &from, const SoundChain::Params &to)
{
    QList<Lane> lanes;
    lanes.append({ &from.preDb, to.preDb, kPreStepLimit, 1 });
    for (int i = 0; i < SoundChain::kBands; ++i)
        lanes.append({ &from.gains[size_t(i)], to.gains[size_t(i)], kGainStepLimit, 1 });
    lanes.append({ &from.shelfDb, to.shelfDb, kGainStepLimit, 1 });
    // Only a graph with the 8D branch has its weights to move.
    if (from.has8D)
        lanes.append({ &from.panWet, to.panWet, kWeightStepLimit, 2 });
    lanes.append({ &from.wet, to.wet, kWeightStepLimit, 2 });
    lanes.append({ &from.limit, to.limit, kLimitStepLimit, 3 });
    return lanes;
}

} // namespace

bool SoundChain::Settings::operator==(const Settings &o) const
{
    if (slowedReverb != o.slowedReverb || !same(slowedSpeed, o.slowedSpeed) || reverbLevel != o.reverbLevel
        || nightcore != o.nightcore || !same(nightcoreSpeed, o.nightcoreSpeed) || eightD != o.eightD
        || highBass != o.highBass || highBassDb != o.highBassDb || eq != o.eq || preset != o.preset)
        return false;
    for (int i = 0; i < kBands; ++i) {
        if (!same(gains[size_t(i)], o.gains[size_t(i)]))
            return false;
    }
    return true;
}

bool SoundChain::Params::operator==(const Params &o) const
{
    if (!same(preDb, o.preDb) || !same(shelfDb, o.shelfDb) || !same(panWet, o.panWet) || !same(wet, o.wet)
        || !same(limit, o.limit) || has8D != o.has8D)
        return false;
    for (int i = 0; i < kBands; ++i) {
        if (!same(gains[size_t(i)], o.gains[size_t(i)]))
            return false;
    }
    return true;
}

QString SoundChain::number(double value, int decimals)
{
    QString text = QString::number(roundTo(value, decimals), 'f', decimals);
    if (text.contains(QLatin1Char('.'))) {
        while (text.endsWith(QLatin1Char('0')))
            text.chop(1);
        if (text.endsWith(QLatin1Char('.')))
            text.chop(1);
    }
    if (text == QLatin1String("-0") || text.isEmpty())
        text = QStringLiteral("0");
    return text;
}

SoundChain::Settings SoundChain::sanitized(const Settings &settings)
{
    Settings s = settings;
    s.slowedSpeed = nearest(kSlowedSpeeds, s.slowedSpeed);
    s.nightcoreSpeed = nearest(kNightcoreSpeeds, s.nightcoreSpeed);
    s.reverbLevel = qBound(0, s.reverbLevel, 3);
    int bass = kHighBassSteps[0];
    for (int step : kHighBassSteps) {
        if (std::abs(step - s.highBassDb) < std::abs(bass - s.highBassDb))
            bass = step;
    }
    s.highBassDb = bass;
    for (double &gain : s.gains) {
        if (!std::isfinite(gain))
            gain = 0.0;
        gain = std::round(qBound(-kMaxGainDb, gain, kMaxGainDb) / kGainStepDb) * kGainStepDb;
        if (gain == 0.0)
            gain = 0.0;
    }
    // Both own mpv's speed: Slowed wins a tie, which only a damaged setting
    // can make.
    if (s.slowedReverb)
        s.nightcore = false;
    if (s.preset != QLatin1String("custom") && !preset(s.preset))
        s.preset = presetFor(s.gains);
    return s;
}

double SoundChain::reverbWet(int level)
{
    return kReverbWets[size_t(qBound(0, level, 3))];
}

SoundChain::Params SoundChain::neutral()
{
    return Params();
}

SoundChain::Params SoundChain::paramsFor(const Settings &settings)
{
    const Settings s = sanitized(settings);
    Params p;
    if (s.eq) {
        for (int i = 0; i < kBands; ++i)
            p.gains[size_t(i)] = roundTo(s.gains[size_t(i)], 1);
    }
    if (s.highBass)
        p.shelfDb = s.highBassDb;
    double wet = s.slowedReverb ? reverbWet(s.reverbLevel) : 0.0;
    // 8D brings a light room with it; Slowed's own reverb wins if it is more.
    if (s.eightD) {
        wet = std::max(wet, kEightDRoom);
        p.panWet = kEightDWeight;
        p.has8D = true;
    }
    p.wet = roundTo(wet, 2);
    // Down to the 0.1 dB it is written with, never up: rounded to the
    // nearest, 0.05 dB more than the +6 dB could reach the limiter.
    const double pre = std::floor(preGainDb(peakResponseDb(p.gains, p.shelfDb), p.wet) * 10.0 + 1e-6) / 10.0;
    p.preDb = pre == 0.0 ? 0.0 : pre;
    p.limit = anyEffect(p) ? kLimit : 1.0;
    return p;
}

bool SoundChain::needsChain(const Settings &settings)
{
    return anyEffect(paramsFor(settings));
}

bool SoundChain::isNeutral(const Params &params)
{
    return !anyEffect(params) && same(params.preDb, 0.0) && same(params.limit, 1.0);
}

double SoundChain::speedFactor(const Settings &settings)
{
    const Settings s = sanitized(settings);
    if (s.slowedReverb)
        return s.slowedSpeed;
    if (s.nightcore)
        return s.nightcoreSpeed;
    return 1.0;
}

bool SoundChain::ownsSpeed(const Settings &settings)
{
    return settings.slowedReverb || settings.nightcore;
}

QString SoundChain::graph(const Params &p)
{
    QString text = QStringLiteral("aformat=channel_layouts=stereo,volume@pre=volume=%1dB").arg(number(p.preDb, 1));
    for (int i = 0; i < kBands; ++i) {
        text += QStringLiteral(",equalizer@b%1=f=%2:t=q:w=1.41:g=%3")
                    .arg(i)
                    .arg(kBandHz[size_t(i)])
                    .arg(number(p.gains[size_t(i)], 1));
    }
    text += QStringLiteral(",lowshelf@hb=f=100:t=q:w=0.707:g=%1").arg(number(p.shelfDb, 1));
    if (p.has8D) {
        text += QStringLiteral(",asplit=2[p0][p1];[p1]") + QLatin1String(kEightD)
                + QStringLiteral("[p8];[p0][p8]amix@pan=inputs=2:weights=") + panWeights(p.panWet)
                + QStringLiteral(":normalize=0");
    }
    text += QStringLiteral(",asplit=2[d][w];") + QLatin1String(kRoomIr)
            + QStringLiteral(";[w][ir]afir@rev=irnorm=2:minp=1024[r];[d][r]amix@mix=inputs=2:weights=1 ")
            + number(p.wet, 2) + QStringLiteral(":normalize=0,alimiter@lim=limit=") + number(p.limit, 3)
            + QStringLiteral(":attack=5:release=100:level=0:latency=1");
    return text;
}

QString SoundChain::afValue(const QString &graphText)
{
    // mpv reads exactly that many bytes as the value, whatever is in them:
    // the graph's own commas, colons and quotes need no escaping of mpv's.
    return QStringLiteral("@fx:lavfi=graph=%") + QString::number(graphText.toUtf8().size()) + QLatin1Char('%')
           + graphText;
}

QString SoundChain::afValue(const Params &params)
{
    return afValue(graph(params));
}

QList<SoundChain::Command> SoundChain::commands(const Params &from, const Params &to)
{
    QList<Command> list;
    const auto add = [&list](const QString &option, const QString &before, const QString &after, const QString &target) {
        if (before != after)
            list.append({ option, after, target });
    };
    add(QStringLiteral("volume"), number(from.preDb, 1) + QStringLiteral("dB"), number(to.preDb, 1) + QStringLiteral("dB"),
        QStringLiteral("volume@pre"));
    for (int i = 0; i < kBands; ++i) {
        add(QStringLiteral("g"), number(from.gains[size_t(i)], 1), number(to.gains[size_t(i)], 1),
            QStringLiteral("equalizer@b%1").arg(i));
    }
    add(QStringLiteral("g"), number(from.shelfDb, 1), number(to.shelfDb, 1), QStringLiteral("lowshelf@hb"));
    if (from.has8D)
        add(QStringLiteral("weights"), panWeights(from.panWet), panWeights(to.panWet), QStringLiteral("amix@pan"));
    add(QStringLiteral("weights"), QStringLiteral("1 ") + number(from.wet, 2), QStringLiteral("1 ") + number(to.wet, 2),
        QStringLiteral("amix@mix"));
    add(QStringLiteral("limit"), number(from.limit, 3), number(to.limit, 3), QStringLiteral("alimiter@lim"));
    return list;
}

QList<SoundChain::Command> SoundChain::commandsFor(const Params &p)
{
    QList<Command> list;
    list.append({ QStringLiteral("volume"), number(p.preDb, 1) + QStringLiteral("dB"), QStringLiteral("volume@pre") });
    for (int i = 0; i < kBands; ++i)
        list.append({ QStringLiteral("g"), number(p.gains[size_t(i)], 1), QStringLiteral("equalizer@b%1").arg(i) });
    list.append({ QStringLiteral("g"), number(p.shelfDb, 1), QStringLiteral("lowshelf@hb") });
    if (p.has8D)
        list.append({ QStringLiteral("weights"), panWeights(p.panWet), QStringLiteral("amix@pan") });
    list.append({ QStringLiteral("weights"), QStringLiteral("1 ") + number(p.wet, 2), QStringLiteral("amix@mix") });
    list.append({ QStringLiteral("limit"), number(p.limit, 3), QStringLiteral("alimiter@lim") });
    return list;
}

// FFmpeg's own test (af_biquads.c): a band whose 2·pi·f / rate is past pi.
// At exactly half the rate (16 kHz at 32 kHz) it builds the band, and takes
// commands.
QList<SoundChain::Command> SoundChain::commandsAt(const QList<Command> &commands, int sampleRate)
{
    static const QString band = QStringLiteral("equalizer@b");
    QList<Command> list;
    for (const Command &command : commands) {
        if (command.target.startsWith(band)) {
            bool ok = false;
            const int index = QStringView(command.target).mid(band.size()).toInt(&ok);
            if (!ok || index < 0 || index >= kBands || sampleRate <= 0
                || 2 * qint64(kBandHz[size_t(index)]) > sampleRate)
                continue;
        }
        list.append(command);
    }
    return list;
}

int SoundChain::stepsBetween(const Params &from, const Params &to)
{
    Params scratch = from;
    int steps = 0;
    for (const Lane &lane : lanesOf(scratch, to)) {
        const double distance = std::fabs(lane.to - *lane.value);
        if (distance > 1e-9)
            steps = std::max(steps, int(std::ceil(distance / lane.limit - 1e-6)));
    }
    return steps;
}

SoundChain::Params SoundChain::stepToward(const Params &from, const Params &to)
{
    Params next = from;
    const int steps = stepsBetween(from, to);
    if (steps == 0)
        return next;
    // Each value a share of what is left, so they all arrive on the same
    // step: the pre-gain keeps pace with the boost it makes room for.
    for (const Lane &lane : lanesOf(next, to)) {
        if (steps == 1)
            *lane.value = lane.to;
        else
            *lane.value = roundTo(*lane.value + (lane.to - *lane.value) / steps, lane.decimals);
    }
    return next;
}

double SoundChain::responseDb(const std::array<double, kBands> &gains, double shelfDb, double hz)
{
    std::complex<double> h(1.0, 0.0);
    for (int i = 0; i < kBands; ++i) {
        if (!same(gains[size_t(i)], 0.0))
            h *= peaking(kBandHz[size_t(i)], gains[size_t(i)], kBandQ).at(hz);
    }
    if (!same(shelfDb, 0.0))
        h *= lowShelf(kShelfHz, shelfDb, kShelfQ).at(hz);
    return 20.0 * std::log10(std::max(std::abs(h), 1e-12));
}

double SoundChain::peakResponseDb(const std::array<double, kBands> &gains, double shelfDb)
{
    constexpr int kPoints = 240;
    double peak = -1e9;
    for (int k = 0; k < kPoints; ++k) {
        const double hz = 20.0 * std::pow(1000.0, double(k) / double(kPoints - 1));
        peak = std::max(peak, responseDb(gains, shelfDb, hz));
    }
    return peak;
}

double SoundChain::preGainDb(double peakDb, double wet)
{
    const double p = std::max(0.0, peakDb);
    const double db = -(0.5 * std::min(p, kMaxGainDb) + std::max(0.0, p - kMaxGainDb)) - (wet > 0.0 ? 2.0 : 0.0);
    return db == 0.0 ? 0.0 : db;
}

QList<double> SoundChain::responseCurve(const std::array<double, kBands> &gains, double shelfDb, int points)
{
    QList<double> curve;
    if (points < 2)
        return curve;
    const double first = kBandHz.front();
    const double last = kBandHz.back();
    curve.reserve(points);
    for (int k = 0; k < points; ++k) {
        const double hz = first * std::pow(last / first, double(k) / double(points - 1));
        curve.append(responseDb(gains, shelfDb, hz));
    }
    return curve;
}

const QList<SoundChain::Preset> &SoundChain::presets()
{
    // Gentle curves, none above +6 dB on a slider: VLC's and Winamp's reach
    // +16.8 dB once their bands add up.
    static const QList<Preset> list = {
        { QStringLiteral("flat"), QStringLiteral("Flat"), { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 } },
        { QStringLiteral("bass_boost"), QStringLiteral("Bass boost"), { 6, 5, 4, 2, 0, 0, 0, 0, 0, 0 } },
        { QStringLiteral("bass_cut"), QStringLiteral("Bass cut"), { -6, -5, -4, -2, 0, 0, 0, 0, 0, 0 } },
        { QStringLiteral("treble_boost"), QStringLiteral("Treble boost"), { 0, 0, 0, 0, 0, 1, 2, 4, 5, 6 } },
        { QStringLiteral("treble_cut"), QStringLiteral("Treble cut"), { 0, 0, 0, 0, 0, -1, -2, -4, -5, -6 } },
        { QStringLiteral("vocal"), QStringLiteral("Vocal"), { -2, -2, -1, 1, 3, 4, 3, 1, 0, -1 } },
        { QStringLiteral("loudness"), QStringLiteral("Loudness"), { 5, 4, 1, 0, -1, 0, 0, 2, 4, 4 } },
        { QStringLiteral("small_speakers"), QStringLiteral("Small speakers"), { 0, 2, 4, 3, 1, 0, 1, 2, 2, 1 } },
        { QStringLiteral("rock"), QStringLiteral("Rock"), { 4, 3, 2, 0, -1, -1, 1, 2, 3, 4 } },
        { QStringLiteral("pop"), QStringLiteral("Pop"), { -1, 1, 2, 3, 2, 0, -1, -1, 0, 0 } },
        { QStringLiteral("hiphop"), QStringLiteral("Hip-hop"), { 5, 4, 2, 1, -1, -1, 1, 0, 1, 2 } },
        { QStringLiteral("electronic"), QStringLiteral("Electronic"), { 4, 4, 1, 0, -2, 1, 0, 1, 3, 4 } },
        { QStringLiteral("acoustic"), QStringLiteral("Acoustic"), { 3, 3, 2, 1, 1, 1, 2, 2, 2, 1 } },
        { QStringLiteral("classical"), QStringLiteral("Classical"), { 3, 2, 1, 0, 0, 0, 0, 1, 2, 3 } },
        { QStringLiteral("jazz"), QStringLiteral("Jazz"), { 3, 2, 1, 2, -1, -1, 0, 1, 2, 3 } },
        { QStringLiteral("spoken"), QStringLiteral("Spoken word"), { -3, -2, 0, 1, 3, 4, 4, 2, 0, -2 } },
    };
    return list;
}

const SoundChain::Preset *SoundChain::preset(const QString &id)
{
    for (const Preset &p : presets()) {
        if (p.id == id)
            return &p;
    }
    return nullptr;
}

QString SoundChain::presetFor(const std::array<double, kBands> &gains)
{
    for (const Preset &p : presets()) {
        bool match = true;
        for (int i = 0; i < kBands && match; ++i)
            match = same(p.gains[size_t(i)], gains[size_t(i)]);
        if (match)
            return p.id;
    }
    return QStringLiteral("custom");
}

QStringList SoundChain::bandLabels()
{
    QStringList labels;
    for (int hz : kBandHz)
        labels << (hz >= 1000 ? QString::number(hz / 1000) + QLatin1Char('K') : QString::number(hz));
    return labels;
}
