#include "oggselftest.h"

#include "finder.h"
#include "oggtags.h"

// AssertMacros.h, which CoreServices brings in, would define check().
#define __ASSERT_MACROS_DEFINE_VERSIONS_WITHOUT_UNDERSCORES 0
#include <CoreFoundation/CFPlugInCOM.h>
#include <CoreServices/CoreServices.h>

#include <QBuffer>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QList>
#include <QPair>

#include <cmath>

namespace {

// One line per check, and a count at the end, as in the other self-tests.
class Checks
{
public:
    bool check(bool ok, const QString &what, const QString &detail = QString())
    {
        ++m_count;
        if (!ok)
            ++m_failed;
        const QString line = ok || detail.isEmpty() ? what : what + QStringLiteral("  -- ") + detail;
        qWarning("ogg-test: %s  %s", ok ? "ok  " : "FAIL", qPrintable(line));
        return ok;
    }

    int finish()
    {
        qWarning("ogg-test: %d checks, %d failed", m_count, m_failed);
        return m_failed;
    }

private:
    int m_count = 0;
    int m_failed = 0;
};

void putLe32(QByteArray &out, quint32 v)
{
    for (int i = 0; i < 4; ++i)
        out.append(char((v >> (8 * i)) & 0xff));
}

void putBe32(QByteArray &out, quint32 v)
{
    for (int i = 3; i >= 0; --i)
        out.append(char((v >> (8 * i)) & 0xff));
}

quint32 oggCrc(const QByteArray &data)
{
    static quint32 table[256];
    if (!table[1]) {
        for (quint32 i = 0; i < 256; ++i) {
            quint32 r = i << 24;
            for (int k = 0; k < 8; ++k)
                r = (r & 0x80000000u) ? (r << 1) ^ 0x04c11db7u : r << 1;
            table[i] = r;
        }
    }
    quint32 crc = 0;
    for (char c : data)
        crc = (crc << 8) ^ table[((crc >> 24) ^ quint8(c)) & 0xff];
    return crc;
}

// An Ogg stream written a packet at a time, each packet starting a page of
// its own and running over as many as it needs, as Ogg lays them out.
class OggWriter
{
public:
    explicit OggWriter(quint32 serial) : m_serial(serial) {}

    // granule: the position at the end of the packet; the pages before its
    // last say -1 (no packet ends there).
    void packet(QByteArray &file, const QByteArray &data, qint64 granule, bool first = false, bool last = false)
    {
        QList<quint8> lacing;
        qsizetype left = data.size();
        do {
            const quint8 l = quint8(std::min<qsizetype>(left, 255));
            lacing.append(l);
            left -= l;
            if (l < 255)
                break;
        } while (true);
        qsizetype offset = 0;
        for (qsizetype start = 0; start < lacing.size(); start += 255) {
            const qsizetype count = std::min<qsizetype>(lacing.size() - start, 255);
            const bool lastPage = start + count == lacing.size();
            QByteArray page("OggS");
            page.append(char(0));
            quint8 flags = 0;
            if (start > 0)
                flags |= 1;
            if (first && start == 0)
                flags |= 2;
            if (last && lastPage)
                flags |= 4;
            page.append(char(flags));
            const quint64 g = lastPage ? quint64(granule) : ~quint64(0);
            putLe32(page, quint32(g & 0xffffffffu));
            putLe32(page, quint32(g >> 32));
            putLe32(page, m_serial);
            putLe32(page, m_sequence++);
            putLe32(page, 0);   // the checksum, filled in below
            page.append(char(count));
            qsizetype bodySize = 0;
            for (qsizetype i = start; i < start + count; ++i) {
                page.append(char(lacing[i]));
                bodySize += lacing[i];
            }
            page.append(data.mid(offset, bodySize));
            offset += bodySize;
            const quint32 crc = oggCrc(page);
            for (int i = 0; i < 4; ++i)
                page[22 + i] = char((crc >> (8 * i)) & 0xff);
            file.append(page);
        }
    }

private:
    quint32 m_serial;
    quint32 m_sequence = 0;
};

QByteArray opusHead(int channels, int preSkip, quint32 rate)
{
    QByteArray head("OpusHead");
    head.append(char(1));
    head.append(char(channels));
    head.append(char(preSkip & 0xff));
    head.append(char(preSkip >> 8));
    putLe32(head, rate);
    head.append(2, char(0));   // output gain
    head.append(char(0));      // mapping family
    return head;
}

QByteArray comments(const QByteArray &magic, const QList<QByteArray> &list, bool framing)
{
    QByteArray out(magic);
    const QByteArray vendor("ogg-test");
    putLe32(out, quint32(vendor.size()));
    out.append(vendor);
    putLe32(out, quint32(list.size()));
    for (const QByteArray &comment : list) {
        putLe32(out, quint32(comment.size()));
        out.append(comment);
    }
    if (framing)
        out.append(char(1));
    return out;
}

QByteArray pictureComment(quint32 type, const QByteArray &mime, const QByteArray &image)
{
    QByteArray block;
    putBe32(block, type);
    putBe32(block, quint32(mime.size()));
    block.append(mime);
    const QByteArray description("cover");
    putBe32(block, quint32(description.size()));
    block.append(description);
    putBe32(block, 64);
    putBe32(block, 48);
    putBe32(block, 24);
    putBe32(block, 0);
    putBe32(block, quint32(image.size()));
    block.append(image);
    return "METADATA_BLOCK_PICTURE=" + block.toBase64();
}

QByteArray png(int width, int height, QRgb colour)
{
    QImage image(width, height, QImage::Format_RGB32);
    image.fill(colour);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return bytes;
}

bool writeFile(const QString &path, const QByteArray &data)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(data) == data.size();
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

QString value(const OggTags &tags, const char *name, size_t index = 0)
{
    const char *v = ogg_tags_value(&tags, name, index);
    return v ? QString::fromUtf8(v) : QString();
}

// Reads a file as Spotlight would, flags and all, into tags; true when read.
bool read(const QString &path, unsigned flags, OggTags *tags)
{
    return ogg_tags_read(QFile::encodeName(path).constData(), flags, tags) == 0;
}

// The app's own Spotlight importer, loaded as Spotlight loads it, run on
// path; its attributes, or an empty map when it refused the file.
QVariantMap importWithSpotlightImporter(const QString &bundlePath, const QString &path, bool *loaded)
{
    QVariantMap result;
    *loaded = false;
    CFStringRef bundle = bundlePath.toCFString();
    CFURLRef url = CFURLCreateWithFileSystemPath(kCFAllocatorDefault, bundle, kCFURLPOSIXPathStyle, true);
    CFRelease(bundle);
    CFPlugInRef plugin = CFPlugInCreate(kCFAllocatorDefault, url);
    CFRelease(url);
    if (!plugin)
        return result;
    CFArrayRef factories = CFPlugInFindFactoriesForPlugInTypeInPlugIn(kMDImporterTypeID, plugin);
    if (factories && CFArrayGetCount(factories) > 0) {
        auto factory = static_cast<CFUUIDRef>(CFArrayGetValueAtIndex(factories, 0));
        auto **unknown = static_cast<IUnknownVTbl **>(CFPlugInInstanceCreate(kCFAllocatorDefault, factory,
                                                                             kMDImporterTypeID));
        if (unknown) {
            MDImporterInterfaceStruct **importer = nullptr;
            (*unknown)->QueryInterface(unknown, CFUUIDGetUUIDBytes(kMDImporterInterfaceID),
                                       reinterpret_cast<LPVOID *>(&importer));
            (*unknown)->Release(unknown);
            if (importer) {
                *loaded = true;
                CFMutableDictionaryRef attributes = CFDictionaryCreateMutable(
                    kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
                CFStringRef file = path.toCFString();
                if ((*importer)->ImporterImportData(importer, attributes, CFSTR("org.xiph.ogg-audio"), file)) {
                    const CFIndex count = CFDictionaryGetCount(attributes);
                    QList<const void *> keys(count), values(count);
                    CFDictionaryGetKeysAndValues(attributes, keys.data(), values.data());
                    for (CFIndex i = 0; i < count; ++i) {
                        const QString key = QString::fromCFString(static_cast<CFStringRef>(keys[i]));
                        const CFTypeRef v = values[i];
                        if (CFGetTypeID(v) == CFStringGetTypeID()) {
                            result.insert(key, QString::fromCFString(static_cast<CFStringRef>(v)));
                        } else if (CFGetTypeID(v) == CFNumberGetTypeID()) {
                            double number = 0;
                            CFNumberGetValue(static_cast<CFNumberRef>(v), kCFNumberDoubleType, &number);
                            result.insert(key, number);
                        } else if (CFGetTypeID(v) == CFArrayGetTypeID()) {
                            QStringList list;
                            const auto array = static_cast<CFArrayRef>(v);
                            for (CFIndex k = 0; k < CFArrayGetCount(array); ++k)
                                list.append(QString::fromCFString(
                                    static_cast<CFStringRef>(CFArrayGetValueAtIndex(array, k))));
                            result.insert(key, list);
                        }
                    }
                }
                CFRelease(file);
                CFRelease(attributes);
                (*importer)->Release(importer);
            }
        }
    }
    if (factories)
        CFRelease(factories);
    CFRelease(plugin);
    return result;
}

} // namespace

int runOggSelfTest()
{
    Checks t;
    const QString data = qEnvironmentVariable("MONOLIST_DATA_DIR");
    if (data.isEmpty()) {
        t.check(false, QStringLiteral("MONOLIST_DATA_DIR is set"),
                QStringLiteral("the test writes files and sets Finder icons; give it a scratch folder"));
        return t.finish();
    }
    QDir dir(data);
    dir.mkpath(QStringLiteral("ogg-test"));
    dir.cd(QStringLiteral("ogg-test"));
    const auto path = [&](const QString &name) { return dir.filePath(name); };

    const QByteArray cover = png(64, 48, qRgb(200, 30, 30));
    const QByteArray icon = png(32, 32, qRgb(0, 0, 200));

    // An Opus song: two artists, a comment header over three pages (a long
    // description), an icon before the front cover, ten seconds after a
    // pre-skip of 312 samples.
    QByteArray opus;
    {
        OggWriter w(0x1234);
        w.packet(opus, opusHead(2, 312, 44100), 0, true);
        w.packet(opus, comments("OpusTags",
                                { "TITLE=Monkeys Spinning Monkeys", "ARTIST=Kevin MacLeod", "artist=Second Artist",
                                  "album=Comedy Collection", "DATE=20151111", "TRACKNUMBER=3/12",
                                  "DESCRIPTION=" + QByteArray(140000, 'x'), pictureComment(1, "image/png", icon),
                                  pictureComment(3, "image/png", cover), "GENRE=Soundtrack" },
                                false),
                 0);
        w.packet(opus, QByteArray(4000, '\x55'), 48000 * 5 + 312);
        w.packet(opus, QByteArray(4000, '\x66'), 48000 * 10 + 312, false, true);
    }
    const QString opusPath = path(QStringLiteral("song.opus"));
    t.check(writeFile(opusPath, opus), QStringLiteral("an Opus file written"));

    {
        OggTags tags;
        const bool ok = read(opusPath, OGG_TAGS_PICTURE | OGG_TAGS_DURATION, &tags);
        t.check(ok && tags.codec == OGG_TAGS_OPUS, QStringLiteral("Opus: read, and taken for Opus"));
        t.check(tags.channels == 2 && tags.sample_rate == 44100, QStringLiteral("Opus: two channels, the source's rate"),
                QStringLiteral("%1 channels, %2 Hz").arg(tags.channels).arg(tags.sample_rate));
        t.check(value(tags, "TITLE") == QStringLiteral("Monkeys Spinning Monkeys"), QStringLiteral("Opus: the title"));
        t.check(value(tags, "ARTIST", 0) == QStringLiteral("Kevin MacLeod")
                    && value(tags, "ARTIST", 1) == QStringLiteral("Second Artist") && value(tags, "ARTIST", 2).isEmpty(),
                QStringLiteral("Opus: both artists, names compared without case"));
        t.check(value(tags, "Album") == QStringLiteral("Comedy Collection"), QStringLiteral("Opus: the album"));
        t.check(value(tags, "GENRE") == QStringLiteral("Soundtrack"),
                QStringLiteral("Opus: a comment after the pictures and the long one"));
        t.check(value(tags, "DESCRIPTION").size() == 140000, QStringLiteral("Opus: the header over three pages, whole"));
        t.check(value(tags, "METADATA_BLOCK_PICTURE").isEmpty(), QStringLiteral("Opus: pictures are not kept as comments"));
        t.check(std::abs(tags.duration - 10.0) < 1e-9, QStringLiteral("Opus: ten seconds, the pre-skip left out"),
                QString::number(tags.duration, 'f', 6));
        t.check(tags.picture && QByteArray(reinterpret_cast<const char *>(tags.picture), qsizetype(tags.picture_size)) == cover
                    && QByteArray(tags.picture_mime) == "image/png",
                QStringLiteral("Opus: the front cover, not the icon before it"));
        t.check(tags.header_size > 140000 && tags.header_size < opus.size() - 8000,
                QStringLiteral("Opus: where the headers end"), QString::number(tags.header_size));
        ogg_tags_free(&tags);

        const bool plain = read(opusPath, 0, &tags);
        t.check(plain && !tags.picture && tags.duration == 0 && value(tags, "TITLE").size() > 0,
                QStringLiteral("Opus: no picture or length unless asked for"));
        ogg_tags_free(&tags);
    }

    // A Vorbis song, after a stream of another kind (as a video's would be),
    // its pages between the song's, and that stream's last page the file's.
    QByteArray vorbis;
    {
        OggWriter other(0x77), w(0x99);
        other.packet(vorbis, QByteArray("\x80theora") + QByteArray(40, '\0'), 0, true);
        QByteArray id("\x01vorbis", 7);
        putLe32(id, 0);
        id.append(char(1));
        putLe32(id, 44100);
        putLe32(id, 0);
        putLe32(id, 128000);
        putLe32(id, 0);
        id.append(char(0xb8));
        id.append(char(1));
        w.packet(vorbis, id, 0, true);
        other.packet(vorbis, QByteArray(100, 't'), 0);
        w.packet(vorbis, comments(QByteArray("\x03vorbis", 7), { "TITLE=Vörbis", "ARTIST=A" }, true), 0);
        w.packet(vorbis, QByteArray("\x05vorbis", 7) + QByteArray(50, 's'), 0);
        w.packet(vorbis, QByteArray(3000, 'a'), 44100 * 3, false, true);
        other.packet(vorbis, QByteArray(3000, 'v'), 900000, false, true);
    }
    const QString vorbisPath = path(QStringLiteral("song.ogg"));
    writeFile(vorbisPath, vorbis);
    {
        OggTags tags;
        const bool ok = read(vorbisPath, OGG_TAGS_PICTURE | OGG_TAGS_DURATION, &tags);
        t.check(ok && tags.codec == OGG_TAGS_VORBIS && tags.sample_rate == 44100 && tags.channels == 1,
                QStringLiteral("Vorbis: read beside another stream"));
        t.check(value(tags, "TITLE") == QStringLiteral("Vörbis"), QStringLiteral("Vorbis: the title, UTF-8"));
        t.check(std::abs(tags.duration - 3.0) < 1e-9, QStringLiteral("Vorbis: the length from its own last page"),
                QString::number(tags.duration, 'f', 6));
        t.check(!tags.picture, QStringLiteral("Vorbis: no cover where there is none"));
        ogg_tags_free(&tags);
    }

    // Files that are not, or no longer, what they say.
    {
        OggTags tags;
        const auto refused = [&](const QString &name, const QByteArray &bytes) {
            writeFile(path(name), bytes);
            const bool ok = read(path(name), OGG_TAGS_PICTURE | OGG_TAGS_DURATION, &tags);
            ogg_tags_free(&tags);
            return !ok;
        };
        t.check(refused(QStringLiteral("empty.opus"), QByteArray()), QStringLiteral("an empty file is refused"));
        t.check(refused(QStringLiteral("m4a.opus"), QByteArray("\0\0\0\x20" "ftypM4A ", 12) + QByteArray(2000, 'm')),
                QStringLiteral("an M4A named .opus is refused"));
        t.check(refused(QStringLiteral("cut.opus"), opus.left(60000)), QStringLiteral("a file cut inside its tags is refused"));
        QByteArray damaged = opus;
        damaged[200] = char(damaged[200] ^ 0x40);   // inside the comment header's first page
        t.check(refused(QStringLiteral("damaged.opus"), damaged),
                QStringLiteral("a page whose checksum is wrong is not read"));

        // The tags whole but the audio's end cut off: no length, no harm.
        writeFile(path(QStringLiteral("short.opus")), opus.left(opus.size() - 100));
        const bool ok = read(path(QStringLiteral("short.opus")), OGG_TAGS_DURATION, &tags);
        t.check(ok && value(tags, "TITLE").size() > 0 && std::abs(tags.duration - 5.0) < 1e-9,
                QStringLiteral("a cut file's length is its last whole page's"), QString::number(tags.duration));
        ogg_tags_free(&tags);
    }
    {
        // Lengths that point past their end: a picture that claims more than
        // it holds, a count of comments larger than the list.
        QByteArray bad;
        QByteArray block;
        putBe32(block, 3);
        putBe32(block, 9);
        block.append("image/png");
        putBe32(block, 0);
        putBe32(block, 1);
        putBe32(block, 1);
        putBe32(block, 24);
        putBe32(block, 0);
        putBe32(block, 1000000);
        block.append(cover);
        QByteArray tagsPacket = comments("OpusTags", { "TITLE=T", "METADATA_BLOCK_PICTURE=" + block.toBase64(),
                                                       pictureComment(3, "-->", "https://example.com/cover.png") },
                                         false);
        putLe32(tagsPacket, 5);   // one more comment, said to be five bytes, cut off
        tagsPacket[8 + 4 + 8] = char(9);   // the count: nine comments, three there
        OggWriter w(5);
        w.packet(bad, opusHead(1, 0, 48000), 0, true);
        w.packet(bad, tagsPacket, 0);
        w.packet(bad, QByteArray(100, 'a'), 48000, false, true);
        writeFile(path(QStringLiteral("lengths.opus")), bad);
        OggTags tags;
        const bool ok = read(path(QStringLiteral("lengths.opus")), OGG_TAGS_PICTURE | OGG_TAGS_DURATION, &tags);
        t.check(ok && value(tags, "TITLE") == QStringLiteral("T") && !tags.picture,
                QStringLiteral("lengths past the end: what is whole is kept, the rest passed over"));
        ogg_tags_free(&tags);
    }

    // The app's Spotlight importer, as Spotlight runs it.
    const QString importer = QDir(QCoreApplication::applicationDirPath())
                                 .absoluteFilePath(QStringLiteral("../Library/Spotlight/MonolistOgg.mdimporter"));
    bool loaded = false;
    const QVariantMap imported = importWithSpotlightImporter(importer, opusPath, &loaded);
    t.check(loaded, QStringLiteral("the app's Spotlight importer loads"), importer);
    t.check(imported.value(QStringLiteral("kMDItemTitle")).toString() == QStringLiteral("Monkeys Spinning Monkeys")
                && imported.value(QStringLiteral("kMDItemDisplayName")).toString()
                       == QStringLiteral("Monkeys Spinning Monkeys"),
            QStringLiteral("Spotlight: the title, which it also shows"));
    t.check(imported.value(QStringLiteral("kMDItemAuthors")).toStringList()
                == QStringList { QStringLiteral("Kevin MacLeod"), QStringLiteral("Second Artist") },
            QStringLiteral("Spotlight: the artists, as authors"),
            imported.value(QStringLiteral("kMDItemAuthors")).toStringList().join(QStringLiteral(", ")));
    t.check(imported.value(QStringLiteral("kMDItemAlbum")).toString() == QStringLiteral("Comedy Collection"),
            QStringLiteral("Spotlight: the album"));
    t.check(std::abs(imported.value(QStringLiteral("kMDItemDurationSeconds")).toDouble() - 10.0) < 1e-9,
            QStringLiteral("Spotlight: the length"));
    t.check(imported.value(QStringLiteral("kMDItemAudioTrackNumber")).toInt() == 3
                && imported.value(QStringLiteral("kMDItemRecordingYear")).toInt() == 2015
                && imported.value(QStringLiteral("kMDItemMusicalGenre")).toString() == QStringLiteral("Soundtrack"),
            QStringLiteral("Spotlight: track, year and genre"));
    t.check(imported.value(QStringLiteral("kMDItemCodecs")).toStringList() == QStringList { QStringLiteral("Opus") }
                && imported.value(QStringLiteral("kMDItemAudioSampleRate")).toDouble() == 48000
                && imported.value(QStringLiteral("kMDItemAudioChannelCount")).toInt() == 2,
            QStringLiteral("Spotlight: Opus, at 48 kHz, in stereo"));
    bool loadedAgain = false;
    t.check(importWithSpotlightImporter(importer, path(QStringLiteral("m4a.opus")), &loadedAgain).isEmpty() && loadedAgain,
            QStringLiteral("Spotlight: a file that is not Ogg is refused"));

    // The cover as the file's Finder icon, the audio untouched.
    const QByteArray before = readFile(opusPath);
    QElapsedTimer timer;
    timer.start();
    const bool set = MacFinder::setCover(opusPath);
    const qint64 took = timer.elapsed();
    t.check(set && MacFinder::hasCustomIcon(opusPath), QStringLiteral("the cover is the Opus file's Finder icon"),
            QStringLiteral("%1 ms").arg(took));
    t.check(readFile(opusPath) == before, QStringLiteral("the audio is left exactly as it was"));
    t.check(!MacFinder::setCover(vorbisPath) && !MacFinder::hasCustomIcon(vorbisPath),
            QStringLiteral("a file with no cover keeps its icon"));
    const QString m4a = path(QStringLiteral("song.m4a"));
    writeFile(m4a, opus);
    t.check(!MacFinder::setCover(m4a) && !MacFinder::hasCustomIcon(m4a),
            QStringLiteral("only Ogg files are given one (the Finder reads an M4A's itself)"));
    qWarning("ogg-test: setting the icon took %lld ms", took);

    return t.finish();
}
