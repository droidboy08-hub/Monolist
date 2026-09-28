#pragma once

#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QVariantMap>
#include <QVector>

class QProcess;
class QThread;

// How far apart in time two copies of the same recording are: YouTube's and
// JioSaavn's, for the mid-song move (QT7). They are usually the same master,
// but not always the same file: of ten songs measured in engine step JS, one
// had its music 138 ms earlier in JioSaavn's file than in YouTube's, and one
// 840 ms later (a longer silence before it). A move "at the same second"
// would skip or repeat that much, so the player lines the two up by what they
// sound like, not by their clocks alone.
//
// A few seconds of each are decoded by FFmpeg, from the start of each file
// (so each file's own start, the codec's priming and the container's edit
// list, counts exactly as a player counts it: seeking there directly moved
// the result by 16 ms), and the lag of the best normalised cross-correlation
// is found: first coarsely, then to a quarter of a millisecond around it.
class AudioAlign : public QObject
{
    Q_OBJECT
public:
    struct Source {
        QString url;
        QVariantMap headers;   // what the link must be fetched with (User-Agent and the rest)
    };

    explicit AudioAlign(QObject *parent = nullptr);
    ~AudioAlign() override;

    // Compares `reference` over [atMs, atMs + 4 s) with `other` searched
    // `rangeMs` either side of the same moment. False, with nothing begun,
    // when FFmpeg is missing or a comparison is already running; otherwise
    // measured() follows, within about 30 s.
    bool measure(const Source &reference, const Source &other, qint64 atMs, qint64 rangeMs);
    void cancel();
    bool busy() const { return m_running; }

    // The lag at which `other` best matches `reference`, both mono at `rate`
    // Hz, `other` starting `range` samples before `reference`: positive when
    // the sound comes later in `other`. `peak` is the normalised correlation
    // there, 1 for identical sound. Public for the self-test.
    static bool bestLag(const QVector<float> &reference, const QVector<float> &other, int rate, int range,
                        double *offsetMs, double *peak);

Q_SIGNALS:
    // `offsetMs`: how much later the sound is in `other` than in `reference`
    // (negative: earlier). `peak` is how alike they were there (0-1).
    void measured(bool ok, double offsetMs, double peak, const QString &detail);

private:
    void decoded();
    void finish(bool ok, double offsetMs, double peak, const QString &detail);
    QProcess *startDecode(const Source &source, qint64 fromMs, qint64 lengthMs, bool reference);
    void stop();

    QPointer<QProcess> m_reference;
    QPointer<QProcess> m_other;
    QByteArray m_referenceBytes;
    QByteArray m_otherBytes;
    int m_generation = 0;
    bool m_running = false;
    int m_range = 0;   // samples
    QList<QThread *> m_workers;   // comparisons running, waited for when this goes
};
