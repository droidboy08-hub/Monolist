#include <QElapsedTimer>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
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
#include "innertube.h"
#include "library.h"
#include "lyrics.h"
#include "mediaextractor.h"
#include "mpvengine.h"
#include "playbackcontroller.h"
#include "streamresolver.h"
#include "trackmodel.h"
#include "videosurface.h"
#include "windowchrome.h"
#include "appinfo.h"
#include "rec/catalog.h"
#include "rec/suitable.h"
#include <QTextStream>
#include <QFile>
#include "rec/graph.h"
#include "rec/shelves.h"
#include "recommender.h"
#include "rec/taste.h"
#include "rec/vectorsearch.h"
#include "ytdlp.h"
#ifdef Q_OS_MACOS
#include "macos/mediasession.h"
#endif

#include <clocale>
#include <memory>

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    // Qt sets the process locale from the environment on macOS and Linux, and
    // libmpv refuses to start under any numeric locale but "C" — mpv_create()
    // returns nothing, and the app runs without sound. A Mac set to German, or
    // any launch from a Terminal with LANG set, hit this; Windows never does.
    // Only number formatting in C functions changes: QLocale is unaffected.
    std::setlocale(LC_NUMERIC, "C");
    // Before anything looks for yt-dlp, FFmpeg or Deno.
    YtDlp::extendSearchPath();

    app.setOrganizationName(QStringLiteral("Monolist"));
    app.setApplicationName(QStringLiteral("Monolist"));
    app.setApplicationDisplayName(QStringLiteral("Monolist"));
    // One source for the version, generated from the repository at build time
    // rather than typed in two places that drift apart.
    app.setApplicationVersion(AppInfo().version());

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

    // --set <key> <value>: writes one setting (region, lrclib_url,
    // piped_instances, invidious_instances) before anything reads it.
    {
        const QStringList arguments = app.arguments();
        for (int i = arguments.indexOf(QStringLiteral("--set")); i >= 0 && i + 2 < arguments.size();
             i = arguments.indexOf(QStringLiteral("--set"), i + 1)) {
            library.setSetting(arguments.at(i + 1), arguments.at(i + 2));
            qWarning("Monolist: setting %s = %s", qPrintable(arguments.at(i + 1)), qPrintable(arguments.at(i + 2)));
        }
    }

    // The country to browse, before anything asks YouTube Music. One it does
    // not serve is dropped as soon as it answers 400, rather than leaving
    // every request failing.
    InnerTube::setRegion(library.settingValue(QStringLiteral("region")));
    InnerTube::setRegionRejectedHandler([&library](const QString &code) {
        library.dropRegion(code);
    });

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
    player.setVideoHeight(library.videoQuality());
    QObject::connect(&library, &Library::videoQualityChanged, &player, [&player, &library]() {
        player.setVideoHeight(library.videoQuality());
    });
    // Open with the library queued and its first song ready, not playing.
    player.loadModel(library.tracks(), 0);

    MediaExtractor extractor;

    // Lyrics for the song playing, looked up while the Now Playing view shows.
    Lyrics lyrics(&player);
    lyrics.setLrclibUrl(library.settingValue(QStringLiteral("lrclib_url")));

    // Home's content: YouTube Music's feed and new releases, fetched once at
    // start, and the songs played lately, refreshed whenever one starts.
    Catalog catalog;
    catalog.refresh();
    catalog.reloadRecent();
    QObject::connect(&player, &PlaybackController::currentTrackChanged, &catalog, &Catalog::reloadRecent);
    QObject::connect(&player, &PlaybackController::currentTrackChanged, &library, &Library::reloadHistory);
    QObject::connect(&library, &Library::historyCleared, &catalog, &Catalog::reloadRecent);
    // Another country's music is a different feed.
    QObject::connect(&library, &Library::regionChanged, &catalog, &Catalog::refresh);

    // A completed download becomes a library row; reload so it is playable
    // straight away rather than after a restart.
    QObject::connect(&downloads, &DownloadManager::libraryChanged,
                     library.tracks(), &TrackModel::reload);

    if (!YtDlp::isAvailable()) {
        qWarning("Monolist: yt-dlp not found — search and downloads are disabled, "
                 "and streaming falls back to public instances. %s",
                 qPrintable(YtDlp::installHint()));
    }

    // — QML —
    ArtworkFetcher artworkFetcher;
    PaletteTool palette(&artworkFetcher);

#ifdef Q_OS_MACOS
    // The media keys, Control Center and the lock screen.
    MacMediaSession mediaSession(&player, artworkFetcher.network());
#endif

    qmlRegisterSingletonInstance("Monolist.Backend", 1, 0, "Library",   &library);
    qmlRegisterSingletonInstance("Monolist.Backend", 1, 0, "Player",    &player);
    qmlRegisterSingletonInstance("Monolist.Backend", 1, 0, "Extractor", &extractor);
    qmlRegisterSingletonInstance("Monolist.Backend", 1, 0, "Downloads", &downloads);
    // Not "Palette": QtQuick has a type of that name, which would win.
    qmlRegisterSingletonInstance("Monolist.Backend", 1, 0, "CoverPalette", &palette);
    qmlRegisterSingletonInstance("Monolist.Backend", 1, 0, "Catalog",   &catalog);
    qmlRegisterSingletonInstance("Monolist.Backend", 1, 0, "Lyrics",    &lyrics);
    WindowChrome chrome;
    qmlRegisterSingletonInstance("Monolist.Backend", 1, 0, "Chrome",    &chrome);
    AppInfo appInfo;
    qmlRegisterSingletonInstance("Monolist.Backend", 1, 0, "About",     &appInfo);
    Recommender recommender;
    recommender.setPlayer(&player);
    // "Popular in" follows the country the rest of the app browses as.
    QObject::connect(&library, &Library::regionChanged, &recommender, &Recommender::refresh);
    qmlRegisterSingletonInstance("Monolist.Backend", 1, 0, "Recs",      &recommender);
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
    qmlRegisterUncreatableType<LyricsModel>(
        "Monolist.Backend", 1, 0, "LyricsModel",
        QStringLiteral("Obtained from Lyrics.lines"));
    // The picture, drawn inside the scene wherever a view places it. The type
    // itself belongs to the Monolist module (QML_ELEMENT); this is only which
    // player it draws.
    VideoSurface::setEngine(&engine);

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
        if (arguments.contains(QStringLiteral("--now-playing")))
            initial.insert(QStringLiteral("nowPlayingOpen"), true);
        if (!initial.isEmpty())
            qmlEngine.setInitialProperties(initial);
    }

    qmlEngine.loadFromModule("Monolist", "Main");

    // The window is created hidden, so that it loses the system title bar
    // before it is ever drawn with one.
    if (auto *window = qobject_cast<QWindow *>(qmlEngine.rootObjects().value(0))) {
        chrome.attach(window);
        window->show();
#ifdef Q_OS_MACOS
        // A Mac app outlives its window: closing it (the red button, ⌘W)
        // leaves the music playing, and clicking the Dock icon brings it back.
        // Qt reports that click as the app becoming active, even when it
        // already was. ⌘Q quits.
        app.setQuitOnLastWindowClosed(false);
        QObject::connect(&app, &QGuiApplication::applicationStateChanged, window,
                         [window](Qt::ApplicationState state) {
                             if (state == Qt::ApplicationActive && !window->isVisible())
                                 window->show();
                         });
#endif
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
        // A library song plays under its own name, so the lyrics can be found.
        QVariantMap known;
        if (const int row = library.tracks()->indexOfSource(videoId); row >= 0)
            known = library.tracks()->get(row);
        const QString title = known.value(QStringLiteral("title"), QStringLiteral("Selftest")).toString();
        const QString artist = known.value(QStringLiteral("artist"), QStringLiteral("Selftest")).toString();
        // --at <seconds> jumps there once the audio starts.
        const int atFlag = args.indexOf(QStringLiteral("--at"));
        const qint64 startAt = atFlag >= 0 && atFlag + 1 < args.size() ? args.at(atFlag + 1).toLongLong() * 1000 : 0;
        if (startAt > 0) {
            // Once the position moves: a seek sent before the stream is open
            // is dropped.
            auto jumped = std::make_shared<bool>(false);
            QObject::connect(&player, &PlaybackController::positionChanged, &app, [&player, startAt, jumped]() {
                if (*jumped || player.position() < 500)
                    return;
                *jumped = true;
                player.setPosition(startAt);
            });
        }

        QObject::connect(&player, &PlaybackController::playbackError, &app,
                         [](const QString &reason) {
                             qWarning("selftest: playback error: %s", qPrintable(reason));
                         });
        QObject::connect(&player, &PlaybackController::statusChanged, &app, [&player, clock]() {
            qWarning("selftest: +%lld ms status=%s source=%s", (long long)clock->elapsed(),
                     qPrintable(player.statusText()), qPrintable(player.sourceLabel()));
        });

        // --video plays it as something with a picture, so the video switch
        // in Now Playing is live for it; --switch-at <s> then asks for the
        // picture after that many seconds, as pressing the switch would.
        const bool asVideo = args.contains(QStringLiteral("--video"));
        const int switchFlag = args.indexOf(QStringLiteral("--switch-at"));
        if (switchFlag >= 0 && switchFlag + 1 < args.size()) {
            const int after = args.at(switchFlag + 1).toInt();
            QObject::connect(&player, &PlaybackController::notice, &app, [](const QString &text) {
                qWarning("selftest: notice \"%s\"", qPrintable(text));
            });
            QTimer::singleShot(qMax(1, after) * 1000, &app, [&player]() {
                qWarning("selftest: asking for the video (available: %s)",
                         player.videoAvailable() ? "yes" : "no");
                player.setVideoWanted(true);
            });
        }
        const auto start = [&player, videoId, title, artist, known, asVideo]() {
            player.playSource(videoId, title, artist, known.value(QStringLiteral("artwork")).toString(),
                              known.value(QStringLiteral("durationMs")).toLongLong(),
                              known.value(QStringLiteral("album")).toString(), asVideo);
        };
        QTimer::singleShot(500, &app, [videoId, clock, start]() {
            qWarning("selftest: playing %s", qPrintable(videoId));
            clock->start();
            start();
        });
        if (again) {
            QTimer::singleShot(500 + seconds * 500, &app, [videoId, clock, start]() {
                qWarning("selftest: playing %s again", qPrintable(videoId));
                clock->start();
                start();
            });
        }
        QTimer::singleShot(seconds * 1000, &app, [&player]() {
            // A position that moved is the proof audio was actually decoded.
            qWarning("selftest: position %s of %s, %s",
                     qPrintable(player.positionText()), qPrintable(player.durationText()),
                     player.playing() ? "playing" : "not playing");
            qWarning("selftest: playing \"%s\" by %s; video %s",
                     qPrintable(player.currentTrack().value(QStringLiteral("title")).toString()),
                     qPrintable(player.currentTrack().value(QStringLiteral("artist")).toString()),
                     player.videoPlaying() ? "on" : "off");
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

    // --events
    //
    // What the recommender will be trained on: the listens, how much of each
    // was heard, and the label that follows from it. Worth its own flag
    // because a taste profile that comes out wrong is almost always wrong here
    // first, and this is the only way to look.
    if (args.contains(QStringLiteral("--events"))) {
        QSqlQuery events(AppDatabase::connection());
        if (!events.exec(QStringLiteral(
                "SELECT kind, title, artist, track_ms, listened_ms, completed, skipped, label,"
                " repeat_in_session, source FROM play_events ORDER BY id DESC LIMIT 40"))) {
            qWarning("events: %s", qPrintable(events.lastError().text()));
        } else {
            int shown = 0;
            while (events.next()) {
                const qint64 total = events.value(3).toLongLong();
                const qint64 heard = events.value(4).toLongLong();
                const QVariant label = events.value(7);
                const QString source = events.value(9).toString();
                qWarning("events: %-6s %-30s %-20s %4lld/%-4lld s  label %-4s from %-8s %s%s%s",
                         qPrintable(events.value(0).toString()),
                         qPrintable(events.value(1).toString().left(30)),
                         qPrintable(events.value(2).toString().left(20)),
                         (long long)(heard / 1000), (long long)(total / 1000),
                         label.isNull() ? "-" : qPrintable(QString::number(label.toDouble(), 'f', 1)),
                         source.isEmpty() ? "?" : qPrintable(source),
                         events.value(5).toInt() ? "completed " : "",
                         events.value(6).toInt() ? "skipped " : "",
                         events.value(8).toInt() ? "again" : "");
                ++shown;
            }
            qWarning("events: %d rows", shown);
        }
        QTimer::singleShot(0, &app, []() { QCoreApplication::quit(); });
    }

    // --rec-test <directory>
    //
    // The recommender core against the real catalogue: that it loads, that the
    // match ladder answers, that a scan is fast enough to run behind a view,
    // and that the two answers it must refuse it does refuse.
    const int recFlag = args.indexOf(QStringLiteral("--rec-test"));
    if (recFlag >= 0 && recFlag + 1 < args.size()) {
        const QString directory = args.at(recFlag + 1);
        QElapsedTimer clock;
        clock.start();
        auto *catalogue = new Rec::Catalog;
        if (!catalogue->load(directory)) {
            qWarning("rec: could not load a catalogue from %s", qPrintable(directory));
        } else {
            qWarning("rec: %d rows, %d dims, loaded in %lld ms",
                     catalogue->count(), catalogue->dims(), (long long)clock.elapsed());

            const QList<QPair<QString, QString>> probes = {
                { QStringLiteral("Blinding Lights"), QStringLiteral("The Weeknd") },
                { QStringLiteral("Kyoto"), QStringLiteral("Phoebe Bridgers") },
                // The multi-value tag shape: one NUL-separated artist field.
                { QStringLiteral("Die With A Smile"),
                  QStringLiteral("Bruno Mars") + QChar(u'\0') + QStringLiteral("Lady Gaga") },
                // Must be refused: nothing survives normalisation on either side.
                { QStringLiteral("???"), QString() }
            };
            for (const auto &probe : probes) {
                const Rec::Catalog::Match m = catalogue->match(probe.first, probe.second);
                qWarning("rec: match(\"%s\") -> row %d conf %.2f %s",
                         qPrintable(probe.first), m.row, m.confidence,
                         m.row >= 0 ? qPrintable(catalogue->title(m.row) + QStringLiteral(" — ")
                                                 + catalogue->artist(m.row))
                                    : "(no row)");
            }

            // The taste profile, over a fixed set of listens rather than the
            // database's, so the arithmetic is exercised identically on every
            // machine and any change to it shows up as a changed number here.
            {
                const QDateTime now = QDateTime::currentDateTimeUtc();
                const auto listen = [&now](const char *title, const char *artist,
                                           double label, const char *source, int daysAgo) {
                    Rec::PlayEvent event;
                    event.kind = QStringLiteral("play");
                    event.title = QString::fromUtf8(title);
                    event.artist = QString::fromUtf8(artist);
                    event.source = QString::fromLatin1(source);
                    event.when = now.addDays(-daysAgo);
                    event.hasLabel = true;
                    event.label = label;
                    event.listenedMs = 200000;
                    event.trackMs = 220000;
                    return event;
                };
                const QVector<Rec::PlayEvent> events = {
                    listen("Blinding Lights", "The Weeknd", 1.0, "search", 1),
                    listen("Save Your Tears", "The Weeknd", 1.0, "search", 2),
                    listen("Levitating", "Dua Lipa", 1.0, "home", 3),
                    listen("Don't Start Now", "Dua Lipa", 1.0, "playlist", 5),
                    listen("As It Was", "Harry Styles", 1.0, "search", 6),
                    listen("Watermelon Sugar", "Harry Styles", 0.6, "queue", 8),
                    listen("Physical", "Dua Lipa", 1.0, "home", 9),
                    listen("Peaches", "Justin Bieber", 0.6, "queue", 11),
                    listen("Stay", "The Kid LAROI", 1.0, "search", 12),
                    listen("Bad Habits", "Ed Sheeran", 1.0, "home", 14),
                    // Two rejections, so the negative centroid has something.
                    listen("The Sound of Silence", "Disturbed", 0.0, "queue", 4),
                    listen("Master of Puppets", "Metallica", 0.0, "queue", 7),
                };
                const Rec::TasteProfile taste = Rec::buildTaste(*catalogue, events, now);
                qWarning("rec: taste valid=%s  %d/%d events matched, coverage %.2f",
                         taste.valid ? "yes" : "no", taste.tracksMatched,
                         taste.eventsConsidered, taste.coverage);
                qWarning("rec: mass +%.2f -%.2f recent %.2f", taste.positiveMass,
                         taste.negativeMass, taste.recentMass);
                qWarning("rec: top artists: %s",
                         qPrintable(taste.topArtists.join(QStringLiteral(", "))));
                if (taste.valid) {
                    // What it would now recommend: the catalogue ranked by the
                    // profile, which is the whole point of the thing.
                    QVector<QPair<float, int>> ranked;
                    ranked.reserve(catalogue->count());
                    for (int row = 0; row < catalogue->count(); ++row) {
                        if (catalogue->popularity(row) < 55)
                            continue;
                        ranked.append({ taste.score(catalogue->vector(row), catalogue->dims()), row });
                    }
                    const int show = qMin(8, int(ranked.size()));
                    std::partial_sort(ranked.begin(), ranked.begin() + show, ranked.end(),
                                      [](const QPair<float, int> &a, const QPair<float, int> &b) {
                                          return a.first > b.first;
                                      });
                    for (int i = 0; i < show; ++i) {
                        qWarning("rec:   %.3f  %s — %s", ranked.at(i).first,
                                 qPrintable(catalogue->title(ranked.at(i).second)),
                                 qPrintable(catalogue->artist(ranked.at(i).second)));
                    }
                }
            }

            const Rec::Catalog::Match seed =
                catalogue->match(QStringLiteral("Blinding Lights"), QStringLiteral("The Weeknd"));
            if (seed.row >= 0) {
                QVector<float> query(catalogue->dims());
                std::copy_n(catalogue->vector(seed.row), catalogue->dims(), query.begin());
                clock.restart();
                const QVector<Rec::Hit> near =
                    Rec::nearest(*catalogue, query, /*limit=*/8, /*offset=*/0, /*minPopularity=*/0);
                qWarning("rec: one full scan in %lld ms", (long long)clock.elapsed());
                for (const Rec::Hit &hit : near) {
                    qWarning("rec:   %.4f  %s — %s", hit.score,
                             qPrintable(catalogue->title(hit.row)),
                             qPrintable(catalogue->artist(hit.row)));
                }
            }
        }
        delete catalogue;
        QTimer::singleShot(0, &app, []() { QCoreApplication::quit(); });
    }

    // --graph-test <catalogue directory> <graph directory>
    //
    // Evidence for the regional shelf's filters, across every shard: where the
    // entries that must never be shown sit, whether the music catalogue knows
    // them, and how much of each country survives each filter. The filters
    // were chosen from this output, not guessed.
    const int graphFlag = args.indexOf(QStringLiteral("--graph-test"));
    if (graphFlag >= 0 && graphFlag + 2 < args.size()) {
        auto *catalogue = new Rec::Catalog;
        const bool haveCatalogue = catalogue->load(args.at(graphFlag + 1));
        auto *graph = new Rec::Graph;
        QElapsedTimer clock;
        clock.start();
        const bool haveGraph = graph->open(args.at(graphFlag + 2));
        qWarning("graph: open=%s in %lld ms, %d shards; catalogue=%s",
                 haveGraph ? "yes" : "no", (long long)clock.elapsed(),
                 int(graph->installedCodes().size()), haveCatalogue ? "yes" : "no");

        const auto known = [&](const QString &name) {
            return haveCatalogue && catalogue->match(QString(), name).artistId >= 0;
        };

        // The entries the survey found. Each is looked for in its own region.
        const QList<QPair<QString, QString>> probes = {
            { QStringLiteral("DE"), QStringLiteral("Landser") },
            { QStringLiteral("DE"), QStringLiteral("Adolf Hitler") },
            { QStringLiteral("GB"), QStringLiteral("George Orwell") },
            { QStringLiteral("IN"), QStringLiteral("Irrfan Khan") },
            { QStringLiteral("IN"), QStringLiteral("Cliff Richard") },
            { QStringLiteral("IN"), QStringLiteral("Vethathiri Maharishi") },
        };
        for (const auto &probe : probes) {
            const QVector<Rec::GraphArtist> all = graph->artists(probe.first, 6000);
            int rank = -1;
            for (int i = 0; i < all.size(); ++i) {
                if (all.at(i).name.compare(probe.second, Qt::CaseInsensitive) == 0) {
                    rank = i + 1;
                    qWarning("graph: %s in %s: rank %d, mbid %s, pop %d, conf %.2f, catalogue knows it: %s",
                             qPrintable(probe.second), qPrintable(probe.first), rank,
                             qPrintable(all.at(i).mbid), all.at(i).popularity,
                             all.at(i).confidence, known(probe.second) ? "YES" : "no");
                    break;
                }
            }
            if (rank < 0)
                qWarning("graph: %s not found in %s", qPrintable(probe.second), qPrintable(probe.first));
        }

        // Per region: of the 40 seeds the shelf would read, how many have a
        // rating count that distinguishes them, a confident attribution, and a
        // name the catalogue knows.
        int wouldShow = 0;
        for (const QString &code : graph->installedCodes()) {
            if (code == QLatin1String("global") || code.contains(QLatin1Char('-')))
                continue;
            const QVector<Rec::GraphArtist> seeds = graph->artists(code, 40);
            int rated = 0, confident = 0, catalogued = 0, all3 = 0;
            for (const Rec::GraphArtist &artist : seeds) {
                const bool r = artist.popularity >= 3;
                const bool c = artist.confidence >= 0.8;
                const bool k = known(artist.name);
                rated += r;
                confident += c;
                catalogued += k;
                all3 += (r && c && k);
            }
            wouldShow += all3 >= 12;
            qWarning("graph: %-3s seeds %2d  pop>=3 %2d  conf>=0.8 %2d  catalogue %2d  all three %2d  %s",
                     qPrintable(code), int(seeds.size()), rated, confident, catalogued, all3,
                     all3 >= 12 ? "SHOW" : "hide");
        }
        qWarning("graph: %d countries would show the shelf at 12 artists passing all three", wouldShow);

        // The shelves themselves, as the Search tab would build them, for a
        // spread of countries — and a hard check that no blocked or unvetted
        // name reaches a row anywhere.
        const QStringList never = { QStringLiteral("Landser"), QStringLiteral("Adolf Hitler"),
                                    QStringLiteral("George Orwell"), QStringLiteral("Irrfan Khan") };
        int shown = 0, leaks = 0;
        for (const QString &code : graph->installedCodes()) {
            if (code == QLatin1String("global") || code.contains(QLatin1Char('-')))
                continue;
            const QVector<Rec::Shelf> shelves =
                Rec::buildRegionShelves(*graph, catalogue, code, code, {}, 12);
            if (!shelves.isEmpty())
                ++shown;
            for (const Rec::Shelf &shelf : shelves) {
                for (const Rec::Suggestion &row : shelf.rows) {
                    if (never.contains(row.artist, Qt::CaseInsensitive)) {
                        ++leaks;
                        qWarning("graph: LEAK %s: %s — %s", qPrintable(code),
                                 qPrintable(row.title), qPrintable(row.artist));
                    }
                }
            }
            static const QStringList spotlight = { QStringLiteral("US"), QStringLiteral("DE"),
                                                   QStringLiteral("JP"), QStringLiteral("BR"),
                                                   QStringLiteral("IN") };
            if (spotlight.contains(code)) {
                if (shelves.isEmpty()) {
                    qWarning("graph: %s -> no shelf", qPrintable(code));
                    continue;
                }
                for (const Rec::Shelf &shelf : shelves) {
                    QStringList sample;
                    for (int i = 0; i < shelf.rows.size() && i < 6; ++i)
                        sample << shelf.rows.at(i).artist + QStringLiteral(": ") + shelf.rows.at(i).title;
                    qWarning("graph: %s -> \"%s\" (%d rows): %s", qPrintable(code),
                             qPrintable(shelf.title), int(shelf.rows.size()),
                             qPrintable(sample.join(QStringLiteral(" | "))));
                }
            }
        }
        qWarning("graph: %d countries get shelves; %d blocked or unvetted names reached a row", shown, leaks);

        // Without a catalogue there is nothing to vet with, so there must be nothing.
        const QVector<Rec::Shelf> unvetted =
            Rec::buildRegionShelves(*graph, nullptr, QStringLiteral("DE"), QStringLiteral("DE"), {}, 12);
        qWarning("graph: with no catalogue, DE builds %d shelves (must be 0)", int(unvetted.size()));

        delete graph;
        delete catalogue;
        QTimer::singleShot(0, &app, []() { QCoreApplication::quit(); });
    }

    // --artist-test <catalogue directory> <graph directory>
    //
    // "Because you like <artist>" for a spread of real artists — genres,
    // countries, scripts, a joint credit — plus the safety sweep: every artist
    // the shards link to a blocked entry is used as a seed, and no row may
    // name one. Each seed is given two full listens, which is what it takes
    // to count as liked.
    const int artistFlag = args.indexOf(QStringLiteral("--artist-test"));
    if (artistFlag >= 0 && artistFlag + 2 < args.size()) {
        auto *catalogue = new Rec::Catalog;
        auto *graph = new Rec::Graph;
        const bool ok = catalogue->load(args.at(artistFlag + 1)) && graph->open(args.at(artistFlag + 2));
        qWarning("artist: catalogue and graph %s", ok ? "loaded" : "NOT loaded");

        const QDateTime now = QDateTime::currentDateTimeUtc();
        const auto likedTwice = [&now](const QString &artist) {
            QVector<Rec::PlayEvent> history;
            for (int i = 0; i < 2; ++i) {
                Rec::PlayEvent event;
                event.kind = QStringLiteral("play");
                event.title = QStringLiteral("seed song %1").arg(i);
                event.artist = artist;
                event.source = QStringLiteral("search");
                event.when = now.addDays(-i);
                event.hasLabel = true;
                event.label = 1.0;
                // Heard through: likedArtists reads the playhead, not the label.
                event.trackMs = 200000;
                event.listenedMs = 200000;
                history.append(event);
            }
            return history;
        };
        const auto artistShelf = [&](const QString &artist, const QString &region) {
            const QVector<Rec::PlayEvent> history = likedTwice(artist);
            for (const Rec::Shelf &shelf :
                 Rec::buildShelves(*catalogue, Rec::TasteProfile(), history, 12, graph, region)) {
                if (shelf.kind == QLatin1String("artist"))
                    return shelf;
            }
            return Rec::Shelf();
        };

        const QStringList seeds = {
            QStringLiteral("The Weeknd"), QStringLiteral("Billie Eilish"), QStringLiteral("Taylor Swift"),
            QStringLiteral("Rod Wave"), QStringLiteral("Metallica"), QStringLiteral("Radiohead"),
            QStringLiteral("Daft Punk"), QStringLiteral("Kendrick Lamar"), QStringLiteral("Coldplay"),
            QStringLiteral("BTS"), QStringLiteral("BLACKPINK"), QStringLiteral("Bad Bunny"),
            QStringLiteral("Arijit Singh"), QStringLiteral("Diljit Dosanjh"), QStringLiteral("A. R. Rahman"),
            QStringLiteral("Burna Boy"), QStringLiteral("Rammstein"), QStringLiteral("Perfume"),
            QString::fromUtf8("宇多田ヒカル"), QString::fromUtf8("Beyoncé"),
            QStringLiteral("Bruno Mars & Lady Gaga"), QStringLiteral("Phoebe Bridgers"),
        };
        int fromGraph = 0, soundsLike = 0, none = 0;
        for (const QString &seed : seeds) {
            const Rec::GraphArtist found = graph->findArtist(seed, QStringLiteral("US"));
            const Rec::Shelf shelf = artistShelf(seed, QStringLiteral("US"));
            QStringList sample;
            for (int i = 0; i < shelf.rows.size() && i < 6; ++i)
                sample << shelf.rows.at(i).artist;
            const bool graphShelf = shelf.title.startsWith(QLatin1String("Because"));
            fromGraph += graphShelf;
            soundsLike += !graphShelf && !shelf.rows.isEmpty();
            none += shelf.rows.isEmpty();
            qWarning("artist: %-24s graph %-10s | %-30s %2d rows | %s",
                     qPrintable(seed),
                     found.mbid.isEmpty() ? "not found" : qPrintable(found.region),
                     shelf.rows.isEmpty() ? "(no shelf)" : qPrintable(shelf.title.left(30)),
                     int(shelf.rows.size()), qPrintable(sample.join(QStringLiteral(", "))));
            if (!shelf.rows.isEmpty())
                qWarning("artist:   reason: %s", qPrintable(shelf.reason));
        }
        qWarning("artist: %d from the graph, %d fell back to sounds-like, %d with no shelf",
                 fromGraph, soundsLike, none);

        // The safety sweep. Neighbours of the blocked entries are the artists
        // an edge could lead from, straight to them.
        const QStringList blockedIds = { QStringLiteral("808adccd-e52b-4382-9ff6-70a3b2ab25a2"),
                                         QStringLiteral("8530d70e-778c-4eba-b08d-831d16783c03") };
        const QStringList never = { QStringLiteral("Landser"), QStringLiteral("Adolf Hitler") };
        int swept = 0, leaks = 0;
        // Every shard, not only the German one: an edge to them could sit in
        // any country's table.
        QSet<QString> seedsSwept;
        int edgesToBlocked = 0;
        for (const QString &id : blockedIds) {
            for (const QString &code : graph->installedCodes()) {
                for (const Rec::GraphNeighbour &link : graph->neighbours(id, code, 200)) {
                    ++edgesToBlocked;
                    const QString name = graph->artistName(link.mbid, code);
                    if (name.isEmpty() || seedsSwept.contains(name))
                        continue;
                    seedsSwept.insert(name);
                    ++swept;
                    const Rec::Shelf shelf = artistShelf(name, code);
                    for (const Rec::Suggestion &row : shelf.rows) {
                        if (never.contains(row.artist, Qt::CaseInsensitive)) {
                            ++leaks;
                            qWarning("artist: LEAK via %s: %s", qPrintable(name), qPrintable(row.artist));
                        }
                    }
                }
            }
        }
        qWarning("artist: blocked entries have %d edges across all %d shards; %d artists seeded from them; %d leaks",
                 edgesToBlocked, int(graph->installedCodes().size()), swept, leaks);

        // And the general case, which does not depend on the blocked two having
        // edges at all: seed from the top of the German shard, where they live,
        // and check that every row is credited to an artist the catalogue has.
        int generalSeeds = 0, unvetted = 0;
        for (const Rec::GraphArtist &artist : graph->artists(QStringLiteral("DE"), 150)) {
            const Rec::Shelf shelf = artistShelf(artist.name, QStringLiteral("DE"));
            if (shelf.rows.isEmpty())
                continue;
            ++generalSeeds;
            for (const Rec::Suggestion &row : shelf.rows) {
                if (row.row < 0 || never.contains(row.artist, Qt::CaseInsensitive)) {
                    ++unvetted;
                    qWarning("artist: UNVETTED via %s: %s", qPrintable(artist.name), qPrintable(row.artist));
                }
            }
        }
        qWarning("artist: %d German seeds built shelves; %d rows not from a catalogue credit or blocked",
                 generalSeeds, unvetted);

        delete graph;
        delete catalogue;
        QTimer::singleShot(0, &app, []() { QCoreApplication::quit(); });
    }

    // --find-artist <graph directory> <name>
    //
    // Every shard's copy of an artist, with what decides which copy the
    // "Because you like" shelf reads: the attribution confidence, and how
    // many edges each copy's own chain has. For when a shelf's neighbours look
    // like they came from the wrong country, which is what it usually is.
    const int findFlag = args.indexOf(QStringLiteral("--find-artist"));
    if (findFlag >= 0 && findFlag + 2 < args.size()) {
        auto *graph = new Rec::Graph;
        graph->open(args.at(findFlag + 1));
        const QString wanted = args.at(findFlag + 2);
        for (const QString &code : graph->installedCodes()) {
            for (const Rec::GraphArtist &artist : graph->artists(code, 100000)) {
                if (artist.name.compare(wanted, Qt::CaseInsensitive) != 0 || !artist.region.startsWith(
                        code == QLatin1String("global") ? QStringLiteral("ZZ") : code.left(2)))
                    continue;
                const QVector<Rec::GraphNeighbour> edges = graph->neighbours(artist.mbid, artist.region, 12);
                QStringList names;
                int listening = 0;
                for (int i = 0; i < edges.size() && names.size() < 5; ++i) {
                    listening += edges.at(i).source == QLatin1String("listenbrainz");
                    names << graph->artistName(edges.at(i).mbid, artist.region);
                }
                qWarning("find: %-7s mbid %s region %-6s conf %.2f pop %d | %d edges (%d listening): %s",
                         qPrintable(code), qPrintable(artist.mbid), qPrintable(artist.region),
                         artist.confidence, artist.popularity, int(edges.size()), listening,
                         qPrintable(names.join(QStringLiteral(", "))));
                break;
            }
        }
        const Rec::GraphArtist chosen = graph->findArtist(wanted, QStringLiteral("US"));
        qWarning("find: chosen for a US listener: %s in %s", qPrintable(chosen.mbid), qPrintable(chosen.region));
        delete graph;
        QTimer::singleShot(0, &app, []() { QCoreApplication::quit(); });
    }

    // --content-test <catalogue directory> <report file>
    //
    // The suggestion filter against every row in the catalogue: how many it
    // refuses, every refusal written to the report for review, and a fixed
    // list of cases it must get right in both directions — the hateful titles
    // review found, and the anti-fascist songs and innocent names a careless
    // filter catches instead.
    const int contentFlag = args.indexOf(QStringLiteral("--content-test"));
    if (contentFlag >= 0 && contentFlag + 2 < args.size()) {
        auto *catalogue = new Rec::Catalog;
        if (catalogue->load(args.at(contentFlag + 1))) {
            QFile report(args.at(contentFlag + 2));
            report.open(QIODevice::WriteOnly | QIODevice::Truncate);
            QTextStream out(&report);
            out.setEncoding(QStringConverter::Utf8);
            int refused = 0;
            for (int row = 0; row < catalogue->count(); ++row) {
                const QString title = catalogue->title(row);
                const QString artist = catalogue->artist(row);
                if (!Rec::suitableForSuggestion(title, artist)) {
                    ++refused;
                    out << row << '\t' << artist << '\t' << title << '\t' << catalogue->popularity(row) << '\n';
                }
            }
            qWarning("content: %d of %d rows refused", refused, catalogue->count());

            struct Case { const char *title; const char *artist; bool allowed; };
            const Case cases[] = {
                // Must pass: anti-fascist songs, and names that contain or
                // equal a filtered word without being one.
                { "Nazi Punks Fuck Off", "Dead Kennedys", true },
                { "It's Okay to Punch Nazis", "Cheap Perfume", true },
                { "Alle hassen Nazis", "KAFVKA", true },
                { "Ahista Ahista", "Musarrat Nazir", true },
                { "Ragione E Sentimento", "Maria Nazionale", true },
                { "Teresa & Maria", "Jerry Heil", true },
                { "Ganas De Vivir", "Kike Pavón", true },
                { "Renegade", "Aaryan Shah", true },
                { "No Mercy", "Trannos", true },
                { "Chinkapin Oak", "Folk Band", true },
                // Must pass: false positives an earlier version of the filter
                // produced against the real catalogue.
                { "Hitler muss immer wieder sterben", "Mono & Nikitaman", true },
                { "Good Night White Pride", "Loikaemie", true },
                { "Te bajaré la luna (feat. Kike & Manu)", "Maki", true },
                { "Darth Vader vs Adolf Hitler", "Epic Rap Battles of History", true },
                // Must be refused: what review found on real shelves.
                { "I Went Back In Time And Voted For Hitler", "Anal Cunt", false },
                { "Some Other Song", "Anal Cunt", false },
                { "Faggot", "Mindless Self Indulgence", false },
                { "LIKE A CHINK BITCH (G6)", "Eric Reprid", false },
                { "Anything", "Landser & Friends", false },
            };
            int wrong = 0;
            for (const Case &c : cases) {
                const bool allowed = Rec::suitableForSuggestion(QString::fromUtf8(c.title),
                                                                QString::fromUtf8(c.artist));
                if (allowed != c.allowed) {
                    ++wrong;
                    qWarning("content: WRONG  %s — %s  (%s)", c.artist, c.title,
                             allowed ? "allowed, should be refused" : "refused, should be allowed");
                }
            }
            qWarning("content: %d of %d fixed cases wrong", wrong, int(std::size(cases)));
        }
        delete catalogue;
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

    // --net-test
    //
    // What answers, and how fast: YouTube Music, youtube.com, the lyrics
    // service and yt-dlp. When something "is not responding", this says which
    // layer it was.
    if (args.contains(QStringLiteral("--net-test"))) {
        // Owned by the application: they have to outlive this block, which
        // ends long before the answers arrive.
        auto *music = new InnerTube(&app);
        auto *tube = new InnerTube(&app);
        auto *network = new QNetworkAccessManager(&app);
        auto left = std::make_shared<int>(5);
        const auto report = [left](const QString &what, bool ok, qint64 ms, const QString &detail) {
            qWarning("net: %-24s %-4s %6lld ms  %s", qPrintable(what), ok ? "OK" : "FAIL",
                     (long long)ms, qPrintable(detail));
            if (--*left == 0)
                QTimer::singleShot(100, qApp, []() { QCoreApplication::quit(); });
        };
        const auto clock = []() {
            auto timer = std::make_shared<QElapsedTimer>();
            timer->start();
            return timer;
        };

        // YouTube Music: search, and the home feed.
        auto searchClock = clock();
        QObject::connect(music, &InnerTube::searchFinished, &app,
                         [report, searchClock](const QString &, const QList<InnerTube::Track> &tracks) {
                             report(QStringLiteral("YouTube Music search"), !tracks.isEmpty(),
                                    searchClock->elapsed(), QStringLiteral("%1 songs").arg(tracks.size()));
                         });
        QObject::connect(music, &InnerTube::searchFailed, &app,
                         [report, searchClock](const QString &, const QString &reason) {
                             report(QStringLiteral("YouTube Music search"), false, searchClock->elapsed(), reason);
                         });
        music->search(QStringLiteral("daft punk"), InnerTube::Filter::Songs);

        auto homeClock = clock();
        music->browse(QStringLiteral("FEmusic_home"), [report, homeClock](const QJsonObject &root, const QString &error) {
            const int shelves = int(InnerTube::parseShelves(root).size());
            report(QStringLiteral("YouTube Music home"), error.isEmpty() && shelves > 0, homeClock->elapsed(),
                   error.isEmpty() ? QStringLiteral("%1 shelves").arg(shelves) : error);
        });

        // youtube.com, the fallback search.
        auto tubeClock = clock();
        QObject::connect(tube, &InnerTube::youtubeSearchFinished, &app,
                         [report, tubeClock](const QString &, const QList<InnerTube::Track> &tracks) {
                             report(QStringLiteral("youtube.com search"), !tracks.isEmpty(),
                                    tubeClock->elapsed(), QStringLiteral("%1 videos").arg(tracks.size()));
                         });
        QObject::connect(tube, &InnerTube::youtubeSearchFailed, &app,
                         [report, tubeClock](const QString &, const QString &reason) {
                             report(QStringLiteral("youtube.com search"), false, tubeClock->elapsed(), reason);
                         });
        tube->searchYouTube(QStringLiteral("daft punk"));

        // The lyrics service.
        auto lyricsClock = clock();
        {
            QNetworkRequest request(QUrl(QStringLiteral("https://lrclib.net/api/search?track_name=Get%20Lucky&artist_name=Daft%20Punk")));
            request.setHeader(QNetworkRequest::UserAgentHeader, QByteArrayLiteral("Monolist/0.1 (desktop music player)"));
            request.setTransferTimeout(12000);
            QNetworkReply *reply = network->get(request);
            QObject::connect(reply, &QNetworkReply::finished, &app, [reply, report, lyricsClock]() {
                reply->deleteLater();
                const bool ok = reply->error() == QNetworkReply::NoError;
                const int found = ok ? int(QJsonDocument::fromJson(reply->readAll()).array().size()) : 0;
                report(QStringLiteral("LRCLIB lyrics"), ok && found > 0, lyricsClock->elapsed(),
                       ok ? QStringLiteral("%1 matches").arg(found) : reply->errorString());
            });
        }

        // yt-dlp, which resolves what actually plays.
        auto ytdlpClock = clock();
        if (!YtDlp::isAvailable()) {
            report(QStringLiteral("yt-dlp"), false, 0, QStringLiteral("not installed"));
        } else {
            YtDlpRequest *request = YtDlp::resolveAudio(QStringLiteral("LrM_Y39Gmhk"), &app);
            QObject::connect(request, &YtDlpRequest::succeededJson, &app,
                             [report, ytdlpClock](const QJsonDocument &document) {
                                 const QString url = document.object().value(QStringLiteral("url")).toString();
                                 report(QStringLiteral("yt-dlp stream"), !url.isEmpty(), ytdlpClock->elapsed(),
                                        url.isEmpty() ? QStringLiteral("no url") : QStringLiteral("resolved"));
                             });
            QObject::connect(request, &YtDlpRequest::failed, &app,
                             [report, ytdlpClock](const QString &reason) {
                                 report(QStringLiteral("yt-dlp stream"), false, ytdlpClock->elapsed(), reason);
                             });
        }

        qWarning("net: region %s, testing…", qPrintable(InnerTube::region()));
        QTimer::singleShot(40000, &app, []() {
            qWarning("net: timed out");
            QCoreApplication::exit(2);
        });
    }

    // --lyrics "<query>"
    //
    // Lyrics for the first three songs a search finds, as the Now Playing view
    // gets them, timed; then the first once more, which should come from the
    // database without a request.
    const int lyricsFlag = args.indexOf(QStringLiteral("--lyrics"));
    if (lyricsFlag >= 0 && lyricsFlag + 1 < args.size()) {
        const QString query = args.at(lyricsFlag + 1);
        auto songs = std::make_shared<QVariantList>();
        auto step = std::make_shared<int>(0);
        auto clock = std::make_shared<QElapsedTimer>();

        const auto next = [&lyrics, songs, step, clock]() {
            const int count = int(qMin<qsizetype>(3, songs->size()));
            if (*step > count) {
                QCoreApplication::quit();
                return;
            }
            const QVariantMap song = songs->at(*step < count ? *step : 0).toMap();
            ++*step;
            qWarning("selftest: lyrics for \"%s\" by %s, %s",
                     qPrintable(song.value(QStringLiteral("title")).toString()),
                     qPrintable(song.value(QStringLiteral("artist")).toString()),
                     qPrintable(TrackModel::formatDuration(song.value(QStringLiteral("durationMs")).toLongLong())));
            clock->start();
            lyrics.lookup(song);
        };
        QObject::connect(&lyrics, &Lyrics::stateChanged, &app, [&lyrics, next, clock]() {
            if (lyrics.state() == QLatin1String("loading"))
                return;
            const QList<LyricsModel::Line> &lines = lyrics.lines()->lines();
            qWarning("selftest:   %s in %lld ms, %lld lines, from %s%s", qPrintable(lyrics.state()),
                     (long long)clock->elapsed(), (long long)lines.size(),
                     qPrintable(lyrics.source().isEmpty() ? QStringLiteral("-") : lyrics.source()),
                     qPrintable(lyrics.error().isEmpty() ? QString() : QStringLiteral(" (") + lyrics.error() + QLatin1Char(')')));
            int shown = 0;
            for (const LyricsModel::Line &line : lines) {
                if (line.text.isEmpty())
                    continue;
                qWarning("selftest:     %s %s", qPrintable(line.timeMs >= 0 ? TrackModel::formatDuration(line.timeMs)
                                                                           : QStringLiteral("  -  ")),
                         qPrintable(line.text));
                if (++shown == 3)
                    break;
            }
            QTimer::singleShot(0, qApp, next);
        });
        QObject::connect(&extractor, &MediaExtractor::searchFinished, &app,
                         [songs, next](const QVariantList &results) {
                             *songs = results;
                             next();
                         });
        QTimer::singleShot(300, &app, [&extractor, query]() { extractor.search(query); });
        QTimer::singleShot(60000, &app, []() {
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
