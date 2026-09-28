#include "audioalign.h"
#include "ytdlp.h"

#include <QProcess>
#include <QThread>
#include <QTimer>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <memory>

namespace {

// A quarter of a millisecond a sample: finer than any clock the players
// have, and a few seconds of it is small.
constexpr int kRate = 4000;
// How much of the reference is compared.
constexpr qint64 kWindowMs = 4000;
// The first pass compares where the sound grows louder, in blocks of eight
// samples (2 ms); the second the sound itself, sample by sample, around the
// best few lags of the first.
constexpr int kDecimate = 8;
constexpr int kCandidates = 8;
// Both decodes, from the start of each file to past the window.
constexpr int kTimeoutMs = 30000;
// Below this the two are not alike enough there to say where they meet.
constexpr double kMinPeak = 0.8;

QVector<float> samplesOf(const QByteArray &pcm)
{
    const qsizetype count = pcm.size() / 2;
    QVector<float> samples(count);
    const auto *data = reinterpret_cast<const uchar *>(pcm.constData());
    double sum = 0.0;
    for (qsizetype i = 0; i < count; ++i) {
        const qint16 value = qint16(quint16(data[2 * i]) | (quint16(data[2 * i + 1]) << 8));
        samples[i] = float(value);
        sum += value;
    }
    // Without any offset from zero, which would otherwise count as likeness.
    const float mean = count > 0 ? float(sum / double(count)) : 0.0f;
    for (float &sample : samples)
        sample -= mean;
    return samples;
}

// The normalised correlation of `a` with `b`, `n` samples of each.
double correlation(const float *a, const float *b, int n, double energyA)
{
    double sum = 0.0;
    double energyB = 0.0;
    for (int i = 0; i < n; ++i) {
        sum += double(a[i]) * double(b[i]);
        energyB += double(b[i]) * double(b[i]);
    }
    return energyA > 0.0 && energyB > 0.0 ? sum / std::sqrt(energyA * energyB) : 0.0;
}

double energyOf(const float *a, int n)
{
    double energy = 0.0;
    for (int i = 0; i < n; ++i)
        energy += double(a[i]) * double(a[i]);
    return energy;
}

// Where the sound gets louder, block by block of `factor` samples: the rise
// in each block's loudness over the last, or nothing where it falls. A
// small signal for finding the neighbourhood of the answer quickly, and one
// made of the music's attacks, which line up at one lag only, where the
// waveform of a held note lines up again every period.
QVector<float> onsets(const QVector<float> &samples, int factor)
{
    const qsizetype blocks = samples.size() / factor;
    QVector<float> loudness(blocks);
    for (qsizetype i = 0; i < blocks; ++i) {
        double sum = 0.0;
        for (int k = 0; k < factor; ++k) {
            const double value = samples[i * factor + k];
            sum += value * value;
        }
        loudness[i] = float(std::sqrt(sum / factor));
    }
    QVector<float> out(blocks);
    double mean = 0.0;
    for (qsizetype i = 1; i < blocks; ++i) {
        out[i] = qMax(0.0f, loudness[i] - loudness[i - 1]);
        mean += out[i];
    }
    mean /= qMax<qsizetype>(1, blocks);
    for (float &value : out)
        value -= float(mean);
    return out;
}

} // namespace

AudioAlign::AudioAlign(QObject *parent)
    : QObject(parent)
{
}

AudioAlign::~AudioAlign()
{
    stop();
    for (QThread *worker : std::as_const(m_workers)) {
        worker->wait();
        delete worker;
    }
}

bool AudioAlign::bestLag(const QVector<float> &reference, const QVector<float> &other, int rate, int range,
                         double *offsetMs, double *peak)
{
    const int n = int(reference.size());
    // Every lag from `range` early to `range` late, as far as `other` goes.
    const int lags = qMin(2 * range, int(other.size()) - n);
    if (n < rate / 2 || lags < range)
        return false;

    const QVector<float> coarseReference = onsets(reference, kDecimate);
    const QVector<float> coarseOther = onsets(other, kDecimate);
    const int nc = int(coarseReference.size());
    const int lagsC = qMin(lags / kDecimate, int(coarseOther.size()) - nc);
    const double energyC = energyOf(coarseReference.constData(), nc);
    QVector<double> coarse(lagsC + 1);
    for (int lag = 0; lag <= lagsC; ++lag)
        coarse[lag] = correlation(coarseReference.constData(), coarseOther.constData() + lag, nc, energyC);

    // The coarse pass sees only blocks of eight samples, so its best is only
    // near the answer, and in a quiet or steady passage another lag can look
    // as good. The few best of its peaks are each looked at closely, sample
    // by sample, and the closest look decides.
    QList<int> candidates;
    for (int lag = 0; lag <= lagsC; ++lag) {
        const bool peakHere = (lag == 0 || coarse[lag] >= coarse[lag - 1])
                              && (lag == lagsC || coarse[lag] >= coarse[lag + 1]);
        if (peakHere)
            candidates.append(lag);
    }
    std::sort(candidates.begin(), candidates.end(), [&coarse](int a, int b) { return coarse[a] > coarse[b]; });
    if (candidates.size() > kCandidates)
        candidates.resize(kCandidates);

    const double energy = energyOf(reference.constData(), n);
    int best = 0;
    double bestValue = -2.0;
    for (const int candidate : std::as_const(candidates)) {
        const int from = qMax(0, candidate * kDecimate - 4 * kDecimate);
        const int to = qMin(lags, candidate * kDecimate + 4 * kDecimate);
        for (int lag = from; lag <= to; ++lag) {
            const double value = correlation(reference.constData(), other.constData() + lag, n, energy);
            if (value > bestValue) {
                bestValue = value;
                best = lag;
            }
        }
    }
    *offsetMs = double(best - range) * 1000.0 / double(rate);
    *peak = bestValue;
    return true;
}

QProcess *AudioAlign::startDecode(const Source &source, qint64 fromMs, qint64 lengthMs, bool reference)
{
    QStringList args{ QStringLiteral("-nostdin"), QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"),
                      QStringLiteral("error") };
    // As the player fetches it.
    QString headers;
    for (auto it = source.headers.cbegin(); it != source.headers.cend(); ++it) {
        if (it.key().compare(QLatin1String("User-Agent"), Qt::CaseInsensitive) == 0)
            args << QStringLiteral("-user_agent") << it.value().toString();
        else
            headers += it.key() + QStringLiteral(": ") + it.value().toString() + QStringLiteral("\r\n");
    }
    if (!headers.isEmpty())
        args << QStringLiteral("-headers") << headers;
    // -ss after the input: decoded from the start and discarded up to there,
    // so the moment is each file's own, priming and edit list included.
    args << QStringLiteral("-i") << source.url << QStringLiteral("-ss")
         << QString::number(qMax<qint64>(0, fromMs) / 1000.0, 'f', 3) << QStringLiteral("-t")
         << QString::number(lengthMs / 1000.0, 'f', 3) << QStringLiteral("-vn") << QStringLiteral("-ac")
         << QStringLiteral("1") << QStringLiteral("-ar") << QString::number(kRate) << QStringLiteral("-f")
         << QStringLiteral("s16le") << QStringLiteral("pipe:1");

    auto *process = new QProcess(this);
    connect(process, &QProcess::readyReadStandardOutput, this, [this, process, reference]() {
        (reference ? m_referenceBytes : m_otherBytes) += process->readAllStandardOutput();
    });
    connect(process, &QProcess::finished, this, &AudioAlign::decoded);
    connect(process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart && m_running)
            finish(false, 0.0, 0.0, QStringLiteral("FFmpeg would not start"));
    });
    process->start(YtDlp::ffmpegPath(), args);
    return process;
}

bool AudioAlign::measure(const Source &reference, const Source &other, qint64 atMs, qint64 rangeMs)
{
    if (m_running || reference.url.isEmpty() || other.url.isEmpty() || YtDlp::ffmpegPath().isEmpty())
        return false;
    const int generation = ++m_generation;
    m_running = true;
    m_referenceBytes.clear();
    m_otherBytes.clear();
    m_range = int(rangeMs * kRate / 1000);
    m_reference = startDecode(reference, atMs, kWindowMs, /*reference=*/true);
    m_other = startDecode(other, atMs - rangeMs, kWindowMs + 2 * rangeMs, /*reference=*/false);
    QTimer::singleShot(kTimeoutMs, this, [this, generation]() {
        if (generation == m_generation && m_running)
            finish(false, 0.0, 0.0, QStringLiteral("FFmpeg took longer than %1 s").arg(kTimeoutMs / 1000));
    });
    return true;
}

void AudioAlign::decoded()
{
    if (!m_running || !m_reference || !m_other)
        return;
    if (m_reference->state() != QProcess::NotRunning || m_other->state() != QProcess::NotRunning)
        return;   // the other one still decoding
    for (QProcess *process : { m_reference.data(), m_other.data() }) {
        if (process->exitStatus() != QProcess::NormalExit || process->exitCode() != 0) {
            const QString said = QString::fromUtf8(process->readAllStandardError()).trimmed().right(200);
            finish(false, 0.0, 0.0, QStringLiteral("FFmpeg could not read %1 (%2)")
                                        .arg(process == m_reference ? QStringLiteral("the stream playing")
                                                                    : QStringLiteral("the new link"),
                                             said.isEmpty() ? QStringLiteral("exit %1").arg(process->exitCode()) : said));
            return;
        }
        (process == m_reference ? m_referenceBytes : m_otherBytes) += process->readAllStandardOutput();
    }
    stop();
    m_running = true;   // until the comparison below is in

    // A few million multiplications: on a thread of its own, so the window
    // never waits for them.
    struct Result {
        bool ok = false;
        double offsetMs = 0.0;
        double peak = 0.0;
    };
    auto result = std::make_shared<Result>();
    const QVector<float> referenceSamples = samplesOf(m_referenceBytes);
    const QVector<float> otherSamples = samplesOf(m_otherBytes);
    const int range = m_range;
    const int generation = m_generation;
    QThread *worker = QThread::create([result, referenceSamples, otherSamples, range]() {
        result->ok = bestLag(referenceSamples, otherSamples, kRate, range, &result->offsetMs, &result->peak);
    });
    m_workers.append(worker);
    connect(worker, &QThread::finished, this, [this, worker, result, generation, referenceSamples, otherSamples]() {
        m_workers.removeOne(worker);
        worker->deleteLater();
        if (generation != m_generation || !m_running)
            return;
        if (!result->ok) {
            finish(false, 0.0, 0.0, QStringLiteral("too little of the two to compare (%1 and %2 s)")
                                        .arg(referenceSamples.size() / double(kRate), 0, 'f', 1)
                                        .arg(otherSamples.size() / double(kRate), 0, 'f', 1));
            return;
        }
        if (result->peak < kMinPeak) {
            finish(false, result->offsetMs, result->peak,
                   QStringLiteral("the two do not sound alike enough to line up (at best %1 alike)")
                       .arg(result->peak, 0, 'f', 2));
            return;
        }
        finish(true, result->offsetMs, result->peak, QString());
    });
    worker->start();
}

void AudioAlign::finish(bool ok, double offsetMs, double peak, const QString &detail)
{
    stop();
    Q_EMIT measured(ok, offsetMs, peak, detail);
}

void AudioAlign::cancel()
{
    ++m_generation;
    stop();
}

void AudioAlign::stop()
{
    m_running = false;
    for (QProcess *process : { m_reference.data(), m_other.data() }) {
        if (!process)
            continue;
        disconnect(process, nullptr, this, nullptr);
        if (process->state() != QProcess::NotRunning) {
            process->kill();
            process->waitForFinished(2000);
        }
        process->deleteLater();
    }
    m_reference = nullptr;
    m_other = nullptr;
}
