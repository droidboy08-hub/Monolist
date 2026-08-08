#pragma once

#include <QColor>
#include <QImage>
#include <QObject>
#include <QQuickAsyncImageProvider>
#include <QSize>
#include <QString>

class QNetworkAccessManager;

// Asynchronous, disk-cached artwork loading for QML.
//
// This is the role Kingfisher fills in a Swift project. Kingfisher itself is
// Swift-only and cannot be linked into a Qt/C++ app, but everything it provides
// — off-thread decode, memory + disk cache, downsampling to the display size,
// deduplicated in-flight requests — is available from Qt directly, which is
// what this class assembles.
//
// QML usage:  Image { source: "image://artwork/" + track.artwork }
// Local paths and file:// URLs are read straight from disk; anything else is
// fetched over the network and cached.

class ArtworkResponse;

// Owns the QNetworkAccessManager and runs on the thread that created the
// provider, because QNetworkAccessManager is not thread-safe and QML calls the
// provider from its own thread.
class ArtworkFetcher : public QObject
{
    Q_OBJECT
public:
    explicit ArtworkFetcher(QObject *parent = nullptr);

public Q_SLOTS:
    void fetch(ArtworkResponse *response, const QString &source, const QSize &requestedSize);

private:
    QNetworkAccessManager *m_network;
};

class ArtworkCache : public QQuickAsyncImageProvider
{
public:
    explicit ArtworkCache(ArtworkFetcher *fetcher);

    QQuickImageResponse *requestImageResponse(const QString &id,
                                              const QSize &requestedSize) override;

private:
    ArtworkFetcher *m_fetcher;
};

// Small QML-facing helper for palette work.
//
// Stands in for ColorThiefSwift: extracts the dominant colour of an image so
// the interface can tint itself from the artwork. Uses a coarse RGB histogram
// with saturation weighting, which is what makes the result look like an accent
// rather than the muddy average a plain mean produces.
class PaletteTool : public QObject
{
    Q_OBJECT
public:
    explicit PaletteTool(QObject *parent = nullptr);

    // Accepts a local path, a file:// URL, or an already-cached artwork source.
    // Returns an invalid colour when the image cannot be read.
    Q_INVOKABLE QColor dominantColour(const QString &source) const;

    static QColor dominantColour(const QImage &image);
};
