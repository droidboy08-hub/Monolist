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
// libmpv, from every kind of source, answering every command, with Studio
// clarity's ten bands (2026-10-21) since put in after the equaliser's, at 0.
// Kept apart from SoundChain on purpose: a change to the templates has to
// change these too.
const char kGoldenNeutral[] =
    R"(aformat=channel_layouts=stereo,volume@pre=volume=0dB,equalizer@b0=f=31:t=q:w=1.41:g=0,equalizer@b1=f=62:t=q:w=1.41:g=0,equalizer@b2=f=125:t=q:w=1.41:g=0,equalizer@b3=f=250:t=q:w=1.41:g=0,equalizer@b4=f=500:t=q:w=1.41:g=0,equalizer@b5=f=1000:t=q:w=1.41:g=0,equalizer@b6=f=2000:t=q:w=1.41:g=0,equalizer@b7=f=4000:t=q:w=1.41:g=0,equalizer@b8=f=8000:t=q:w=1.41:g=0,equalizer@b9=f=16000:t=q:w=1.41:g=0,equalizer@c0=f=31:t=q:w=1.4142:g=0,equalizer@c1=f=63:t=q:w=1.4142:g=0,equalizer@c2=f=125:t=q:w=1.4141:g=0,equalizer@c3=f=250:t=q:w=1.414:g=0,equalizer@c4=f=500:t=q:w=1.4132:g=0,equalizer@c5=f=1000:t=q:w=1.41:g=0,equalizer@c6=f=2000:t=q:w=1.3975:g=0,equalizer@c7=f=4001:t=q:w=1.3479:g=0,equalizer@c8=f=8001:t=q:w=1.1588:g=0,equalizer@c9=f=16001:t=q:w=0.5319:g=0,lowshelf@hb=f=100:t=q:w=0.707:g=0,asplit=2[d][w];anoisesrc=d=2.5:c=white:r=48000:a=1:seed=11[nl];anoisesrc=d=2.5:c=white:r=48000:a=1:seed=22[nr];[nl][nr]join=inputs=2:channel_layout=stereo:map=0.0-FL|1.0-FR,afade=t=out:st=0:d=3.67:curve=exp,afade=t=in:st=0:d=0.004,adelay=delays=20:all=1,atrim=end=2.5,highpass=f=150,lowpass=f=6000,afade=t=out:st=2.3:d=0.2[ir];[w][ir]afir@rev=irnorm=2:minp=1024[r];[d][r]amix@mix=inputs=2:weights=1 0:normalize=0,alimiter@lim=limit=1:attack=5:release=100:level=0:latency=1)";
const char kGoldenNeutral8D[] =
    R"(aformat=channel_layouts=stereo,volume@pre=volume=0dB,equalizer@b0=f=31:t=q:w=1.41:g=0,equalizer@b1=f=62:t=q:w=1.41:g=0,equalizer@b2=f=125:t=q:w=1.41:g=0,equalizer@b3=f=250:t=q:w=1.41:g=0,equalizer@b4=f=500:t=q:w=1.41:g=0,equalizer@b5=f=1000:t=q:w=1.41:g=0,equalizer@b6=f=2000:t=q:w=1.41:g=0,equalizer@b7=f=4000:t=q:w=1.41:g=0,equalizer@b8=f=8000:t=q:w=1.41:g=0,equalizer@b9=f=16000:t=q:w=1.41:g=0,equalizer@c0=f=31:t=q:w=1.4142:g=0,equalizer@c1=f=63:t=q:w=1.4142:g=0,equalizer@c2=f=125:t=q:w=1.4141:g=0,equalizer@c3=f=250:t=q:w=1.414:g=0,equalizer@c4=f=500:t=q:w=1.4132:g=0,equalizer@c5=f=1000:t=q:w=1.41:g=0,equalizer@c6=f=2000:t=q:w=1.3975:g=0,equalizer@c7=f=4001:t=q:w=1.3479:g=0,equalizer@c8=f=8001:t=q:w=1.1588:g=0,equalizer@c9=f=16001:t=q:w=0.5319:g=0,lowshelf@hb=f=100:t=q:w=0.707:g=0,asplit=2[p0][p1];[p1]aeval=exprs='st(3,2*PI*t/10);st(4,0.8*sin(ld(3)));st(5,max(0,-cos(ld(3))));st(6,(val(0)+val(1))/2);st(0,ld(0)+(1-exp(-2*PI*2000/s))*(ld(6)-ld(0)));st(7,ld(6)+min(1,0.875*max(0,ld(4))+0.5*ld(5))*(ld(0)-ld(6)));st(8,0.5+0.00075*s*max(0,ld(4)));st(8,(1-ld(8))/(1+ld(8)));st(9,ld(8)*ld(7)+ld(1)-ld(8)*ld(2));st(1,ld(7));st(2,ld(9));ld(9)*cos(PI/4*(1+ld(4)))|st(3,2*PI*t/10);st(4,0.8*sin(ld(3)));st(5,max(0,-cos(ld(3))));st(6,(val(0)+val(1))/2);st(0,ld(0)+(1-exp(-2*PI*2000/s))*(ld(6)-ld(0)));st(7,ld(6)+min(1,0.875*max(0,-ld(4))+0.5*ld(5))*(ld(0)-ld(6)));st(8,0.5+0.00075*s*max(0,-ld(4)));st(8,(1-ld(8))/(1+ld(8)));st(9,ld(8)*ld(7)+ld(1)-ld(8)*ld(2));st(1,ld(7));st(2,ld(9));ld(9)*sin(PI/4*(1+ld(4)))':c=stereo[p8];[p0][p8]amix@pan=inputs=2:weights=1 0:normalize=0,asplit=2[d][w];anoisesrc=d=2.5:c=white:r=48000:a=1:seed=11[nl];anoisesrc=d=2.5:c=white:r=48000:a=1:seed=22[nr];[nl][nr]join=inputs=2:channel_layout=stereo:map=0.0-FL|1.0-FR,afade=t=out:st=0:d=3.67:curve=exp,afade=t=in:st=0:d=0.004,adelay=delays=20:all=1,atrim=end=2.5,highpass=f=150,lowpass=f=6000,afade=t=out:st=2.3:d=0.2[ir];[w][ir]afir@rev=irnorm=2:minp=1024[r];[d][r]amix@mix=inputs=2:weights=1 0:normalize=0,alimiter@lim=limit=1:attack=5:release=100:level=0:latency=1)";
const char kGoldenFull8D[] =
    R"(aformat=channel_layouts=stereo,volume@pre=volume=-15dB,equalizer@b0=f=31:t=q:w=1.41:g=6,equalizer@b1=f=62:t=q:w=1.41:g=5,equalizer@b2=f=125:t=q:w=1.41:g=4,equalizer@b3=f=250:t=q:w=1.41:g=2,equalizer@b4=f=500:t=q:w=1.41:g=0,equalizer@b5=f=1000:t=q:w=1.41:g=0,equalizer@b6=f=2000:t=q:w=1.41:g=0,equalizer@b7=f=4000:t=q:w=1.41:g=0,equalizer@b8=f=8000:t=q:w=1.41:g=0,equalizer@b9=f=16000:t=q:w=1.41:g=0,equalizer@c0=f=31:t=q:w=1.4142:g=0,equalizer@c1=f=63:t=q:w=1.4142:g=0,equalizer@c2=f=125:t=q:w=1.4141:g=0,equalizer@c3=f=250:t=q:w=1.414:g=0,equalizer@c4=f=500:t=q:w=1.4132:g=0,equalizer@c5=f=1000:t=q:w=1.41:g=0,equalizer@c6=f=2000:t=q:w=1.3975:g=0,equalizer@c7=f=4001:t=q:w=1.3479:g=0,equalizer@c8=f=8001:t=q:w=1.1588:g=0,equalizer@c9=f=16001:t=q:w=0.5319:g=0,lowshelf@hb=f=100:t=q:w=0.707:g=12,asplit=2[p0][p1];[p1]aeval=exprs='st(3,2*PI*t/10);st(4,0.8*sin(ld(3)));st(5,max(0,-cos(ld(3))));st(6,(val(0)+val(1))/2);st(0,ld(0)+(1-exp(-2*PI*2000/s))*(ld(6)-ld(0)));st(7,ld(6)+min(1,0.875*max(0,ld(4))+0.5*ld(5))*(ld(0)-ld(6)));st(8,0.5+0.00075*s*max(0,ld(4)));st(8,(1-ld(8))/(1+ld(8)));st(9,ld(8)*ld(7)+ld(1)-ld(8)*ld(2));st(1,ld(7));st(2,ld(9));ld(9)*cos(PI/4*(1+ld(4)))|st(3,2*PI*t/10);st(4,0.8*sin(ld(3)));st(5,max(0,-cos(ld(3))));st(6,(val(0)+val(1))/2);st(0,ld(0)+(1-exp(-2*PI*2000/s))*(ld(6)-ld(0)));st(7,ld(6)+min(1,0.875*max(0,-ld(4))+0.5*ld(5))*(ld(0)-ld(6)));st(8,0.5+0.00075*s*max(0,-ld(4)));st(8,(1-ld(8))/(1+ld(8)));st(9,ld(8)*ld(7)+ld(1)-ld(8)*ld(2));st(1,ld(7));st(2,ld(9));ld(9)*sin(PI/4*(1+ld(4)))':c=stereo[p8];[p0][p8]amix@pan=inputs=2:weights=0 1.41:normalize=0,asplit=2[d][w];anoisesrc=d=2.5:c=white:r=48000:a=1:seed=11[nl];anoisesrc=d=2.5:c=white:r=48000:a=1:seed=22[nr];[nl][nr]join=inputs=2:channel_layout=stereo:map=0.0-FL|1.0-FR,afade=t=out:st=0:d=3.67:curve=exp,afade=t=in:st=0:d=0.004,adelay=delays=20:all=1,atrim=end=2.5,highpass=f=150,lowpass=f=6000,afade=t=out:st=2.3:d=0.2[ir];[w][ir]afir@rev=irnorm=2:minp=1024[r];[d][r]amix@mix=inputs=2:weights=1 1:normalize=0,alimiter@lim=limit=0.891:attack=5:release=100:level=0:latency=1)";

// Studio clarity alone: its ten bands at its curve, turned down 3.4 dB, the
// limiter on. Played inside the shipped libmpv by the measuring harness, and
// by --sound-live-test.
const char kGoldenClarity[] =
    R"(aformat=channel_layouts=stereo,volume@pre=volume=-3.4dB,equalizer@b0=f=31:t=q:w=1.41:g=0,equalizer@b1=f=62:t=q:w=1.41:g=0,equalizer@b2=f=125:t=q:w=1.41:g=0,equalizer@b3=f=250:t=q:w=1.41:g=0,equalizer@b4=f=500:t=q:w=1.41:g=0,equalizer@b5=f=1000:t=q:w=1.41:g=0,equalizer@b6=f=2000:t=q:w=1.41:g=0,equalizer@b7=f=4000:t=q:w=1.41:g=0,equalizer@b8=f=8000:t=q:w=1.41:g=0,equalizer@b9=f=16000:t=q:w=1.41:g=0,equalizer@c0=f=31:t=q:w=1.4142:g=2.7,equalizer@c1=f=63:t=q:w=1.4142:g=2.2,equalizer@c2=f=125:t=q:w=1.4141:g=0.6,equalizer@c3=f=250:t=q:w=1.414:g=-2.6,equalizer@c4=f=500:t=q:w=1.4132:g=-0.6,equalizer@c5=f=1000:t=q:w=1.41:g=1.2,equalizer@c6=f=2000:t=q:w=1.3975:g=3,equalizer@c7=f=4001:t=q:w=1.3479:g=4,equalizer@c8=f=8001:t=q:w=1.1588:g=4.3,equalizer@c9=f=16001:t=q:w=0.5319:g=4.8,lowshelf@hb=f=100:t=q:w=0.707:g=0,asplit=2[d][w];anoisesrc=d=2.5:c=white:r=48000:a=1:seed=11[nl];anoisesrc=d=2.5:c=white:r=48000:a=1:seed=22[nr];[nl][nr]join=inputs=2:channel_layout=stereo:map=0.0-FL|1.0-FR,afade=t=out:st=0:d=3.67:curve=exp,afade=t=in:st=0:d=0.004,adelay=delays=20:all=1,atrim=end=2.5,highpass=f=150,lowpass=f=6000,afade=t=out:st=2.3:d=0.2[ir];[w][ir]afir@rev=irnorm=2:minp=1024[r];[d][r]amix@mix=inputs=2:weights=1 0:normalize=0,alimiter@lim=limit=0.891:attack=5:release=100:level=0:latency=1)";

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
    QStringList clarity;
    for (double g : p.clarity)
        clarity << SoundChain::number(g, 1);
    return QStringLiteral("pre %1, gains [%2], clarity [%3], shelf %4, pan %5, wet %6, limit %7%8")
        .arg(SoundChain::number(p.preDb, 1), gains.join(QLatin1Char(' ')), clarity.join(QLatin1Char(' ')),
             SoundChain::number(p.shelfDb, 1), SoundChain::number(p.panWet, 2), SoundChain::number(p.wet, 2),
             SoundChain::number(p.limit, 3), p.has8D ? QStringLiteral(", 8D branch") : QString());
}

// Studio clarity alone.
SoundChain::Settings clarityAlone()
{
    SoundChain::Settings s;
    s.clarity = true;
    return s;
}

bool isClarityCurve(const std::array<double, SoundChain::kBands> &bands)
{
    for (int i = 0; i < SoundChain::kBands; ++i) {
        if (std::fabs(bands[size_t(i)] - SoundChain::kClarityDb[size_t(i)]) > 1e-9)
            return false;
    }
    return true;
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
    t.check(af == QStringLiteral("@fx:lavfi=graph=%1264%") + QLatin1String(kGoldenNeutral),
            QStringLiteral("mpv's af is @fx:lavfi=graph=%<bytes>%<graph>"), af.left(40));
    t.check(afValue(rest8).startsWith(QLatin1String("@fx:lavfi=graph=%2046%")), QStringLiteral("  its bytes counted"),
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
    Settings hallOnly = reverb;
    hallOnly.slowedSpeed = 1.0;
    t.check(speedFactor(hallOnly) == 1.0 && !ownsSpeed(hallOnly) && near(paramsFor(hallOnly).wet, 0.70, 1e-9),
            QStringLiteral("Slowed at 1x: speed 1, pitch correction on, the reverb still in"));

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
    t.check(commandsFor(neutral()).size() == 24 && commandsFor(rest8).size() == 25,
            QStringLiteral("every value as commands: 24, and 25 with the 8D branch"));

    // A band past half the sound's rate is never sent a command: one crashed
    // the shipped libmpv (16 kHz in a 22.05 kHz file). Studio clarity's 4, 8
    // and 16 kHz are written 1 Hz above, so at exactly half the rate they are
    // past it, and passed over.
    const auto bandsAt = [&rest8](int rate) {
        QStringList bands;
        for (const Command &c : commandsAt(commandsFor(rest8), rate)) {
            if (c.target.startsWith(QLatin1String("equalizer@")))
                bands << c.target.mid(10);   // "b3", "c3"
        }
        return bands.join(QLatin1Char(' '));
    };
    const QString eq = QStringLiteral("b0 b1 b2 b3 b4 b5 b6 b7 b8 b9");
    const QString all = eq + QStringLiteral(" c0 c1 c2 c3 c4 c5 c6 c7 c8 c9");
    t.check(bandsAt(48000) == all && bandsAt(44100) == all
                && bandsAt(32000) == eq + QStringLiteral(" c0 c1 c2 c3 c4 c5 c6 c7 c8"),
            QStringLiteral("commands at 48 and 44.1 kHz: every band; at 32 kHz the equaliser's 16 kHz (half of 32, "
                           "which FFmpeg takes), never Studio clarity's 16,001 Hz"),
            bandsAt(32000));
    t.check(bandsAt(22050) == QLatin1String("b0 b1 b2 b3 b4 b5 b6 b7 b8 c0 c1 c2 c3 c4 c5 c6 c7 c8")
                && bandsAt(16000) == QLatin1String("b0 b1 b2 b3 b4 b5 b6 b7 b8 c0 c1 c2 c3 c4 c5 c6 c7")
                && bandsAt(11025) == QLatin1String("b0 b1 b2 b3 b4 b5 b6 b7 c0 c1 c2 c3 c4 c5 c6 c7")
                && bandsAt(8000) == QLatin1String("b0 b1 b2 b3 b4 b5 b6 b7 c0 c1 c2 c3 c4 c5 c6"),
            QStringLiteral("  at 22.05 kHz none to 16 kHz; at 16 kHz none to Studio clarity's 8 kHz either; at "
                           "11.025 kHz none to 8 or 16 kHz; at 8 kHz none to Studio clarity's 4 kHz either"),
            bandsAt(16000) + QStringLiteral(" | ") + bandsAt(8000));
    t.check(bandsAt(0).isEmpty() && commandsAt(commandsFor(rest8), 0).size() == 5
                && commandsAt(commands(neutral(), paramsFor(bass)), 8000) == expected,
            QStringLiteral("  the rate not known: no band, the other five kept; the rest never left out"),
            describe(commandsAt(commandsFor(rest8), 0)));

    t.check(number(-0.04, 1) == QLatin1String("0") && number(0.70, 2) == QLatin1String("0.7")
                && number(-2.99, 1) == QLatin1String("-3") && number(1.0, 3) == QLatin1String("1")
                && number(0.891, 3) == QLatin1String("0.891") && number(4.5, 1) == QLatin1String("4.5"),
            QStringLiteral("numbers as the chain writes them: no trailing zeros, never -0"));
}

// --------------------------------------------------------------- Studio clarity

void testClarity(Checks &t)
{
    t.note(QStringLiteral("- Studio clarity"));
    using namespace SoundChain;
    // The facts read from LastWave: the ISO octave centres (4, 8 and 16 kHz
    // written 1 Hz above, see kClarityHz) and these gains, nothing else.
    const std::array<int, kBands> centres = { 31, 63, 125, 250, 500, 1000, 2000, 4001, 8001, 16001 };
    const std::array<double, kBands> gains = { 2.7, 2.2, 0.6, -2.6, -0.6, 1.2, 3.0, 4.0, 4.3, 4.8 };
    t.check(kClarityHz == centres && isClarityCurve(gains),
            QStringLiteral("its curve: +2.7 +2.2 +0.6 -2.6 -0.6 +1.2 +3 +4 +4.3 +4.8 dB at 31 Hz to 16 kHz"));

    const Settings clarity = clarityAlone();
    const Params p = paramsFor(clarity);
    t.check(graph(p) == QLatin1String(kGoldenClarity),
            QStringLiteral("alone: the golden text, its ten bands at the curve, turned down 3.4 dB, the limiter on"),
            describe(p));
    t.check(needsChain(clarity) && isClarityCurve(p.clarity) && p.gains == Params().gains && p.shelfDb == 0.0
                && p.wet == 0.0 && !p.has8D && near(p.preDb, -3.4, 1e-9) && near(p.limit, kLimit, 1e-9),
            QStringLiteral("  a chain of its own bands only, the equaliser's left at 0"), describe(p));
    t.check(isNeutral(paramsFor(Settings())) && !isNeutral(p), QStringLiteral("  and off, the chain at rest again"));

    // One octave wide, as LastWave's t=o:w=1, written as the Q that FFmpeg
    // turns into the same coefficients (worked out apart, in Python, from
    // FFmpeg's octave formula, to 4 places).
    QStringList at48;
    for (int i = 0; i < kBands; ++i)
        at48 << number(clarityWidth(i, 48000), 4);
    t.check(at48.join(QLatin1Char(' ')) == QLatin1String("1.4142 1.4142 1.4141 1.414 1.4132 1.41 1.3975 1.3479 1.1588 0.5319"),
            QStringLiteral("its widths in the text: an octave each at 48 kHz, as Qs"), at48.join(QLatin1Char(' ')));
    t.check(widthsAt(48000).isEmpty() && widthsAt(0).isEmpty(),
            QStringLiteral("  none to send at 48 kHz, nor with the rate not known"));
    const QList<Command> at44 = widthsAt(44100);
    t.check(describe(at44) == QLatin1String("w 1.4139 equalizer@c3 | w 1.413 equalizer@c4 | w 1.4092 equalizer@c5 | "
                                            "w 1.3944 equalizer@c6 | w 1.3358 equalizer@c7 | w 1.1145 equalizer@c8 | "
                                            "w 0.4034 equalizer@c9"),
            QStringLiteral("  at 44.1 kHz seven, the octave there"), describe(at44));
    const auto targets = [](const QList<Command> &list) {
        QStringList names;
        for (const Command &c : list)
            names << c.target.mid(10);
        return names.join(QLatin1Char(' '));
    };
    t.check(targets(widthsAt(32000)) == QLatin1String("c3 c4 c5 c6 c7 c8")
                && targets(widthsAt(8000)) == QLatin1String("c0 c1 c2 c3 c4 c5 c6"),
            QStringLiteral("  at 32 kHz none to 16 kHz, at 8 kHz none above 2 kHz: an octave there reaches past half "
                           "the rate"),
            targets(widthsAt(32000)) + QStringLiteral(" | ") + targets(widthsAt(8000)));
    bool kept = true;
    for (int rate : { 8000, 11025, 16000, 22050, 32000, 44100, 48000, 88200, 96000 })
        kept = kept && commandsAt(widthsAt(rate), rate) == widthsAt(rate);
    t.check(kept, QStringLiteral("  every width sent is to a band FFmpeg builds at that rate, 8 to 96 kHz"));

    // With the equaliser: two stages, each as it is set. Nothing of one is
    // added into the other, so nothing needs clamping; the two together are
    // turned down as one curve.
    Settings withEq = clarity;
    withEq.eq = true;
    withEq.preset = QStringLiteral("bass_boost");
    withEq.gains = preset(withEq.preset)->gains;
    const Params pe = paramsFor(withEq);
    t.check(pe.gains == preset(withEq.preset)->gains && isClarityCurve(pe.clarity) && near(pe.preDb, -5.2, 1e-9),
            QStringLiteral("with the equaliser on Bass boost: its bands the preset's, Studio clarity's its own, turned "
                           "down 5.2 dB for the two"),
            describe(pe));
    Settings loud = clarity;
    loud.eq = true;
    loud.preset = QStringLiteral("custom");
    loud.gains.fill(12.0);
    const Params pl = paramsFor(loud);
    const double left = peakResponseDb(pl.gains, pl.clarity, pl.shelfDb) + pl.preDb;
    t.check(std::all_of(pl.gains.cbegin(), pl.gains.cend(), [](double g) { return g == 12.0; })
                && isClarityCurve(pl.clarity) && near(pl.preDb, -17.3, 1e-9) && left <= 6.0 + 1e-6,
            QStringLiteral("  with every band at +12: none clamped or moved, 17.3 dB down, +%1 dB left for the "
                           "limiter (at most 6)").arg(num(left)),
            describe(pl));
    Settings down = loud;
    down.gains.fill(-12.0);
    const Params pd = paramsFor(down);
    t.check(pd.gains[0] == -12.0 && isClarityCurve(pd.clarity) && pd.preDb <= 0.0,
            QStringLiteral("  with every band at -12: the same, and nothing turned up"), describe(pd));

    // Not one at a time with anything: Slowed and Nightcore own the speed,
    // Studio clarity the colour.
    Settings slowed = clarity;
    slowed.slowedReverb = true;
    Settings nightcore = clarity;
    nightcore.nightcore = true;
    t.check(sanitized(slowed).clarity && sanitized(nightcore).clarity && near(speedFactor(slowed), 0.85, 1e-9)
                && near(speedFactor(nightcore), 1.25, 1e-9) && isClarityCurve(paramsFor(slowed).clarity)
                && near(paramsFor(slowed).wet, 0.70, 1e-9) && isClarityCurve(paramsFor(nightcore).clarity),
            QStringLiteral("with Slowed + reverb (0.85x, the hall) and with Nightcore (1.25x): kept on, both heard"));

    const QList<Command> on = commands(neutral(), p);
    QStringList onBands;
    for (const Command &c : on) {
        if (c.target.startsWith(QLatin1String("equalizer@c")))
            onBands << c.value;
    }
    t.check(on.size() == 12 && on.first() == Command{ QStringLiteral("volume"), QStringLiteral("-3.4dB"), QStringLiteral("volume@pre") }
                && on.last() == Command{ QStringLiteral("limit"), QStringLiteral("0.891"), QStringLiteral("alimiter@lim") }
                && onBands.join(QLatin1Char(' ')) == QLatin1String("2.7 2.2 0.6 -2.6 -0.6 1.2 3 4 4.3 4.8"),
            QStringLiteral("switched on: twelve commands, the pre-gain, its ten bands, the limiter"), describe(on));
    t.check(commands(p, neutral()).size() == 12 && commands(p, p).isEmpty(),
            QStringLiteral("  and off, the same twelve back; the same values, none"));
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
    const int claritySteps = stepsBetween(neutral(), paramsFor(clarityAlone()));
    t.check(claritySteps == 11, QStringLiteral("Studio clarity on takes 11 too: the same smooth ramp"),
            QString::number(claritySteps));

    // Every preset with every High bass, reverb and 8D, with Studio clarity
    // and without, ramped from rest and back to it: no step beyond its limit,
    // everything there on one step.
    int cases = 0;
    int bad = 0;
    int longest = 0;
    int longestWithout = 0;   // without Studio clarity
    QString firstBad;
    const auto ramp = [&](Params from, const Params &to) {
        const int planned = stepsBetween(from, to);
        const bool clarity = from.clarity[0] != 0.0 || to.clarity[0] != 0.0;
        int taken = 0;
        while (from != to && taken < 200) {
            const Params next = stepToward(from, to);
            bool within = next.has8D == from.has8D && std::fabs(next.preDb - from.preDb) <= 0.5 + 1e-9
                          && std::fabs(next.shelfDb - from.shelfDb) <= 1.0 + 1e-9
                          && std::fabs(next.panWet - from.panWet) <= 0.07 + 1e-9
                          && std::fabs(next.wet - from.wet) <= 0.07 + 1e-9
                          && std::fabs(next.limit - from.limit) <= 0.01 + 1e-9;
            for (int i = 0; i < kBands; ++i) {
                within = within && std::fabs(next.gains[size_t(i)] - from.gains[size_t(i)]) <= 1.0 + 1e-9
                         && std::fabs(next.clarity[size_t(i)] - from.clarity[size_t(i)]) <= 1.0 + 1e-9;
            }
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
        if (!clarity)
            longestWithout = std::max(longestWithout, taken);
        ++cases;
    };
    for (const Preset &preset : presets()) {
        for (int bassDb : { 0, 3, 6, 9, 12 }) {
            for (int level = 0; level <= 3; ++level) {
                for (bool with8D : { false, true }) {
                    for (bool withClarity : { false, true }) {
                        Settings s;
                        s.eq = true;
                        s.gains = preset.gains;
                        s.preset = preset.id;
                        s.highBass = bassDb > 0;
                        s.highBassDb = bassDb > 0 ? bassDb : 6;
                        s.slowedReverb = level > 0;
                        s.reverbLevel = level;
                        s.eightD = with8D;
                        s.clarity = withClarity;
                        const Params target = paramsFor(s);
                        Params rest = neutral();
                        rest.has8D = target.has8D;
                        ramp(rest, target);
                        ramp(target, rest);
                    }
                }
            }
        }
    }
    t.check(bad == 0, QStringLiteral("%1 ramps, there and back: every step within its limit, all values arriving "
                                     "together, the longest %2 steps").arg(cases).arg(longest),
            firstBad);
    t.check(longestWithout <= 30, QStringLiteral("  none longer than 30 steps (450 ms) without Studio clarity"),
            QString::number(longestWithout));
    // Studio clarity on top of the strongest of everything turns the sound
    // down 3 dB more, which the pre-gain's 0.5 dB a step takes 7 steps for.
    t.check(longest <= 37, QStringLiteral("  and 37 (555 ms) with it on top of everything"), QString::number(longest));

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
                && near(fixed.slowedSpeed, 1.00, 1e-9) && fixed.preset == QLatin1String("custom"),
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

    // Studio clarity, with what it can meet: worked out apart (Python, RBJ at
    // 48 kHz, 240 points, the widths and centres the text writes).
    struct ClarityRow {
        const char *name;
        std::array<double, kBands> gains;
        double shelf;
        double wet;
        double peak;
        double pre;
    };
    const QList<ClarityRow> clarityTable = {
        { "Studio clarity", flat, 0, 0, 6.67, -3.34 },
        { "Studio clarity + High bass +6", flat, 6, 0, 9.03, -4.52 },
        { "Studio clarity + Bass boost", gainsOf("bass_boost"), 0, 0, 10.23, -5.12 },
        { "Studio clarity + Treble boost", gainsOf("treble_boost"), 0, 0, 12.76, -6.76 },
        { "Studio clarity + all bands +12", allUp, 0, 0, 23.29, -17.29 },
        { "Studio clarity + reverb", flat, 0, 0.7, 6.67, -5.34 },
    };
    for (const ClarityRow &row : clarityTable) {
        const double peak = peakResponseDb(row.gains, kClarityDb, row.shelf);
        const double pre = preGainDb(peak, row.wet);
        t.check(near(peak, row.peak, 0.05) && near(pre, row.pre, 0.05),
                QStringLiteral("%1: peak %2 dB, turned down %3 dB").arg(QLatin1String(row.name), num(row.peak), num(-row.pre)),
                QStringLiteral("%1, %2").arg(num(peak), num(pre)));
    }
    // Sine tones through kGoldenClarity inside the shipped libmpv at 48 kHz
    // (ao=pcm, the pre-gain taken back off), against the model; which was
    // also LastWave's own octave bands there, within 0.02 dB at every tone.
    const QList<Measured> clarity48 = { { 31, 3.09 }, { 63, 2.68 }, { 125, 0.62 }, { 250, -2.42 }, { 500, -0.66 },
                                        { 1000, 1.78 }, { 2000, 4.23 }, { 4000, 5.88 }, { 6000, 5.39 }, { 8000, 6.66 },
                                        { 10000, 5.82 }, { 12000, 5.23 }, { 14000, 5.30 }, { 16000, 5.29 },
                                        { 18000, 4.47 }, { 20000, 2.74 } };
    double worstClarity = 0.0;
    for (const Measured &m : clarity48)
        worstClarity = std::max(worstClarity, std::fabs(responseDb(flat, kClarityDb, 0, m.hz) - m.db));
    t.check(worstClarity <= 0.05, QStringLiteral("Studio clarity's curve is what its bands were measured doing in "
                                                 "libmpv, 31 Hz to 20 kHz, within 0.05 dB"),
            QStringLiteral("worst %1 dB").arg(num(worstClarity, 3)));

    // Over everything there is to choose.
    bool neverUp = true;
    double mostLeft = -100.0;
    for (const Preset &preset : presets()) {
        for (int bassDb : { 0, 3, 6, 9, 12 }) {
            for (int level = 0; level <= 3; ++level) {
                for (bool with8D : { false, true }) {
                    for (bool withClarity : { false, true }) {
                        Settings s;
                        s.eq = true;
                        s.gains = preset.gains;
                        s.highBass = bassDb > 0;
                        s.highBassDb = bassDb > 0 ? bassDb : 6;
                        s.slowedReverb = level > 0;
                        s.reverbLevel = level;
                        s.eightD = with8D;
                        s.clarity = withClarity;
                        const Params p = paramsFor(s);
                        neverUp = neverUp && p.preDb <= 0.0;
                        mostLeft = std::max(mostLeft, peakResponseDb(p.gains, p.clarity, p.shelfDb) + p.preDb);
                    }
                }
            }
        }
    }
    t.check(neverUp, QStringLiteral("the pre-gain never turns anything up"));
    t.check(mostLeft <= 6.0 + 1e-6, QStringLiteral("  and never leaves more than +6 dB for the limiter, over every "
                                                   "preset, High bass, reverb, 8D and Studio clarity"),
            QStringLiteral("at most %1 dB").arg(num(mostLeft)));
    t.check(near(20.0 * std::log10(kLimit), -1.0, 0.01), QStringLiteral("the limiter's 0.891 is -1.00 dBFS"),
            num(20.0 * std::log10(kLimit), 3));
    const QList<double> curve = responseCurve(gainsOf("bass_boost"), 6, 120);
    t.check(curve.size() == 120 && near(curve.first(), responseDb(gainsOf("bass_boost"), 6, 31), 1e-9)
                && near(curve.last(), responseDb(gainsOf("bass_boost"), 6, 16000), 1e-9),
            QStringLiteral("the drawn curve: 120 points from 31 Hz to 16 kHz"));
    const QList<double> withClarity = responseCurve(gainsOf("bass_boost"), kClarityDb, 6, 120);
    t.check(withClarity.size() == 120 && near(withClarity.last(), responseDb(gainsOf("bass_boost"), kClarityDb, 6, 16000), 1e-9)
                && withClarity.last() > curve.last() + 4.0,
            QStringLiteral("  and with Studio clarity, its lift at the top in it"),
            QStringLiteral("%1 against %2 dB at 16 kHz").arg(num(withClarity.last()), num(curve.last())));
}

// --------------------------------------------------------------- the model

void clearSoundSettings(Library *library)
{
    for (const char *key : { "sound.slowed_reverb", "sound.slowed_speed", "sound.reverb", "sound.nightcore",
                             "sound.nightcore_speed", "sound.8d", "sound.high_bass", "sound.high_bass_db",
                             "sound.clarity", "sound.eq", "sound.eq_preset", "sound.eq_gains" })
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
        t.check(!s.slowedReverb && !s.nightcore && !s.eightD && !s.highBass && !s.clarity && !s.eq
                    && near(s.slowedSpeed, 0.85, 1e-9)
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
    s = restored({ { "sound.clarity", "1" }, { "sound.nightcore", "1" } });
    t.check(s.clarity && s.nightcore, QStringLiteral("Studio clarity kept on beside Nightcore: both"));
    s = restored({ { "sound.clarity", "yes" } });
    t.check(!s.clarity, QStringLiteral("  and \"yes\" for it is off, as for the others"));

    t.note(QStringLiteral("- Studio clarity, kept and shown"));
    clearSoundSettings(library);
    {
        SoundEffects clear(nullptr, library);
        clear.restore();
        int said = 0;
        QObject::connect(&clear, &SoundEffects::effectsChanged, &clear, [&said]() { ++said; });
        clear.setStudioClarity(true);
        t.check(clear.studioClarity() && setting(library, "sound.clarity") == QLatin1String("1") && said == 1,
                QStringLiteral("Studio clarity on: written at once, said once"), QString::number(said));
        clear.setStudioClarity(true);
        t.check(said == 1, QStringLiteral("  and the same again not at all"));
        t.check(clear.active() && clear.summary() == QStringLiteral("Studio clarity") && near(clear.headroomDb(), 3.4, 1e-9),
                QStringLiteral("  the effects active (the bar's glyph red), the summary naming it, turned down 3.4 dB"),
                QStringLiteral("%1, %2 dB").arg(clear.summary(), num(clear.headroomDb(), 1)));
        clear.setSlowedReverb(true);
        clear.setHighBass(true);
        clear.setEqualiser(true);
        clear.applyPreset(QStringLiteral("bass_boost"));
        t.check(clear.studioClarity() && clear.slowedReverb()
                    && clear.summary() == QStringLiteral("Slowed 0.85× + reverb · High bass +6 dB · Studio clarity · EQ Bass boost"),
                QStringLiteral("  with Slowed + reverb, High bass and the equaliser: all of them on, named in that order"),
                clear.summary());
        t.check(clear.settings().gains == SoundChain::preset(QStringLiteral("bass_boost"))->gains
                    && clear.bandGains().first().toDouble() == 6.0,
                QStringLiteral("  the equaliser's bands still the preset's: nothing added into them"));
        clear.setNightcore(true);
        t.check(clear.studioClarity() && clear.nightcore() && !clear.slowedReverb(),
                QStringLiteral("  Nightcore on: Slowed gives way, Studio clarity does not"));
        const QVariantList drawn = clear.responseCurve(120);
        std::array<double, SoundChain::kBands> bass = SoundChain::preset(QStringLiteral("bass_boost"))->gains;
        t.check(drawn.size() == 120
                    && near(drawn.last().toDouble(), SoundChain::responseDb(bass, SoundChain::kClarityDb, 6, 16000), 1e-9),
                QStringLiteral("  the curve behind the sliders carries it while it is on"),
                num(drawn.last().toDouble()));
    }
    {
        SoundEffects next(nullptr, library);
        next.restore();
        t.check(next.studioClarity() && next.nightcore() && next.highBass(),
                QStringLiteral("the next launch reads Studio clarity back on, beside the rest"), next.summary());
        next.turnAllOff();
        t.check(!next.studioClarity() && !next.active() && setting(library, "sound.clarity") == QLatin1String("0"),
                QStringLiteral("  Turn all off turns it off too, and writes it"));
        t.check(next.responseCurve(120).last().toDouble() < 1.0,
                QStringLiteral("  and the curve behind the sliders is without it again"),
                num(next.responseCurve(120).last().toDouble()));
    }

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
    shown.setSlowedSpeed(1.0);
    shown.setReverbLevel(2);
    t.check(shown.summary().startsWith(QStringLiteral("Reverb · 8D"))
                && setting(library, "sound.slowed_speed") == QLatin1String("1.00"),
            QStringLiteral("  at 1x it is the reverb alone, and 1.00 is kept"), shown.summary());
    shown.setSlowedSpeed(0.8);
    shown.setReverbLevel(0);
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
    t.check(shown.presets().size() == 16 && shown.bandLabels().size() == 10 && shown.slowedSpeeds().size() == 4
                && shown.nightcoreSpeeds().size() == 3 && shown.highBassSteps().size() == 4 && shown.reverbLevels().size() == 4,
            QStringLiteral("the chips: 16 presets, 10 bands, 4 slowed and 3 nightcore speeds, 4 bass steps, 4 reverbs"));
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
    testClarity(t);
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
    // Studio clarity's other rates: 44.1 kHz, which takes widths of its own,
    // and 32 kHz, half of which is 16 kHz.
    const QString cdPath = QDir(data).filePath(QStringLiteral("sound-tone44k.wav"));
    const QString path32 = QDir(data).filePath(QStringLiteral("sound-tone32k.wav"));
    const QByteArray slowTone = toneWav(8000, 1, 120.0, 440.0, 0.25);
    if (!writeFile(tonePath, tone) || !writeFile(monoPath, toneWav(48000, 1, 20.0, 440.0, 0.25))
        || !writeFile(lowPath, toneWav(8000, 1, 6.0, 440.0, 0.25))
        || !writeFile(cdPath, toneWav(44100, 2, 20.0, 440.0, 0.25))
        || !writeFile(path32, toneWav(32000, 2, 10.0, 440.0, 0.25))) {
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

    t.note(QStringLiteral("- Studio clarity on mid-song, beside High bass"));
    s.clarity = true;
    before = pos;
    engine.setEffects(s);
    took = settle(s);
    t.check(took >= 0 && took <= 600 && isClarityCurve(engine.effectParams().clarity),
            QStringLiteral("its ten bands ramped to the curve within 600 ms, as the others' are"),
            QStringLiteral("%1 ms: %2").arg(took).arg(describe(engine.effectParams())));
    refused = engine.effectsRefused();
    t.check(refused.isEmpty() && engine.effectsAnswer(), QStringLiteral("  every filter in it takes its values, its ten "
                                                                       "bands too"),
            refused.join(QStringLiteral(", ")));
    t.check(engine.clarityWidthsSent() == 0, QStringLiteral("  at 48 kHz no widths sent: the text's are the octave there"),
            QString::number(engine.clarityWidthsSent()));
    pause(400);
    t.check(pos > before + 300, QStringLiteral("  the clock kept moving"), QStringLiteral("%1 -> %2 ms").arg(before).arg(pos));

    t.note(QStringLiteral("- Slowed + reverb, Studio clarity kept on"));
    s.slowedReverb = true;
    engine.setEffects(s);
    t.check(near(engine.speedProperty(), 0.85, 1e-6) && !engine.pitchCorrection() && near(engine.playbackRate(), 0.85, 1e-9),
            QStringLiteral("speed 0.85, pitch correction off"), num(engine.speedProperty()));
    took = settle(s);
    t.check(took >= 0 && isClarityCurve(engine.effectParams().clarity),
            QStringLiteral("  the reverb in, the pre-gain down for it, Studio clarity's bands where they were"),
            describe(engine.effectParams()));
    // Measured once what was made at the old speed (the output's buffers,
    // about 0.4 s with no device) has been heard.
    pause(600);
    double r = rate(3000);
    t.check(r >= 0.80 && r <= 0.90, QStringLiteral("  the clock runs at 0.85x"), num(r, 3));
    t.check(near(double(engine.duration()), 40000.0, 50.0), QStringLiteral("  the song's length unchanged"),
            QString::number(engine.duration()));
    s.slowedSpeed = 1.0;
    engine.setEffects(s);
    t.check(near(engine.speedProperty(), 1.0, 1e-6) && engine.pitchCorrection() && near(engine.playbackRate(), 1.0, 1e-9)
                && !engine.audioFilterProperty().isEmpty(),
            QStringLiteral("  at 1x: speed 1, pitch correction on, the reverb's chain still in"), num(engine.speedProperty()));
    s.slowedSpeed = 0.85;
    engine.setEffects(s);
    t.check(near(engine.speedProperty(), 0.85, 1e-6) && !engine.pitchCorrection(),
            QStringLiteral("  and back to 0.85"), num(engine.speedProperty()));

    t.note(QStringLiteral("- Nightcore"));
    s.slowedReverb = false;
    s.nightcore = true;
    engine.setEffects(s);
    t.check(near(engine.speedProperty(), 1.25, 1e-6) && !engine.pitchCorrection(),
            QStringLiteral("speed 1.25, pitch correction off"), num(engine.speedProperty()));
    pause(600);
    r = rate(3000);
    t.check(r >= 1.18 && r <= 1.32, QStringLiteral("  the clock runs at 1.25x"), num(r, 3));
    t.check(engine.effects().clarity && isClarityCurve(engine.effectParams().clarity) && engine.effectsAnswer(),
            QStringLiteral("  Studio clarity still on with it"), describe(engine.effectParams()));
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
    t.check(sought && engine.audioFilterProperty().contains(QLatin1String("equalizer@b1=f=62:t=q:w=1.41:g=4.5"))
                && engine.audioFilterProperty().contains(QLatin1String("equalizer@c9=f=16001:t=q:w=0.5319:g=4.8")),
            QStringLiteral("  after a seek the chain is written with it, Studio clarity's bands beside it, and answers"));
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
    s.clarity = true;
    engine.setEffects(s);
    t.check(play(lowPath), QStringLiteral("everything on, Studio clarity too, an 8 kHz mono file plays"));
    t.check(waitUntil([&]() { return engine.effectsAnswer(); }, 2000), QStringLiteral("  and its chain answers"));
    // 8 and 16 kHz are past what an 8 kHz file holds (4 kHz): FFmpeg passed
    // those two bands over, and a command to either crashed libmpv. None is
    // sent them, by the ramp, by a new device or by this check; nor to Studio
    // clarity's 4,001 Hz, past it too, nor a width above 2 kHz.
    refused = engine.effectsRefused();
    t.check(refused.isEmpty(), QStringLiteral("  every filter in it takes its values and widths (the bands past 4 kHz "
                                              "never sent one)"),
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

    t.note(QStringLiteral("- Studio clarity at 44.1 and 32 kHz"));
    engine.stop();
    s = clarityAlone();
    engine.setEffects(s);
    int widthsBefore = engine.clarityWidthsSent();
    t.check(play(cdPath), QStringLiteral("Studio clarity on, a 44.1 kHz file plays"));
    t.check(waitUntil([&]() { return engine.effectsAnswer(); }, 2000) && settle(s) >= 0,
            QStringLiteral("  its chain answers, at the curve"), describe(engine.effectParams()));
    // Sent once the graph built for the file answers (the first tick).
    const bool sentCd = waitUntil([&]() { return engine.clarityWidthsSent() >= widthsBefore + 7; }, 1000);
    t.check(sentCd, QStringLiteral("  its seven widths for 44.1 kHz sent once the graph answered"),
            QStringLiteral("%1 sent").arg(engine.clarityWidthsSent() - widthsBefore));
    widthsBefore = engine.clarityWidthsSent();
    // effectsRefused sends the widths too, as sendWidths does, and waits for
    // each answer: all seven taken says mpv takes the command, at this rate.
    refused = engine.effectsRefused();
    t.check(refused.isEmpty() && SoundChain::widthsAt(44100).size() == 7,
            QStringLiteral("  every filter takes its values, and seven bands their widths for 44.1 kHz"),
            refused.join(QStringLiteral(", ")));
    before = pos;
    pause(500);
    t.check(pos > before + 300, QStringLiteral("  its clock moving"), QStringLiteral("%1 -> %2 ms").arg(before).arg(pos));
    engine.seekAbsolute(10000);
    const bool soughtCd = waitUntil([&]() {
        return pos >= 10100 && engine.effectsAnswer() && engine.clarityWidthsSent() >= widthsBefore + 7;
    }, 4000);
    refused = engine.effectsRefused();
    t.check(soughtCd && refused.isEmpty(),
            QStringLiteral("  and after a seek, which builds the graph again: the seven sent again, and taken"),
            QStringLiteral("%1 sent; %2").arg(engine.clarityWidthsSent() - widthsBefore).arg(refused.join(QStringLiteral(", "))));
    engine.stop();
    widthsBefore = engine.clarityWidthsSent();
    t.check(play(path32), QStringLiteral("Studio clarity on, a 32 kHz file plays"));
    const bool sent32 = waitUntil([&]() { return engine.effectsAnswer(); }, 2000)
                        && waitUntil([&]() { return engine.clarityWidthsSent() >= widthsBefore + 6; }, 1000);
    t.check(sent32, QStringLiteral("  and its chain answers, six widths sent (none to 16 kHz)"),
            QStringLiteral("%1 sent").arg(engine.clarityWidthsSent() - widthsBefore));
    // Half of 32 kHz is 16 kHz: Studio clarity's top band, written at 16,001 Hz,
    // is past it, passed over by FFmpeg, and never sent anything.
    refused = engine.effectsRefused();
    t.check(refused.isEmpty(), QStringLiteral("  every filter takes its values and widths (none sent to 16,001 Hz)"),
            refused.join(QStringLiteral(", ")));
    before = pos;
    pause(500);
    t.check(pos > before + 300, QStringLiteral("  its clock moving"), QStringLiteral("%1 -> %2 ms").arg(before).arg(pos));

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
            all.clarity = true;
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
        t.check(fails.isEmpty(), QStringLiteral("%1 with High bass +12, heavy reverb and Studio clarity answers, every "
                                                "filter of it, with and without 8D").arg(preset.name),
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
    // The output reopens about half a second in (the chain's stereo meets a
    // mono song), and the graph takes no commands while it does: the ramp
    // waits it out, for as long as that takes on the machine and the build
    // (the release build's ramp was seen caught by it, a step short at
    // 600 ms). So more time than a stereo song needs.
    took = settle(s, 2500);
    t.check(took >= 0, QStringLiteral("High bass on, the output reopened under it: its values reached"),
            took >= 0 ? QStringLiteral("%1 ms").arg(took) : describe(engine.effectParams()));
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
    s.clarity = true;
    engine.setEffects(s);
    t.check(play(server.url(QStringLiteral("/upA"))),
            QStringLiteral("a stream plays with Slowed + reverb, High bass and Studio clarity"));
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

        // The sleep timer's "end of this song", on the real player: the song
        // heard to its end, the next one made ready and left waiting.
        resolver.setTestAnswer(QStringLiteral("fxCtl2"), StreamResolver::TierInnerTube, server.url(QStringLiteral("/fxCtl")));
        player.playTracks({ row(QStringLiteral("fxCtl"), QStringLiteral("Slowed song"), 40000),
                            row(QStringLiteral("fxCtl2"), QStringLiteral("Next song"), 40000) },
                          0, QStringLiteral("selftest"));
        waitUntil([&]() { return engine.hasAudioStarted() && player.playing(); }, 8000);
        int stopped = 0;
        QObject::connect(&player, &PlaybackController::stoppedAfterCurrent, &player, [&stopped]() { ++stopped; });
        player.setStopAfterCurrent(true);
        player.seekFraction(0.96);
        waitUntil([&]() { return stopped > 0; }, 12000);
        pause(800);
        t.check(stopped == 1 && !player.playing() && player.currentIndex() == 1 && !player.stopAfterCurrent(),
                QStringLiteral("stop after this song: it ended, the next is ready and not playing, the switch off"),
                QStringLiteral("stopped %1, playing %2, at %3").arg(stopped).arg(player.playing()).arg(player.currentIndex()));
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
    QFile::remove(cdPath);
    QFile::remove(path32);
    return t.finish();
}
