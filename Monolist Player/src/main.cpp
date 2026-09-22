#include <QElapsedTimer>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QSqlError>
#include <QSqlQuery>
#include <QTimer>
#include <QWindow>

#include "appdatabase.h"
#include "artworkcache.h"
#include "catalog.h"
#include "downloadmanager.h"
#include "library.h"
#include "mediaextractor.h"
#include "mpvengine.h"
#include "playbackcontroller.h"
#include "streamresolver.h"
#include "trackmodel.h"
#include "windowchrome.h"
#include "ytdlp.h"

#include <memory>

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("Monolist"));
    app.setApplicationName(QStringLiteral("Monolist"));
    app.setApplicationDisplayName(QStringLiteral("Monolist"));
    app.setApplicationVersion(QStringLiteral("0.1"));

    // Neutral control style: the design is drawn entirely by the QML components,
    // platform styles would override paddings and colors.
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    // Load the bundled typeface. The design names "Archivo" directly and its
    // letter-spacing is tuned to that face, so a fallback sans does not just
    // look different — the tracking is wrong for it. Registering the fonts from
    // resources means no system install is required on any platform.
    {
        const QStringList faces = {
            QStringLiteral(":/qt/qml/Monolist/fonts/Archivo-Regular.ttf"),
            QStringLiteral(":/qt/qml/Monolist/fonts/Archivo-SemiBold.ttf"),
            QStringLiteral(":/qt/qml/Monolist/fonts/Archivo-ExtraBold.ttf")
        };
        bool loadedAny = false;
        for (const QString &face : faces) {
            if (QFontDatabase::addApplicationFont(face) >= 0)
                loadedAny = true;
            else
                qWarning("Monolist: could not load bundled font %s", qPrintable(face));
        }
        if (!loadedAny) {
            qWarning("Monolist: no bundled fonts loaded — the interface will fall back "
                     "to the default sans and the design will not match.");
        }
    }

    AppDatabase database;
    if (!database.open())
        qWarning("Monolist: local database unavailable, running with in-memory data only.");
    database.createSchema();
    database.migrate();   // also clears the prototype's invented content, once

    Library library;
    library.load();

    // — engines —
    MpvEngine engine;
    if (!engine.isValid())
        qWarning("Monolist: %s", qPrintable(engine.lastError()));

    StreamResolver resolver;
    // Instance lists are stored in settings so a dead host can be swapped out
    // without rebuilding; the compiled defaults apply on first run.
    const QString pipedSetting = library.settingValue(QStringLiteral("piped_instances"));
    const QString invidiousSetting = library.settingValue(QStringLiteral("invidious_instances"));
    if (!pipedSetting.isEmpty())
        resolver.setPipedInstances(pipedSetting.split(QLatin1Char(','), Qt::SkipEmptyParts));
    if (!invidiousSetting.isEmpty())
        resolver.setInvidiousInstances(invidiousSetting.split(QLatin1Char(','), Qt::SkipEmptyParts));

    DownloadManager downloads;

    PlaybackController player(&engine, &resolver, &downloads);
    player.setLibrary(&library);
    // Open with the library queued and its first song ready, not playing.
    player.loadModel(library.tracks(), 0);

    MediaExtractor extractor;

    // Home's content: YouTube Music's feed and new releases, fetched once at
    // start, and the songs played lately, refreshed whenever one starts.
    Catalog catalog;
    catalog.refresh();
    catalog.reloadRecent();
    QObject::connect(&player, &PlaybackController::currentTrackChanged, &catalog, &Catalog::reloadRecent);
    QObject::connect(&player, &PlaybackController::currentTrackChanged, &library, &Library::reloadHistory);
    QObject::connect(&library, &Library::historyCleared, &catalog, &Catalog::reloadRecent);

    // A completed download becomes a library row; reload so it is playable
    // straight away rather than after a restart.
    QObject::connect(&downloads, &DownloadManager::libraryChanged,
                     library.tracks(), &TrackModel::reload);

    if (!YtDlp::isAvailable()) {
        qWarning("Monolist: yt-dlp not found — search and downloads are disabled, "
                 "and streaming falls back to public instances. Install with "
                 "`pip install yt-dlp`.");
    }

    // — QML —
    ArtworkFetcher artworkFetcher;
    PaletteTool palette;

    qmlRegisterSingletonInstance("Monolist.Backend", 1, 0, "Library",   &library);
    qmlRegisterSingletonInstance("Monolist.Backend", 1, 0, "Player",    &player);
    qmlRegisterSingletonInstance("Monolist.Backend", 1, 0, "Extractor", &extractor);
    qmlRegisterSingletonInstance("Monolist.Backend", 1, 0, "Downloads", &downloads);
    qmlRegisterSingletonInstance("Monolist.Backend", 1, 0, "Palette",   &palette);
    qmlRegisterSingletonInstance("Monolist.Backend", 1, 0, "Catalog",   &catalog);
    WindowChrome chrome;
    qmlRegisterSingletonInstance("Monolist.Backend", 1, 0, "Chrome",    &chrome);
    qmlRegisterUncreatableType<SearchResultModel>(
        "Monolist.Backend", 1, 0, "SearchResultModel",
        QStringLiteral("Obtained from Extractor.results"));
    qmlRegisterUncreatableType<QueueModel>(
        "Monolist.Backend", 1, 0, "QueueModel",
        QStringLiteral("Obtained from Player.queue"));
    qmlRegisterUncreatableType<DownloadQueueModel>(
        "Monolist.Backend", 1, 0, "DownloadQueueModel",
        QStringLiteral("Obtained from Downloads.queue"));
    qmlRegisterUncreatableType<DownloadLibraryModel>(
        "Monolist.Backend", 1, 0, "DownloadLibraryModel",
        QStringLiteral("Obtained from Downloads.library"));
    qmlRegisterUncreatableType<PlaylistModel>(
        "Monolist.Backend", 1, 0, "PlaylistModel",
        QStringLiteral("Obtained from Library.playlists"));
    qmlRegisterUncreatableType<AlbumModel>(
        "Monolist.Backend", 1, 0, "AlbumModel",
        QStringLiteral("Obtained from Library.albums"));
    qmlRegisterUncreatableType<TrackModel>(
        "Monolist.Backend", 1, 0, "TrackModel",
        QStringLiteral("Obtained from Library.tracks"));

    QQmlApplicationEngine qmlEngine;
    qmlEngine.addImageProvider(QStringLiteral("artwork"), new ArtworkCache(&artworkFetcher));

    QObject::connect(&qmlEngine, &QQmlApplicationEngine::objectCreationFailed,
                     &app, []() { QCoreApplication::exit(-1); },
                     Qt::QueuedConnection);

    // --view <home|search|library|downloads> opens on that page, and
    // --query "<text>" on the search page with that typed in: for checking or
    // capturing a view without clicking through to it.
    {
        const QStringList arguments = app.arguments();
        QVariantMap initial;
        const int viewFlag = arguments.indexOf(QStringLiteral("--view"));
        if (viewFlag >= 0 && viewFlag + 1 < arguments.size())
            initial.insert(QStringLiteral("currentView"), arguments.at(viewFlag + 1));
        const int queryFlag = arguments.indexOf(QStringLiteral("--query"));
        if (queryFlag >= 0 && queryFlag + 1 < arguments.size()) {
            initial.insert(QStringLiteral("currentView"), QStringLiteral("search"));
            initial.insert(QStringLiteral("initialQuery"), arguments.at(queryFlag + 1));
        }
        if (arguments.contains(QStringLiteral("--open-queue")))
            initial.insert(QStringLiteral("queueOpen"), true);
        if (!initial.isEmpty())
            qmlEngine.setInitialProperties(initial);
    }

    qmlEngine.loadFromModule("Monolist", "Main");

    // The window is created hidden, so that it loses the system title bar
    // before it is ever drawn with one.
    if (auto *window = qobject_cast<QWindow *>(qmlEngine.rootObjects().value(0))) {
        chrome.attach(window);
        window->show();
    }

    // --play <videoId> [seconds]
    //
    // Drives one playback attempt without anyone touching the interface, so the
    // resolve -> mpv -> artwork path can be exercised under a debugger or in a
    // script. Quits on its own so it cannot hang a test run.
    const QStringList args = app.arguments();
    const int flag = args.indexOf(QStringLiteral("--play"));
    if (flag >= 0 && flag + 1 < args.size()) {
        const QString videoId = args.at(flag + 1);
        const bool numeric = flag + 2 < args.size() && args.at(flag + 2).toInt() > 0;
        const int seconds = numeric ? args.at(flag + 2).toInt() : 20;
        // --again plays the same track a second time halfway through, which
        // should start from the resolver's cache instead of from yt-dlp.
        const bool again = args.contains(QStringLiteral("--again"));
        auto clock = std::make_shared<QElapsedTimer>();

        QObject::connect(&player, &PlaybackController::playbackError, &app,
                         [](const QString &reason) {
                             qWarning("selftest: playback error: %s", qPrintable(reason));
                         });
        QObject::connect(&player, &PlaybackController::statusChanged, &app, [&player, clock]() {
            qWarning("selftest: +%lld ms status=%s source=%s", (long long)clock->elapsed(),
                     qPrintable(player.statusText()), qPrintable(player.sourceLabel()));
        });

        QTimer::singleShot(500, &app, [&player, videoId, clock]() {
            qWarning("selftest: playing %s", qPrintable(videoId));
            clock->start();
            player.playSource(videoId, QStringLiteral("Selftest"), QStringLiteral("Selftest"));
        });
        if (again) {
            QTimer::singleShot(500 + seconds * 500, &app, [&player, videoId, clock]() {
                qWarning("selftest: playing %s again", qPrintable(videoId));
                clock->start();
                player.playSource(videoId, QStringLiteral("Selftest"), QStringLiteral("Selftest"));
            });
        }
        QTimer::singleShot(seconds * 1000, &app, [&player]() {
            // A position that moved is the proof audio was actually decoded.
            qWarning("selftest: position %s of %s, %s",
                     qPrintable(player.positionText()), qPrintable(player.durationText()),
                     player.playing() ? "playing" : "not playing");
            QueueModel *queue = player.queue();
            QStringList upcoming;
            int fromRadio = 0;
            for (int row = queue->currentIndex() + 1; row < queue->rowCount(); ++row) {
                const QVariantMap track = queue->get(row);
                if (track.value(QStringLiteral("fromRadio")).toBool())
                    ++fromRadio;
                if (upcoming.size() < 4)
                    upcoming << track.value(QStringLiteral("title")).toString() + QStringLiteral(" (")
                                    + track.value(QStringLiteral("artist")).toString() + QLatin1Char(')');
            }
            qWarning("selftest: queue %d, %d up next, %d from autoplay: %s", queue->rowCount(),
                     queue->upcomingCount(), fromRadio, qPrintable(upcoming.join(QStringLiteral(" / "))));
            qWarning("selftest: done, quitting");
            QCoreApplication::quit();
        });
    }

    // --diag: what the local database holds, for support and for checking a
    // change end to end. Quits straight away.
    if (args.contains(QStringLiteral("--diag"))) {
        QSqlQuery count(AppDatabase::connection());
        for (const char *table : { "tracks", "downloads", "recent", "history", "settings",
                                   "playlists", "playlist_tracks", "albums", "lyrics" }) {
            const QString name = QString::fromLatin1(table);
            const bool ok = count.exec(QStringLiteral("SELECT COUNT(*) FROM %1").arg(name)) && count.next();
            qWarning("diag: %-9s %s", table, ok ? qPrintable(count.value(0).toString())
                                               : qPrintable(count.lastError().text()));
        }
        qWarning("diag: database %s", qPrintable(AppDatabase::databaseFilePath()));
        qWarning("diag: recently played in the catalog: %d", catalog.recent()->rowCount());
        qWarning("diag: user \"%s\" (%s), %d liked, %d playlists, %d saved albums, %d saved playlists",
                 qPrintable(library.userName()), qPrintable(library.userInitials()),
                 library.liked()->rowCount(), library.playlists()->rowCount(),
                 library.albums()->rowCount(), library.savedPlaylists()->rowCount());
        QTimer::singleShot(0, &app, []() { QCoreApplication::quit(); });
    }

    // --library-test "<query>"
    //
    // The library end to end, on real songs: searches, makes a playlist of the
    // results, adds one twice, likes three, saves Home's newest album, then
    // removes, renames and reports. Meant for a scratch database
    // (MONOLIST_DATA_DIR), which it leaves filled for a look at the views.
    const int libraryFlag = args.indexOf(QStringLiteral("--library-test"));
    if (libraryFlag >= 0 && libraryFlag + 1 < args.size()) {
        const QString query = args.at(libraryFlag + 1);
        auto done = std::make_shared<int>(0);   // the search and Home, both needed

        QObject::connect(&library, &Library::notice, &app, [](const QString &text) {
            qWarning("selftest: notice \"%s\"", qPrintable(text));
        });
        const auto report = [&library]() {
            const QVariantMap open = library.playlist();
            qWarning("selftest: playlist %d \"%s\": %d songs, %s; %d playlists, %d liked, %d saved albums",
                     open.value(QStringLiteral("playlistId")).toInt(),
                     qPrintable(open.value(QStringLiteral("name")).toString()),
                     open.value(QStringLiteral("trackCount")).toInt(),
                     qPrintable(open.value(QStringLiteral("durationText")).toString()),
                     library.playlists()->rowCount(), library.liked()->rowCount(),
                     library.albums()->rowCount());
        };
        const auto finish = [&library, &catalog, report, done]() {
            if (++*done < 2)
                return;
            const QVariantMap featured = catalog.featured();
            if (!featured.isEmpty()) {
                library.setSaved(featured, true);
                qWarning("selftest: saved \"%s\": %s", qPrintable(featured.value(QStringLiteral("title")).toString()),
                         library.isSaved(featured.value(QStringLiteral("browseId")).toString()) ? "yes" : "NO");
            }
            report();
            QTimer::singleShot(600, qApp, []() { QCoreApplication::quit(); });
        };

        QObject::connect(&catalog, &Catalog::homeChanged, &app, [&catalog, finish]() {
            if (!catalog.loading())
                finish();
        });
        QObject::connect(&extractor, &MediaExtractor::searchFinished, &app,
                         [&library, report, finish](const QVariantList &results) {
                             const int id = library.createPlaylist(QStringLiteral("Selftest mix"));
                             library.openPlaylist(id);
                             library.addAllToPlaylist(id, results.mid(0, 8));
                             const bool again = library.addToPlaylist(id, results.value(0).toMap());
                             qWarning("selftest: adding the first song again %s", again ? "ADDED A DUPLICATE" : "was refused");
                             for (int i = 0; i < 3 && i < results.size(); ++i)
                                 library.setLiked(results.at(i).toMap(), true);
                             qWarning("selftest: first song liked: %s", library.isLiked(results.value(0).toMap()
                                      .value(QStringLiteral("sourceId")).toString()) ? "yes" : "NO");
                             report();
                             const int second = library.playlistTracks()->get(1).value(QStringLiteral("entryId")).toInt();
                             library.removeFromPlaylist(id, second);
                             library.renamePlaylist(id, QStringLiteral("Night Drive"));
                             library.setLiked(results.value(2).toMap(), false);
                             report();
                             finish();
                         });
        QTimer::singleShot(300, &app, [&extractor, query]() { extractor.search(query); });
        QTimer::singleShot(30000, &app, []() {
            qWarning("selftest: timed out");
            QCoreApplication::exit(2);
        });
    }

    // --search "<query>"
    //
    // One search as the interface would run it (YouTube Music first, yt-dlp
    // if that fails), timed, with the first results and the suggestions for
    // the first half of the query.
    const int searchFlag = args.indexOf(QStringLiteral("--search"));
    if (searchFlag >= 0 && searchFlag + 1 < args.size()) {
        const QString query = args.at(searchFlag + 1);
        auto clock = std::make_shared<QElapsedTimer>();

        QObject::connect(&extractor, &MediaExtractor::suggestionsChanged, &app, [&extractor, clock]() {
            qWarning("selftest: suggestions after %lld ms: %s", (long long)clock->elapsed(),
                     qPrintable(extractor.suggestions().join(QStringLiteral(" / "))));
        });
        QObject::connect(&extractor, &MediaExtractor::searchFinished, &app,
                         [&extractor, clock](const QVariantList &results) {
                             qWarning("selftest: %lld results from %s in %lld ms", (long long)results.size(),
                                      qPrintable(extractor.source()), (long long)clock->elapsed());
                             for (qsizetype i = 0; i < qMin<qsizetype>(6, results.size()); ++i) {
                                 const QVariantMap r = results.at(i).toMap();
                                 qWarning("selftest:   %s | %s | %s | %s | %s | art: %s",
                                          qPrintable(r.value(QStringLiteral("sourceId")).toString()),
                                          qPrintable(r.value(QStringLiteral("title")).toString()),
                                          qPrintable(r.value(QStringLiteral("artist")).toString()),
                                          qPrintable(r.value(QStringLiteral("album")).toString()),
                                          qPrintable(TrackModel::formatDuration(r.value(QStringLiteral("durationMs")).toLongLong())),
                                          qPrintable(r.value(QStringLiteral("artwork")).toString().left(60)));
                             }
                             QTimer::singleShot(400, qApp, []() { QCoreApplication::quit(); });
                         });
        QObject::connect(&extractor, &MediaExtractor::failed, &app, [](const QString &reason) {
            qWarning("selftest: search failed: %s", qPrintable(reason));
            QCoreApplication::exit(1);
        });

        QTimer::singleShot(300, &app, [&extractor, query, clock]() {
            clock->start();
            extractor.suggest(query.left(qMax<qsizetype>(3, query.size() / 2)));
            extractor.search(query);
        });
        QTimer::singleShot(30000, &app, []() {
            qWarning("selftest: timed out");
            QCoreApplication::exit(2);
        });
    }

    // --download <videoId> [seconds]
    //
    // The same for the offline path: one download through yt-dlp and FFmpeg,
    // reporting progress, and quitting when the file is written, when it fails,
    // or when the time runs out.
    const int downloadFlag = args.indexOf(QStringLiteral("--download"));
    if (downloadFlag >= 0 && downloadFlag + 1 < args.size()) {
        const QString videoId = args.at(downloadFlag + 1);
        const int seconds = (downloadFlag + 2 < args.size()) ? args.at(downloadFlag + 2).toInt() : 120;

        QObject::connect(&downloads, &DownloadManager::progressChanged, &app,
                         [](const QString &, qreal progress) {
                             qWarning("selftest: download %d%%", int(progress * 100));
                         });
        QObject::connect(downloads.queue(), &QAbstractItemModel::dataChanged, &app,
                         [&downloads]() {
                             const QModelIndex first = downloads.queue()->index(0, 0);
                             if (first.isValid() && first.data(DownloadQueueModel::StateRole).toString()
                                                        == QLatin1String("processing"))
                                 qWarning("selftest: %s", qPrintable(first.data(DownloadQueueModel::DetailRole).toString()));
                         });
        QObject::connect(&downloads, &DownloadManager::completed, &app,
                         [](const QString &, const QString &path) {
                             qWarning("selftest: saved %s", qPrintable(path));
                             QCoreApplication::quit();
                         });
        QObject::connect(&downloads, &DownloadManager::failed, &app,
                         [](const QString &, const QString &reason) {
                             qWarning("selftest: download failed: %s", qPrintable(reason));
                             QCoreApplication::exit(1);
                         });

        QTimer::singleShot(500, &app, [&downloads, videoId]() {
            qWarning("selftest: downloading %s as %s (tools: yt-dlp %s, ffmpeg %s, deno %s)",
                     qPrintable(videoId), qPrintable(downloads.format()),
                     downloads.available() ? "yes" : "no",
                     downloads.canConvert() ? "yes" : "no",
                     YtDlp::denoPath().isEmpty() ? "no" : "yes");
            if (downloads.isDownloaded(videoId)) {
                qWarning("selftest: already saved at %s", qPrintable(downloads.localPathFor(videoId)));
                QCoreApplication::quit();
                return;
            }
            downloads.enqueue(videoId, QString(), QString());
        });
        QTimer::singleShot(seconds * 1000, &app, []() {
            qWarning("selftest: timed out");
            QCoreApplication::exit(2);
        });
    }

    return app.exec();
}
