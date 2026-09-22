#include "artworkcache.h"

#include <QDir>
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
#include <QStandardPaths>
#include <QUrl>

#include <array>

namespace {

// Video thumbnails (i.ytimg.com's hqdefault and sddefault) are 4:3 with the
// 16:9 frame letterboxed in black; cropped square for a cover, the bars stay.
// Rows that are black all the way across, at the top and the bottom, are cut
// off. Only for video thumbnails: an album cover may well be black at the edge.
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

} // namespace

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
        QImage image(path);
        if (image.isNull())
            response->fail(QStringLiteral("Could not read artwork: %1").arg(path));
        else
            response->succeed(scaled(image));
        return;
    }

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
    QObject::connect(reply, &QNetworkReply::finished, this, [reply, response, scaled, source]() {
        reply->deleteLater();
        response->detachReply();

        if (reply->error() != QNetworkReply::NoError) {
            response->fail(reply->errorString());
            return;
        }

        // Artwork is a thumbnail. Anything wildly larger is a redirect to a
        // page, a captive portal, or a broken instance — refuse it rather than
        // decoding it.
        constexpr qint64 kMaxArtworkBytes = 24LL * 1024 * 1024;
        if (reply->bytesAvailable() > kMaxArtworkBytes) {
            response->fail(QStringLiteral("Artwork response too large (%1 bytes).")
                               .arg(reply->bytesAvailable()));
            return;
        }

        QImage image;
        if (!image.loadFromData(reply->readAll())) {
            response->fail(QStringLiteral("Artwork data was not a readable image."));
            return;
        }
        response->succeed(scaled(withoutLetterbox(image, source)));
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
