#include "vectorsearch.h"

#include "catalog.h"
#include "matchkey.h"

#include <QHash>
#include <QString>
#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace Rec {
namespace {

// 4,096 rows of 64 floats is a 1 MiB scratch buffer, which stays in cache
// instead of round-tripping to memory between the widening pass and the
// arithmetic. It is also large enough that the per-block bookkeeping disappears.
constexpr int kBlockRows = 4096;

// The floor every anchor family scans to. A page of 20 rows needs far more than
// 20 candidates, because capPerArtist() then deletes most of a genre's head.
constexpr int kScanFloor = 120;

// Popularity floor for the families that do not take one. 8 is the iOS value:
// low enough to keep the long tail, high enough to drop the rows that exist in
// the source dataset but that nobody has ever played.
constexpr int kAnchorPopularity = 8;

// One row per artist on an open-ended shelf, two when the shelf is *about* an
// artist or a genre and some of their own work belongs in it. Both numbers come
// from iOS, where they were tuned against this embedding.
constexpr int kOpenShelfArtistCap = 1;
constexpr int kAnchoredShelfArtistCap = 2;

// Rows averaged into a genre's centroid. The same cap Catalog uses for artist
// centroids, for the same reason: the 41st row says nothing the first 40 did
// not, and finding it costs a scan.
constexpr int kCentroidRows = 40;

// Catalog::halfToFloat is the reference decoder and handles every corner of the
// format, but it is a call per component and a full scan makes 25 million of
// them. Normal values and zero are a shift and an add, so they are done here;
// subnormals, infinities and NaN go to the reference rather than being written
// out a second time and drifting from it.
inline float widenHalf(quint16 bits)
{
    const quint32 exponent = (bits >> 10) & 0x1Fu;
    if (exponent == 0u) {
        if (bits & 0x03FFu)
            return Catalog::halfToFloat(bits);
        return (bits & 0x8000u) ? -0.0f : 0.0f;
    }
    if (exponent == 0x1Fu)
        return Catalog::halfToFloat(bits);

    // Re-bias 15 -> 127 and move the 10-bit mantissa up to 23 bits.
    const quint32 out = (quint32(bits & 0x8000u) << 16)
                        | ((exponent + 112u) << 23)
                        | (quint32(bits & 0x03FFu) << 13);
    float value;
    std::memcpy(&value, &out, sizeof value);
    return value;
}

// One catalogue row, from the mapped payload into `dst`. Bounds were checked by
// the caller once for the whole block; doing it per component is most of the
// cost of the loop.
inline void widenRow(const uchar *src, int dims, int componentBytes, float *dst)
{
    switch (componentBytes) {
    case 2:
        for (int i = 0; i < dims; ++i)
            dst[i] = widenHalf(qFromLittleEndian<quint16>(src + qint64(i) * 2));
        break;
    case 4:
        for (int i = 0; i < dims; ++i) {
            const quint32 raw = qFromLittleEndian<quint32>(src + qint64(i) * 4);
            std::memcpy(&dst[i], &raw, sizeof(float));
        }
        break;
    default:
        for (int i = 0; i < dims; ++i)
            dst[i] = float(static_cast<qint8>(src[i])) / 127.0f;
        break;
    }
}

// Four running sums rather than one. A single accumulator serialises on the
// latency of the add and no compiler may reassociate floats to fix that; four
// independent ones are the same arithmetic in a fixed order, and they are also
// the shape the vectoriser recognises, so this becomes packed multiplies
// without asking for fast maths and without hand-written SIMD.
inline float dot(const float *a, const float *b, int dims)
{
    float s0 = 0.0f;
    float s1 = 0.0f;
    float s2 = 0.0f;
    float s3 = 0.0f;
    int d = 0;
    for (; d + 4 <= dims; d += 4) {
        s0 += a[d] * b[d];
        s1 += a[d + 1] * b[d + 1];
        s2 += a[d + 2] * b[d + 2];
        s3 += a[d + 3] * b[d + 3];
    }
    for (; d < dims; ++d)
        s0 += a[d] * b[d];
    return (s0 + s1) + (s2 + s3);
}

// Keeps only the k best seen, as a min-heap so the worst of them is at the top.
// wouldAccept() is the hot path: once the heap is full it is one float compare
// that rejects nearly every row before a Hit is ever built.
class BoundedMinHeap
{
public:
    explicit BoundedMinHeap(int capacity)
        : m_capacity(capacity)
    {
        m_items.reserve(capacity);
    }

    bool wouldAccept(float score) const
    {
        return m_items.size() < m_capacity || score > m_items.at(0).score;
    }

    void insert(int row, float score)
    {
        if (m_items.size() < m_capacity) {
            m_items.append(Hit{row, score});
            siftUp(int(m_items.size()) - 1);
        } else if (score > m_items.at(0).score) {
            m_items[0] = Hit{row, score};
            siftDown(0);
        }
    }

    QVector<Hit> sortedDescending() const
    {
        QVector<Hit> out = m_items;
        std::sort(out.begin(), out.end(), [](const Hit &a, const Hit &b) {
            // Ties break on the row id. iOS leaves them to an unstable sort and
            // gets a different page each run; a pager that reshuffles under the
            // reader between pages shows the same track twice and skips another.
            return a.score != b.score ? a.score > b.score : a.row < b.row;
        });
        return out;
    }

private:
    void siftUp(int start)
    {
        int child = start;
        while (child > 0) {
            const int parent = (child - 1) / 2;
            if (!(m_items.at(child).score < m_items.at(parent).score))
                break;
            std::swap(m_items[child], m_items[parent]);
            child = parent;
        }
    }

    void siftDown(int start)
    {
        const int n = int(m_items.size());
        int parent = start;
        for (;;) {
            const int left = 2 * parent + 1;
            const int right = left + 1;
            int smallest = parent;
            if (left < n && m_items.at(left).score < m_items.at(smallest).score)
                smallest = left;
            if (right < n && m_items.at(right).score < m_items.at(smallest).score)
                smallest = right;
            if (smallest == parent)
                return;
            std::swap(m_items[parent], m_items[smallest]);
            parent = smallest;
        }
    }

    QVector<Hit> m_items;
    int m_capacity;
};

// max(120, offset + limit): deep pages have to be scanned for, not paged into.
inline int scanDepth(int limit, int offset)
{
    return qMax(kScanFloor, offset + limit);
}

// The last step of every anchor family. Past the end is an empty page, not a
// clamped one: a pager that keeps handing back the final rows never ends.
QVector<Hit> pageOf(const QVector<Hit> &hits, int offset, int limit)
{
    if (limit <= 0 || offset < 0 || offset >= hits.size())
        return QVector<Hit>();
    return hits.mid(offset, limit);
}

// Unit-length copy of `v`, or empty when it has no direction to give.
QVector<float> normalised(QVector<float> v)
{
    double norm = 0.0;
    for (const float x : v)
        norm += double(x) * double(x);
    norm = std::sqrt(norm);
    if (!(norm > 1e-6))
        return QVector<float>();
    for (float &x : v)
        x = float(double(x) / norm);
    return v;
}

} // namespace

// ----------------------------------------------------------------- the scan

QVector<QVector<Hit>> topK(const Catalog &catalog,
                           const QVector<QVector<float>> &queries,
                           int k,
                           int minPopularity,
                           const std::function<bool(int row)> &keep)
{
    QVector<QVector<Hit>> out(queries.size());

    const int dims = catalog.dims();
    const int count = catalog.count();
    const uchar *payload = catalog.vectorData();
    const int componentBytes = catalog.componentBytes();
    if (!catalog.isLoaded() || !payload || k <= 0 || dims <= 0 || count <= 0)
        return out;

    // A query of the wrong length is dropped here rather than at the end, so a
    // caller that built one bad column still gets the other eleven, in place.
    QVector<int> columnOf;
    QVector<const float *> columnData;
    columnOf.reserve(queries.size());
    columnData.reserve(queries.size());
    for (int i = 0; i < queries.size(); ++i) {
        if (queries.at(i).size() != dims)
            continue;
        columnOf.append(i);
        columnData.append(queries.at(i).constData());
    }
    if (columnOf.isEmpty())
        return out;

    QVector<BoundedMinHeap> heaps;
    heaps.reserve(columnOf.size());
    for (int c = 0; c < columnOf.size(); ++c)
        heaps.append(BoundedMinHeap(k));

    QVector<float> block(qsizetype(kBlockRows) * dims);
    QVector<int> rowIds(kBlockRows);

    const qint64 rowBytes = qint64(dims) * componentBytes;
    const int columns = columnOf.size();
    // Pulled out of the loops: QVector's operator[] checks for a shared buffer
    // every time, which is noise 25 million calls deep.
    float *blockData = block.data();
    int *rowData = rowIds.data();
    BoundedMinHeap *heapData = heaps.data();
    const float *const *queryData = columnData.constData();
    const bool filtered = bool(keep);

    for (int start = 0; start < count; start += kBlockRows) {
        const int rowsInBlock = qMin(kBlockRows, count - start);

        // Gather and widen in one pass. A row that fails a filter never reaches
        // the f16 decode, let alone the arithmetic, which is the reason this is
        // a hand-written scan and not a library call.
        int survivors = 0;
        for (int i = 0; i < rowsInBlock; ++i) {
            const int row = start + i;
            if (catalog.popularity(row) < minPopularity)
                continue;
            if (filtered && !keep(row))
                continue;
            rowData[survivors] = row;
            widenRow(payload + qint64(row) * rowBytes, dims, componentBytes,
                     blockData + qsizetype(survivors) * dims);
            ++survivors;
        }

        // Rows outside, columns inside: the row stays in L1 while every query
        // reads it, where the other order walks the whole 1 MiB block once per
        // column. Same products, same order within a column, fewer cache misses.
        for (int n = 0; n < survivors; ++n) {
            const float *v = blockData + qsizetype(n) * dims;
            const int row = rowData[n];
            for (int c = 0; c < columns; ++c) {
                const float score = dot(v, queryData[c], dims);
                if (heapData[c].wouldAccept(score))
                    heapData[c].insert(row, score);
            }
        }
    }

    for (int c = 0; c < columns; ++c)
        out[columnOf.at(c)] = heapData[c].sortedDescending();
    return out;
}

QVector<Hit> capPerArtist(const Catalog &catalog, const QVector<Hit> &hits, int cap)
{
    QVector<Hit> out;
    if (cap <= 0)
        return out;
    out.reserve(hits.size());

    QHash<QString, int> taken;
    // One normalisation per artist, not per hit: a shelf is usually a handful
    // of artists and primaryArtist() is a full pass over the name.
    QHash<int, QString> keyOf;
    for (const Hit &hit : hits) {
        const int id = catalog.artistId(hit.row);
        auto cached = keyOf.find(id);
        if (cached == keyOf.end())
            cached = keyOf.insert(id, Rec::primaryArtist(catalog.artistName(id)));

        int &used = taken[cached.value()];
        if (used >= cap)
            continue;
        ++used;
        out.append(hit);
    }
    return out;
}

// ------------------------------------------------------------ anchor families

QVector<Hit> nearest(const Catalog &catalog,
                     const QVector<float> &query,
                     int limit,
                     int offset,
                     int minPopularity)
{
    if (query.isEmpty() || limit <= 0 || offset < 0)
        return QVector<Hit>();

    const QVector<QVector<Hit>> columns =
        topK(catalog, {query}, scanDepth(limit, offset), minPopularity, {});
    return pageOf(capPerArtist(catalog, columns.value(0), kOpenShelfArtistCap), offset, limit);
}

QVector<Hit> artistNeighbours(const Catalog &catalog, int artistId, int limit, int offset)
{
    if (limit <= 0 || offset < 0)
        return QVector<Hit>();

    // The anchor's own rows are not filtered out, so up to two of them may come
    // back. That is deliberate: "more like this artist" that refuses to play
    // the artist reads as a mistake. In practice few of them do - the centroid
    // is the mean of a whole discography and no single track is the mean - so
    // this is an allowance rather than a reservation.
    const QVector<float> centroid = catalog.artistCentroid(artistId);
    if (centroid.isEmpty())
        return QVector<Hit>();

    const QVector<QVector<Hit>> columns =
        topK(catalog, {centroid}, scanDepth(limit, offset), kAnchorPopularity, {});
    return pageOf(capPerArtist(catalog, columns.value(0), kAnchoredShelfArtistCap), offset, limit);
}

QVector<Hit> genreNeighbours(const Catalog &catalog, int genreId, int limit, int offset)
{
    if (limit <= 0 || offset < 0 || genreId < 0 || genreId >= catalog.genreCount())
        return QVector<Hit>();

    const int dims = catalog.dims();
    if (dims <= 0)
        return QVector<Hit>();

    // The genre file carries its own 56-dimension embeddings, but they live in
    // the genre half of the space only and Catalog does not hand them out. The
    // mean of the genre's own rows is in the same space as everything being
    // ranked, which is what matters, and the catalogue's row order puts the
    // rows people actually play first.
    QVector<float> centroid(dims, 0.0f);
    QVector<float> row(dims, 0.0f);
    int sampled = 0;
    for (int r = 0; r < catalog.count() && sampled < kCentroidRows; ++r) {
        if (catalog.genreId(r) != genreId)
            continue;
        if (!catalog.loadVector(r, row.data()))
            continue;
        for (int i = 0; i < dims; ++i)
            centroid[i] += row.at(i);
        ++sampled;
    }
    if (sampled == 0)
        return QVector<Hit>();

    const QVector<float> query = normalised(centroid);
    if (query.isEmpty())
        return QVector<Hit>();

    // Inside the genre only. Without the filter this returns the neighbouring
    // genres too - they sit at ~0.99 in a space built out of genre - and the
    // shelf stops meaning what its title says.
    const QVector<QVector<Hit>> columns =
        topK(catalog, {query}, scanDepth(limit, offset), kAnchorPopularity,
             [&catalog, genreId](int r) { return catalog.genreId(r) == genreId; });
    return pageOf(capPerArtist(catalog, columns.value(0), kAnchoredShelfArtistCap), offset, limit);
}

} // namespace Rec
