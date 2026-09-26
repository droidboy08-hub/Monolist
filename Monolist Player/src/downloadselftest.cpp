#include "downloadselftest.h"

#include "downloadmanager.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QTemporaryDir>

namespace {

// One line per check, and a count at the end, as in connectionselftest.cpp.
// Descriptions stay ASCII: the console these are read in is not always UTF-8.
class Checks
{
public:
    explicit Checks(const char *prefix) : m_prefix(prefix) {}

    bool check(bool ok, const QString &what, const QString &detail = QString())
    {
        ++m_count;
        if (!ok)
            ++m_failed;
        const QString line = ok || detail.isEmpty() ? what : what + QStringLiteral("  -- ") + detail;
        qWarning("%s: %s  %s", m_prefix, ok ? "ok  " : "FAIL", qPrintable(line));
        return ok;
    }

    void note(const QString &text) { qWarning("%s: %s", m_prefix, qPrintable(text)); }

    int finish()
    {
        qWarning("%s: %d checks, %d failed", m_prefix, m_count, m_failed);
        return m_failed;
    }

private:
    const char *m_prefix;
    int m_count = 0;
    int m_failed = 0;
};

// An environment variable put back as it was, set or not, when the scope ends.
class SavedVariable
{
public:
    explicit SavedVariable(const char *name)
        : m_name(name), m_set(qEnvironmentVariableIsSet(name)), m_value(qgetenv(name)) {}
    ~SavedVariable()
    {
        if (m_set)
            qputenv(m_name, m_value);
        else
            qunsetenv(m_name);
    }
    SavedVariable(const SavedVariable &) = delete;
    SavedVariable &operator=(const SavedVariable &) = delete;

private:
    const char *m_name;
    bool m_set;
    QByteArray m_value;
};

// An invented file. Its content does not matter, only that it is not empty,
// as a finished download never is.
bool make(const QDir &dir, const QString &name)
{
    QFile file(dir.filePath(name));
    return file.open(QIODevice::WriteOnly) && file.write(QByteArray(4096, 'x')) == 4096;
}

bool samePath(const QString &a, const QString &b)
{
    return QDir::cleanPath(QDir::fromNativeSeparators(a)).compare(
               QDir::cleanPath(QDir::fromNativeSeparators(b)), Qt::CaseInsensitive) == 0;
}

struct Leftover {
    QString name;
    bool goes;          // deleted by the cleanup
    const char *what;
};

} // namespace

int runDownloadCleanupSelfTest()
{
    Checks t("cleanup");
    const QString data = qEnvironmentVariable("MONOLIST_DATA_DIR");
    if (data.isEmpty()) {
        t.check(false, QStringLiteral("MONOLIST_DATA_DIR is set"),
                QStringLiteral("refusing to run: this test opens the downloads on the database there"));
        return t.finish();
    }
    // Inside the data folder, and gone again when the test ends.
    QTemporaryDir scratch(QDir(data).filePath(QStringLiteral("cleanup-test-XXXXXX")));
    if (!scratch.isValid()) {
        t.check(false, QStringLiteral("a scratch folder"), scratch.errorString());
        return t.finish();
    }
    const QDir dir(scratch.path());
    t.note(QStringLiteral("scratch folder %1").arg(QDir::toNativeSeparators(dir.path())));

    // — where downloads go —
    {
        SavedVariable keepNamed("MONOLIST_DOWNLOAD_DIR");
        qunsetenv("MONOLIST_DOWNLOAD_DIR");
        {
            // What every test run gets: a scratch database, nothing else set.
            DownloadManager manager;
            const QString expected = QDir(data).absoluteFilePath(QStringLiteral("downloads"));
            t.check(samePath(manager.downloadDirectory(), expected) && QFileInfo(expected).isDir(),
                    QStringLiteral("MONOLIST_DATA_DIR alone: downloads go to \"downloads\" inside it"),
                    manager.downloadDirectory());
        }

        const QString named = dir.filePath(QStringLiteral("named folder"));
        qputenv("MONOLIST_DOWNLOAD_DIR", QFile::encodeName(QDir::toNativeSeparators(named)));
        {
            DownloadManager manager;
            t.check(samePath(manager.downloadDirectory(), named) && QFileInfo(named).isDir(),
                    QStringLiteral("MONOLIST_DOWNLOAD_DIR: downloads go there, made if missing"),
                    manager.downloadDirectory());
        }
        qunsetenv("MONOLIST_DOWNLOAD_DIR");

        // Named only: nothing is made or written there.
        SavedVariable keepData("MONOLIST_DATA_DIR");
        qunsetenv("MONOLIST_DATA_DIR");
        const QString real = DownloadManager::chooseDirectory();
        t.check(QFileInfo(real).fileName() == QLatin1String("Monolist")
                    && !real.startsWith(QDir::cleanPath(QDir(data).absolutePath()), Qt::CaseInsensitive),
                QStringLiteral("neither set: <Music>/Monolist, the user's own folder"), real);
    }

    // — what a failed or cancelled download may delete —
    //
    // One track, "Selftest001", downloaded again after the database lost
    // track of the first download: its finished files are in the folder
    // already, beside another track's. The attempt writes yt-dlp's usual
    // working files, then fails.
    const QString id = QStringLiteral("Selftest001");   // eleven characters, like a video id
    const QString song = QStringLiteral("Selftest Artist - New Song [Selftest001]");
    const QString old = QStringLiteral("An Old Title [Selftest001]");

    const QList<Leftover> before = {
        { song + QStringLiteral(".m4a"), false, "finished earlier under the same name, the database knowing nothing of it" },
        { old + QStringLiteral(".opus"), false, "finished earlier under an older title" },
        { old + QStringLiteral(".mp3"), false, "finished earlier, as MP3" },
        { old + QStringLiteral(".webm"), false, "finished earlier, audio as YouTube serves it" },
        { old + QStringLiteral(".orig.m4a"), false, "the original yt-dlp sets aside while converting" },
        { old + QStringLiteral(".webm.part"), true, "a partial download from a run that crashed" },
        { old + QStringLiteral(".webp"), true, "a thumbnail left behind" },
        { QStringLiteral("Another Song [Selftest002].opus"), false, "another track's download" },
        { QStringLiteral("Another Song [Selftest002].webm.part"), false, "another track's download, under way" },
        { QStringLiteral("Shouting [SELFTEST001].opus"), false, "another video: ids are case-sensitive" },
        { QStringLiteral("Selftest001 notes.txt"), false, "the id without its brackets" },
        { QStringLiteral("Unrelated.mp3"), false, "not a download" },
    };
    const QList<Leftover> during = {
        { song + QStringLiteral(".webm.part"), true, "the download under way" },
        { song + QStringLiteral(".webm.ytdl"), true, "yt-dlp's record for resuming" },
        { song + QStringLiteral(".f251.webm"), true, "one stream of a merge" },
        { song + QStringLiteral(".f140.m4a.part-Frag7"), true, "a fragment" },
        { song + QStringLiteral(".temp.m4a"), true, "FFmpeg's working copy" },
        { song + QStringLiteral(".meta"), true, "the chapter list for FFmpeg" },
        { song + QStringLiteral(".webp"), true, "the thumbnail as fetched" },
        { song + QStringLiteral(".jpg"), true, "the thumbnail converted to embed" },
        { song + QStringLiteral(".webm"), true, "this attempt's download, not yet converted" },
        { song + QStringLiteral(".mp3"), true, "FFmpeg's output, cut off when the attempt stopped" },
        { QStringLiteral("Held By The Database [Selftest001].m4a"), false, "the file the database holds" },
        { QStringLiteral("Another Song [Selftest002].f251.webm"), false, "another track's, written meanwhile" },
    };

    bool made = true;
    for (const Leftover &file : before)
        made = make(dir, file.name) && made;
    const QSet<QString> seen = DownloadManager::filesFor(dir.path(), id);
    for (const Leftover &file : during)
        made = make(dir, file.name) && made;
    t.check(made, QStringLiteral("%1 invented files written").arg(before.size() + during.size()));
    t.check(seen.size() == 7 && seen.contains(song + QStringLiteral(".m4a"))
                && seen.contains(old + QStringLiteral(".webm.part"))
                && !seen.contains(QStringLiteral("Shouting [SELFTEST001].opus")),
            QStringLiteral("at the start, the seven files of this track that are already there are noted"),
            QStringList(seen.values()).join(QStringLiteral(", ")));

    // As yt-dlp reports a path on Windows, and as the database stores it.
    const QString kept = QDir::toNativeSeparators(dir.filePath(QStringLiteral("Held By The Database [Selftest001].m4a")));
    const QStringList removed = DownloadManager::removeLeftovers(dir.path(), id, seen, kept);

    int expected = 0;
    for (const QList<Leftover> *list : { &before, &during }) {
        const bool earlier = list == &before;
        for (const Leftover &file : *list) {
            expected += file.goes;
            const bool there = dir.exists(file.name);
            t.check(there != file.goes && removed.contains(file.name) == file.goes,
                    QStringLiteral("%1 %2: %3%4").arg(file.goes ? QStringLiteral("removed") : QStringLiteral("kept   "),
                                                      file.name, QString::fromLatin1(file.what),
                                                      earlier ? QStringLiteral(" (there before)") : QString()),
                    there ? QStringLiteral("still there") : QStringLiteral("gone"));
        }
    }
    t.check(removed.size() == expected,
            QStringLiteral("%1 removed in all, and nothing else").arg(expected),
            removed.join(QStringLiteral(", ")));

    // With no record of the start, which a download that ran always has:
    // everything counts as having been there, and only yt-dlp's own
    // leftovers go.
    {
        const QString again = QStringLiteral("Selftest Artist - Retry [Selftest003]");
        const bool ok = make(dir, again + QStringLiteral(".opus")) && make(dir, again + QStringLiteral(".opus.part"))
                        && make(dir, again + QStringLiteral(".temp.opus"));
        const QStringList gone = DownloadManager::removeLeftovers(
            dir.path(), QStringLiteral("Selftest003"),
            DownloadManager::filesFor(dir.path(), QStringLiteral("Selftest003")), QString());
        t.check(ok && dir.exists(again + QStringLiteral(".opus")) && !dir.exists(again + QStringLiteral(".opus.part"))
                    && !dir.exists(again + QStringLiteral(".temp.opus")) && gone.size() == 2,
                QStringLiteral("no record of the start: the finished .opus stays, .part and .temp go"),
                gone.join(QStringLiteral(", ")));
    }

    return t.finish();
}
