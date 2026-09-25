#include "catalog.h"
// The name rules that produced every key in these files. They live apart from
// the catalogue so they can be tested against graph/match_key_golden.tsv
// without standing up 73 MB of binaries.
#include "matchkey.h"

#include <QDir>
#include <QtEndian>

#include <cmath>
#include <cstring>
#include <limits>

namespace Rec {
namespace {

// The files are little-endian and packed, so nothing in them is guaranteed to
// be aligned. qFromLittleEndian reads byte-wise and byte-swaps only where it
// has to, which is both correct on a big-endian host and free on this one.
inline quint16 le16(const uchar *p, qint64 o) { return qFromLittleEndian<quint16>(p + o); }
inline quint32 le32(const uchar *p, qint64 o) { return qFromLittleEndian<quint32>(p + o); }
inline quint64 le64(const uchar *p, qint64 o) { return qFromLittleEndian<quint64>(p + o); }

inline bool magicIs(const uchar *p, const char *fourCC)
{
    return std::memcmp(p, fourCC, 4) == 0;
}

// Counts come out of the file as u32 and are used as array bounds. Anything
// that does not fit in an int would silently wrap into a negative index, so a
// hostile or corrupt header is rejected here rather than trusted downstream.
inline bool fitsInt(quint32 v) { return v <= quint32(std::numeric_limits<int>::max()); }

} // namespace

// --------------------------------------------------------------------- Mapping

bool Catalog::Mapping::open(const QString &path)
{
    close();
    file.setFileName(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    size = file.size();
    if (size <= 0) {
        close();
        return false;
    }
    // QFile::map is the portable spelling of mmap/MapViewOfFile and gives back
    // clean, read-only, file-backed pages on every platform the player ships on.
    data = file.map(0, size);
    if (!data) {
        close();
        return false;
    }
    return true;
}

void Catalog::Mapping::close()
{
    if (data) {
        file.unmap(const_cast<uchar *>(data));
        data = nullptr;
    }
    size = 0;
    if (file.isOpen())
        file.close();
}

// ------------------------------------------------------------------- lifetime

Catalog::Catalog() = default;

Catalog::~Catalog()
{
    unload();
}

bool Catalog::load(const QString &directory)
{
    unload();

    const QDir dir(directory);
    if (!m_vectors.open(dir.filePath(QStringLiteral("embeat_v1_vectors.bin")))
        || !m_keys.open(dir.filePath(QStringLiteral("embeat_v1_keys.bin")))
        || !m_meta.open(dir.filePath(QStringLiteral("embeat_v1_meta.bin")))) {
        unload();
        return false;
    }

    if (!readVectorHeader() || !readKeyHeader() || !readMetaHeader()) {
        unload();
        return false;
    }

    // Genres are optional and independently validated: a bad genre file must
    // cost the catalogue its labels, never its vectors.
    if (m_genres.open(dir.filePath(QStringLiteral("embeat_v1_genres.bin")))) {
        if (!readGenreHeader())
            m_genres.close();
    }

    m_loaded = true;
    return true;
}

void Catalog::unload()
{
    m_loaded = false;

    m_vectors.close();
    m_keys.close();
    m_meta.close();
    m_genres.close();

    m_count = m_dims = m_componentBytes = 0;
    m_k1Count = m_k2Count = 0;
    m_k1Offset = m_k2Offset = 0;
    m_artistCount = 0;
    m_titleIndexOffset = m_artistIdOffset = m_artistIndexOffset = 0;
    m_titleBlobOffset = m_titleBlobLen = 0;
    m_artistBlobOffset = m_artistBlobLen = 0;
    m_popularityOffset = 0;
    m_genreCount = m_genreDims = 0;
    m_genreNameIndexOffset = m_genreBlobOffset = m_genreBlobLen = m_genrePrimaryOffset = 0;

    QMutexLocker lock(&m_artistMutex);
    m_artistIndexBuilt = false;
    m_artistKey.clear();
    m_keyRows.clear();
    m_keyLead.clear();
}

// ---------------------------------------------------------- header validation

bool Catalog::readVectorHeader()
{
    const uchar *p = m_vectors.data;
    if (m_vectors.size <= kVectorHeaderBytes)
        return false;
    if (!magicIs(p, "AMEB") || le16(p, 4) != 1)
        return false;

    const int dtype = int(le16(p, 6));
    const quint32 dims = le32(p, 8);
    const quint32 count = le32(p, 12);
    // 0 f32, 1 f16 (what ships), 2 int8 scaled by 1/127.
    const int width = dtype == 0 ? 4 : dtype == 1 ? 2 : dtype == 2 ? 1 : 0;
    if (width == 0 || dims != 64 || count == 0 || !fitsInt(count))
        return false;

    // The size is derived and never trusted from `count`: a download that was
    // cut short has to be indistinguishable from a file that is not there.
    if (m_vectors.size != qint64(kVectorHeaderBytes) + qint64(count) * qint64(dims) * width)
        return false;

    m_dims = int(dims);
    m_count = int(count);
    m_componentBytes = width;
    return true;
}

bool Catalog::readKeyHeader()
{
    const uchar *p = m_keys.data;
    if (m_keys.size < 16)
        return false;
    if (!magicIs(p, "AMEK") || le16(p, 4) != 1)
        return false;

    const quint32 k1 = le32(p, 8);
    const quint32 k2 = le32(p, 12);
    if (!fitsInt(k1) || !fitsInt(k2))
        return false;

    m_k1Count = int(k1);
    m_k2Count = int(k2);
    m_k1Offset = 16;
    m_k2Offset = m_k1Offset + qint64(m_k1Count) * kKeyEntryBytes;
    // Only "at least": the key and meta files are allowed a trailing tail, and
    // one build did emit padding there. The vector file is the strict one.
    return m_keys.size >= m_k2Offset + qint64(m_k2Count) * kKeyEntryBytes;
}

bool Catalog::readMetaHeader()
{
    const uchar *p = m_meta.data;
    if (m_meta.size < 20)
        return false;
    if (!magicIs(p, "AMEM") || le16(p, 4) != 1)
        return false;

    const quint32 trackCount = le32(p, 8);
    const quint32 artistCount = le32(p, 12);
    const quint32 titleBlobLen = le32(p, 16);
    if (!fitsInt(trackCount) || !fitsInt(artistCount) || int(trackCount) != m_count)
        return false;

    m_artistCount = int(artistCount);
    m_titleBlobLen = titleBlobLen;

    // Field by field, in file order. The u32 between the artist index and the
    // blobs is the length of the artist blob, and it is the easiest thing here
    // to read as part of the index and be 4 bytes wrong for the rest of time.
    m_titleIndexOffset = 20;                                                  // count * {u32 offset, u8 len}
    m_artistIdOffset = m_titleIndexOffset + qint64(m_count) * 5;              // count * u32
    m_artistIndexOffset = m_artistIdOffset + qint64(m_count) * 4;             // artistCount * {u32, u8}
    const qint64 artistBlobLenOffset = m_artistIndexOffset + qint64(m_artistCount) * 5;
    if (m_meta.size <= artistBlobLenOffset + 4)
        return false;

    m_artistBlobLen = le32(p, artistBlobLenOffset);
    m_titleBlobOffset = artistBlobLenOffset + 4;
    m_artistBlobOffset = m_titleBlobOffset + m_titleBlobLen;
    // The u32 popularity count that follows the blobs is written but never
    // needed — it always equals the track count — so it is stepped over.
    m_popularityOffset = m_artistBlobOffset + m_artistBlobLen + 4;
    return m_meta.size >= m_popularityOffset + m_count;
}

bool Catalog::readGenreHeader()
{
    const uchar *p = m_genres.data;
    if (m_genres.size <= 20)
        return false;
    if (!magicIs(p, "AMEG") || le16(p, 4) != 1)
        return false;

    const quint32 genreDims = le16(p, 6);
    const quint32 genreCount = le32(p, 8);
    const quint32 trackCount = le32(p, 12);
    const quint32 nameBlobLen = le32(p, 16);
    if (!fitsInt(genreCount) || !fitsInt(trackCount) || genreDims == 0 || genreCount == 0)
        return false;
    if (int(trackCount) != m_count)
        return false;

    const qint64 vectorOffset = 20;
    m_genreNameIndexOffset = vectorOffset + qint64(genreCount) * genreDims * 2;
    m_genreBlobOffset = m_genreNameIndexOffset + qint64(genreCount) * 5;
    m_genreBlobLen = nameBlobLen;
    m_genrePrimaryOffset = m_genreBlobOffset + m_genreBlobLen;
    if (m_genres.size < m_genrePrimaryOffset + qint64(m_count) * 2)
        return false;

    m_genreCount = int(genreCount);
    m_genreDims = int(genreDims);
    return true;
}

// --------------------------------------------------------------------- vectors

float Catalog::halfToFloat(quint16 bits)
{
    const quint32 sign = quint32(bits & 0x8000u) << 16;
    const quint32 exponent = (bits >> 10) & 0x1Fu;
    const quint32 mantissa = bits & 0x03FFu;

    quint32 out;
    if (exponent == 0) {
        if (mantissa == 0) {
            out = sign;   // signed zero
        } else {
            // Subnormal half, normal float. Shift the mantissa left until the
            // implicit leading 1 appears and charge the exponent for each step.
            quint32 m = mantissa;
            quint32 shifts = 0;
            while ((m & 0x0400u) == 0) {
                m <<= 1;
                ++shifts;
            }
            m &= 0x03FFu;
            out = sign | ((127u - 15u + 1u - shifts) << 23) | (m << 13);
        }
    } else if (exponent == 0x1Fu) {
        // Infinity, or a NaN whose payload is carried across rather than
        // flattened — a quiet NaN that becomes an infinity stops being visible.
        out = sign | 0x7F800000u | (mantissa << 13);
    } else {
        out = sign | ((exponent + (127u - 15u)) << 23) | (mantissa << 13);
    }

    float value;
    std::memcpy(&value, &out, sizeof value);
    return value;
}

bool Catalog::loadVector(int row, float *dst) const
{
    if (!m_loaded || !dst || row < 0 || row >= m_count)
        return false;

    const uchar *p = m_vectors.data + kVectorHeaderBytes
                     + qint64(row) * m_dims * m_componentBytes;
    switch (m_componentBytes) {
    case 2:
        for (int i = 0; i < m_dims; ++i)
            dst[i] = halfToFloat(le16(p, qint64(i) * 2));
        break;
    case 4:
        for (int i = 0; i < m_dims; ++i) {
            const quint32 raw = le32(p, qint64(i) * 4);
            std::memcpy(&dst[i], &raw, sizeof(float));
        }
        break;
    default:
        for (int i = 0; i < m_dims; ++i)
            dst[i] = float(static_cast<qint8>(p[i])) / 127.0f;
        break;
    }
    return true;
}

const float *Catalog::vector(int row) const
{
    if (!m_loaded || row < 0 || row >= m_count)
        return nullptr;

    // Four slots, because comparing a couple of rows to each other is the
    // common use and making every caller own a buffer for that is noise. Any
    // caller that needs a row to survive longer owns its storage already and
    // uses loadVector().
    // "slots" is a Qt keyword macro, hence the name.
    constexpr int ringSlots = 4;
    static thread_local QVector<float> ring;
    static thread_local int nextSlot = 0;

    if (ring.size() != ringSlots * m_dims) {
        ring.fill(0.0f, ringSlots * m_dims);
        nextSlot = 0;
    }

    float *dst = ring.data() + nextSlot * m_dims;
    nextSlot = (nextSlot + 1) % ringSlots;
    loadVector(row, dst);
    return dst;
}

// -------------------------------------------------------------------- metadata

QString Catalog::blobString(const uchar *base, qint64 blobStart, qint64 blobSize,
                            qint64 entryOffset) const
{
    const qint64 start = le32(base, entryOffset);
    const qint64 length = base[entryOffset + 4];
    if (length <= 0 || start < 0 || start + length > blobSize)
        return QString();
    // Strings were cut at 255 BYTES when the catalogue was built, so a tail can
    // end mid code point. fromUtf8 substitutes U+FFFD for the stump instead of
    // failing, which is what "decode leniently" has to mean when the alternative
    // is losing the row.
    return QString::fromUtf8(reinterpret_cast<const char *>(base + blobStart + start),
                             int(length));
}

QString Catalog::title(int row) const
{
    if (!m_loaded || row < 0 || row >= m_count)
        return QString();
    return blobString(m_meta.data, m_titleBlobOffset, m_titleBlobLen,
                      m_titleIndexOffset + qint64(row) * 5);
}

int Catalog::artistId(int row) const
{
    if (!m_loaded || row < 0 || row >= m_count)
        return -1;
    const quint32 id = le32(m_meta.data, m_artistIdOffset + qint64(row) * 4);
    return id < quint32(m_artistCount) ? int(id) : -1;
}

QString Catalog::artistName(int artistId) const
{
    if (!m_loaded || artistId < 0 || artistId >= m_artistCount)
        return QString();
    return blobString(m_meta.data, m_artistBlobOffset, m_artistBlobLen,
                      m_artistIndexOffset + qint64(artistId) * 5);
}

QString Catalog::artist(int row) const
{
    return artistName(artistId(row));
}

int Catalog::popularity(int row) const
{
    if (!m_loaded || row < 0 || row >= m_count)
        return 0;
    return int(m_meta.data[m_popularityOffset + row]);
}

int Catalog::genreId(int row) const
{
    if (!m_loaded || !m_genres.data || row < 0 || row >= m_count)
        return -1;
    const quint16 id = le16(m_genres.data, m_genrePrimaryOffset + qint64(row) * 2);
    return id == 0xFFFFu ? -1 : int(id);
}

QString Catalog::genreName(int genreId) const
{
    if (!m_loaded || !m_genres.data || genreId < 0 || genreId >= m_genreCount)
        return QString();
    return blobString(m_genres.data, m_genreBlobOffset, m_genreBlobLen,
                      m_genreNameIndexOffset + qint64(genreId) * 5);
}

// ------------------------------------------------------------------ key lookup

QVector<int> Catalog::keyRows(quint64 hash, qint64 offset, int entries) const
{
    QVector<int> rows;
    if (entries <= 0)
        return rows;

    // Entries are sorted by hash and then by row, so equal hashes form a run.
    // The search has to land on the LEFTMOST of that run — stopping at any
    // member and scanning forward would miss every earlier row sharing the key.
    int lo = 0;
    int hi = entries - 1;
    int found = -1;
    while (lo <= hi) {
        const int mid = lo + (hi - lo) / 2;
        const quint64 h = le64(m_keys.data, offset + qint64(mid) * kKeyEntryBytes);
        if (h == hash) {
            found = mid;
            hi = mid - 1;
        } else if (h < hash) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    if (found < 0)
        return rows;

    for (int i = found; i < entries; ++i) {
        const qint64 at = offset + qint64(i) * kKeyEntryBytes;
        if (le64(m_keys.data, at) != hash)
            break;
        const quint32 row = le32(m_keys.data, at + 8);
        if (row < quint32(m_count))
            rows.append(int(row));
    }
    return rows;
}

int Catalog::bestRow(const QVector<int> &rows) const
{
    if (rows.isEmpty())
        return -1;
    // A genuine collision is two different acts with the same name; the more
    // popular one is the better guess, and ties keep the first row so the
    // answer does not move between runs.
    int best = rows.first();
    int bestPop = popularity(best);
    for (int i = 1; i < rows.size(); ++i) {
        const int pop = popularity(rows.at(i));
        if (pop > bestPop) {
            best = rows.at(i);
            bestPop = pop;
        }
    }
    return best;
}

// -------------------------------------------------------------------- matching

Catalog::Match Catalog::match(const QString &title, const QString &artist) const
{
    Match out;
    if (!m_loaded)
        return out;

    // A title and an artist that both normalise away hash to FNV1a64("|"), and
    // the catalogue holds five rows under exactly that key: novelty titles
    // written entirely in punctuation. Without this guard a track called "???"
    // by nobody resolves to one of them at confidence 1.0 — the tier callers
    // trust absolutely — and its vector is then folded into the taste profile
    // at full weight.
    //
    // Spec section 4 hands this decision to the port ("decide a policy before
    // shipping"); the iOS original has no guard. The policy here is to refuse:
    // a key with nothing on either side of the bar identifies nothing, and a
    // confident wrong answer is worse than none. The lower tiers are
    // unaffected, and artistsAgree already refuses two empty names.
    if (Rec::titleCore(title).isEmpty() && Rec::primaryArtist(artist).isEmpty())
        return out;

    const int strict = bestRow(keyRows(Rec::strictKey(title, artist), m_k1Offset, m_k1Count));
    if (strict >= 0) {
        out.row = strict;
        out.artistId = artistId(strict);
        out.confidence = 1.0f;
        return out;
    }

    // K2 holds only titles that are unique across the whole catalogue, so a hit
    // is already meaningful; the artist check is the last guard against the two
    // unrelated songs that happen to share a distinctive name.
    const int byTitle = bestRow(keyRows(Rec::titleKey(title), m_k2Offset, m_k2Count));
    if (byTitle >= 0 && Rec::artistsAgree(artistName(artistId(byTitle)), artist)) {
        out.row = byTitle;
        out.artistId = artistId(byTitle);
        out.confidence = 0.75f;
        return out;
    }

    const QString key = Rec::primaryArtist(artist);
    if (!key.isEmpty()) {
        QMutexLocker lock(&m_artistMutex);
        ensureArtistIndex();
        const auto it = m_keyLead.constFind(key);
        if (it != m_keyLead.constEnd()) {
            out.artistId = it.value();
            out.confidence = 0.45f;
        }
    }
    return out;
}

void Catalog::ensureArtistIndex() const
{
    if (m_artistIndexBuilt)
        return;
    m_artistIndexBuilt = true;

    m_artistKey.resize(m_artistCount);
    for (int id = 0; id < m_artistCount; ++id)
        m_artistKey[id] = Rec::primaryArtist(artistName(id));

    m_keyRows.reserve(m_artistCount);
    m_keyLead.reserve(m_artistCount);
    for (int row = 0; row < m_count; ++row) {
        const int id = artistId(row);
        if (id < 0)
            continue;
        const QString &key = m_artistKey.at(id);
        if (key.isEmpty())   // 154 rows have a name that normalises away
            continue;
        QVector<int> &rows = m_keyRows[key];
        if (rows.isEmpty())
            m_keyLead.insert(key, id);
        if (rows.size() < kCentroidRows)
            rows.append(row);
    }
}

QVector<float> Catalog::artistCentroid(int artistId) const
{
    if (!m_loaded || artistId < 0 || artistId >= m_artistCount)
        return QVector<float>();

    QVector<int> rows;
    {
        QMutexLocker lock(&m_artistMutex);
        ensureArtistIndex();
        const QString key = m_artistKey.value(artistId);
        if (key.isEmpty())
            return QVector<float>();
        rows = m_keyRows.value(key);
    }
    if (rows.isEmpty())
        return QVector<float>();

    QVector<float> sum(m_dims, 0.0f);
    QVector<float> row(m_dims, 0.0f);
    for (const int r : rows) {
        if (!loadVector(r, row.data()))
            continue;
        for (int i = 0; i < m_dims; ++i)
            sum[i] += row.at(i);
    }

    double norm = 0.0;
    for (const float x : sum)
        norm += double(x) * double(x);
    norm = std::sqrt(norm);
    // Rows are unit vectors, so this only fails when an artist's rows cancel out
    // exactly — possible in principle, never worth returning a direction for.
    if (!(norm > 1e-6))
        return QVector<float>();
    for (float &x : sum)
        x = float(double(x) / norm);
    return sum;
}

QVector<int> Catalog::artistRows(int artistId, int limit) const
{
    if (!m_loaded || artistId < 0 || artistId >= m_artistCount || limit <= 0)
        return {};

    QVector<int> rows;
    {
        QMutexLocker lock(&m_artistMutex);
        ensureArtistIndex();
        const QString key = m_artistKey.value(artistId);
        if (key.isEmpty())
            return {};
        rows = m_keyRows.value(key);
    }
    // Row order happens to run from most to least popular in the shipped build,
    // but the spec is explicit that nothing guarantees it across rebuilds, so
    // the order is made rather than assumed. Stable, so ties keep row order.
    std::stable_sort(rows.begin(), rows.end(), [this](int a, int b) {
        return popularity(a) > popularity(b);
    });
    if (rows.size() > limit)
        rows.resize(limit);
    return rows;
}

} // namespace Rec
