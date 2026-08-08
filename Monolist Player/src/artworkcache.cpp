#include "artworkcache.h"

#include <QDir>
#include <QFileInfo>
#include <QMetaObject>
#include <QNetworkAccessManager>
#include <QNetworkDiskCache>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QQuickTextureFactory>
#include <QStandardPaths>
#include <QUrl>

#include <array>

// ------------------------------------------------------------ ArtworkResponse

class ArtworkResponse : public QQuickImageResponse
{
    Q_OBJECT
public:
    QQuickTextureFactory *textureFactory() const override
    {
        return QQuickTextureFactory::textureFactoryForImage(m_image);
    }

    QString errorString() const override { return m_error; }

    void cancel() override
    {
        if (m_reply && m_reply->isRunning())
            m_reply->abort();
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

public:
    QPointer<QNetworkReply> m_reply;

private:
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
    response->m_reply = reply;

    QObject::connect(reply, &QNetworkReply::finished, response, [reply, response, scaled]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            response->fail(reply->errorString());
            return;
        }
        QImage image;
        if (!image.loadFromData(reply->readAll())) {
            response->fail(QStringLiteral("Artwork data was not a readable image."));
            return;
        }
        response->succeed(scaled(image));
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

    // requestImageResponse runs on QML's image thread; hop to the fetcher's
    // thread before touching the network stack.
    QMetaObject::invokeMethod(m_fetcher, "fetch", Qt::QueuedConnection,
                              Q_ARG(ArtworkResponse *, response),
                              Q_ARG(QString, id),
                              Q_ARG(QSize, requestedSize));
    return response;
}

// ----------------------------------------------------------------- PaletteTool

PaletteTool::PaletteTool(QObject *parent)
    : QObject(parent)
{
}

QColor PaletteTool::dominantColour(const QString &source) const
{
    const QUrl url(source);
    const QString path = url.isLocalFile() ? url.toLocalFile() : source;
    const QImage image(path);
    return image.isNull() ? QColor() : dominantColour(image);
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
