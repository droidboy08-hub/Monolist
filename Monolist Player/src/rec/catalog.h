#pragma once

#include <QFile>
#include <QHash>
#include <QMutex>
#include <QString>
#include <QVector>

namespace Rec {

// The offline embedding catalogue: 400,000 tracks, each with a title, an
// interned artist name, a popularity byte, an optional genre id and a
// 64-dimension unit vector. Four little-endian, packed files built by
// graph/build_embeddings.py and shipped beside the app.
//
// Read-only and memory-mapped rather than read. The vector payload alone is
// 51 MB of f16; widening all of it to float32 up front would cost 100 MB of
// dirty RAM to save a conversion loop over the few thousand rows a query
// actually touches. Mapped pages are clean and file-backed, so the OS can drop
// them under pressure and fault them back in without the app noticing.
//
// Every failure path returns false, -1 or an empty value instead of reporting
// an error. A missing or truncated catalogue has to be indistinguishable from
// "no recommendation data" so the rest of the app falls back to its other
// sources rather than breaking. Only the three core files are required: a
// genre file that fails its own checks is dropped silently and the catalogue
// still loads, it just cannot name anything.
//
// Const methods are safe to call from several threads at once, which is the
// point — a full scan of 400,000 rows does not belong on the GUI thread. The
// one piece of lazy state (the artist index behind the centroid tier) is built
// under a mutex on first use.
//
// Include this as "rec/catalog.h": the app already has an unrelated Catalog in
// src/catalog.h, which is why this one is Rec::Catalog and why a bare
// "catalog.h" from outside this directory finds the other file.
class Catalog
{
public:
    Catalog();
    ~Catalog();
    Catalog(const Catalog &) = delete;
    Catalog &operator=(const Catalog &) = delete;

    // What match() made of a track the app knows only by name.
    //
    // `row` is -1 on the artist-centroid tier, which describes an artist and
    // not a recording: there is no row to point at, and `artistId` is then the
    // artist to hand to artistCentroid(). confidence is 0 when nothing matched.
    struct Match
    {
        int row = -1;
        int artistId = -1;
        float confidence = 0.0f;
    };

    // Maps embeat_v1_{vectors,keys,meta,genres}.bin out of `directory`.
    // False when any of the three required files is missing or fails its
    // header checks; the catalogue is then left unloaded, not half-loaded.
    bool load(const QString &directory);
    void unload();
    bool isLoaded() const { return m_loaded; }

    int count() const { return m_count; }   // catalogue rows
    int dims() const { return m_dims; }     // 64
    int k1Count() const { return m_k1Count; }
    int k2Count() const { return m_k2Count; }
    int artistCount() const { return m_artistCount; }
    int genreCount() const { return m_genreCount; }

    // One row widened to float, or null when `row` is out of range.
    //
    // The catalogue holds f16, so there is no float array in the file to point
    // at and the row is widened into a small per-thread ring of buffers. The
    // pointer stays good until the fourth further vector() call on the same
    // thread; anything that needs a row to outlive that owns its storage and
    // calls loadVector() instead.
    const float *vector(int row) const;

    // Writes dims() floats into `dst`, which the caller owns. False when the
    // row is out of range, in which case `dst` is untouched.
    bool loadVector(int row, float *dst) const;

    // The mapped payload and its component width, for a scanner that wants to
    // widen whole blocks itself rather than pay a call per row. 2 bytes for
    // the shipped f16 build. Null and 0 when nothing is loaded.
    const uchar *vectorData() const { return m_loaded ? m_vectors.data + kVectorHeaderBytes : nullptr; }
    int componentBytes() const { return m_componentBytes; }

    QString title(int row) const;
    QString artist(int row) const;
    int artistId(int row) const;              // -1 when the row is out of range
    QString artistName(int artistId) const;
    int popularity(int row) const;            // 0..100 as built, 0 when unknown
    int genreId(int row) const;               // -1 when none, or no genre file
    QString genreName(int genreId) const;

    // Best catalogue row for a track known only by name, down the three-tier
    // ladder: strict title+artist key (confidence 1.0), globally unique title
    // whose artist still plausibly agrees (0.75), then the artist's centroid
    // with no row at all (0.45).
    //
    // Nothing below that: an unmatched track keeps whatever ranking it came
    // with instead of being scored against a vector that is not really its own.
    //
    // iOS also takes a duration here and ignores it. It is not in this
    // signature, because an argument that does nothing invites a caller to
    // believe duration disambiguates a collision when it does not.
    Match match(const QString &title, const QString &artist) const;

    // Mean of the rows credited to this artist, L2-normalised; empty when the
    // artist has no rows. This is the vector behind the 0.45 tier of match().
    //
    // "Credited to this artist" means every interned name that normalises to
    // the same primary-artist key, not just this one spelling, so "Beyoncé"
    // and "Beyonce" contribute to one centroid the way they do on iOS.
    QVector<float> artistCentroid(int artistId) const;

    // An artist's rows, most popular first, at most `limit`, drawn from the
    // same credit set as the centroid. The catalogue's popularity comes from
    // listening, which makes it a better judge of which songs to offer than the
    // graph's rating counts — those put "Love Story / interlude" at the top of
    // Rod Wave.
    QVector<int> artistRows(int artistId, int limit) const;

    // f16 -> f32 by bit surgery: no compiler or CPU half type is assumed, and
    // subnormals, infinities and NaN payloads all survive.
    static float halfToFloat(quint16 bits);

private:
    // The payload starts here so that it is page-aligned however the file is
    // mapped; the header itself is 20 bytes of the 16 KiB.
    static constexpr int kVectorHeaderBytes = 16384;
    static constexpr int kKeyEntryBytes = 12;      // u64 hash + u32 row, packed
    // A centroid averaging more rows than this says nothing new about the
    // artist, and iOS caps it at the same place, so the numbers agree.
    static constexpr int kCentroidRows = 40;

    // One mapped file. The QFile has to outlive the pointer: Qt unmaps on
    // close() and on destruction, so these are members rather than locals.
    struct Mapping
    {
        QFile file;
        const uchar *data = nullptr;
        qint64 size = 0;

        bool open(const QString &path);
        void close();
    };

    bool readVectorHeader();
    bool readKeyHeader();
    bool readMetaHeader();
    bool readGenreHeader();   // false only drops the labels, never the load

    QVector<int> keyRows(quint64 hash, qint64 offset, int entries) const;
    // Highest popularity wins, first row on ties, matching iOS.
    int bestRow(const QVector<int> &rows) const;
    QString blobString(const uchar *base, qint64 blobStart, qint64 blobSize,
                       qint64 entryOffset) const;
    // Call with m_artistMutex held.
    void ensureArtistIndex() const;

    bool m_loaded = false;

    Mapping m_vectors;
    Mapping m_keys;
    Mapping m_meta;
    Mapping m_genres;

    int m_count = 0;
    int m_dims = 0;
    int m_componentBytes = 0;

    int m_k1Count = 0;
    int m_k2Count = 0;
    qint64 m_k1Offset = 0;
    qint64 m_k2Offset = 0;

    int m_artistCount = 0;
    qint64 m_titleIndexOffset = 0;
    qint64 m_artistIdOffset = 0;
    qint64 m_artistIndexOffset = 0;
    qint64 m_titleBlobOffset = 0;
    qint64 m_titleBlobLen = 0;
    qint64 m_artistBlobOffset = 0;
    qint64 m_artistBlobLen = 0;
    qint64 m_popularityOffset = 0;

    int m_genreCount = 0;
    int m_genreDims = 0;
    qint64 m_genreNameIndexOffset = 0;
    qint64 m_genreBlobOffset = 0;
    qint64 m_genreBlobLen = 0;
    qint64 m_genrePrimaryOffset = 0;

    // Artist name -> rows, built once on the first centroid lookup. Without it
    // a centroid miss rescans and re-normalises every artist string in the
    // catalogue, and a library the catalogue matches poorly asks constantly.
    // Built over interned ids, so it costs one normalisation per distinct
    // artist rather than one per row.
    mutable QMutex m_artistMutex;
    mutable bool m_artistIndexBuilt = false;
    mutable QVector<QString> m_artistKey;          // interned id -> primary key
    mutable QHash<QString, QVector<int>> m_keyRows;
    mutable QHash<QString, int> m_keyLead;         // key -> its first row's id
};

} // namespace Rec
