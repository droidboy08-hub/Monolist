#include "libraryeditselftest.h"

#include "appdatabase.h"
#include "library.h"
#include "queuemodel.h"
#include "trackfiltermodel.h"

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

QString titlesOf(const TrackFilterModel &view)
{
    QStringList titles;
    for (int row = 0; row < view.rowCount(); ++row)
        titles << view.get(row).value(QStringLiteral("title")).toString();
    return titles.join(QLatin1Char('|'));
}

void testFilter(Checks &checks)
{
    checks.note(QStringLiteral("- a list filtered and put in order"));
    const auto song = [](const char *id, const char *title, const char *artist, const char *album, qint64 ms) {
        SearchResultModel::Item item;
        item.sourceId = QString::fromUtf8(id);
        item.title = QString::fromUtf8(title);
        item.artist = QString::fromUtf8(artist);
        item.album = QString::fromUtf8(album);
        item.durationMs = ms;
        return item;
    };
    SearchResultModel songs;
    songs.replace({ song("a", "Numb", "Linkin Park", "Meteora", 187000),
                    song("b", "Despacito", "Luis Fonsi & Daddy Yankee", "VIDA", 281000),
                    song("c", "bohemian rhapsody", "Queen", "A Night at the Opera", 354000),
                    song("d", "Song 10", "Daddy Yankee", "Barrio Fino", 200000),
                    song("e", "Song 9", "Queen", "Innuendo", 150000) });
    TrackFilterModel view;
    view.setSourceModel(&songs);
    checks.same(titlesOf(view), QStringLiteral("Numb|Despacito|bohemian rhapsody|Song 10|Song 9"),
                QStringLiteral("nothing asked: the list as it is"));
    checks.check(!view.rearranged(), QStringLiteral("  and not rearranged"));
    view.setFilterText(QStringLiteral("queen opera"));
    checks.same(titlesOf(view), QStringLiteral("bohemian rhapsody"),
                QStringLiteral("every word typed must be in the title, the artist or the album"));
    view.setFilterText(QString());
    view.setArtist(QStringLiteral("daddy yankee"));
    checks.same(titlesOf(view), QStringLiteral("Despacito|Song 10"),
                QStringLiteral("one artist's songs, their collaborations too, whatever the case"));
    view.setArtist(QString());
    view.setSortKey(QStringLiteral("title"));
    checks.same(titlesOf(view), QStringLiteral("bohemian rhapsody|Despacito|Numb|Song 9|Song 10"),
                QStringLiteral("A-Z, without regard to case, and numbers as numbers"));
    view.setDescending(true);
    checks.same(titlesOf(view), QStringLiteral("Song 10|Song 9|Numb|Despacito|bohemian rhapsody"),
                QStringLiteral("  and Z-A"));
    view.setDescending(false);
    view.setSortKey(QStringLiteral("artist"));
    checks.same(titlesOf(view), QStringLiteral("Song 10|Numb|Despacito|bohemian rhapsody|Song 9"),
                QStringLiteral("by artist, each artist's songs by title"));
    view.setSortKey(QStringLiteral("duration"));
    checks.same(titlesOf(view), QStringLiteral("Song 9|Numb|Song 10|Despacito|bohemian rhapsody"),
                QStringLiteral("by length"));
    checks.check(view.rearranged(), QStringLiteral("  which is rearranged"));
    view.setSortKey(QString());
    checks.same(titlesOf(view), QStringLiteral("Numb|Despacito|bohemian rhapsody|Song 10|Song 9"),
                QStringLiteral("and back to the list's own order"));
    checks.same(TrackFilterModel::artistsIn(QStringLiteral("A, B & C feat. D x E ft. F")).join(QLatin1Char('|')),
                QStringLiteral("A|B|C|D|E|F"), QStringLiteral("the names a credit line holds"));
}

void testSongs(Checks &checks, Library *library)
{
    checks.note(QStringLiteral("- every song the library holds"));
    QSqlQuery(AppDatabase::connection()).exec(QStringLiteral(
        "INSERT INTO tracks (position, title, artist, album, duration_ms, source_url, source_id, artwork, favourite,"
        " liked_at) VALUES (900, 'Liked one', 'Artist One', '', 1000, '', 'songsA', '', 1, '2026-01-01 10:00:00')"));
    QSqlQuery(AppDatabase::connection()).exec(QStringLiteral(
        "INSERT INTO playlists (name) VALUES ('Songs test')"));
    QSqlQuery id(AppDatabase::connection());
    id.exec(QStringLiteral("SELECT id FROM playlists WHERE name = 'Songs test'"));
    const int playlist = id.next() ? id.value(0).toInt() : 0;
    QSqlQuery add(AppDatabase::connection());
    add.prepare(QStringLiteral(
        "INSERT INTO playlist_tracks (playlist_id, position, video_id, title, artist, album, duration_ms, added_at)"
        " VALUES (?, ?, ?, ?, ?, '', 1000, ?)"));
    for (const auto &[position, video, title, artist, when] :
         { std::tuple{ 0, "songsA", "Liked one", "Artist One", "2026-02-01 10:00:00" },
           std::tuple{ 1, "songsB", "Listed one", "Artist One & Artist Two", "2026-03-01 10:00:00" } }) {
        add.addBindValue(playlist);
        add.addBindValue(position);
        add.addBindValue(QString::fromUtf8(video));
        add.addBindValue(QString::fromUtf8(title));
        add.addBindValue(QString::fromUtf8(artist));
        add.addBindValue(QString::fromUtf8(when));
        add.exec();
    }
    library->reloadSongs();
    QStringList ids;
    for (int row = 0; row < library->songs()->rowCount(); ++row) {
        const QString video = library->songs()->get(row).value(QStringLiteral("sourceId")).toString();
        if (video.startsWith(QLatin1String("songs")))
            ids << video;
    }
    checks.same(ids.join(QLatin1Char('|')), QStringLiteral("songsB|songsA"),
                QStringLiteral("a song liked and in a playlist is listed once, the latest added first"));
    int one = 0;
    int two = 0;
    for (const QVariant &artist : library->songArtists()) {
        const QVariantMap map = artist.toMap();
        if (map.value(QStringLiteral("name")).toString() == QLatin1String("Artist One"))
            one = map.value(QStringLiteral("count")).toInt();
        if (map.value(QStringLiteral("name")).toString() == QLatin1String("Artist Two"))
            two = map.value(QStringLiteral("count")).toInt();
    }
    checks.check(one == 2 && two == 1, QStringLiteral("their artists, each with their songs, collaborations counted for both"),
                 QStringLiteral("Artist One %1, Artist Two %2").arg(one).arg(two));
    QSqlQuery(AppDatabase::connection()).exec(QStringLiteral("DELETE FROM tracks WHERE source_id = 'songsA'"));
    QSqlQuery(AppDatabase::connection()).exec(QStringLiteral("DELETE FROM playlists WHERE name = 'Songs test'"));
    QSqlQuery(AppDatabase::connection()).exec(QStringLiteral("DELETE FROM playlist_tracks WHERE video_id LIKE 'songs%'"));
}

void testPlaylistOrder(Checks &checks, Library *library)
{
    checks.note(QStringLiteral("- the playlists' order"));
    const int a = library->createPlaylist(QStringLiteral("Order A"));
    const int b = library->createPlaylist(QStringLiteral("Order B"));
    const int c = library->createPlaylist(QStringLiteral("Order C"));
    const auto order = [library, a, b, c]() {
        QStringList names;
        for (int row = 0; row < library->playlists()->rowCount(); ++row) {
            const QVariantMap item = library->playlists()->get(row);
            const int id = item.value(QStringLiteral("playlistId")).toInt();
            if (id == a || id == b || id == c)
                names << item.value(QStringLiteral("name")).toString().mid(6)
                             + (item.value(QStringLiteral("pinned")).toBool() ? QStringLiteral("*") : QString());
        }
        return names.join(QLatin1Char(' '));
    };
    checks.same(order(), QStringLiteral("A B C"), QStringLiteral("new playlists in the order they were made"));
    library->setPlaylistPinned(c, true);
    checks.same(order(), QStringLiteral("C* A B"), QStringLiteral("a pinned one goes above the rest"));
    library->movePlaylist(b, library->playlistRow(a));
    checks.same(order(), QStringLiteral("C* B A"), QStringLiteral("one moved goes where it is put"));
    library->movePlaylist(a, 0);
    checks.same(order(), QStringLiteral("C* A B"), QStringLiteral("  and not above the pinned ones"));
    library->playlists()->reload();
    checks.same(order(), QStringLiteral("C* A B"), QStringLiteral("  and the order is kept"));
    library->setPlaylistPinned(c, false);
    checks.check(!library->isPlaylistPinned(c), QStringLiteral("unpinned, it is with the rest again"), order());
    for (const int id : { a, b, c })
        library->deletePlaylist(id);
}

void testSearches(Checks &checks, Library *library)
{
    checks.note(QStringLiteral("- recent searches"));
    library->clearSearches();
    library->rememberSearch(QStringLiteral("  daft   punk "));
    library->rememberSearch(QStringLiteral("queen"));
    checks.same(library->recentSearches().join(QLatin1Char('|')), QStringLiteral("queen|daft punk"),
                QStringLiteral("a search used is kept, the latest first, its spaces tidied"));
    library->rememberSearch(QStringLiteral("Daft Punk"));
    checks.same(library->recentSearches().join(QLatin1Char('|')), QStringLiteral("Daft Punk|queen"),
                QStringLiteral("  the same search again moves up, once, as last typed"));
    library->rememberSearch(QStringLiteral("   "));
    checks.check(library->recentSearches().size() == 2, QStringLiteral("  nothing typed is not kept"));
    library->forgetSearch(QStringLiteral("queen"));
    checks.same(library->recentSearches().join(QLatin1Char('|')), QStringLiteral("Daft Punk"),
                QStringLiteral("one can be taken away"));
    for (int i = 0; i < 60; ++i)
        library->rememberSearch(QStringLiteral("search %1").arg(i));
    checks.check(library->recentSearches().size() == 50
                     && library->recentSearches().first() == QLatin1String("search 59")
                     && !library->recentSearches().contains(QStringLiteral("Daft Punk")),
                 QStringLiteral("the latest 50 are kept"),
                 QStringLiteral("%1, first %2").arg(library->recentSearches().size())
                     .arg(library->recentSearches().value(0)));
    library->clearSearches();
    checks.check(library->recentSearches().isEmpty(), QStringLiteral("and Clear all clears them"));
}

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
    testSearches(checks, library);
    testFilter(checks);
    testSongs(checks, library);
    testPlaylistOrder(checks, library);
    return checks.finish();
}
