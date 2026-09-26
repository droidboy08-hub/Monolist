#include "artistselftest.h"

#include "appdatabase.h"
#include "artistlinks.h"
#include "innertube.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QSqlQuery>

#include <memory>

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
        qWarning("artist-links-test: %s  %s", ok ? "ok  " : "FAIL", qPrintable(line));
        return ok;
    }

    int finish()
    {
        qWarning("artist-links-test: %d checks, %d failed", m_count, m_failed);
        return m_failed;
    }

private:
    int m_count = 0;
    int m_failed = 0;
};

// — canned InnerTube answers, shaped like the real ones (September 2026) —

QJsonObject obj(const char *key, const QJsonValue &value)
{
    return { { QString::fromLatin1(key), value } };
}

QJsonObject runs(const QJsonArray &list)
{
    return obj("runs", list);
}

QJsonObject browseTo(const QString &browseId, const char *pageType)
{
    QJsonObject browse = obj("browseEndpointContextSupportedConfigs",
                             obj("browseEndpointContextMusicConfig", obj("pageType", QString::fromLatin1(pageType))));
    browse.insert(QStringLiteral("browseId"), browseId);
    return obj("browseEndpoint", browse);
}

QJsonObject run(const QString &text)
{
    return obj("text", text);
}

QJsonObject linkedRun(const QString &text, const QString &browseId, const char *pageType)
{
    QJsonObject object = run(text);
    object.insert(QStringLiteral("navigationEndpoint"), browseTo(browseId, pageType));
    return object;
}

QJsonObject artistRun(const QString &name, const QString &browseId)
{
    return linkedRun(name, browseId, "MUSIC_PAGE_TYPE_ARTIST");
}

QJsonObject flexColumn(const QJsonArray &list)
{
    return obj("musicResponsiveListItemFlexColumnRenderer", obj("text", runs(list)));
}

QJsonObject listItem(const QString &videoId, const QString &title, const QJsonArray &subtitle)
{
    QJsonArray columns{ flexColumn({ run(title) }) };
    if (!subtitle.isEmpty())
        columns.append(flexColumn(subtitle));
    QJsonObject item = obj("playlistItemData", obj("videoId", videoId));
    item.insert(QStringLiteral("flexColumns"), columns);
    return obj("musicResponsiveListItemRenderer", item);
}

QJsonObject browseCard(const QString &title, const QString &subtitle, const QString &browseId, const char *pageType)
{
    QJsonObject item = obj("title", runs({ run(title) }));
    item.insert(QStringLiteral("subtitle"), runs({ run(subtitle) }));
    item.insert(QStringLiteral("navigationEndpoint"), browseTo(browseId, pageType));
    return obj("musicTwoRowItemRenderer", item);
}

QJsonObject carousel(const QString &title, const QJsonArray &contents)
{
    QJsonObject shelf = obj("header", obj("musicCarouselShelfBasicHeaderRenderer",
                                          obj("title", runs({ run(title) }))));
    shelf.insert(QStringLiteral("contents"), contents);
    return obj("musicCarouselShelfRenderer", shelf);
}

QJsonObject browsePage(const QJsonArray &sections)
{
    const QJsonObject tab = obj("tabRenderer", obj("content", obj("sectionListRenderer", obj("contents", sections))));
    return obj("contents", obj("singleColumnBrowseResultsRenderer", obj("tabs", QJsonArray{ tab })));
}

QString joined(const QList<InnerTube::Credit> &credits)
{
    QString text;
    for (const InnerTube::Credit &credit : credits)
        text += credit.text;
    return text;
}

// "[Lady Gaga>UCx] & [Bruno Mars>UCy]", for the report.
QString describe(const QVariantList &pieces)
{
    QString text;
    for (const QVariant &value : pieces) {
        const QVariantMap piece = value.toMap();
        if (piece.value(QStringLiteral("link")).toBool())
            text += QStringLiteral("[%1>%2]").arg(piece.value(QStringLiteral("text")).toString(),
                                                  piece.value(QStringLiteral("id")).toString());
        else
            text += piece.value(QStringLiteral("text")).toString();
    }
    return text;
}

QString describe(const QList<InnerTube::Credit> &credits)
{
    return describe(InnerTube::creditsToVariant(credits));
}

} // namespace

int runArtistLinksSelfTest()
{
    Checks t;
    if (qEnvironmentVariableIsEmpty("MONOLIST_DATA_DIR")) {
        t.check(false, QStringLiteral("MONOLIST_DATA_DIR is set"),
                QStringLiteral("refusing to run: this test writes to the artist_links table"));
        return t.finish();
    }

    auto links = std::make_unique<ArtistLinks>();
    links->load();
    InnerTube::setArtistHook([&links](const QString &name, const QString &browseId, bool artistPage) {
        links->remember(name, browseId, artistPage);
    });

    // — credits, from song rows —
    const QString gaga = QStringLiteral("UCselftestGaga");
    const QString mars = QStringLiteral("UCselftestMars");
    const QString tyler = QStringLiteral("UCselftestTyler");
    const QJsonObject bullet = run(QStringLiteral(" • "));
    const QJsonArray rows{
        listItem(QStringLiteral("selftestA1"), QStringLiteral("Die With A Smile"),
                 { artistRun(QStringLiteral("Lady Gaga"), gaga), run(QStringLiteral(" & ")),
                   artistRun(QStringLiteral("Bruno Mars"), mars), bullet,
                   linkedRun(QStringLiteral("Die With A Smile"), QStringLiteral("MPREselftestAlbum"), "MUSIC_PAGE_TYPE_ALBUM"),
                   bullet, run(QStringLiteral("4:12")) }),
        listItem(QStringLiteral("selftestA2"), QStringLiteral("EARFQUAKE"),
                 { artistRun(QStringLiteral("Tyler, The Creator"), tyler), bullet, run(QStringLiteral("3:10")) }),
        listItem(QStringLiteral("selftestA3"), QStringLiteral("Unlinked"),
                 { run(QStringLiteral("Song")), bullet, run(QStringLiteral("Some Band")), bullet, run(QStringLiteral("2:01")) }),
        listItem(QStringLiteral("selftestA4"), QStringLiteral("Half linked"),
                 { artistRun(QStringLiteral("Selftest One"), QStringLiteral("UCselftestOne")), run(QStringLiteral(", ")),
                   run(QStringLiteral("Selftest Nobody")), bullet, run(QStringLiteral("1:00")) }),
    };
    const QList<InnerTube::Shelf> shelves = InnerTube::parseShelves(browsePage({ carousel(QStringLiteral("Quick picks"), rows) }));
    const QList<InnerTube::Track> songs = shelves.isEmpty() ? QList<InnerTube::Track>() : shelves.first().songs;
    const InnerTube::Track none;
    const InnerTube::Track &duet = songs.size() > 0 ? songs.at(0) : none;
    const InnerTube::Track &comma = songs.size() > 1 ? songs.at(1) : none;
    const InnerTube::Track &plain = songs.size() > 2 ? songs.at(2) : none;
    const InnerTube::Track &half = songs.size() > 3 ? songs.at(3) : none;

    t.check(duet.credits.size() == 3 && duet.credits.at(0).browseId == gaga && duet.credits.at(1).browseId.isEmpty()
                && !duet.credits.at(1).name && duet.credits.at(2).browseId == mars,
            QStringLiteral("a joint credit: each name with its own page, the joiner between"), describe(duet.credits));
    t.check(joined(duet.credits) == duet.artist,
            QStringLiteral("the pieces add up to the artist line"), joined(duet.credits) + QStringLiteral(" vs ") + duet.artist);
    t.check(duet.albumId == QLatin1String("MPREselftestAlbum"), QStringLiteral("the album's page is kept"), duet.albumId);
    t.check(comma.credits.size() == 1 && comma.credits.at(0).text == QLatin1String("Tyler, The Creator")
                && comma.credits.at(0).browseId == tyler,
            QStringLiteral("a name with a comma in it is one credit"), describe(comma.credits));
    t.check(plain.credits.isEmpty() && plain.artist == QLatin1String("Some Band"),
            QStringLiteral("nothing linked: no pieces, the name alone"), describe(plain.credits));
    t.check(half.credits.size() == 3 && half.credits.at(0).browseId == QLatin1String("UCselftestOne")
                && half.credits.at(2).name && half.credits.at(2).browseId.isEmpty(),
            QStringLiteral("a name without a link is still a name"), describe(half.credits));

    // — what the answers taught ArtistLinks —
    t.check(links->idFor(QStringLiteral("Lady Gaga")) == gaga && links->idFor(QStringLiteral("Bruno Mars")) == mars,
            QStringLiteral("every linked name is remembered as it is read"),
            links->idFor(QStringLiteral("Lady Gaga")) + QStringLiteral(" ") + links->idFor(QStringLiteral("Bruno Mars")));
    t.check(links->idFor(QStringLiteral("lady gaga")) == gaga, QStringLiteral("in another case"));

    const auto split = [&links](const QString &line) { return links->credits(line); };
    const QVariantList pair = split(QStringLiteral("Bruno Mars & Lady Gaga"));
    t.check(describe(pair) == QStringLiteral("[Bruno Mars>%1] & [Lady Gaga>%2]").arg(mars, gaga),
            QStringLiteral("a line kept without links is split into known names"), describe(pair));
    const QVariantList listed = split(QStringLiteral("Lady Gaga, Bruno Mars"));
    t.check(describe(listed) == QStringLiteral("[Lady Gaga>%1], [Bruno Mars>%2]").arg(gaga, mars),
            QStringLiteral("at a comma too, when both sides are names"), describe(listed));
    const QVariantList whole = split(QStringLiteral("Tyler, The Creator"));
    t.check(describe(whole) == QStringLiteral("[Tyler, The Creator>%1]").arg(tyler),
            QStringLiteral("but not inside a name that has one"), describe(whole));
    const QVariantList three = split(QStringLiteral("Tyler, The Creator feat. Lady Gaga"));
    t.check(describe(three) == QStringLiteral("[Tyler, The Creator>%1] feat. [Lady Gaga>%2]").arg(tyler, gaga),
            QStringLiteral("the longest known name first"), describe(three));
    const QVariantList unknown = split(QStringLiteral("Lady Gaga & Somebody Unheard"));
    t.check(describe(unknown) == QStringLiteral("[Lady Gaga & Somebody Unheard>]"),
            QStringLiteral("an unknown name keeps the line whole, to be looked up by what it says"), describe(unknown));
    const QVariantList stranger = split(QStringLiteral("Selftest Stranger"));
    t.check(describe(stranger) == QStringLiteral("[Selftest Stranger>]"),
            QStringLiteral("one unknown name: a link by its text"), describe(stranger));
    t.check(split(QStringLiteral("  ")).isEmpty(), QStringLiteral("no artist, no pieces"));

    // The track's own credits win, and a name in them without a page is
    // filled in where it is known.
    const QList<InnerTube::Credit> given{
        InnerTube::Credit{ QStringLiteral("Lady Gaga"), QString(), true },
        InnerTube::Credit{ QStringLiteral(" x "), QString(), false },
        InnerTube::Credit{ QStringLiteral("Selftest Other"), QStringLiteral("UCselftestOther"), true } };
    const QVariantList own = links->credits(QStringLiteral("anything"), InnerTube::creditsToVariant(given));
    t.check(describe(own) == QStringLiteral("[Lady Gaga>%1] x [Selftest Other>UCselftestOther]").arg(gaga),
            QStringLiteral("a track's own credits are used, gaps filled"), describe(own));

    links->remember(QStringLiteral("Guns N' Roses"), QStringLiteral("UCselftestGnr"), true);
    t.check(links->idFor(QString::fromUtf8("Guns N\xE2\x80\x99 Roses")) == QLatin1String("UCselftestGnr")
                && links->idFor(QStringLiteral("GUNS N ROSES")) == QLatin1String("UCselftestGnr"),
            QStringLiteral("another spelling of the same name"));
    links->remember(QStringLiteral("Selftest Twin"), QStringLiteral("UCselftestArtist"), true);
    links->remember(QStringLiteral("Selftest Twin"), QStringLiteral("UCselftestChannel"), false);
    t.check(links->idFor(QStringLiteral("Selftest Twin")) == QLatin1String("UCselftestArtist"),
            QStringLiteral("a plain channel does not take an artist's name"), links->idFor(QStringLiteral("Selftest Twin")));

    // — kept across a restart —
    links.reset();   // writes what it learnt
    links = std::make_unique<ArtistLinks>();
    links->load();
    t.check(links->idFor(QStringLiteral("Tyler, The Creator")) == tyler
                && links->idFor(QStringLiteral("Selftest Twin")) == QLatin1String("UCselftestArtist"),
            QStringLiteral("what was learnt is still known after a restart"),
            links->idFor(QStringLiteral("Tyler, The Creator")));

    // — an artist's page —
    {
        const auto watch = [](const QString &videoId, const QString &playlistId, const QString &params) {
            QJsonObject endpoint = obj("videoId", videoId);
            endpoint.insert(QStringLiteral("playlistId"), playlistId);
            endpoint.insert(QStringLiteral("params"), params);
            return obj("buttonRenderer", obj("navigationEndpoint", obj("watchEndpoint", endpoint)));
        };
        QJsonObject header = obj("title", runs({ run(QString::fromUtf8("\xD0\x9A\xD0\xB8\xD0\xBD\xD0\xBE")) }));   // Кино
        header.insert(QStringLiteral("description"), runs({ run(QStringLiteral("A band from Leningrad.")) }));
        header.insert(QStringLiteral("monthlyListenerCount"), runs({ run(QStringLiteral("1.2M monthly audience")) }));
        header.insert(QStringLiteral("thumbnail"), obj("musicThumbnailRenderer", obj("thumbnail", obj("thumbnails", QJsonArray{
            obj("url", QStringLiteral("https://lh3.googleusercontent.com/selftest=w540-h225-p-l90-rj")),
            obj("url", QStringLiteral("https://lh3.googleusercontent.com/selftest=w2880-h1200-p-l90-rj")) }))));
        header.insert(QStringLiteral("playButton"), watch(QStringLiteral("selftestS1"), QStringLiteral("RDAOselftest"),
                                                          QStringLiteral("wAEB8gECGAE%3D")));
        header.insert(QStringLiteral("startRadioButton"), watch(QStringLiteral("selftestR1"), QStringLiteral("RDEMselftest"),
                                                                QStringLiteral("wAEB")));

        const QString kino = QStringLiteral("UCselftestKino");
        QJsonObject top = obj("title", runs({ linkedRun(QStringLiteral("Top songs"), QStringLiteral("VLselftestAll"),
                                                        "MUSIC_PAGE_TYPE_PLAYLIST") }));
        top.insert(QStringLiteral("contents"), QJsonArray{
            listItem(QStringLiteral("selftestK1"), QStringLiteral("Gruppa krovi"),
                     { artistRun(QString::fromUtf8("\xD0\x9A\xD0\xB8\xD0\xBD\xD0\xBE"), kino),
                       run(QStringLiteral(" • ")), run(QStringLiteral("120M plays")), run(QStringLiteral(" • ")),
                       linkedRun(QStringLiteral("Gruppa krovi"), QStringLiteral("MPREselftestKino"), "MUSIC_PAGE_TYPE_ALBUM") }),
            listItem(QStringLiteral("selftestK2"), QStringLiteral("Kukushka"),
                     { artistRun(QString::fromUtf8("\xD0\x9A\xD0\xB8\xD0\xBD\xD0\xBE"), kino) }) });

        QJsonObject page = browsePage({
            obj("musicShelfRenderer", top),
            carousel(QStringLiteral("Albums"), { browseCard(QStringLiteral("Gruppa krovi"), QStringLiteral("1988"),
                                                            QStringLiteral("MPREselftestKino"), "MUSIC_PAGE_TYPE_ALBUM") }),
            carousel(QStringLiteral("Podcasts"), { browseCard(QStringLiteral("A podcast"), QStringLiteral("Podcast"),
                                                              QStringLiteral("MPSPselftest"),
                                                              "MUSIC_PAGE_TYPE_PODCAST_SHOW_DETAIL_PAGE") }),
            carousel(QStringLiteral("Fans might also like"), {
                browseCard(QStringLiteral("Selftest Aquarium"), QStringLiteral("900K monthly audience"),
                           QStringLiteral("UCselftestAquarium"), "MUSIC_PAGE_TYPE_ARTIST") }),
            obj("musicDescriptionShelfRenderer", obj("description", runs({ run(QStringLiteral("Not this one.")) })))
        });
        page.insert(QStringLiteral("header"), obj("musicImmersiveHeaderRenderer", header));

        const InnerTube::Artist artist = InnerTube::parseArtist(kino, page);
        t.check(artist.name == QString::fromUtf8("\xD0\x9A\xD0\xB8\xD0\xBD\xD0\xBE") && artist.artistPage
                    && artist.audience == QLatin1String("1.2M monthly audience")
                    && artist.description == QLatin1String("A band from Leningrad."),
                QStringLiteral("the header: name, audience, the header's own description"),
                artist.audience + QStringLiteral(" / ") + artist.description);
        t.check(artist.artwork == QLatin1String("https://lh3.googleusercontent.com/selftest=w544-h544-p-l90-rj"),
                QStringLiteral("the header's picture, asked for square"), artist.artwork);
        t.check(artist.shuffle.playlistId == QLatin1String("RDAOselftest") && artist.shuffle.videoId == QLatin1String("selftestS1")
                    && artist.radio.playlistId == QLatin1String("RDEMselftest") && artist.radio.params == QLatin1String("wAEB"),
                QStringLiteral("Shuffle and Mix: what each plays"));
        t.check(artist.songs.size() == 2 && artist.songsTitle == QLatin1String("Top songs")
                    && artist.songsId == QLatin1String("VLselftestAll"),
                QStringLiteral("the top songs and the playlist of all of them"),
                QStringLiteral("%1 songs, %2").arg(artist.songs.size()).arg(artist.songsId));
        t.check(!artist.songs.isEmpty() && artist.songs.first().album == QLatin1String("Gruppa krovi")
                    && artist.songs.first().albumId == QLatin1String("MPREselftestKino")
                    && artist.songs.first().credits.size() == 1 && artist.songs.first().credits.first().browseId == kino,
                QStringLiteral("a top song's album and artist, each with its page, the play count left out"),
                artist.songs.isEmpty() ? QString() : artist.songs.first().artist + QStringLiteral(" / ") + artist.songs.first().album);
        QStringList titles;
        for (const InnerTube::Shelf &shelf : artist.shelves)
            titles << shelf.title;
        t.check(titles == QStringList{ QStringLiteral("Albums"), QStringLiteral("Fans might also like") },
                QStringLiteral("the shelves in the page's order, a podcast's (which cannot open) left out"),
                titles.join(QStringLiteral(", ")));
        t.check(artist.shelves.size() == 2 && artist.shelves.at(1).cards.value(0).type == QLatin1String("artist")
                    && links->idFor(QStringLiteral("Selftest Aquarium")) == QLatin1String("UCselftestAquarium"),
                QStringLiteral("a similar artist is a card that opens their page, and is remembered"));
        t.check(links->idFor(QString::fromUtf8("\xD0\x9A\xD0\xB8\xD0\xBD\xD0\xBE")) == kino,
                QStringLiteral("a name in another script is kept as written"));
    }

    // — a search for artists —
    {
        QJsonObject row = obj("flexColumns", QJsonArray{ flexColumn({ run(QStringLiteral("Selftest Radiohead")) }),
                                                        flexColumn({ run(QStringLiteral("Artist")) }) });
        row.insert(QStringLiteral("navigationEndpoint"),
                   browseTo(QStringLiteral("UCselftestRadiohead"), "MUSIC_PAGE_TYPE_ARTIST"));
        const QJsonObject album = listItem(QStringLiteral("selftestNot"), QStringLiteral("A song"), {});
        const QJsonObject shelf = obj("musicShelfRenderer", obj("contents", QJsonArray{
            obj("musicResponsiveListItemRenderer", row), album }));
        const QJsonObject tab = obj("tabRenderer", obj("content", obj("sectionListRenderer",
                                                                      obj("contents", QJsonArray{ shelf }))));
        const QList<InnerTube::ArtistHit> hits = InnerTube::parseArtistSearch(
            obj("contents", obj("tabbedSearchResultsRenderer", obj("tabs", QJsonArray{ tab }))));
        t.check(hits.size() == 1 && hits.first().name == QLatin1String("Selftest Radiohead")
                    && hits.first().browseId == QLatin1String("UCselftestRadiohead"),
                QStringLiteral("an artist search: artists only, each with its page"),
                QStringLiteral("%1 hits").arg(hits.size()));
    }

    // Nothing invented is left behind.
    InnerTube::setArtistHook({});
    links.reset();
    QSqlQuery cleanup(AppDatabase::connection());
    cleanup.exec(QStringLiteral("DELETE FROM artist_links WHERE browse_id LIKE 'UCselftest%'"));
    QSqlQuery left(AppDatabase::connection());
    left.exec(QStringLiteral("SELECT COUNT(*) FROM artist_links WHERE browse_id LIKE 'UCselftest%'"));
    t.check(left.next() && left.value(0).toInt() == 0, QStringLiteral("the invented names are removed again"));
    return t.finish();
}
