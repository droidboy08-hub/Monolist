#include "soundselftest.h"

#include "appdatabase.h"
#include "downloadmanager.h"
#include "library.h"
#include "mpvengine.h"
#include "playbackcontroller.h"
#include "soundchain.h"
#include "soundeffects.h"
#include "streamresolver.h"

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QHash>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QSet>
#include <QSqlQuery>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>

namespace {

// One line per check, and a count at the end, as in sessionselftest.cpp.
class Checks
{
public:
    explicit Checks(const char *prefix) : m_prefix(prefix) {}

    bool check(bool ok, const QString &what, const QString &detail = QString())
    {
        ++m_count;
        if (!ok)
            ++m_failed;
        const QString line = detail.isEmpty() ? what : what + QStringLiteral("  -- ") + detail;
        qWarning("%s: %s  %s", m_prefix, ok ? "ok  " : "FAIL", qUtf8Printable(line));
        return ok;
    }

    void note(const QString &text) { qWarning("%s: %s", m_prefix, qUtf8Printable(text)); }

    int finish()
    {
        qWarning("%s: %d checks, %d failed", m_prefix, m_count, m_failed);
        return m_failed;
    }

private:
    const char *m_prefix;
    int m_count = 0;
    int m_failed = 0;
};

bool waitUntil(const std::function<bool()> &done, int timeoutMs)
{
    if (done())
        return true;
    QEventLoop loop;
    QTimer poll;
    poll.setInterval(10);
    QObject::connect(&poll, &QTimer::timeout, &loop, [&]() {
        if (done())
            loop.quit();
    });
    QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
    poll.start();
    loop.exec();
    return done();
}

void pause(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

bool near(double a, double b, double within)
{
    return std::fabs(a - b) <= within;
}

QString num(double value, int decimals = 2)
{
    return QString::number(value, 'f', decimals);
}

// The chain at rest, without and with the 8D branch, and with everything up
// (Bass boost, High bass +12, heavy reverb, 8D): the very texts the research
// harness (eqresearch/build/fullgraph.py, graph()) played inside the shipped
// libmpv, from every kind of source, answering every command. Kept apart from
// SoundChain on purpose: a change to the templates has to change these too.
const char kGoldenNeutral[] =
    R"(aformat=channel_layouts=stereo,volume@pre=volume=0dB,equalizer@b0=f=31:t=q:w=1.41:g=0,equalizer@b1=f=62:t=q:w=1.41:g=0,equalizer@b2=f=125:t=q:w=1.41:g=0,equalizer@b3=f=250:t=q:w=1.41:g=0,equalizer@b4=f=500:t=q:w=1.41:g=0,equalizer@b5=f=1000:t=q:w=1.41:g=0,equalizer@b6=f=2000:t=q:w=1.41:g=0,equalizer@b7=f=4000:t=q:w=1.41:g=0,equalizer@b8=f=8000:t=q:w=1.41:g=0,equalizer@b9=f=16000:t=q:w=1.41:g=0,lowshelf@hb=f=100:t=q:w=0.707:g=0,asplit=2[d][w];anoisesrc=d=2.5:c=white:r=48000:a=1:seed=11[nl];anoisesrc=d=2.5:c=white:r=48000:a=1:seed=22[nr];[nl][nr]join=inputs=2:channel_layout=stereo:map=0.0-FL|1.0-FR,afade=t=out:st=0:d=3.67:curve=exp,afade=t=in:st=0:d=0.004,adelay=delays=20:all=1,atrim=end=2.5,highpass=f=150,lowpass=f=6000,afade=t=out:st=2.3:d=0.2[ir];[w][ir]afir@rev=irnorm=2:minp=1024[r];[d][r]amix@mix=inputs=2:weights=1 0:normalize=0,alimiter@lim=limit=1:attack=5:release=100:level=0:latency=1)";
const char kGoldenNeutral8D[] =
    R"(aformat=channel_layouts=stereo,volume@pre=volume=0dB,equalizer@b0=f=31:t=q:w=1.41:g=0,equalizer@b1=f=62:t=q:w=1.41:g=0,equalizer@b2=f=125:t=q:w=1.41:g=0,equalizer@b3=f=250:t=q:w=1.41:g=0,equalizer@b4=f=500:t=q:w=1.41:g=0,equalizer@b5=f=1000:t=q:w=1.41:g=0,equalizer@b6=f=2000:t=q:w=1.41:g=0,equalizer@b7=f=4000:t=q:w=1.41:g=0,equalizer@b8=f=8000:t=q:w=1.41:g=0,equalizer@b9=f=16000:t=q:w=1.41:g=0,lowshelf@hb=f=100:t=q:w=0.707:g=0,asplit=2[p0][p1];[p1]aeval=exprs='st(3,2*PI*t/10);st(4,0.8*sin(ld(3)));st(5,max(0,-cos(ld(3))));st(6,(val(0)+val(1))/2);st(0,ld(0)+(1-exp(-2*PI*2000/s))*(ld(6)-ld(0)));st(7,ld(6)+min(1,0.875*max(0,ld(4))+0.5*ld(5))*(ld(0)-ld(6)));st(8,0.5+0.00075*s*max(0,ld(4)));st(8,(1-ld(8))/(1+ld(8)));st(9,ld(8)*ld(7)+ld(1)-ld(8)*ld(2));st(1,ld(7));st(2,ld(9));ld(9)*cos(PI/4*(1+ld(4)))|st(3,2*PI*t/10);st(4,0.8*sin(ld(3)));st(5,max(0,-cos(ld(3))));st(6,(val(0)+val(1))/2);st(0,ld(0)+(1-exp(-2*PI*2000/s))*(ld(6)-ld(0)));st(7,ld(6)+min(1,0.875*max(0,-ld(4))+0.5*ld(5))*(ld(0)-ld(6)));st(8,0.5+0.00075*s*max(0,-ld(4)));st(8,(1-ld(8))/(1+ld(8)));st(9,ld(8)*ld(7)+ld(1)-ld(8)*ld(2));st(1,ld(7));st(2,ld(9));ld(9)*sin(PI/4*(1+ld(4)))':c=stereo[p8];[p0][p8]amix@pan=inputs=2:weights=1 0:normalize=0,asplit=2[d][w];anoisesrc=d=2.5:c=white:r=48000:a=1:seed=11[nl];anoisesrc=d=2.5:c=white:r=48000:a=1:seed=22[nr];[nl][nr]join=inputs=2:channel_layout=stereo:map=0.0-FL|1.0-FR,afade=t=out:st=0:d=3.67:curve=exp,afade=t=in:st=0:d=0.004,adelay=delays=20:all=1,atrim=end=2.5,highpass=f=150,lowpass=f=6000,afade=t=out:st=2.3:d=0.2[ir];[w][ir]afir@rev=irnorm=2:minp=1024[r];[d][r]amix@mix=inputs=2:weights=1 0:normalize=0,alimiter@lim=limit=1:attack=5:release=100:level=0:latency=1)";
const char kGoldenFull8D[] =
    R"(aformat=channel_layouts=stereo,volume@pre=volume=-15dB,equalizer@b0=f=31:t=q:w=1.41:g=6,equalizer@b1=f=62:t=q:w=1.41:g=5,equalizer@b2=f=125:t=q:w=1.41:g=4,equalizer@b3=f=250:t=q:w=1.41:g=2,equalizer@b4=f=500:t=q:w=1.41:g=0,equalizer@b5=f=1000:t=q:w=1.41:g=0,equalizer@b6=f=2000:t=q:w=1.41:g=0,equalizer@b7=f=4000:t=q:w=1.41:g=0,equalizer@b8=f=8000:t=q:w=1.41:g=0,equalizer@b9=f=16000:t=q:w=1.41:g=0,lowshelf@hb=f=100:t=q:w=0.707:g=12,asplit=2[p0][p1];[p1]aeval=exprs='st(3,2*PI*t/10);st(4,0.8*sin(ld(3)));st(5,max(0,-cos(ld(3))));st(6,(val(0)+val(1))/2);st(0,ld(0)+(1-exp(-2*PI*2000/s))*(ld(6)-ld(0)));st(7,ld(6)+min(1,0.875*max(0,ld(4))+0.5*ld(5))*(ld(0)-ld(6)));st(8,0.5+0.00075*s*max(0,ld(4)));st(8,(1-ld(8))/(1+ld(8)));st(9,ld(8)*ld(7)+ld(1)-ld(8)*ld(2));st(1,ld(7));st(2,ld(9));ld(9)*cos(PI/4*(1+ld(4)))|st(3,2*PI*t/10);st(4,0.8*sin(ld(3)));st(5,max(0,-cos(ld(3))));st(6,(val(0)+val(1))/2);st(0,ld(0)+(1-exp(-2*PI*2000/s))*(ld(6)-ld(0)));st(7,ld(6)+min(1,0.875*max(0,-ld(4))+0.5*ld(5))*(ld(0)-ld(6)));st(8,0.5+0.00075*s*max(0,-ld(4)));st(8,(1-ld(8))/(1+ld(8)));st(9,ld(8)*ld(7)+ld(1)-ld(8)*ld(2));st(1,ld(7));st(2,ld(9));ld(9)*sin(PI/4*(1+ld(4)))':c=stereo[p8];[p0][p8]amix@pan=inputs=2:weights=0 1.41:normalize=0,asplit=2[d][w];anoisesrc=d=2.5:c=white:r=48000:a=1:seed=11[nl];anoisesrc=d=2.5:c=white:r=48000:a=1:seed=22[nr];[nl][nr]join=inputs=2:channel_layout=stereo:map=0.0-FL|1.0-FR,afade=t=out:st=0:d=3.67:curve=exp,afade=t=in:st=0:d=0.004,adelay=delays=20:all=1,atrim=end=2.5,highpass=f=150,lowpass=f=6000,afade=t=out:st=2.3:d=0.2[ir];[w][ir]afir@rev=irnorm=2:minp=1024[r];[d][r]amix@mix=inputs=2:weights=1 1:normalize=0,alimiter@lim=limit=0.891:attack=5:release=100:level=0:latency=1)";

// Everything up: Bass boost on the bands, High bass +12, Slowed with heavy
// reverb, 8D.
SoundChain::Settings everything()
{
    SoundChain::Settings s;
    s.slowedReverb = true;
    s.reverbLevel = 3;
    s.eightD = true;
    s.highBass = true;
    s.highBassDb = 12;
    s.eq = true;
    s.preset = QStringLiteral("bass_boost");
    s.gains = SoundChain::preset(s.preset)->gains;
    return s;
}

QString describe(const QList<SoundChain::Command> &commands)
{
    QStringList parts;
    for (const SoundChain::Command &c : commands)
        parts << c.option + QLatin1Char(' ') + c.value + QLatin1Char(' ') + c.target;
    return parts.join(QStringLiteral(" | "));
}

QString describe(const SoundChain::Params &p)
{
    QStringList gains;
    for (double g : p.gains)
        gains << SoundChain::number(g, 1);
    return QStringLiteral("pre %1, gains [%2], shelf %3, pan %4, wet %5, limit %6%7")
        .arg(SoundChain::number(p.preDb, 1), gains.join(QLatin1Char(' ')), SoundChain::number(p.shelfDb, 1),
             SoundChain::number(p.panWet, 2), SoundChain::number(p.wet, 2), SoundChain::number(p.limit, 3),
             p.has8D ? QStringLiteral(", 8D branch") : QString());
}

// --------------------------------------------------------------- the chain

void testChain(Checks &t)
{
    t.note(QStringLiteral("- the chain"));
    using namespace SoundChain;
    t.check(graph(neutral()) == QLatin1String(kGoldenNeutral), QStringLiteral("the chain at rest is the golden text"),
            graph(neutral()).left(120));
    Params rest8 = neutral();
    rest8.has8D = true;
    t.check(graph(rest8) == QLatin1String(kGoldenNeutral8D),
            QStringLiteral("  and with the 8D branch, the golden text with it"));
    t.check(graph(paramsFor(everything())) == QLatin1String(kGoldenFull8D),
            QStringLiteral("  and with everything up (Bass boost, High bass +12, heavy reverb, 8D)"),
            describe(paramsFor(everything())));
    t.check(graph(neutral()).contains(QLatin1String("lowshelf@hb=f=100:t=q:w=0.707"))
                && !graph(neutral()).contains(QLatin1String("bass@")),
            QStringLiteral("High bass is FFmpeg's lowshelf, the shape the maths models, not its 'bass'"));
    const QString af = afValue(neutral());
    t.check(af == QStringLiteral("@fx:lavfi=graph=%903%") + QLatin1String(kGoldenNeutral),
            QStringLiteral("mpv's af is @fx:lavfi=graph=%<bytes>%<graph>"), af.left(40));
    t.check(afValue(rest8).startsWith(QLatin1String("@fx:lavfi=graph=%1685%")), QStringLiteral("  its bytes counted"),
            afValue(rest8).left(30));

    Settings none;
    Settings slowedDry;
    slowedDry.slowedReverb = true;
    slowedDry.reverbLevel = 0;
    Settings nightcore;
    nightcore.nightcore = true;
    Settings flatEq;
    flatEq.eq = true;
    Settings reverb;
    reverb.slowedReverb = true;
    Settings bass;
    bass.highBass = true;
    Settings curve;
    curve.eq = true;
    curve.gains[3] = 0.5;
    Settings eightD;
    eightD.eightD = true;
    t.check(!needsChain(none) && !needsChain(slowedDry) && !needsChain(nightcore) && !needsChain(flatEq),
            QStringLiteral("no chain for nothing on, Slowed without reverb, Nightcore, or a flat equaliser"));
    t.check(needsChain(reverb) && needsChain(bass) && needsChain(curve) && needsChain(eightD),
            QStringLiteral("a chain for reverb, High bass, a curve on the bands, and 8D"));

    Params p = paramsFor(bass);
    t.check(near(p.shelfDb, 6.0, 1e-9) && near(p.preDb, -3.0, 0.05) && near(p.limit, kLimit, 1e-9) && p.wet == 0.0
                && p.panWet == 0.0 && !p.has8D,
            QStringLiteral("High bass +6: the shelf at 6, turned down 3.0 dB, the limiter at 0.891"), describe(p));
    p = paramsFor(reverb);
    t.check(near(p.wet, 0.70, 1e-9) && near(p.preDb, -2.0, 1e-9) && near(p.limit, kLimit, 1e-9),
            QStringLiteral("Slowed + reverb, medium: the reverb at 0.70, turned down 2 dB"), describe(p));
    p = paramsFor(eightD);
    t.check(p.has8D && near(p.panWet, 1.41, 1e-9) && near(p.wet, 0.30, 1e-9) && near(p.preDb, -2.0, 1e-9)
                && near(p.limit, kLimit, 1e-9),
            QStringLiteral("8D: its branch in at 1.41 (+3 dB), a light room (0.30), turned down 2 dB"), describe(p));
    Settings eightLight = eightD;
    eightLight.slowedReverb = true;
    eightLight.reverbLevel = 1;
    Settings eightHeavy = eightLight;
    eightHeavy.reverbLevel = 3;
    Settings eightDry = eightLight;
    eightDry.reverbLevel = 0;
    t.check(near(paramsFor(eightLight).wet, 0.35, 1e-9) && near(paramsFor(eightHeavy).wet, 1.0, 1e-9)
                && near(paramsFor(eightDry).wet, 0.30, 1e-9),
            QStringLiteral("  with Slowed's reverb the larger of the two: light 0.35, heavy 1.00, off 0.30"));

    Settings both;
    both.slowedReverb = true;
    both.nightcore = true;
    t.check(speedFactor(none) == 1.0 && near(speedFactor(reverb), 0.85, 1e-9) && near(speedFactor(nightcore), 1.25, 1e-9),
            QStringLiteral("speed: 1, Slowed 0.85, Nightcore 1.25"));
    t.check(!sanitized(both).nightcore && near(speedFactor(both), 0.85, 1e-9),
            QStringLiteral("Slowed and Nightcore never together: Slowed wins a damaged setting"));
    t.check(ownsSpeed(reverb) && ownsSpeed(nightcore) && ownsSpeed(slowedDry) && !ownsSpeed(bass) && !ownsSpeed(eightD),
            QStringLiteral("pitch correction off for Slowed and Nightcore alone"));

    const QList<Command> on = commands(neutral(), paramsFor(bass));
    const QList<Command> expected = { { QStringLiteral("volume"), QStringLiteral("-3dB"), QStringLiteral("volume@pre") },
                                      { QStringLiteral("g"), QStringLiteral("6"), QStringLiteral("lowshelf@hb") },
                                      { QStringLiteral("limit"), QStringLiteral("0.891"), QStringLiteral("alimiter@lim") } };
    t.check(on == expected, QStringLiteral("High bass on is three commands: the pre-gain, the shelf, the limiter"),
            describe(on));
    t.check(commands(paramsFor(bass), paramsFor(bass)).isEmpty(), QStringLiteral("  the same values, none"));
    Settings light = reverb;
    light.reverbLevel = 1;
    const QList<Command> lighter = commands(paramsFor(reverb), paramsFor(light));
    t.check(lighter.size() == 1 && lighter.first().option == QLatin1String("weights")
                && lighter.first().value == QLatin1String("1 0.35") && lighter.first().target == QLatin1String("amix@mix"),
            QStringLiteral("  reverb medium to light: one, weights \"1 0.35\" to amix@mix"), describe(lighter));
    const QList<Command> in8 = commands(rest8, paramsFor(eightD));
    t.check(in8.contains(Command{ QStringLiteral("weights"), QStringLiteral("0 1.41"), QStringLiteral("amix@pan") })
                && in8.contains(Command{ QStringLiteral("weights"), QStringLiteral("1 0.3"), QStringLiteral("amix@mix") }),
            QStringLiteral("  8D in a graph with its branch: amix@pan to \"0 1.41\", the room to 0.3"), describe(in8));
    bool noPan = true;
    for (const Command &c : commands(neutral(), paramsFor(eightD)))
        noPan = noPan && c.target != QLatin1String("amix@pan");
    t.check(noPan, QStringLiteral("  and none to a graph without the branch, which commands cannot add"));
    t.check(commandsFor(neutral()).size() == 14 && commandsFor(rest8).size() == 15,
            QStringLiteral("every value as commands: 14, and 15 with the 8D branch"));

    // A band past half the sound's rate is never sent a command: one crashed
    // the shipped libmpv (16 kHz in a 22.05 kHz file).
    const auto bandsAt = [&rest8](int rate) {
        QStringList bands;
        for (const Command &c : commandsAt(commandsFor(rest8), rate)) {
            if (c.target.startsWith(QLatin1String("equalizer@")))
                bands << c.target.mid(10);   // "b3"
        }
        return bands.join(QLatin1Char(' '));
    };
    const QString all = QStringLiteral("b0 b1 b2 b3 b4 b5 b6 b7 b8 b9");
    t.check(bandsAt(48000) == all && bandsAt(44100) == all && bandsAt(32000) == all,
            QStringLiteral("commands at 48, 44.1 and 32 kHz: every band (16 kHz is half of 32, which FFmpeg takes)"),
            bandsAt(32000));
    t.check(bandsAt(22050) == QLatin1String("b0 b1 b2 b3 b4 b5 b6 b7 b8")
                && bandsAt(16000) == QLatin1String("b0 b1 b2 b3 b4 b5 b6 b7 b8")
                && bandsAt(11025) == QLatin1String("b0 b1 b2 b3 b4 b5 b6 b7")
                && bandsAt(8000) == QLatin1String("b0 b1 b2 b3 b4 b5 b6 b7"),
            QStringLiteral("  at 22.05 and 16 kHz none to 16 kHz; at 11.025 and 8 kHz none to 8 or 16 kHz"),
            bandsAt(22050) + QStringLiteral(" | ") + bandsAt(8000));
    t.check(bandsAt(0).isEmpty() && commandsAt(commandsFor(rest8), 0).size() == 5
                && commandsAt(commands(neutral(), paramsFor(bass)), 8000) == expected,
            QStringLiteral("  the rate not known: no band, the other five kept; the rest never left out"),
            describe(commandsAt(commandsFor(rest8), 0)));

    t.check(number(-0.04, 1) == QLatin1String("0") && number(0.70, 2) == QLatin1String("0.7")
                && number(-2.99, 1) == QLatin1String("-3") && number(1.0, 3) == QLatin1String("1")
                && number(0.891, 3) == QLatin1String("0.891") && number(4.5, 1) == QLatin1String("4.5"),
            QStringLiteral("numbers as the chain writes them: no trailing zeros, never -0"));
}

// --------------------------------------------------------------- the ramps

void testRamps(Checks &t)
{
    t.note(QStringLiteral("- the ramps"));
    using namespace SoundChain;
    Settings bass;
    bass.highBass = true;
    const int steps = stepsBetween(neutral(), paramsFor(bass));
    t.check(steps == 11, QStringLiteral("High bass +6 on takes 11 steps (165 ms): the limiter's ceiling, 0.01 a step"),
            QString::number(steps));

    // Every preset with every High bass, reverb and 8D, ramped from rest and
    // back to it: no step beyond its limit, everything there on one step.
    int cases = 0;
    int bad = 0;
    int longest = 0;
    QString firstBad;
    const auto ramp = [&](Params from, const Params &to) {
        const int planned = stepsBetween(from, to);
        int taken = 0;
        while (from != to && taken < 200) {
            const Params next = stepToward(from, to);
            bool within = next.has8D == from.has8D && std::fabs(next.preDb - from.preDb) <= 0.5 + 1e-9
                          && std::fabs(next.shelfDb - from.shelfDb) <= 1.0 + 1e-9
                          && std::fabs(next.panWet - from.panWet) <= 0.07 + 1e-9
                          && std::fabs(next.wet - from.wet) <= 0.07 + 1e-9
                          && std::fabs(next.limit - from.limit) <= 0.01 + 1e-9;
            for (int i = 0; i < kBands; ++i)
                within = within && std::fabs(next.gains[size_t(i)] - from.gains[size_t(i)]) <= 1.0 + 1e-9;
            if (!within && firstBad.isEmpty())
                firstBad = describe(from) + QStringLiteral(" -> ") + describe(next);
            if (!within)
                ++bad;
            from = next;
            ++taken;
        }
        if (from != to || taken != planned) {
            ++bad;
            if (firstBad.isEmpty())
                firstBad = QStringLiteral("%1 steps planned, %2 taken, to %3").arg(planned).arg(taken).arg(describe(to));
        }
        longest = std::max(longest, taken);
        ++cases;
    };
    for (const Preset &preset : presets()) {
        for (int bassDb : { 0, 3, 6, 9, 12 }) {
            for (int level = 0; level <= 3; ++level) {
                for (bool with8D : { false, true }) {
                    Settings s;
                    s.eq = true;
                    s.gains = preset.gains;
                    s.preset = preset.id;
                    s.highBass = bassDb > 0;
                    s.highBassDb = bassDb > 0 ? bassDb : 6;
                    s.slowedReverb = level > 0;
                    s.reverbLevel = level;
                    s.eightD = with8D;
                    const Params target = paramsFor(s);
                    Params rest = neutral();
                    rest.has8D = target.has8D;
                    ramp(rest, target);
                    ramp(target, rest);
                }
            }
        }
    }
    t.check(bad == 0, QStringLiteral("%1 ramps, there and back: every step within its limit, all values arriving "
                                     "together, the longest %2 steps").arg(cases).arg(longest),
            firstBad);
    t.check(longest <= 30, QStringLiteral("  none longer than 30 steps (450 ms)"), QString::number(longest));

    // The graph's shape is the one it was built with.
    Settings eightD;
    eightD.eightD = true;
    const Params toward8 = stepToward(neutral(), paramsFor(eightD));
    t.check(!toward8.has8D && toward8.panWet == 0.0,
            QStringLiteral("a graph without the 8D branch keeps none, whatever it is moved toward"));
    const Params off8 = paramsFor(eightD);
    Params goal = neutral();
    goal.has8D = true;
    t.check(stepToward(off8, goal).has8D && stepToward(off8, goal).panWet < off8.panWet,
            QStringLiteral("  and one with it turns it down, keeping it"));
}

// --------------------------------------------------------------- presets

void testPresets(Checks &t)
{
    t.note(QStringLiteral("- the presets"));
    using namespace SoundChain;
    const QList<Preset> &list = presets();
    QSet<QString> ids;
    bool inRange = true;
    for (const Preset &p : list) {
        ids.insert(p.id);
        for (double g : p.gains)
            inRange = inRange && std::fabs(g) <= kMaxGainDb && !p.name.isEmpty();
    }
    t.check(list.size() == 16 && ids.size() == 16, QStringLiteral("sixteen presets, each its own id"),
            QString::number(list.size()));
    t.check(inRange, QStringLiteral("  ten gains each, all within +-12 dB, each named"));
    t.check(list.first().id == QLatin1String("flat")
                && std::all_of(list.first().gains.cbegin(), list.first().gains.cend(), [](double g) { return g == 0.0; }),
            QStringLiteral("  Flat first, all zeros"));
    std::array<double, kBands> moved = preset(QStringLiteral("bass_boost"))->gains;
    t.check(presetFor(moved) == QLatin1String("bass_boost"), QStringLiteral("Bass boost's curve is Bass boost"));
    moved[4] = 0.5;
    t.check(presetFor(moved) == QLatin1String("custom"), QStringLiteral("  one band moved off it, custom"));
    t.check(bandLabels() == QStringList({ QStringLiteral("31"), QStringLiteral("62"), QStringLiteral("125"),
                                          QStringLiteral("250"), QStringLiteral("500"), QStringLiteral("1K"),
                                          QStringLiteral("2K"), QStringLiteral("4K"), QStringLiteral("8K"),
                                          QStringLiteral("16K") }),
            QStringLiteral("the bands are labelled 31 to 16K"), bandLabels().join(QLatin1Char(' ')));
    Settings odd;
    odd.gains = { 40.0, -40.0, 3.3, -0.2, std::nan(""), 0, 0, 0, 0, 0 };
    odd.slowedSpeed = 7.0;
    odd.highBassDb = 5;
    odd.reverbLevel = 9;
    odd.preset = QStringLiteral("nonsense");
    const Settings fixed = sanitized(odd);
    t.check(fixed.gains[0] == 12.0 && fixed.gains[1] == -12.0 && fixed.gains[2] == 3.5 && fixed.gains[3] == 0.0
                && fixed.gains[4] == 0.0 && fixed.reverbLevel == 3 && fixed.highBassDb == 6
                && near(fixed.slowedSpeed, 0.90, 1e-9) && fixed.preset == QLatin1String("custom"),
            QStringLiteral("values put right: gains to +-12 in 0.5 steps, steps to the nearest, an unknown preset by its curve"));
}

// --------------------------------------------------------------- clipping

void testClipping(Checks &t)
{
    t.note(QStringLiteral("- the clipping maths"));
    using namespace SoundChain;
    const auto gainsOf = [](const char *id) { return preset(QString::fromLatin1(id))->gains; };
    std::array<double, kBands> flat {};
    std::array<double, kBands> allUp;
    allUp.fill(12.0);
    const std::array<double, kBands> vlcRock = { 8.0, 4.8, -5.6, -8.0, -3.2, 4.0, 8.8, 11.2, 11.2, 11.2 };
    // eqresearch/plan/golden.py (numpy, RBJ at 48 kHz, 240 points), with
    // High bass as the lowshelf it is.
    struct Row {
        const char *name;
        std::array<double, kBands> gains;
        double shelf;
        double wet;
        double peak;
        double pre;
    };
    const QList<Row> table = {
        { "Flat", flat, 0, 0, 0.00, 0.00 },
        { "High bass +6", flat, 6, 0, 5.99, -2.99 },
        { "High bass +9", flat, 9, 0, 8.98, -4.49 },
        { "High bass +12", flat, 12, 0, 11.97, -5.99 },
        { "Bass boost", gainsOf("bass_boost"), 0, 0, 7.14, -3.57 },
        { "Treble boost", gainsOf("treble_boost"), 0, 0, 6.40, -3.20 },
        { "Vocal", gainsOf("vocal"), 0, 0, 5.15, -2.58 },
        { "Loudness", gainsOf("loudness"), 0, 0, 5.80, -2.90 },
        { "Rock", gainsOf("rock"), 0, 0, 4.64, -2.32 },
        { "Hip-hop", gainsOf("hiphop"), 0, 0, 5.85, -2.92 },
        { "Small speakers", gainsOf("small_speakers"), 0, 0, 4.95, -2.48 },
        { "Spoken word", gainsOf("spoken"), 0, 0, 5.38, -2.69 },
        { "VLC's Rock", vlcRock, 0, 0, 15.07, -9.07 },
        { "All bands +12", allUp, 0, 0, 18.42, -12.42 },
        { "Bass boost + High bass +12", gainsOf("bass_boost"), 12, 0, 18.97, -12.97 },
        { "Reverb alone", flat, 0, 0.7, 0.00, -2.00 },
        { "High bass +6 + reverb", flat, 6, 0.7, 5.99, -4.99 },
        { "8D's room", flat, 0, 0.3, 0.00, -2.00 },
    };
    for (const Row &row : table) {
        const double peak = peakResponseDb(row.gains, row.shelf);
        const double pre = preGainDb(peak, row.wet);
        t.check(near(peak, row.peak, 0.05) && near(pre, row.pre, 0.05),
                QStringLiteral("%1: peak %2 dB, turned down %3 dB").arg(QLatin1String(row.name), num(row.peak), num(-row.pre)),
                QStringLiteral("%1, %2").arg(num(peak), num(pre)));
    }

    // The real filters inside the shipped libmpv, sine tones through ao=pcm
    // (eqresearch/build/fullgraph.py parts d and e): the model is what they do.
    struct Measured {
        double hz;
        double db;
    };
    const QList<Measured> shelf6 = { { 30, 5.95 }, { 60, 5.27 }, { 100, 3.00 }, { 150, 1.03 }, { 200, 0.38 }, { 1000, 0.00 } };
    const QList<Measured> shelf12 = { { 30, 11.87 }, { 60, 10.33 }, { 100, 6.00 }, { 150, 2.31 }, { 200, 0.90 }, { 1000, 0.00 } };
    const QList<Measured> band6 = { { 250, 0.22 }, { 500, 1.14 }, { 707, 3.00 }, { 1000, 6.00 },
                                    { 1414, 3.00 }, { 2000, 1.13 }, { 4000, 0.21 } };
    double worst = 0.0;
    for (const Measured &m : shelf6)
        worst = std::max(worst, std::fabs(responseDb(flat, 6, m.hz) - m.db));
    for (const Measured &m : shelf12)
        worst = std::max(worst, std::fabs(responseDb(flat, 12, m.hz) - m.db));
    std::array<double, kBands> up1k {};
    up1k[5] = 6.0;
    std::array<double, kBands> down1k {};
    down1k[5] = -6.0;
    for (const Measured &m : band6) {
        worst = std::max(worst, std::fabs(responseDb(up1k, 0, m.hz) - m.db));
        worst = std::max(worst, std::fabs(responseDb(down1k, 0, m.hz) + m.db));
    }
    t.check(worst <= 0.05, QStringLiteral("the curve is what FFmpeg's lowshelf (+6, +12) and equalizer (+-6) were "
                                          "measured doing in libmpv, within 0.05 dB"),
            QStringLiteral("worst %1 dB").arg(num(worst, 3)));

    // Over everything there is to choose.
    bool neverUp = true;
    double mostLeft = -100.0;
    for (const Preset &preset : presets()) {
        for (int bassDb : { 0, 3, 6, 9, 12 }) {
            for (int level = 0; level <= 3; ++level) {
                for (bool with8D : { false, true }) {
                    Settings s;
                    s.eq = true;
                    s.gains = preset.gains;
                    s.highBass = bassDb > 0;
                    s.highBassDb = bassDb > 0 ? bassDb : 6;
                    s.slowedReverb = level > 0;
                    s.reverbLevel = level;
                    s.eightD = with8D;
                    const Params p = paramsFor(s);
                    neverUp = neverUp && p.preDb <= 0.0;
                    mostLeft = std::max(mostLeft, peakResponseDb(p.gains, p.shelfDb) + p.preDb);
                }
            }
        }
    }
    t.check(neverUp, QStringLiteral("the pre-gain never turns anything up"));
    t.check(mostLeft <= 6.0 + 1e-6, QStringLiteral("  and never leaves more than +6 dB for the limiter, over every "
                                                   "preset, High bass, reverb and 8D"),
            QStringLiteral("at most %1 dB").arg(num(mostLeft)));
    t.check(near(20.0 * std::log10(kLimit), -1.0, 0.01), QStringLiteral("the limiter's 0.891 is -1.00 dBFS"),
            num(20.0 * std::log10(kLimit), 3));
    const QList<double> curve = responseCurve(gainsOf("bass_boost"), 6, 120);
    t.check(curve.size() == 120 && near(curve.first(), responseDb(gainsOf("bass_boost"), 6, 31), 1e-9)
                && near(curve.last(), responseDb(gainsOf("bass_boost"), 6, 16000), 1e-9),
            QStringLiteral("the drawn curve: 120 points from 31 Hz to 16 kHz"));
}

// --------------------------------------------------------------- the model

void clearSoundSettings(Library *library)
{
    for (const char *key : { "sound.slowed_reverb", "sound.slowed_speed", "sound.reverb", "sound.nightcore",
                             "sound.nightcore_speed", "sound.8d", "sound.high_bass", "sound.high_bass_db", "sound.eq",
                             "sound.eq_preset", "sound.eq_gains" })
        library->setSetting(QString::fromLatin1(key), QString());
}

QString setting(Library *library, const char *key)
{
    return library->settingValue(QString::fromLatin1(key));
}

void testModel(Checks &t, Library *library)
{
    t.note(QStringLiteral("- what is kept"));
    clearSoundSettings(library);
    {
        SoundEffects fresh(nullptr, library);
        fresh.restore();
        const SoundChain::Settings s = fresh.settings();
        t.check(!s.slowedReverb && !s.nightcore && !s.eightD && !s.highBass && !s.eq && near(s.slowedSpeed, 0.85, 1e-9)
                    && s.reverbLevel == 2 && near(s.nightcoreSpeed, 1.25, 1e-9) && s.highBassDb == 6
                    && s.preset == QLatin1String("flat") && s.gains == SoundChain::Settings().gains,
                QStringLiteral("a fresh data folder: everything off, 0.85x, medium reverb, 1.25x, +6 dB, Flat"));
        t.check(!fresh.active() && fresh.summary().isEmpty() && fresh.headroomDb() == 0.0 && !fresh.failed(),
                QStringLiteral("  nothing active, no summary, nothing turned down"));
    }

    SoundEffects sound(nullptr, library);
    sound.restore();
    int changes = 0;
    QObject::connect(&sound, &SoundEffects::effectsChanged, &sound, [&changes]() { ++changes; });
    sound.setSlowedReverb(true);
    sound.setSlowedSpeed(0.80);
    sound.setReverbLevel(1);
    sound.setHighBass(true);
    sound.setHighBassDb(9);
    sound.setEightD(true);
    sound.setEqualiser(true);
    t.check(setting(library, "sound.slowed_reverb") == QLatin1String("1") && setting(library, "sound.slowed_speed") == QLatin1String("0.80")
                && setting(library, "sound.reverb") == QLatin1String("1") && setting(library, "sound.high_bass") == QLatin1String("1")
                && setting(library, "sound.high_bass_db") == QLatin1String("9") && setting(library, "sound.8d") == QLatin1String("1")
                && setting(library, "sound.eq") == QLatin1String("1"),
            QStringLiteral("every switch and strength is written at once"));
    t.check(changes == 7, QStringLiteral("  each change said once"), QString::number(changes));
    sound.setHighBassDb(9);
    t.check(changes == 7, QStringLiteral("  and the same value again not at all"));

    sound.setNightcore(true);
    t.check(sound.nightcore() && !sound.slowedReverb() && setting(library, "sound.nightcore") == QLatin1String("1")
                && setting(library, "sound.slowed_reverb") == QLatin1String("0"),
            QStringLiteral("Nightcore on turns Slowed + reverb off, both written"));
    sound.setSlowedReverb(true);
    t.check(sound.slowedReverb() && !sound.nightcore() && setting(library, "sound.nightcore") == QLatin1String("0"),
            QStringLiteral("  and Slowed + reverb on turns Nightcore off"));

    sound.applyPreset(QStringLiteral("bass_boost"));
    t.check(sound.preset() == QLatin1String("bass_boost") && setting(library, "sound.eq_preset") == QLatin1String("bass_boost")
                && sound.bandGains().first().toDouble() == 6.0,
            QStringLiteral("a preset is applied and written at once"));

    // A band dragged through four values in 120 ms.
    QElapsedTimer drag;
    drag.start();
    for (double db : { 5.5, 5.0, 4.5, 4.0 }) {
        sound.setBandGain(0, db);
        pause(30);
    }
    t.check(sound.preset() == QLatin1String("custom") && sound.bandGains().first().toDouble() == 4.0,
            QStringLiteral("a band dragged: the preset is custom at once"));
    t.check(setting(library, "sound.eq_preset") == QLatin1String("bass_boost") && setting(library, "sound.eq_gains").isEmpty(),
            QStringLiteral("  but nothing is written while it moves"));
    const bool written = waitUntil([&]() { return setting(library, "sound.eq_preset") == QLatin1String("custom"); }, 1500);
    const qint64 after = drag.elapsed();
    const QJsonArray kept = QJsonDocument::fromJson(setting(library, "sound.eq_gains").toUtf8()).array();
    t.check(written && after >= 120 + 350 && kept.size() == 10 && kept.at(0).toDouble() == 4.0 && kept.at(1).toDouble() == 5.0,
            QStringLiteral("  written once it has settled, 400 ms after the last move"),
            QStringLiteral("%1 ms after the drag began: %2").arg(after).arg(setting(library, "sound.eq_gains")));

    const SoundChain::Settings before = sound.settings();
    {
        SoundEffects next(nullptr, library);
        next.restore();
        t.check(next.settings() == before, QStringLiteral("the next launch reads the same state"),
                next.summary() + QStringLiteral(" / ") + sound.summary());
        next.setBandGain(9, -3.0);   // and closes at once
    }
    {
        SoundEffects third(nullptr, library);
        third.restore();
        t.check(third.bandGains().at(9).toDouble() == -3.0 && third.preset() == QLatin1String("custom"),
                QStringLiteral("a drag just before closing is written as it closes"),
                setting(library, "sound.eq_gains"));
    }

    const auto restored = [library](const QList<QPair<const char *, const char *>> &values) {
        clearSoundSettings(library);
        for (const auto &value : values)
            library->setSetting(QString::fromLatin1(value.first), QString::fromLatin1(value.second));
        SoundEffects s(nullptr, library);
        s.restore();
        return s.settings();
    };
    SoundChain::Settings s = restored({ { "sound.eq_preset", "bass_boost" }, { "sound.eq_gains", "[1,1,1,1,1,1,1,1,1,1]" } });
    t.check(s.gains == SoundChain::preset(QStringLiteral("bass_boost"))->gains && s.preset == QLatin1String("bass_boost"),
            QStringLiteral("a preset wins over the gains kept beside it"));
    s = restored({ { "sound.eq_preset", "bass_boost" } });
    t.check(s.gains == SoundChain::preset(QStringLiteral("bass_boost"))->gains,
            QStringLiteral("  and needs none (--set sound.eq_preset bass_boost alone)"));
    s = restored({ { "sound.eq_preset", "custom" }, { "sound.eq_gains", "[1,2,3,4,5,6,7,8,9,10]" } });
    t.check(s.preset == QLatin1String("custom") && s.gains[9] == 10.0 && s.gains[0] == 1.0,
            QStringLiteral("a custom curve is read from its gains"));
    s = restored({ { "sound.eq_preset", "custom" }, { "sound.eq_gains", "[40,0,0,0,0,0,0,0,0,-40]" } });
    t.check(s.gains[0] == 12.0 && s.gains[9] == -12.0, QStringLiteral("  gains of 40 are read as 12"));
    s = restored({ { "sound.eq_preset", "custom" }, { "sound.eq_gains", "{not json" } });
    t.check(s.gains == SoundChain::Settings().gains && s.preset == QLatin1String("flat"),
            QStringLiteral("  gains that do not read are Flat"));
    s = restored({ { "sound.eq_preset", "custom" }, { "sound.eq_gains", "[1,2,3]" } });
    t.check(s.gains == SoundChain::Settings().gains, QStringLiteral("  and so are too few of them"));
    s = restored({ { "sound.eq_preset", "nonsense" } });
    t.check(s.preset == QLatin1String("flat"), QStringLiteral("an unknown preset is Flat"));
    s = restored({ { "sound.slowed_speed", "7" }, { "sound.nightcore_speed", "abc" }, { "sound.reverb", "9" },
                   { "sound.high_bass_db", "5" }, { "sound.high_bass", "yes" } });
    t.check(near(s.slowedSpeed, 0.85, 1e-9) && near(s.nightcoreSpeed, 1.25, 1e-9) && s.reverbLevel == 2
                && s.highBassDb == 6 && !s.highBass,
            QStringLiteral("values out of range are the defaults (speed 7, reverb 9, +5 dB, \"yes\")"));
    s = restored({ { "sound.slowed_reverb", "1" }, { "sound.nightcore", "1" } });
    t.check(s.slowedReverb && !s.nightcore, QStringLiteral("Slowed and Nightcore both kept on: Slowed"));

    t.note(QStringLiteral("- what is shown"));
    clearSoundSettings(library);
    SoundEffects shown(nullptr, library);
    shown.restore();
    shown.setSlowedReverb(true);
    shown.setEightD(true);
    shown.setHighBass(true);
    shown.setEqualiser(true);
    shown.applyPreset(QStringLiteral("bass_boost"));
    t.check(shown.summary() == QStringLiteral("Slowed 0.85× + reverb · 8D · High bass +6 dB · EQ Bass boost"),
            QStringLiteral("the summary names what is on"), shown.summary());
    t.check(shown.active(), QStringLiteral("  and the effects are active"));
    shown.setReverbLevel(0);
    shown.setSlowedSpeed(0.8);
    shown.setBandGain(3, -1.0);
    t.check(shown.summary() == QStringLiteral("Slowed 0.80× · 8D · High bass +6 dB · EQ Custom"),
            QStringLiteral("  Slowed with no reverb, a custom curve"), shown.summary());
    shown.setNightcore(true);
    shown.setEightD(false);
    shown.setHighBass(false);
    shown.setEqualiser(false);
    t.check(shown.summary() == QStringLiteral("Nightcore 1.25×"), QStringLiteral("  Nightcore"), shown.summary());
    shown.setNightcore(false);
    shown.setHighBass(true);
    t.check(near(shown.headroomDb(), 3.0, 1e-9), QStringLiteral("High bass +6 alone: turned down 3.0 dB"),
            num(shown.headroomDb(), 1));
    t.check(shown.responseCurve(120).size() == 120 && shown.responseCurve(120).first().toDouble() > 5.0,
            QStringLiteral("the curve for the sliders carries the shelf"));
    t.check(shown.presets().size() == 16 && shown.bandLabels().size() == 10 && shown.slowedSpeeds().size() == 3
                && shown.nightcoreSpeeds().size() == 3 && shown.highBassSteps().size() == 4 && shown.reverbLevels().size() == 4,
            QStringLiteral("the chips: 16 presets, 10 bands, 3 speeds each, 4 bass steps, 4 reverbs"));
    shown.setEightD(true);
    shown.setEqualiser(true);
    shown.turnAllOff();
    t.check(!shown.active() && setting(library, "sound.high_bass") == QLatin1String("0") && setting(library, "sound.8d") == QLatin1String("0")
                && setting(library, "sound.eq") == QLatin1String("0") && shown.highBassDb() == 6 && shown.preset() == QLatin1String("custom"),
            QStringLiteral("Turn all off: every switch off and written, the strengths and the curve kept"));
    shown.resetBands();
    t.check(shown.preset() == QLatin1String("flat") && shown.bandGains().at(3).toDouble() == 0.0,
            QStringLiteral("resetting the bands is Flat"));
    clearSoundSettings(library);
}

// --------------------------------------------------------------- live

// A tone as a 16-bit PCM WAV file.
QByteArray toneWav(int rate, int channels, double seconds, double hz, double amplitude)
{
    constexpr double kPi = 3.14159265358979323846;
    const quint32 frames = quint32(rate * seconds);
    const quint32 dataBytes = frames * quint32(channels) * 2;
    QByteArray wav;
    wav.reserve(int(44 + dataBytes));
    const auto u32 = [&wav](quint32 value) {
        char bytes[4];
        qToLittleEndian(value, bytes);
        wav.append(bytes, 4);
    };
    const auto u16 = [&wav](quint16 value) {
        char bytes[2];
        qToLittleEndian(value, bytes);
        wav.append(bytes, 2);
    };
    wav.append("RIFF", 4);
    u32(36 + dataBytes);
    wav.append("WAVE", 4);
    wav.append("fmt ", 4);
    u32(16);
    u16(1);
    u16(quint16(channels));
    u32(quint32(rate));
    u32(quint32(rate * channels * 2));
    u16(quint16(channels * 2));
    u16(16);
    wav.append("data", 4);
    u32(dataBytes);
    for (quint32 i = 0; i < frames; ++i) {
        const auto sample = quint16(qint16(32767.0 * amplitude * std::sin(2.0 * kPi * hz * double(i) / rate)));
        for (int c = 0; c < channels; ++c)
            u16(sample);
    }
    return wav;
}

// A picture with no sound, as YUV4MPEG2: plain grey frames, which FFmpeg
// reads with no decoder of its own. Loaded with a tone as its sound (load's
// audioUrl), it is YouTube's larger sizes in small, the picture and the sound
// in two files.
QByteArray pictureY4m(int width, int height, int fps, double seconds)
{
    QByteArray y4m = QStringLiteral("YUV4MPEG2 W%1 H%2 F%3:1 Ip A1:1 C420jpeg\n")
                         .arg(width).arg(height).arg(fps).toLatin1();
    const QByteArray frame = QByteArray("FRAME\n") + QByteArray(width * height, char(110))
                             + QByteArray(width * height / 2, char(128));
    const int frames = int(fps * seconds);
    y4m.reserve(y4m.size() + frames * frame.size());
    for (int i = 0; i < frames; ++i)
        y4m += frame;
    return y4m;
}

// The stand-in for a CDN on this computer: every path serves the body, whole
// or from a Range, but a path starting /missing, which is 404, and one
// starting /flaky, whose first request is 503 (a CDN's passing refusal) and
// later ones are served. Throttled, it sends `bytesPerTick` every 100 ms, and
// answers a Range from anywhere but the start only after `rangeDelayMs`: a
// slow stream, and a slow seek in it.
class StandIn : public QObject
{
public:
    explicit StandIn(const QByteArray &body, int bytesPerTick = 0, int rangeDelayMs = 0)
        : m_body(body)
        , m_bytesPerTick(bytesPerTick)
        , m_rangeDelayMs(rangeDelayMs)
    {
        QObject::connect(&m_server, &QTcpServer::newConnection, this, [this]() {
            while (QTcpSocket *socket = m_server.nextPendingConnection())
                serve(socket);
        });
    }

    bool listen() { return m_server.listen(QHostAddress::LocalHost, 0); }
    QString url(const QString &path) const
    {
        return QStringLiteral("http://127.0.0.1:%1%2").arg(m_server.serverPort()).arg(path);
    }
    int ranges() const { return m_ranges; }
    // The requests for `path` (its query left out) so far.
    int hits(const QString &path) const { return m_hits.value(path.toUtf8()); }

private:
    void serve(QTcpSocket *socket)
    {
        auto head = std::make_shared<QByteArray>();
        QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        QObject::connect(socket, &QTcpSocket::readyRead, this, [this, socket, head]() {
            head->append(socket->readAll());
            const qsizetype end = head->indexOf("\r\n\r\n");
            if (end < 0)
                return;
            QObject::disconnect(socket, &QTcpSocket::readyRead, this, nullptr);
            answer(socket, head->left(end));
        });
    }

    void answer(QTcpSocket *socket, const QByteArray &head)
    {
        qint64 from = 0;
        bool ranged = false;
        QByteArray path;
        const QList<QByteArray> lines = head.split('\n');
        if (!lines.isEmpty())
            path = lines.first().split(' ').value(1);
        for (const QByteArray &line : lines) {
            const QByteArray field = line.trimmed();
            if (field.toLower().startsWith("range: bytes=")) {
                ranged = true;
                from = field.mid(13).split('-').value(0).toLongLong();
            }
        }
        path = path.split('?').value(0);
        const int hit = ++m_hits[path];
        if (path.startsWith("/missing")) {
            socket->write("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
            socket->disconnectFromHost();
            return;
        }
        if (path.startsWith("/flaky") && hit == 1) {
            socket->write("HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
            socket->disconnectFromHost();
            return;
        }
        const qint64 size = m_body.size();
        if (from >= size) {
            socket->write("HTTP/1.1 416 Range Not Satisfiable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
            socket->disconnectFromHost();
            return;
        }
        QByteArray reply = QByteArray("HTTP/1.1 ") + (ranged ? "206 Partial Content" : "200 OK")
                           + "\r\nContent-Type: audio/wav\r\nAccept-Ranges: bytes\r\nConnection: close\r\n"
                           + "Content-Length: " + QByteArray::number(size - from) + "\r\n";
        if (ranged) {
            reply += "Content-Range: bytes " + QByteArray::number(from) + '-' + QByteArray::number(size - 1) + '/'
                     + QByteArray::number(size) + "\r\n";
        }
        reply += "\r\n";
        if (m_bytesPerTick <= 0) {
            reply += m_body.mid(from);
            socket->write(reply);
            socket->disconnectFromHost();
            return;
        }
        if (from > 0)
            ++m_ranges;
        const QPointer<QTcpSocket> guard(socket);
        const auto start = [this, guard, reply, from]() {
            if (!guard)
                return;
            guard->write(reply);
            auto *tick = new QTimer(guard);
            auto sent = std::make_shared<qint64>(from);
            QObject::connect(tick, &QTimer::timeout, guard, [this, guard, tick, sent]() {
                if (!guard || guard->state() != QAbstractSocket::ConnectedState) {
                    tick->stop();
                    return;
                }
                const QByteArray chunk = m_body.mid(*sent, m_bytesPerTick);
                *sent += chunk.size();
                guard->write(chunk);
                if (*sent >= m_body.size()) {
                    tick->stop();
                    guard->disconnectFromHost();
                }
            });
            tick->start(100);
        };
        if (from > 0 && m_rangeDelayMs > 0)
            QTimer::singleShot(m_rangeDelayMs, socket, start);
        else
            start();
    }

    QTcpServer m_server;
    QByteArray m_body;
    int m_bytesPerTick = 0;
    int m_rangeDelayMs = 0;
    int m_ranges = 0;
    QHash<QByteArray, int> m_hits;
};

bool writeFile(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

QVariantMap row(const QString &videoId, const QString &title, qint64 durationMs)
{
    return QVariantMap{ { QStringLiteral("title"), title },
                        { QStringLiteral("artist"), QStringLiteral("Selftest") },
                        { QStringLiteral("durationMs"), durationMs },
                        { QStringLiteral("sourceId"), videoId } };
}

// The values the chain should reach for these settings, in the graph's shape.
SoundChain::Params goalFor(const SoundChain::Settings &s, bool has8D)
{
    SoundChain::Params p = SoundChain::paramsFor(s);
    p.has8D = has8D;
    if (!has8D)
        p.panWet = 0.0;
    return p;
}

} // namespace

int runSoundSelfTest(Library *library)
{
    Checks t("sound");
    if (qEnvironmentVariable("MONOLIST_DATA_DIR").isEmpty() || !library) {
        t.check(false, QStringLiteral("MONOLIST_DATA_DIR set to a scratch folder"));
        return t.finish();
    }
    testChain(t);
    testRamps(t);
    testPresets(t);
    testClipping(t);
    testModel(t, library);
    return t.finish();
}

int runSoundLiveSelfTest(Library *library)
{
    Checks t("sound-live");
    const QString data = qEnvironmentVariable("MONOLIST_DATA_DIR");
    if (data.isEmpty() || !library) {
        t.check(false, QStringLiteral("MONOLIST_DATA_DIR set to a scratch folder"));
        return t.finish();
    }
    // No sound device, before any player is made.
    qputenv("MONOLIST_MPV_AO", "null");
    clearSoundSettings(library);

    const QByteArray tone = toneWav(48000, 2, 40.0, 440.0, 0.25);
    const QString tonePath = QDir(data).filePath(QStringLiteral("sound-tone48.wav"));
    const QString monoPath = QDir(data).filePath(QStringLiteral("sound-mono48.wav"));
    const QString lowPath = QDir(data).filePath(QStringLiteral("sound-tone8k.wav"));
    const QByteArray slowTone = toneWav(8000, 1, 120.0, 440.0, 0.25);
    if (!writeFile(tonePath, tone) || !writeFile(monoPath, toneWav(48000, 1, 20.0, 440.0, 0.25))
        || !writeFile(lowPath, toneWav(8000, 1, 6.0, 440.0, 0.25))) {
        t.check(false, QStringLiteral("the tones written to the scratch folder"), data);
        return t.finish();
    }
    StandIn server(tone);
    // About 64 KB/s (four times the 8 kHz tone's rate), and a seek's Range
    // answered after 2.5 s: the stream r5 saw the health check misread.
    StandIn slow(slowTone, 6400, 2500);
    if (!t.check(server.listen() && slow.listen(), QStringLiteral("stand-in servers on this computer")))
        return t.finish();

    MpvEngine engine;
    if (!engine.isValid()) {
        t.check(false, QStringLiteral("an audio engine to drive"), engine.lastError());
        return t.finish();
    }
    qint64 pos = 0;
    int failures = 0;
    int effectsFailures = 0;
    QObject::connect(&engine, &MpvEngine::positionChanged, &engine, [&pos](qint64 ms) { pos = ms; });
    QObject::connect(&engine, &MpvEngine::loadFailed, &engine, [&failures](const QString &) { ++failures; });
    QObject::connect(&engine, &MpvEngine::effectsFailed, &engine, [&effectsFailures](const QString &) { ++effectsFailures; });
    engine.setVolume(0.0);
    engine.setLevelling(true, 0.0);

    const auto play = [&](const QString &path, qint64 at = 0) {
        engine.load(path, true, QString(), at);
        return waitUntil([&]() { return engine.hasAudioStarted(); }, 8000);
    };
    const auto settle = [&](const SoundChain::Settings &s, int ms = 600) {
        const SoundChain::Params goal = goalFor(s, engine.effectParams().has8D);
        QElapsedTimer clock;
        clock.start();
        const bool arrived = waitUntil([&]() { return engine.effectParams() == goal; }, ms);
        return arrived ? clock.elapsed() : -1;
    };
    // How fast the song's clock runs against the wall's, over `ms`.
    const auto rate = [&](int ms) {
        const qint64 from = pos;
        QElapsedTimer clock;
        clock.start();
        pause(ms);
        return double(pos - from) / double(qMax<qint64>(1, clock.elapsed()));
    };

    t.note(QStringLiteral("- nothing on"));
    t.check(engine.audioFilterProperty().isEmpty() && near(engine.speedProperty(), 1.0, 1e-9) && engine.pitchCorrection(),
            QStringLiteral("nothing on: no chain, speed 1, pitch correction on"));
    t.check(play(tonePath), QStringLiteral("a 40 s tone plays, to no device"));
    pause(400);
    t.check(engine.audioFilterProperty().isEmpty() && engine.levellingPreamp() == QLatin1String("4"),
            QStringLiteral("  with no chain, and tagged files raised 4 dB as before"), engine.levellingPreamp());

    t.note(QStringLiteral("- High bass on mid-song"));
    SoundChain::Settings s;
    s.highBass = true;
    qint64 before = pos;
    engine.setEffects(s);
    t.check(!engine.audioFilterProperty().isEmpty(), QStringLiteral("the chain goes in"));
    qint64 took = settle(s);
    t.check(took >= 0 && took <= 600, QStringLiteral("  its values arrive within 600 ms (11 steps of 15 ms)"),
            QStringLiteral("%1 ms: %2").arg(took).arg(describe(engine.effectParams())));
    t.check(engine.effectsAnswer(), QStringLiteral("  and it answers"));
    // The limiter answering says the graph runs; each filter's own answer
    // says the ramp's commands, sent without waiting, reach it.
    QStringList refused = engine.effectsRefused();
    t.check(refused.isEmpty(), QStringLiteral("  every filter in it takes its values"), refused.join(QStringLiteral(", ")));
    pause(400);
    t.check(pos > before + 300, QStringLiteral("  the clock kept moving"), QStringLiteral("%1 -> %2 ms").arg(before).arg(pos));
    t.check(engine.levellingPreamp() == QLatin1String("4"),
            QStringLiteral("  the levelling preamp unchanged mid-song (a 4 dB step otherwise)"), engine.levellingPreamp());

    t.note(QStringLiteral("- Slowed + reverb"));
    s.slowedReverb = true;
    engine.setEffects(s);
    t.check(near(engine.speedProperty(), 0.85, 1e-6) && !engine.pitchCorrection() && near(engine.playbackRate(), 0.85, 1e-9),
            QStringLiteral("speed 0.85, pitch correction off"), num(engine.speedProperty()));
    took = settle(s);
    t.check(took >= 0, QStringLiteral("  the reverb in, the pre-gain down to -5.0 dB"), describe(engine.effectParams()));
    // Measured once what was made at the old speed (the output's buffers,
    // about 0.4 s with no device) has been heard.
    pause(600);
    double r = rate(3000);
    t.check(r >= 0.80 && r <= 0.90, QStringLiteral("  the clock runs at 0.85x"), num(r, 3));
    t.check(near(double(engine.duration()), 40000.0, 50.0), QStringLiteral("  the song's length unchanged"),
            QString::number(engine.duration()));

    t.note(QStringLiteral("- Nightcore"));
    s.slowedReverb = false;
    s.nightcore = true;
    engine.setEffects(s);
    t.check(near(engine.speedProperty(), 1.25, 1e-6) && !engine.pitchCorrection(),
            QStringLiteral("speed 1.25, pitch correction off"), num(engine.speedProperty()));
    pause(600);
    r = rate(3000);
    t.check(r >= 1.18 && r <= 1.32, QStringLiteral("  the clock runs at 1.25x"), num(r, 3));
    s.nightcore = false;
    s.slowedReverb = true;
    engine.setEffects(s);
    settle(s);

    t.note(QStringLiteral("- a band, then a seek"));
    s.eq = true;
    s.gains[1] = 4.5;
    s.preset = QStringLiteral("custom");
    engine.setEffects(s);
    took = settle(s);
    t.check(took >= 0 && engine.effectsAnswer(), QStringLiteral("a band moved mid-song arrives"), describe(engine.effectParams()));
    engine.seekAbsolute(20000);
    const bool sought = waitUntil([&]() { return pos >= 20100 && engine.effectsAnswer(); }, 4000);
    t.check(sought && engine.audioFilterProperty().contains(QLatin1String("equalizer@b1=f=62:t=q:w=1.41:g=4.5")),
            QStringLiteral("  after a seek the chain is written with it, and answers"));
    t.check(engine.levellingPreamp() == QLatin1String("0"), QStringLiteral("  and the preamp follows the chain there: 0"),
            engine.levellingPreamp());
    t.check(settle(s) >= 0, QStringLiteral("  its values the wanted ones"), describe(engine.effectParams()));

    t.note(QStringLiteral("- the next file"));
    t.check(play(server.url(QStringLiteral("/next"))), QStringLiteral("the next file, streamed, plays"));
    t.check(waitUntil([&]() { return engine.effectsAnswer(); }, 2000) && near(engine.speedProperty(), 0.85, 1e-6),
            QStringLiteral("  its chain answers once its sound starts, and the speed is still 0.85"));

    t.note(QStringLiteral("- 8D"));
    s.eightD = true;
    pause(300);
    before = pos;
    engine.setEffects(s);
    t.check(engine.audioFilterProperty().contains(QLatin1String("aeval=")) && engine.effectParams().has8D,
            QStringLiteral("8D on mid-song: the graph rebuilt with its branch"));
    took = settle(s);
    t.check(took >= 0 && near(engine.effectParams().panWet, 1.41, 1e-9) && engine.effectsAnswer(),
            QStringLiteral("  turned up to 1.41 (+3 dB), the room at 0.70"), describe(engine.effectParams()));
    refused = engine.effectsRefused();
    t.check(refused.isEmpty(), QStringLiteral("  every filter in it takes its values, amix@pan's weights too"),
            refused.join(QStringLiteral(", ")));
    pause(500);
    t.check(pos > before + 300, QStringLiteral("  the clock kept moving through the rebuild"),
            QStringLiteral("%1 -> %2 ms").arg(before).arg(pos));
    s.eightD = false;
    engine.setEffects(s);
    took = settle(s);
    t.check(took >= 0 && engine.effectParams().has8D && engine.effectParams().panWet == 0.0
                && engine.audioFilterProperty().contains(QLatin1String("aeval=")),
            QStringLiteral("8D off mid-song: its branch turned down, left in until the next file"),
            describe(engine.effectParams()));
    const bool nextPlays = play(tonePath);
    t.check(nextPlays && !engine.audioFilterProperty().contains(QLatin1String("aeval="))
                && !engine.audioFilterProperty().isEmpty(),
            QStringLiteral("  and gone from the next"));

    t.note(QStringLiteral("- the 8 kHz mono file with everything on"));
    engine.stop();
    s = everything();
    engine.setEffects(s);
    t.check(play(lowPath), QStringLiteral("everything on, an 8 kHz mono file plays"));
    t.check(waitUntil([&]() { return engine.effectsAnswer(); }, 2000), QStringLiteral("  and its chain answers"));
    // 8 and 16 kHz are past what an 8 kHz file holds (4 kHz): FFmpeg passed
    // those two bands over, and a command to either crashed libmpv. None is
    // sent them, by the ramp, by a new device or by this check.
    refused = engine.effectsRefused();
    t.check(refused.isEmpty(), QStringLiteral("  every filter in it takes its values (the bands past 4 kHz never sent one)"),
            refused.join(QStringLiteral(", ")));
    before = pos;
    pause(500);
    t.check(pos > before + 200, QStringLiteral("  its clock moving"), QStringLiteral("%1 -> %2 ms").arg(before).arg(pos));
    s.preset = QStringLiteral("treble_boost");
    s.gains = SoundChain::preset(s.preset)->gains;
    engine.setEffects(s);
    took = settle(s, 1500);
    before = pos;
    pause(500);
    t.check(took >= 0 && pos > before + 200 && engine.effectsAnswer(),
            QStringLiteral("  Treble boost picked mid-song (8 and 16 kHz up): ramped to, and it plays on"),
            QStringLiteral("%1 ms; %2 -> %3 ms").arg(took).arg(before).arg(pos));
    engine.setAudioDevice(QString());
    pause(800);
    before = pos;
    pause(500);
    t.check(pos > before + 200 && engine.effectsAnswer(),
            QStringLiteral("  the output set again mid-song (every value sent again): it plays on"),
            QStringLiteral("%1 -> %2 ms").arg(before).arg(pos));

    t.note(QStringLiteral("- every preset, with and without the 8D branch"));
    engine.stop();
    for (const SoundChain::Preset &preset : SoundChain::presets()) {
        QStringList fails;
        for (bool with8D : { false, true }) {
            SoundChain::Settings all;
            all.eq = true;
            all.gains = preset.gains;
            all.preset = preset.id;
            all.highBass = true;
            all.highBassDb = 12;
            all.slowedReverb = true;
            all.reverbLevel = 3;
            all.eightD = with8D;
            engine.setEffects(all);
            const bool played = play(tonePath);
            const bool answered = waitUntil([&]() { return engine.effectsAnswer(); }, 2000);
            const bool sameText = engine.audioFilterProperty().contains(
                SoundChain::graph(SoundChain::paramsFor(all)).left(200));
            const QStringList refusing = engine.effectsRefused();
            if (!played || !answered || !sameText || !refusing.isEmpty())
                fails << QStringLiteral("%1%2%3%4%5").arg(with8D ? QStringLiteral("8D: ") : QString(),
                                                          played ? QString() : QStringLiteral("did not play "),
                                                          answered ? QString() : QStringLiteral("no answer "),
                                                          sameText ? QString() : QStringLiteral("other text "),
                                                          refusing.isEmpty() ? QString()
                                                                             : QStringLiteral("refused by ")
                                                                                   + refusing.join(QStringLiteral(", ")));
            engine.stop();
        }
        t.check(fails.isEmpty(), QStringLiteral("%1 with High bass +12 and heavy reverb answers, every filter of it, "
                                                "with and without 8D").arg(preset.name),
                fails.join(QStringLiteral("; ")));
    }

    t.note(QStringLiteral("- everything off mid-song"));
    s = SoundChain::Settings();
    s.slowedReverb = true;
    s.highBass = true;
    engine.setEffects(s);
    t.check(play(tonePath) && waitUntil([&]() { return engine.effectsAnswer(); }, 2000),
            QStringLiteral("Slowed + reverb and High bass from the start"));
    t.check(engine.levellingPreamp() == QLatin1String("0"), QStringLiteral("  the preamp 0 with the chain in"));
    const QString chainBefore = engine.audioFilterProperty();
    const QString bad = SoundChain::afValue(QStringLiteral("nosuchfilter=1"));
    t.check(!engine.setAudioFilters(bad) && engine.audioFilterProperty() == chainBefore && engine.effectsAnswer(),
            QStringLiteral("a chain mpv refuses: refused, and the one in place stays, answering"));
    engine.setEffects(SoundChain::Settings());
    t.check(near(engine.speedProperty(), 1.0, 1e-9) && engine.pitchCorrection(),
            QStringLiteral("everything off: speed 1 and pitch correction on, at once"));
    took = settle(SoundChain::Settings());
    t.check(took >= 0 && SoundChain::isNeutral(engine.effectParams()) && !engine.audioFilterProperty().isEmpty(),
            QStringLiteral("  the chain turned to rest and left in, mid-song"), describe(engine.effectParams()));
    t.check(engine.levellingPreamp() == QLatin1String("0"), QStringLiteral("  the preamp unchanged mid-song"));
    const bool nextOut = play(tonePath);
    t.check(nextOut && engine.audioFilterProperty().isEmpty() && engine.levellingPreamp() == QLatin1String("4"),
            QStringLiteral("  at the next file the chain is out, and the preamp 4 again"), engine.levellingPreamp());
    engine.setLevelling(true, -6.0);
    s = SoundChain::Settings();
    s.highBass = true;
    engine.setEffects(s);
    t.check(near(engine.fallbackGain(), -6.0, 0.01), QStringLiteral("levelling with effects on: a song measured +6 dB "
                                                                    "is played 6 dB quieter"),
            num(engine.fallbackGain()));
    engine.setLevelling(true, 0.0);

    t.note(QStringLiteral("- a mono song, its output reopened mid-song"));
    engine.setEffects(SoundChain::Settings());
    engine.stop();
    t.check(play(monoPath), QStringLiteral("a 48 kHz mono file plays with no chain"));
    pause(800);
    s = SoundChain::Settings();
    s.highBass = true;
    engine.setEffects(s);
    took = settle(s);
    bool held = took >= 0;
    QString dropped;
    QElapsedTimer watch;
    watch.start();
    const SoundChain::Params goal = goalFor(s, false);
    while (watch.elapsed() < 1500) {
        if (engine.effectParams() != goal && dropped.isEmpty()) {
            held = false;
            dropped = QStringLiteral("at %1 ms: %2").arg(watch.elapsed()).arg(describe(engine.effectParams()));
        }
        pause(10);
    }
    t.check(held, QStringLiteral("High bass on: its values kept through the output reopening (no rewind)"), dropped);
    t.check(engine.effectsAnswer() && effectsFailures == 0, QStringLiteral("  the chain answering"));

    t.note(QStringLiteral("- a slow seek on a slow stream"));
    engine.stop();
    s = SoundChain::Settings();
    s.highBass = true;
    s.slowedReverb = true;
    engine.setEffects(s);
    t.check(play(slow.url(QStringLiteral("/slow"))), QStringLiteral("a stream at 64 KB/s plays"));
    pause(1500);
    // A ramp under way as the seek is asked for, and another begun while it
    // waits: the health check asks the limiter on every tick of a ramp, and
    // must not take a graph waiting for the network for one that will not
    // run. (With nothing ramping, nothing would ask it, and no false alarm
    // could be raised to be caught.)
    s.reverbLevel = 3;
    engine.setEffects(s);
    engine.seekAbsolute(100000);
    pause(1000);
    s.highBassDb = 9;
    engine.setEffects(s);
    const bool arrived = waitUntil([&]() { return pos >= 100300; }, 15000);
    pause(1200);
    t.check(arrived && slow.ranges() > 0, QStringLiteral("  a seek past what it has, answered after 2.5 s, with the "
                                                         "effects changed as it was asked and while it waited"),
            QStringLiteral("at %1 ms, %2 ranges").arg(pos).arg(slow.ranges()));
    t.check(effectsFailures == 0 && !engine.effectsBroken() && engine.effectsAnswer(),
            QStringLiteral("  no false alarm while it waited, and the chain answers after"));
    t.check(settle(s) >= 0, QStringLiteral("  at the values chosen while it waited"), describe(engine.effectParams()));

    t.note(QStringLiteral("- the takeover carries the effects over"));
    engine.stop();
    s = SoundChain::Settings();
    s.slowedReverb = true;
    s.highBass = true;
    engine.setEffects(s);
    t.check(play(server.url(QStringLiteral("/upA"))), QStringLiteral("a stream plays with Slowed + reverb and High bass"));
    waitUntil([&]() { return engine.duration() > 0 && engine.hasLoadedFile(); }, 3000);
    pause(1000);
    bool swapped = false;
    bool finished = false;
    QString detail;
    const QMetaObject::Connection upgraded = QObject::connect(
        &engine, &MpvEngine::upgradeFinished, &engine, [&](bool ok, const QString &why, int, bool) {
            finished = true;
            swapped = ok;
            detail = why;
        });
    const bool started = engine.startUpgrade(server.url(QStringLiteral("/upB")), 0.0, 0);
    t.check(started, QStringLiteral("  a takeover by the same tone begins"));
    waitUntil([&]() { return finished; }, 25000);
    QObject::disconnect(upgraded);
    t.check(finished && swapped, QStringLiteral("  and swaps"), detail.left(200));
    t.check(!engine.audioFilterProperty().isEmpty() && !engine.pitchCorrection() && near(engine.speedProperty(), 0.85, 1e-6),
            QStringLiteral("  the new player has the chain, pitch correction off and speed 0.85"),
            QStringLiteral("%1, %2").arg(engine.pitchCorrection()).arg(engine.speedProperty()));
    t.check(waitUntil([&]() { return engine.effectsAnswer(); }, 2000) && settle(s) >= 0,
            QStringLiteral("  and its chain answers, at the wanted values"), describe(engine.effectParams()));
    refused = engine.effectsRefused();
    t.check(refused.isEmpty(), QStringLiteral("  every filter in it takes its values"), refused.join(QStringLiteral(", ")));
    before = pos;
    pause(500);
    t.check(pos > before + 250, QStringLiteral("  its clock moving"));
    engine.stop();

    t.note(QStringLiteral("- a launch with Slowed + reverb kept"));
    StreamResolver resolver;
    resolver.setSaavnEnabled(false);
    resolver.setTestAnswer(QStringLiteral("fxCtl"), StreamResolver::TierInnerTube, server.url(QStringLiteral("/fxCtl")));
    resolver.setTestAnswer(QStringLiteral("fxBroken"), StreamResolver::TierInnerTube, server.url(QStringLiteral("/fxBroken")));
    DownloadManager downloads;
    library->setSetting(QStringLiteral("sound.slowed_reverb"), QStringLiteral("1"));
    {
        SoundEffects sound(&engine, library);
        sound.restore();
        PlaybackController player(&engine, &resolver, &downloads);
        player.setLibrary(library);
        resolver.setSaavnEnabled(false);
        player.setAutoplay(false);
        QString atStart;
        double speedAtStart = 0.0;
        const QMetaObject::Connection first = QObject::connect(&engine, &MpvEngine::audioStarted, &engine, [&]() {
            atStart = engine.audioFilterProperty();
            speedAtStart = engine.speedProperty();
        });
        player.playTracks({ row(QStringLiteral("fxCtl"), QStringLiteral("Slowed song"), 40000) }, 0, QStringLiteral("selftest"));
        waitUntil([&]() { return engine.hasAudioStarted() && player.playing(); }, 8000);
        QObject::disconnect(first);
        t.check(atStart.contains(QLatin1String("amix@mix=inputs=2:weights=1 0.7")) && near(speedAtStart, 0.85, 1e-6),
                QStringLiteral("its first song has the reverb and 0.85x from its first second"),
                QStringLiteral("%1, %2").arg(atStart.isEmpty() ? QStringLiteral("no chain") : QStringLiteral("chain"))
                    .arg(speedAtStart));
        t.check(near(player.playbackRate(), 0.85, 1e-9), QStringLiteral("  and the player says 0.85 to the media controls"));
        player.pause();
        pause(200);
        engine.stop();
    }
    clearSoundSettings(library);

    t.note(QStringLiteral("- a chain broken while nothing plays"));
    {
        // A fresh session: the effects are broken once a session.
        MpvEngine broken;
        broken.setVolume(0.0);
        broken.breakEffectsForTest(true);
        int brokenFailures = 0;
        int brokenEffects = 0;
        QObject::connect(&broken, &MpvEngine::loadFailed, &broken, [&](const QString &) { ++brokenFailures; });
        QObject::connect(&broken, &MpvEngine::effectsFailed, &broken, [&](const QString &) { ++brokenEffects; });
        // The song plays from JioSaavn, as a match found before (the
        // stand-in's link, given by the resolver as JioSaavn's): a load
        // failure that reached the player would then forget its match
        // (refuseSaavn), which is what the row below is there to show.
        resolver.setTestAnswer(QStringLiteral("fxBroken"), StreamResolver::TierJioSaavn,
                               server.url(QStringLiteral("/fxBroken-saavn")));
        QSqlQuery(AppDatabase::connection()).exec(QStringLiteral("DELETE FROM saavn_matches WHERE video_id = 'fxBroken'"));
        QSqlQuery insert(AppDatabase::connection());
        insert.prepare(QStringLiteral("INSERT INTO saavn_matches (video_id, matched, saavn_id, url, kbps, expires_at, "
                                      "signature, matcher) VALUES ('fxBroken', 1, 'selftest', "
                                      "'https://aac.saavncdn.com/selftest_320.mp4', 320, ?, 'selftest', 0)"));
        insert.addBindValue(QDateTime::currentSecsSinceEpoch() + 86400);
        insert.exec();
        SoundEffects sound(&broken, nullptr);
        int notices = 0;
        QObject::connect(&sound, &SoundEffects::notice, &sound, [&notices](const QString &) { ++notices; });
        sound.setHighBass(true);
        t.check(!broken.audioFilterProperty().isEmpty(), QStringLiteral("a broken chain is taken while nothing plays"));
        PlaybackController player(&broken, &resolver, &downloads);
        player.setLibrary(library);
        resolver.setSaavnEnabled(true);   // asks no one: fxBroken's JioSaavn answer is the test's
        player.setAutoplay(false);
        player.playTracks({ row(QStringLiteral("fxBroken"), QStringLiteral("Broken effects song"), 40000) }, 0,
                          QStringLiteral("selftest"));
        const bool played = waitUntil([&]() { return broken.hasAudioStarted() && player.playing(); }, 10000);
        pause(500);
        t.check(played && !player.statusError() && player.sourceLabel().startsWith(QLatin1String("JioSaavn")),
                QStringLiteral("  the song still plays, from JioSaavn"),
                QStringLiteral("%1 · %2").arg(player.statusText(), player.sourceLabel()));
        t.check(brokenEffects == 1 && notices == 1 && sound.failed() && broken.effectsBroken(),
                QStringLiteral("  the effects are off, said once"), QStringLiteral("%1 effectsFailed, %2 notices")
                                                                         .arg(brokenEffects).arg(notices));
        t.check(brokenFailures == 0, QStringLiteral("  and no load failure reached the player"),
                QString::number(brokenFailures));
        QSqlQuery match(AppDatabase::connection());
        match.exec(QStringLiteral("SELECT COUNT(*) FROM saavn_matches WHERE video_id = 'fxBroken'"));
        t.check(match.next() && match.value(0).toInt() == 1 && player.sourceLabel().startsWith(QLatin1String("JioSaavn")),
                QStringLiteral("  the song's saved JioSaavn match untouched, and still what it plays from"),
                player.sourceLabel());
        t.check(broken.audioFilterProperty().isEmpty() && near(broken.speedProperty(), 1.0, 1e-9),
                QStringLiteral("  no chain left in"));
        player.next();
        player.playTracks({ row(QStringLiteral("fxBroken"), QStringLiteral("Broken effects song"), 40000) }, 0,
                          QStringLiteral("selftest"));
        const bool again = waitUntil([&]() { return broken.hasAudioStarted() && player.playing(); }, 8000);
        t.check(again && broken.audioFilterProperty().isEmpty() && brokenEffects == 1,
                QStringLiteral("  the next song plays without them, nothing said again"));
        player.pause();
        pause(200);
        broken.stop();
        resolver.setSaavnEnabled(false);
        QSqlQuery(AppDatabase::connection()).exec(QStringLiteral("DELETE FROM saavn_matches WHERE video_id = 'fxBroken'"));
    }
    {
        // The chain broken and the link not there either: the link's
        // failure, which never got as far as the chain.
        MpvEngine both;
        both.breakEffectsForTest(true);
        int bothFailures = 0;
        int bothEffects = 0;
        QObject::connect(&both, &MpvEngine::loadFailed, &both, [&](const QString &) { ++bothFailures; });
        QObject::connect(&both, &MpvEngine::effectsFailed, &both, [&](const QString &) { ++bothEffects; });
        SoundChain::Settings bass;
        bass.highBass = true;
        both.setEffects(bass);
        both.load(server.url(QStringLiteral("/missing")), true);
        waitUntil([&]() { return bothFailures > 0; }, 8000);
        pause(300);
        t.check(bothFailures == 1 && bothEffects == 0 && !both.effectsBroken(),
                QStringLiteral("a broken chain and a link that is not there (404): the link's failure, once; the "
                               "effects not blamed"),
                QStringLiteral("%1 load failures, %2 effectsFailed").arg(bothFailures).arg(bothEffects));
        both.stop();
    }

    t.note(QStringLiteral("- links that will not open, with effects on"));
    {
        MpvEngine links;
        links.setVolume(0.0);
        int linkFailures = 0;
        int linkEffects = 0;
        QObject::connect(&links, &MpvEngine::loadFailed, &links, [&](const QString &) { ++linkFailures; });
        QObject::connect(&links, &MpvEngine::effectsFailed, &links, [&](const QString &) { ++linkEffects; });
        SoundChain::Settings bass;
        bass.highBass = true;
        links.setEffects(bass);
        const QString chain = links.audioFilterProperty();
        // A port nothing listens on: taken, and given back. A second try
        // would take the chain out first (retryWithoutEffects), so the chain
        // still in afterwards says the link was asked for once.
        QTcpServer closed;
        closed.listen(QHostAddress::LocalHost, 0);
        const quint16 port = closed.serverPort();
        closed.close();
        QElapsedTimer clock;
        clock.start();
        links.load(QStringLiteral("http://127.0.0.1:%1/song.webm").arg(port), true);
        waitUntil([&]() { return linkFailures > 0; }, 15000);
        const qint64 failedIn = clock.elapsed();
        pause(500);
        t.check(linkFailures == 1 && linkEffects == 0 && !links.effectsBroken(),
                QStringLiteral("a link nothing answers: a load failure, once; the effects not blamed"),
                QStringLiteral("%1 load failures in %2 ms, %3 effectsFailed").arg(linkFailures).arg(failedIn).arg(linkEffects));
        t.check(!chain.isEmpty() && links.audioFilterProperty() == chain,
                QStringLiteral("  asked once: the chain never taken out for a second try"));

        // Refused once, then served: a CDN's passing 503. Its failure is the
        // player's to recover from, as with no effects on; tried again here,
        // it would have played, and the effects been blamed for the session.
        const QString flaky = server.url(QStringLiteral("/flaky-once"));
        links.load(flaky, true);
        waitUntil([&]() { return linkFailures > 1 || links.hasAudioStarted(); }, 8000);
        pause(500);
        t.check(linkFailures == 2 && linkEffects == 0 && !links.effectsBroken()
                    && server.hits(QStringLiteral("/flaky-once")) == 1 && links.audioFilterProperty() == chain,
                QStringLiteral("a link refused once (503): a load failure at once, asked once; the effects not blamed"),
                QStringLiteral("%1 load failures, %2 effectsFailed, %3 requests")
                    .arg(linkFailures).arg(linkEffects).arg(server.hits(QStringLiteral("/flaky-once"))));
        const bool plays = links.load(flaky, true) && waitUntil([&]() { return links.hasAudioStarted(); }, 8000);
        t.check(plays && waitUntil([&]() { return links.effectsAnswer(); }, 2000) && linkEffects == 0,
                QStringLiteral("  asked again, it plays, its chain answering: the effects still on"));
        links.stop();
    }

    t.note(QStringLiteral("- a picture with its sound, the chain broken while nothing plays"));
    {
        const QString picturePath = QDir(data).filePath(QStringLiteral("sound-picture.y4m"));
        t.check(writeFile(picturePath, pictureY4m(64, 48, 25, 20.0)),
                QStringLiteral("a 20 s picture written to the scratch folder"));
        // Shown nowhere: there is no surface here to draw it on.
        qputenv("MONOLIST_MPV_VO", "null");
        MpvEngine pictured;
        qunsetenv("MONOLIST_MPV_VO");
        pictured.setVolume(0.0);
        pictured.breakEffectsForTest(true);
        pictured.setVideoEnabled(true);
        qint64 at = 0;
        int pictureFailures = 0;
        int pictureEffects = 0;
        QObject::connect(&pictured, &MpvEngine::positionChanged, &pictured, [&at](qint64 ms) { at = ms; });
        QObject::connect(&pictured, &MpvEngine::loadFailed, &pictured, [&](const QString &) { ++pictureFailures; });
        QObject::connect(&pictured, &MpvEngine::effectsFailed, &pictured, [&](const QString &) { ++pictureEffects; });
        SoundChain::Settings bass;
        bass.highBass = true;
        pictured.setEffects(bass);
        t.check(!pictured.audioFilterProperty().isEmpty(), QStringLiteral("  the broken chain taken, the picture on"));
        // The picture and the sound in two files, as YouTube's larger sizes
        // come. mpv drops the sound whose chain will not build and plays the
        // picture on: no END_FILE, so only the health check can see it.
        pictured.load(picturePath, true, tonePath);
        const bool back = waitUntil([&]() {
            return pictureEffects > 0 && pictured.hasAudioStarted() && pictured.hasSoundTrack()
                   && pictured.audioFilterProperty().isEmpty();
        }, 10000);
        pause(300);
        t.check(back && !pictured.videoSize().isEmpty(),
                QStringLiteral("  it plays, picture and sound, the sound played again without the chain"),
                QStringLiteral("sound track %1, chain %2, picture %3x%4")
                    .arg(pictured.hasSoundTrack() ? QStringLiteral("in") : QStringLiteral("dropped"),
                         pictured.audioFilterProperty().isEmpty() ? QStringLiteral("out") : QStringLiteral("in"))
                    .arg(pictured.videoSize().width()).arg(pictured.videoSize().height()));
        t.check(pictureEffects == 1 && pictureFailures == 0 && pictured.effectsBroken(),
                QStringLiteral("  the effects are off, said once, and no load failure"),
                QStringLiteral("%1 effectsFailed, %2 load failures").arg(pictureEffects).arg(pictureFailures));
        const qint64 from = at;
        pause(600);
        t.check(at > from + 300 && pictured.hasSoundTrack(), QStringLiteral("  its clock moving, its sound kept"),
                QStringLiteral("%1 -> %2 ms").arg(from).arg(at));
        pictured.stop();
        QFile::remove(picturePath);
    }

    engine.stop();
    QFile::remove(tonePath);
    QFile::remove(monoPath);
    QFile::remove(lowPath);
    return t.finish();
}
