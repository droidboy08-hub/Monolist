#include "artworkcache.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>
#include <QNetworkAccessManager>
#include <QNetworkDiskCache>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QQuickTextureFactory>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QThread>
#include <QUrl>

#include <array>
#include <atomic>

namespace {

// Video thumbnails (i.ytimg.com's hqdefault and sddefault) are 4:3 with the
// 16:9 frame letterboxed in black; cropped square for a cover, the bars stay.
// Rows that are black all the way across, at the top and the bottom, are cut
// off. Only for video thumbnails: an album cover may well be black at the edge.
// A video's thumbnail comes in fixed sizes under fixed names: hqdefault is
// 480x360, maxresdefault 1280x720 where YouTube made one (older and smaller
// uploads have none, and answer 404 — hence the fallback to what was asked
// for). Album covers are not affected: those carry their size in the address.
QUrl largestThumbnail(const QUrl &url)
{
    if (!url.host().endsWith(QLatin1String("i.ytimg.com")))
        return {};
    const QString path = url.path();
    static const QRegularExpression name(QStringLiteral(R"(^/vi/[^/]+/(\w+)\.jpg$)"));
    const QRegularExpressionMatch match = name.match(path);
    if (!match.hasMatch() || match.captured(1) == QLatin1String("maxresdefault"))
        return {};
    QUrl best = url;
    best.setPath(path.left(match.capturedStart(1)) + QStringLiteral("maxresdefault.jpg"));
    return best;
}

QImage withoutLetterbox(const QImage &image, const QString &source)
{
    if (image.isNull() || !source.contains(QLatin1String("i.ytimg.com")))
        return image;
    const QImage rgb = image.convertToFormat(QImage::Format_RGB32);
    const auto isBar = [&rgb](int y) {
        const auto *line = reinterpret_cast<const QRgb *>(rgb.constScanLine(y));
        for (int x = 0; x < rgb.width(); x += 4) {
            if (qMax(qRed(line[x]), qMax(qGreen(line[x]), qBlue(line[x]))) > 24)
                return false;
        }
        return true;
    };
    int top = 0;
    while (top < rgb.height() / 3 && isBar(top))
        ++top;
    int bottom = rgb.height() - 1;
    while (bottom > rgb.height() * 2 / 3 && isBar(bottom))
        --bottom;
    // Bars on both sides, or it is not a letterbox but a dark picture.
    const int minimum = rgb.height() / 20;
    if (top < minimum || rgb.height() - 1 - bottom < minimum)
        return image;
    return image.copy(0, top, image.width(), bottom - top + 1);
}

// Decoding a cover is the one heavy thing done per picture, so it is counted:
// how many, how long, and how many of them held up the interface's thread.
struct DecodeTally {
    std::atomic<int> count { 0 };
    std::atomic<int> onGuiThread { 0 };
    std::atomic<qint64> totalUs { 0 };
    std::atomic<qint64> longestUs { 0 };
};

DecodeTally &tally()
{
    static DecodeTally decodes;
    return decodes;
}

void countDecode(qint64 us)
{
    DecodeTally &t = tally();
    ++t.count;
    const QCoreApplication *app = QCoreApplication::instance();
    if (app && QThread::currentThread() == app->thread())
        ++t.onGuiThread;
    t.totalUs += us;
    qint64 longest = t.longestUs.load();
    while (us > longest && !t.longestUs.compare_exchange_weak(longest, us)) {
    }
}

} // namespace

QString ArtworkFetcher::decodeReport()
{
    const DecodeTally &t = tally();
    return QStringLiteral("%1 covers decoded, %2 ms in all, the longest %3 ms; %4 of them on the "
                          "interface's thread")
        .arg(t.count.load())
        .arg(t.totalUs.load() / 1000.0, 0, 'f', 1)
        .arg(t.longestUs.load() / 1000.0, 0, 'f', 1)
        .arg(t.onGuiThread.load());
}

// ------------------------------------------------------------ ArtworkResponse

// Lives on QML's pixmap reader thread; the fetcher drives it from the main
// thread. Emitting finished() from another thread is the documented pattern for
// QQuickImageResponse, but the reply pointer is shared, so it is guarded.
class ArtworkResponse : public QQuickImageResponse
{
    Q_OBJECT
public:
    QQuickTextureFactory *textureFactory() const override
    {
        return QQuickTextureFactory::textureFactoryForImage(m_image);
    }

    QString errorString() const override { return m_error; }

    // Called by QML, on QML's thread. The reply belongs to the fetcher's
    // thread, so the abort is posted there rather than invoked directly.
    void cancel() override
    {
        QMutexLocker lock(&m_mutex);
        if (m_reply)
            QMetaObject::invokeMethod(m_reply, "abort", Qt::QueuedConnection);
    }

    void attachReply(QNetworkReply *reply)
    {
        QMutexLocker lock(&m_mutex);
        m_reply = reply;
    }

    void detachReply()
    {
        QMutexLocker lock(&m_mutex);
        m_reply = nullptr;
    }

public Q_SLOTS:
    void succeed(const QImage &image)
    {
        m_image = image;
        Q_EMIT finished();
    }

    void fail(const QString &reason)
    {
        m_error = reason;
        Q_EMIT finished();
    }

private:
    mutable QMutex m_mutex;
    QNetworkReply *m_reply = nullptr;
    QImage m_image;
    QString m_error;
};

// ------------------------------------------------------------- ArtworkFetcher

ArtworkFetcher::ArtworkFetcher(QObject *parent)
    : QObject(parent)
    , m_network(new QNetworkAccessManager(this))
{
    // Required because requestImageResponse hands the pointer across threads
    // through a queued invocation.
    qRegisterMetaType<ArtworkResponse *>("ArtworkResponse*");

    // Persistent HTTP cache so artwork survives restarts and the app is not
    // re-fetching the same thumbnails on every launch.
    auto *cache = new QNetworkDiskCache(this);
    const QString dir = QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation))
                            .filePath(QStringLiteral("artwork"));
    QDir().mkpath(dir);
    cache->setCacheDirectory(dir);
    cache->setMaximumCacheSize(256LL * 1024 * 1024);
    m_network->setCache(cache);

    // A shelf's worth of covers arrives at once; a few at a time is enough to
    // keep up without crowding the audio and the render thread.
    m_decoders.setMaxThreadCount(qBound(1, QThread::idealThreadCount() / 2, 4));
}

ArtworkFetcher::~ArtworkFetcher()
{
    m_decoders.clear();
    m_decoders.waitForDone();
}

void ArtworkFetcher::fetch(ArtworkResponse *response,
                           const QString &source,
                           const QSize &requestedSize)
{
    if (source.isEmpty()) {
        response->fail(QStringLiteral("No artwork source."));
        return;
    }

    // Downscale on decode where possible — a 1280px thumbnail rendered into a
    // 96px cell wastes both memory and upload bandwidth to the GPU.
    const auto scaled = [requestedSize](QImage image) {
        if (image.isNull() || !requestedSize.isValid()
            || requestedSize.width() <= 0 || requestedSize.height() <= 0)
            return image;
        return image.scaled(requestedSize, Qt::KeepAspectRatioByExpanding,
                            Qt::SmoothTransformation);
    };

    const QUrl url(source);
    const bool isLocal = !url.isValid() || url.isLocalFile() || url.scheme().isEmpty();

    if (isLocal) {
        const QString path = url.isLocalFile() ? url.toLocalFile() : source;
        m_decoders.start([this, response, path, scaled]() {
            QElapsedTimer decoding;
            decoding.start();
            QImage image(path);
            if (!image.isNull()) {
                image = scaled(image);
                countDecode(decoding.nsecsElapsed() / 1000);
            }
            // Answered from this object's thread, as a network cover is.
            QMetaObject::invokeMethod(this, [response, path, image]() {
                if (image.isNull())
                    response->fail(QStringLiteral("Could not read artwork: %1").arg(path));
                else
                    response->succeed(image);
            }, Qt::QueuedConnection);
        });
        return;
    }

    // Shown larger than the small thumbnail holds: ask for the big one, and
    // keep the small one in hand for the videos that have no big one.
    QUrl wanted = url;
    QUrl fallback;
    if (qMax(requestedSize.width(), requestedSize.height()) > 480) {
        const QUrl larger = largestThumbnail(url);
        if (!larger.isEmpty()) {
            wanted = larger;
            fallback = url;
        }
    }
    download(response, wanted, fallback, source, scaled);
}

void ArtworkFetcher::download(ArtworkResponse *response,
                              const QUrl &url,
                              const QUrl &fallback,
                              const QString &source,
                              const std::function<QImage(QImage)> &scaled)
{
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                         QNetworkRequest::PreferCache);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Monolist/0.1"));
    request.setTransferTimeout(10000);

    QNetworkReply *reply = m_network->get(request);
    response->attachReply(reply);

    // Context is `this`, not `response`.
    //
    // QQuickAsyncImageProvider calls the provider from QML's pixmap reader
    // thread, so `response` lives on that thread. Using it as the context made
    // this lambda run there while the reply is owned by the QNetworkAccessManager
    // on this thread — reading it from the wrong thread raced with the socket
    // still appending, produced a garbage internal length, and QByteArray::resize
    // then threw std::bad_alloc. Everything that touches the reply has to stay
    // here; only the finished QImage crosses threads.
    QObject::connect(reply, &QNetworkReply::finished, this,
                     [this, reply, response, scaled, source, fallback]() {
        reply->deleteLater();
        response->detachReply();

        // A size YouTube never made answers 404; the smaller one is there.
        // A cancelled request is never retried: QML cancels when the picture
        // is no longer wanted and waits for this one answer, so anything else
        // would leave it waiting on a reply for a response it has dropped.
        const auto giveUp = [this, response, scaled, source, fallback](const QString &reason,
                                                                       bool mayRetry = true) {
            if (fallback.isEmpty() || !mayRetry)
                response->fail(reason);
            else
                download(response, fallback, {}, source, scaled);
        };

        if (reply->error() != QNetworkReply::NoError) {
            giveUp(reply->errorString(), reply->error() != QNetworkReply::OperationCanceledError);
            return;
        }

        // Artwork is a thumbnail. Anything wildly larger is a redirect to a
        // page, a captive portal, or a broken instance — refuse it rather than
        // decoding it.
        constexpr qint64 kMaxArtworkBytes = 24LL * 1024 * 1024;
        if (reply->bytesAvailable() > kMaxArtworkBytes) {
            giveUp(QStringLiteral("Artwork response too large (%1 bytes).")
                       .arg(reply->bytesAvailable()));
            return;
        }

        // Only the bytes are read here. The decoding and scaling go to the
        // pool, and the picture comes back to this thread to be handed over:
        // that way nothing on the pool touches a response once the app has
        // stopped answering events, and QML is never given one twice.
        m_decoders.start([this, data = reply->readAll(), response, source, scaled, giveUp]() {
            QElapsedTimer decoding;
            decoding.start();
            QImage image;
            const bool readable = image.loadFromData(data);
            if (readable) {
                image = scaled(withoutLetterbox(image, source));
                countDecode(decoding.nsecsElapsed() / 1000);
            }
            QMetaObject::invokeMethod(this, [response, image, readable, giveUp]() {
                if (readable)
                    response->succeed(image);
                else
                    giveUp(QStringLiteral("Artwork data was not a readable image."), true);
            }, Qt::QueuedConnection);
        });
    });
}

// --------------------------------------------------------------- ArtworkCache

ArtworkCache::ArtworkCache(ArtworkFetcher *fetcher)
    : QQuickAsyncImageProvider()
    , m_fetcher(fetcher)
{
}

QQuickImageResponse *ArtworkCache::requestImageResponse(const QString &id,
                                                        const QSize &requestedSize)
{
    auto *response = new ArtworkResponse;

    // Artwork.qml percent-encodes the address so the "//" in the scheme
    // survives QML's url parsing; undo that here.
    const QString source = QUrl::fromPercentEncoding(id.toUtf8());

    // requestImageResponse runs on QML's image thread; hop to the fetcher's
    // thread before touching the network stack.
    QMetaObject::invokeMethod(m_fetcher, "fetch", Qt::QueuedConnection,
                              Q_ARG(ArtworkResponse *, response),
                              Q_ARG(QString, source),
                              Q_ARG(QSize, requestedSize));
    return response;
}

// ----------------------------------------------------------------- PaletteTool

PaletteTool::PaletteTool(ArtworkFetcher *fetcher, QObject *parent)
    : QObject(parent)
    , m_fetcher(fetcher)
{
}

QColor PaletteTool::dominantColour(const QString &source) const
{
    const QUrl url(source);
    const QString path = url.isLocalFile() ? url.toLocalFile() : source;
    const QImage image(path);
    return image.isNull() ? QColor() : dominantColour(image);
}

void PaletteTool::answer(const QString &source, const QColor &colour)
{
    if (!colour.isValid())
        return;
    m_known.insert(source, colour);
    Q_EMIT colourReady(source, colour);
}

void PaletteTool::request(const QString &source)
{
    if (source.isEmpty())
        return;
    // Always answered later, never from inside the call: QML asks from a
    // change handler and connects to the answer in the same breath.
    if (m_known.contains(source)) {
        QMetaObject::invokeMethod(this, [this, source]() {
            Q_EMIT colourReady(source, m_known.value(source));
        }, Qt::QueuedConnection);
        return;
    }

    const QUrl url(source);
    const bool local = !url.isValid() || url.isLocalFile() || url.scheme().isEmpty();
    if (local || !m_fetcher) {
        const QColor colour = local ? dominantColour(source) : QColor();
        QMetaObject::invokeMethod(this, [this, source, colour]() { answer(source, colour); },
                                  Qt::QueuedConnection);
        return;
    }

    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::PreferCache);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Monolist/0.1"));
    request.setTransferTimeout(10000);
    QNetworkReply *reply = m_fetcher->network()->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, source]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError)
            return;
        QImage image;
        if (image.loadFromData(reply->readAll()))
            answer(source, dominantColour(withoutLetterbox(image, source)));
    });
}

QColor PaletteTool::dominantColour(const QImage &image)
{
    if (image.isNull())
        return {};

    // 5 bits per channel — 32^3 buckets is fine-grained enough to separate
    // distinct hues but coarse enough that near-identical pixels combine.
    constexpr int kBits = 5;
    constexpr int kLevels = 1 << kBits;                 // 32
    constexpr int kShift = 8 - kBits;
    constexpr int kBuckets = kLevels * kLevels * kLevels;

    // Sample a bounded number of pixels so cost does not scale with resolution.
    const QImage sample = image.scaled(128, 128, Qt::KeepAspectRatio, Qt::FastTransformation)
                              .convertToFormat(QImage::Format_RGB32);

    std::vector<double> weights(kBuckets, 0.0);
    std::vector<std::array<double, 3>> sums(kBuckets, { 0.0, 0.0, 0.0 });

    for (int y = 0; y < sample.height(); ++y) {
        const auto *line = reinterpret_cast<const QRgb *>(sample.constScanLine(y));
        for (int x = 0; x < sample.width(); ++x) {
            const QColor pixel = QColor::fromRgb(line[x]);
            int h, s, v;
            pixel.getHsv(&h, &s, &v);

            // Near-white, near-black and grey pixels dominate most artwork by
            // count while carrying no identity — weight them down rather than
            // excluding them, so a genuinely monochrome cover still resolves.
            if (v < 24 || v > 244)
                continue;
            const double weight = 0.15 + (double(s) / 255.0) * 1.85;

            const int index = ((pixel.red()   >> kShift) << (kBits * 2))
                            | ((pixel.green() >> kShift) << kBits)
                            |  (pixel.blue()  >> kShift);

            weights[index] += weight;
            sums[index][0] += pixel.red()   * weight;
            sums[index][1] += pixel.green() * weight;
            sums[index][2] += pixel.blue()  * weight;
        }
    }

    int best = -1;
    double bestWeight = 0.0;
    for (int i = 0; i < kBuckets; ++i) {
        if (weights[i] > bestWeight) {
            bestWeight = weights[i];
            best = i;
        }
    }

    if (best < 0 || bestWeight <= 0.0)
        return {};

    return QColor::fromRgb(int(sums[best][0] / bestWeight),
                           int(sums[best][1] / bestWeight),
                           int(sums[best][2] / bestWeight));
}

#include "artworkcache.moc"
