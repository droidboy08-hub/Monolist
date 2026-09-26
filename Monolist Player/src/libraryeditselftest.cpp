#include "libraryeditselftest.h"

#include "appdatabase.h"
#include "library.h"
#include "queuemodel.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QMimeData>
#include <QSqlQuery>
#include <QStringList>

namespace {

// One line per check, and a count at the end, as in the other self-tests.
// Descriptions stay ASCII: the console these are read in is not always UTF-8.
class Checks
{
public:
    bool check(bool ok, const QString &what, const QString &detail = QString())
    {
        ++m_count;
        if (!ok)
            ++m_failed;
        const QString line = ok || detail.isEmpty() ? what : what + QStringLiteral("  -- ") + detail;
        qWarning("library-edit-test: %s  %s", ok ? "ok  " : "FAIL", qPrintable(line));
        return ok;
    }

    // Equal, and when not, both sides in the line.
    bool same(const QString &got, const QString &want, const QString &what)
    {
        return check(got == want, what, QStringLiteral("got \"%1\", want \"%2\"").arg(got, want));
    }

    void note(const QString &text) { qWarning("library-edit-test: %s", qPrintable(text)); }

    int finish()
    {
        qWarning("library-edit-test: %d checks, %d failed", m_count, m_failed);
        return m_failed;
    }

private:
    int m_count = 0;
    int m_failed = 0;
};

QueueTrack queued(const QString &id, bool radio = false)
{
    QueueTrack track;
    track.videoId = id;
    track.title = id;
    track.artist = QStringLiteral("Selftest");
    track.fromRadio = radio;
    return track;
}

QString orderOf(const QueueModel &queue)
{
    QStringList ids;
    for (int row = 0; row < queue.rowCount(); ++row)
        ids << queue.at(row)->videoId;
    return ids.join(QLatin1Char(' '));
}

QString upcomingOf(const QueueModel &queue)
{
    QStringList ids;
    for (int row = queue.currentIndex() + 1; row < queue.rowCount(); ++row)
        ids << queue.at(row)->videoId;
    return ids.join(QLatin1Char(' '));
}

QVariantMap song(const QString &videoId)
{
    return {
        { QStringLiteral("sourceId"), videoId },
        { QStringLiteral("title"), QStringLiteral("Selftest ") + videoId },
        { QStringLiteral("artist"), QStringLiteral("Selftest") },
        { QStringLiteral("durationMs"), 180000 }
    };
}

QString idsOf(const QVariantList &tracks)
{
    QStringList ids;
    for (const QVariant &track : tracks)
        ids << track.toMap().value(QStringLiteral("sourceId")).toString();
    return ids.join(QLatin1Char(' '));
}

QString openPlaylistIds(Library *library)
{
    return idsOf(library->playlistTrackList());
}

int entryOf(Library *library, const QString &videoId)
{
    const QVariantList tracks = library->playlistTrackList();
    for (const QVariant &track : tracks) {
        const QVariantMap map = track.toMap();
        if (map.value(QStringLiteral("sourceId")).toString() == videoId)
            return map.value(QStringLiteral("entryId")).toInt();
    }
    return 0;
}

bool historyHas(Library *library, const QString &videoId)
{
    SearchResultModel *history = library->history();
    for (int row = 0; row < history->rowCount(); ++row) {
        if (history->get(row).value(QStringLiteral("sourceId")).toString() == videoId)
            return true;
    }
    return false;
}

void testQueue(Checks &checks)
{
    checks.note(QStringLiteral("- the queue"));
    QueueModel queue;
    queue.replace({ queued(QStringLiteral("a")), queued(QStringLiteral("b")), queued(QStringLiteral("c")),
                    queued(QStringLiteral("d")), queued(QStringLiteral("e")), queued(QStringLiteral("f")) }, 1);

    checks.check(queue.move(4, 2), QStringLiteral("an upcoming song moves up"));
    checks.same(orderOf(queue), QStringLiteral("a b e c d f"), QStringLiteral("  and lands where it was put"));
    checks.check(queue.currentIndex() == 1, QStringLiteral("  the song playing stays current"));

    queue.move(3, 5);
    checks.same(orderOf(queue), QStringLiteral("a b e d f c"), QStringLiteral("an upcoming song moves down to the end"));

    queue.move(1, 3);
    checks.check(queue.currentIndex() == 3 && queue.at(3)->videoId == QLatin1String("b"),
                 QStringLiteral("the current song moved keeps being current"));
    queue.move(0, 4);
    checks.check(queue.at(queue.currentIndex())->videoId == QLatin1String("b"),
                 QStringLiteral("a past song moved past it shifts the current index with it"),
                 QStringLiteral("current %1").arg(queue.currentIndex()));
    checks.check(!queue.move(0, 6) && !queue.move(-1, 2) && !queue.move(2, 2),
                 QStringLiteral("moves out of range, or nowhere, are refused"));

    // The radio's songs.
    queue.replace({ queued(QStringLiteral("a")), queued(QStringLiteral("c1")), queued(QStringLiteral("c2")),
                    queued(QStringLiteral("r1"), true), queued(QStringLiteral("r2"), true) }, 0);
    checks.check(queue.radioStartIndex() == 3, QStringLiteral("the radio starts after the songs chosen"));
    queue.move(4, 1);
    checks.check(!queue.at(1)->fromRadio, QStringLiteral("a radio song put first is chosen from then on"));
    checks.check(queue.radioStartIndex() == 4, QStringLiteral("  and the radio heading stays over the radio"),
                 QStringLiteral("radio starts at %1").arg(queue.radioStartIndex()));
    queue.replace({ queued(QStringLiteral("a")), queued(QStringLiteral("c1")), queued(QStringLiteral("r1"), true),
                    queued(QStringLiteral("r2"), true), queued(QStringLiteral("r3"), true) }, 0);
    queue.move(4, 3);
    checks.check(queue.at(3)->fromRadio && queue.radioStartIndex() == 2,
                 QStringLiteral("a radio song moved within the radio stays the radio's"));
    queue.move(3, 2);
    checks.check(!queue.at(2)->fromRadio && queue.radioStartIndex() == 3,
                 QStringLiteral("a radio song put straight after a chosen one is chosen"));

    // Shuffled, then put back in order.
    const QList<QueueTrack> six = { queued(QStringLiteral("a")), queued(QStringLiteral("b")), queued(QStringLiteral("c")),
                                    queued(QStringLiteral("d")), queued(QStringLiteral("e")), queued(QStringLiteral("f")) };
    for (int round = 0; round < 3; ++round) {
        queue.replace(six, 0);
        queue.shuffleUpcoming();
        // The last upcoming song, put after the first; then, in the last
        // round, first of all.
        const bool toFront = round == 2;
        const QString moved = queue.at(5)->videoId;
        const QString after = queue.at(1)->videoId;
        queue.move(5, toFront ? 1 : 2);
        queue.restoreOrder();
        QStringList expected = { QStringLiteral("b"), QStringLiteral("c"), QStringLiteral("d"),
                                 QStringLiteral("e"), QStringLiteral("f") };
        expected.removeAll(moved);
        expected.insert(toFront ? 0 : int(expected.indexOf(after)) + 1, moved);
        checks.same(upcomingOf(queue), expected.join(QLatin1Char(' ')),
                    toFront ? QStringLiteral("shuffle off after a move to the front keeps it first")
                            : QStringLiteral("shuffle off after a move keeps the song after the one it was put after"));
    }
}

void testPlaylists(Checks &checks, Library *library)
{
    checks.note(QStringLiteral("- a playlist's order"));
    const int list = library->createPlaylist(QStringLiteral("Edit test"));
    checks.check(list > 0, QStringLiteral("a playlist is made"));
    library->openPlaylist(list);
    const QString t1 = QStringLiteral("edittest001"), t2 = QStringLiteral("edittest002"),
                  t3 = QStringLiteral("edittest003"), t4 = QStringLiteral("edittest004"),
                  t5 = QStringLiteral("edittest005");
    library->addAllToPlaylist(list, { song(t1), song(t2), song(t3), song(t4) });
    checks.same(openPlaylistIds(library), QStringList({ t1, t2, t3, t4 }).join(QLatin1Char(' ')),
                QStringLiteral("four songs, in the order added"));

    library->movePlaylistEntry(list, entryOf(library, t4), 0);
    checks.same(openPlaylistIds(library), QStringList({ t4, t1, t2, t3 }).join(QLatin1Char(' ')),
                QStringLiteral("the last dragged to the top"));
    checks.same(idsOf(library->playlistTracksFor(list)), QStringList({ t4, t1, t2, t3 }).join(QLatin1Char(' ')),
                QStringLiteral("  and the database says the same"));
    library->movePlaylistEntry(list, entryOf(library, t4), 99);
    checks.same(openPlaylistIds(library), QStringList({ t1, t2, t3, t4 }).join(QLatin1Char(' ')),
                QStringLiteral("a move past the end goes to the end"));
    library->movePlaylistEntry(list, entryOf(library, t2), 2);
    library->movePlaylistEntry(list, 999999, 0);
    library->openPlaylist(list);
    checks.same(openPlaylistIds(library), QStringList({ t1, t3, t2, t4 }).join(QLatin1Char(' ')),
                QStringLiteral("one down, read back after opening it again"));

    checks.note(QStringLiteral("- the library and Remove from library"));
    checks.check(library->isInLibrary(t1), QStringLiteral("a song in a playlist is in the library"));
    checks.check(!library->isInLibrary(QStringLiteral("edittest999")), QStringLiteral("a song nowhere is not"));
    library->setLiked(song(t5), true);
    checks.check(library->isInLibrary(t5), QStringLiteral("a liked song is"));

    const int other = library->createPlaylist(QStringLiteral("Edit test two"));
    library->addToPlaylist(other, song(t1));
    library->setLiked(song(t1), true);
    library->removeFromLibrary(song(t1));
    checks.check(!library->isLiked(t1), QStringLiteral("removed from the library, it is no longer liked"));
    checks.check(!library->isInLibrary(t1), QStringLiteral("  nor in the library"));
    library->openPlaylist(list);
    checks.same(openPlaylistIds(library), QStringList({ t3, t2, t4 }).join(QLatin1Char(' ')),
                QStringLiteral("  nor in the open playlist"));
    checks.check(library->playlistTracksFor(other).isEmpty(), QStringLiteral("  nor in the other playlist"));
    checks.check(library->isInLibrary(t3) && library->isLiked(t5),
                 QStringLiteral("  and the other songs are where they were"));

    library->deletePlaylist(list);
    library->deletePlaylist(other);
    library->setLiked(song(t5), false);
}

void testHistory(Checks &checks, Library *library)
{
    checks.note(QStringLiteral("- Remove from history"));
    const QString h1 = QStringLiteral("edittesth01"), h2 = QStringLiteral("edittesth02");
    for (const QString &id : { h1, h2 }) {
        QSqlQuery q(AppDatabase::connection());
        q.prepare(QStringLiteral("INSERT OR REPLACE INTO recent (video_id, title, artist) VALUES (?, ?, 'Selftest')"));
        q.addBindValue(id);
        q.addBindValue(QStringLiteral("Selftest ") + id);
        q.exec();
    }
    library->reloadHistory();
    checks.check(historyHas(library, h1) && historyHas(library, h2), QStringLiteral("two songs in the history"));
    int changes = 0;
    const QMetaObject::Connection counting = QObject::connect(library, &Library::historyChanged, [&changes]() { ++changes; });
    library->removeFromHistory(h1);
    checks.check(!historyHas(library, h1), QStringLiteral("one removed is gone"));
    checks.check(historyHas(library, h2), QStringLiteral("  and the other is not"));
    checks.check(changes == 1, QStringLiteral("  and Home's Recently played is told"));
    library->removeFromHistory(h1);
    checks.check(changes == 1, QStringLiteral("removing it again changes nothing"));
    QObject::disconnect(counting);
    library->removeFromHistory(h2);
}

void testCopyLink(Checks &checks, Library *library)
{
    checks.note(QStringLiteral("- Copy link"));
    QClipboard *clipboard = QGuiApplication::clipboard();
    const QMimeData *before = clipboard ? clipboard->mimeData() : nullptr;
    // Only over plain text or nothing, which can be put back as it was; a
    // picture, a file or formatted text is left alone and the check skipped.
    const bool restorable = !before || before->formats().isEmpty()
                            || (before->hasText() && !before->hasImage() && !before->hasUrls() && !before->hasHtml());
    if (!clipboard || !restorable) {
        checks.note(QStringLiteral("  skipped: the clipboard holds something that could not be put back"));
        return;
    }
    const QString kept = clipboard->text();
    const QString link = QStringLiteral("https://music.youtube.com/watch?v=edittest001");
    library->copyLink(link);
    checks.same(clipboard->text(), link, QStringLiteral("the link is on the clipboard"));
    clipboard->setText(kept);
}

} // namespace

int runLibraryEditSelfTest(Library *library)
{
    Checks checks;
    if (qEnvironmentVariableIsEmpty("MONOLIST_DATA_DIR")) {
        checks.check(false, QStringLiteral("MONOLIST_DATA_DIR is set"),
                     QStringLiteral("this test writes into the database and runs only in a scratch data folder"));
        return checks.finish();
    }
    testQueue(checks);
    testPlaylists(checks, library);
    testHistory(checks, library);
    testCopyLink(checks, library);
    return checks.finish();
}
