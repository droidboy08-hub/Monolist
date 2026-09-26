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
#include <QFileInfo>
#include "rec/graph.h"
#include "rec/shelves.h"
#include "recdata.h"
#include "recommender.h"
#include "rec/taste.h"
#include "rec/vectorsearch.h"
#include "ytdlp.h"

#include <functional>
#include <memory>

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
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
    // Volume, shuffle, repeat and autoplay as they were left: before the
    // queue below is built, so a shuffle left on shuffles it, and before QML
    // reads any of them.
    player.restoreSettings();
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
    // start, and the songs played lately, refreshed whenever a play is
    // recorded — which for a song only loaded is when Play is pressed, not
    // when it became the current track.
    Catalog catalog;
    catalog.refresh();
    catalog.reloadRecent();
    QObject::connect(&player, &PlaybackController::playRecorded, &catalog, &Catalog::reloadRecent);
    QObject::connect(&player, &PlaybackController::playRecorded, &library, &Library::reloadHistory);
    QObject::connect(&library, &Library::historyCleared, &catalog, &Catalog::reloadRecent);
    // Another country's music is a different feed.
    QObject::connect(&library, &Library::regionChanged, &catalog, &Catalog::refresh);

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
    PaletteTool palette(&artworkFetcher);

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
    // Tools installed or updated from Settings are used by downloads at once,
    // not after a restart.
    QObject::connect(&appInfo, &AppInfo::toolsUpdated, &downloads, &DownloadManager::refreshTools);
    // And tools put in place any other way — the setup script run from a
    // terminal, pip, a package manager, a copy into tools/ — are put there
    // from outside the app, so coming back to its window is when to look.
    QObject::connect(&app, &QGuiApplication::applicationStateChanged, &downloads,
                     [&downloads](Qt::ApplicationState state) {
                         if (state == Qt::ApplicationActive)
                             downloads.refreshToolsIfStale();
                     });
    qmlRegisterSingletonInstance("Monolist.Backend", 1, 0, "About",     &appInfo);
    Recommender recommender;
    recommender.setPlayer(&player);
    // "Popular in" follows the country the rest of the app browses as.
    QObject::connect(&library, &Library::regionChanged, &recommender, &Recommender::refresh);
    qmlRegisterSingletonInstance("Monolist.Backend", 1, 0, "Recs",      &recommender);
    // What it recommends from, downloadable from Settings and from an empty
    // Search page.
    RecData recData;
    recData.setRecommender(&recommender);
    qmlRegisterSingletonInstance("Monolist.Backend", 1, 0, "RecData",   &recData);
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

    // --queue-test <videoId> [<videoId> ...]
    //
    // The queue driven the way a listener drives it after a launch: the first
    // song loaded paused, then played, then left with Next while its last
    // second runs out, then gone back to with Previous twice, then paused and
    // moved on with Next before Play is pressed again. What was
    // recorded, and where the clock stood, is reported at each step, so a skip
    // that lets the old song's sound, clock or ending reach the new one shows
    // up as a wrong line. A single id checks the quiet half: a paused song
    // that will not resolve says so on the status line, not in a toast, and
    // Play then tries it again out loud. --early presses Play while the first
    // song is still resolving, which must start it when it arrives. --recover
    // runs the recovery script described below instead.
    const int queueFlag = args.indexOf(QStringLiteral("--queue-test"));
    if (queueFlag >= 0 && queueFlag + 1 < args.size()) {
        QStringList ids;
        for (int i = queueFlag + 1; i < args.size() && !args.at(i).startsWith(QLatin1String("--")); ++i)
            ids << args.at(i);
        const bool early = args.contains(QStringLiteral("--early"));
        auto clock = std::make_shared<QElapsedTimer>();
        clock->start();
        auto toasts = std::make_shared<int>(0);
        const auto say = [clock](const QString &text) {
            qWarning("selftest: +%6lld ms  %s", (long long)clock->elapsed(), qPrintable(text));
        };
        const auto recorded = []() {
            QSqlQuery q(AppDatabase::connection());
            const qint64 plays = q.exec(QStringLiteral("SELECT COALESCE(SUM(play_count), 0) FROM recent"))
                                         && q.next() ? q.value(0).toLongLong() : -1;
            const qint64 events = q.exec(QStringLiteral("SELECT COUNT(*) FROM play_events"))
                                          && q.next() ? q.value(0).toLongLong() : -1;
            return QStringLiteral("recorded: %1 plays, %2 play events").arg(plays).arg(events);
        };
        const auto where = [&player]() {
            return QStringLiteral("on \"%1\" at %2, %3")
                .arg(player.currentTrack().value(QStringLiteral("title")).toString(),
                     player.positionText(),
                     player.playing() ? QStringLiteral("playing") : QStringLiteral("paused"));
        };
        // Polls until `ready` holds, or `limitMs` passes, then carries on.
        const auto waitFor = [](std::function<bool()> ready, int limitMs, std::function<void()> then) {
            auto *poll = new QTimer(qApp);
            auto waited = std::make_shared<QElapsedTimer>();
            waited->start();
            QObject::connect(poll, &QTimer::timeout, qApp, [poll, waited, ready, limitMs, then]() {
                if (!ready() && waited->elapsed() < limitMs)
                    return;
                poll->stop();
                poll->deleteLater();
                then();
            });
            poll->start(100);
        };
        // The clock four times a second while a skip is under way.
        auto *sampler = new QTimer(&app);
        sampler->setInterval(250);
        QObject::connect(sampler, &QTimer::timeout, &app, [say, where]() { say(QStringLiteral("  ") + where()); });

        QObject::connect(&player, &PlaybackController::playbackError, &app, [say, toasts](const QString &reason) {
            ++*toasts;
            say(QStringLiteral("ERROR TOAST: ") + reason);
        });
        QObject::connect(&player, &PlaybackController::notice, &app, [say](const QString &text) {
            say(QStringLiteral("notice: ") + text);
        });
        QObject::connect(&player, &PlaybackController::statusChanged, &app, [say, &player]() {
            say(QStringLiteral("status \"%1\"%2").arg(player.statusText(),
                                                     player.statusError() ? QStringLiteral(" (error)") : QString()));
        });
        QObject::connect(&player, &PlaybackController::currentTrackChanged, &app, [say, &player]() {
            say(QStringLiteral("current track: \"%1\", row %2")
                    .arg(player.currentTrack().value(QStringLiteral("title")).toString())
                    .arg(player.currentIndex()));
        });

        const auto finish = [say, recorded]() {
            say(recorded());
            QSqlQuery q(AppDatabase::connection());
            q.exec(QStringLiteral("SELECT title, listened_ms, track_ms, label FROM play_events ORDER BY id"));
            while (q.next()) {
                say(QStringLiteral("  event \"%1\" heard %2 of %3 s, label %4")
                        .arg(q.value(0).toString())
                        .arg(q.value(1).toLongLong() / 1000).arg(q.value(2).toLongLong() / 1000)
                        .arg(q.value(3).isNull() ? QStringLiteral("-") : q.value(3).toString()));
            }
            say(QStringLiteral("done, quitting"));
            QTimer::singleShot(200, qApp, []() { QCoreApplication::quit(); });
        };

        // --recover <videoId that will not resolve>: the ways back from a
        // failure, the first id being a song that plays. It plays; the broken
        // one is played and fails for good; Play on it tries again and fails
        // again, and neither attempt may record anything, since nothing was
        // heard. Then the good song is played again: the bar must say playing,
        // and Pause must pause it. Last, three copies of the good song are
        // queued and paused, and Next is pressed twice, the second time while
        // the first is still resolving: that stays paused and records nothing.
        const int recoverFlag = args.indexOf(QStringLiteral("--recover"));
        const QString broken = recoverFlag >= 0 && recoverFlag + 1 < args.size()
                                   ? args.at(recoverFlag + 1) : QString();
        const auto recover = [=, &player, &resolver]() {
            if (ids.isEmpty()) {
                say(QStringLiteral("--recover needs a video id that plays before it"));
                finish();
                return;
            }
            const QString good = ids.first();
            // Only the songs asked for: no radio stepping in after a failure.
            player.setAutoplay(false);
            say(QStringLiteral("before: ") + recorded());
            player.playSource(good, QStringLiteral("Good song"), QStringLiteral("Selftest"));
            waitFor([&player]() { return player.position() >= 2000; }, 60000, [=, &player, &resolver]() {
                say(QStringLiteral("playing: ") + where() + QStringLiteral("; ") + recorded());
                player.playSource(broken, QStringLiteral("Broken song"), QStringLiteral("Selftest"));
                waitFor([&player]() { return player.statusError(); }, 90000, [=, &player, &resolver]() {
                    say(QStringLiteral("failed: ") + where() + QStringLiteral("; ") + recorded()
                        + QStringLiteral(" (no more than when playing)"));
                    say(QStringLiteral("Play on the song that failed"));
                    player.play();
                    waitFor([&player]() { return !player.resolving(); }, 90000, [=, &player, &resolver]() {
                        say(QStringLiteral("failed again: ") + where() + QStringLiteral("; ") + recorded()
                            + QStringLiteral("; %1 error toasts").arg(*toasts));
                        player.playSource(good, QStringLiteral("Good song again"), QStringLiteral("Selftest"));
                        waitFor([&player]() { return player.position() >= 2000; }, 60000, [=, &player, &resolver]() {
                            say(QStringLiteral("good again: ") + where() + QStringLiteral(" (must be playing)"));
                            say(QStringLiteral("Play/Pause"));
                            player.togglePlay();
                            QTimer::singleShot(1500, qApp, [=, &player, &resolver]() {
                                const qint64 at = player.position();
                                QTimer::singleShot(1000, qApp, [=, &player, &resolver]() {
                                    say(QStringLiteral("after Play/Pause: %1; the clock moved %2 ms in a second"
                                                       " (must be paused, and about 0)")
                                            .arg(where()).arg(player.position() - at));
                                    QVariantList three;
                                    for (int i = 1; i <= 3; ++i) {
                                        three.append(QVariantMap{
                                            { QStringLiteral("sourceId"), good },
                                            { QStringLiteral("title"), QStringLiteral("Recover test %1").arg(i) },
                                            { QStringLiteral("artist"), QStringLiteral("Selftest") } });
                                    }
                                    player.playTracks(three, 0);
                                    waitFor([&player]() { return player.position() >= 1000; }, 60000,
                                            [=, &player, &resolver]() {
                                        player.pause();
                                        waitFor([&player]() { return !player.playing(); }, 5000,
                                                [=, &player, &resolver]() {
                                            say(QStringLiteral("paused: ") + where() + QStringLiteral("; ")
                                                + recorded());
                                            // Resolved afresh, so the first Next is still resolving when
                                            // the second comes.
                                            resolver.invalidate(good);
                                            player.next();
                                            say(QStringLiteral("Next while paused: ") + where()
                                                + (player.resolving() ? QStringLiteral(", resolving") : QString()));
                                            player.next();
                                            say(QStringLiteral("Next again: ") + where());
                                            waitFor([&player]() { return !player.resolving(); }, 90000, [=]() {
                                                QTimer::singleShot(1500, qApp, [=]() {
                                                    say(QStringLiteral("after both: ") + where() + QStringLiteral("; ")
                                                        + recorded() + QStringLiteral(" (must be paused on Recover"
                                                                                      " test 3, nothing new recorded)"));
                                                    finish();
                                                });
                                            });
                                        });
                                    });
                                });
                            });
                        });
                    });
                });
            });
        };

        QTimer::singleShot(500, &app, [=, &player, &resolver]() {
            if (!broken.isEmpty()) {
                recover();
                return;
            }
            say(QStringLiteral("before: ") + recorded());
            // An empty queue takes its first song paused, as a launch loads
            // the library.
            for (int i = 0; i < ids.size(); ++i) {
                player.addToQueue({ { QStringLiteral("sourceId"), ids.at(i) },
                                    { QStringLiteral("title"), QStringLiteral("Queue test %1").arg(i + 1) },
                                    { QStringLiteral("artist"), QStringLiteral("Selftest") } });
            }
            if (early) {
                say(QStringLiteral("Play while it resolves: ") + where());
                player.play();
            }
            waitFor([&player]() { return !player.resolving(); }, 90000, [=, &player, &resolver]() {
                // Time for mpv to open it, still paused.
                QTimer::singleShot(2000, qApp, [=, &player, &resolver]() {
                    say(QStringLiteral("loaded: ") + where() + QStringLiteral("; ") + recorded()
                        + QStringLiteral("; %1 error toasts").arg(*toasts));
                    if (player.statusError()) {
                        say(QStringLiteral("Play on the song that failed"));
                        player.play();
                        waitFor([&player]() { return !player.resolving(); }, 90000, [=]() {
                            say(QStringLiteral("after Play: ") + where() + QStringLiteral("; ") + recorded()
                                + QStringLiteral("; %1 error toasts").arg(*toasts));
                            finish();
                        });
                        return;
                    }
                    if (early || ids.size() < 2) {
                        finish();
                        return;
                    }
                    player.play();
                    QTimer::singleShot(300, qApp, [=]() { say(QStringLiteral("after Play: ") + recorded()); });
                    waitFor([&player]() { return player.position() >= 4000; }, 60000, [=, &player, &resolver]() {
                        say(QStringLiteral("playing: ") + where());
                        player.setPosition(player.duration() - 1500);
                        QTimer::singleShot(1000, qApp, [=, &player, &resolver]() {
                            // Resolved afresh, not from the prefetch, so the
                            // first song's end falls inside the wait.
                            resolver.invalidate(ids.at(1));
                            say(QStringLiteral("Next, half a second before the end: ") + where());
                            sampler->start();
                            player.next();
                            QTimer::singleShot(7000, qApp, [=, &player]() {
                                sampler->stop();
                                say(QStringLiteral("after Next: ") + where() + QStringLiteral("; ") + recorded());
                                player.previous();
                                say(QStringLiteral("Previous once: ") + where());
                                player.previous();
                                sampler->start();
                                QTimer::singleShot(4000, qApp, [=, &player]() {
                                    sampler->stop();
                                    say(QStringLiteral("after Previous twice: ") + where());
                                    // Next while paused loads without playing:
                                    // nothing recorded until Play. (Once mpv has
                                    // said it paused, which the player follows.)
                                    player.pause();
                                    const auto thenPlay = [=, &player]() {
                                        say(QStringLiteral("Next while paused: ") + where()
                                            + QStringLiteral("; ") + recorded());
                                        player.play();
                                        QTimer::singleShot(1500, qApp, [=]() {
                                            say(QStringLiteral("then Play: ") + where());
                                            finish();
                                        });
                                    };
                                    waitFor([&player]() { return !player.playing(); }, 5000, [=, &player]() {
                                        player.next();
                                        waitFor([&player]() { return !player.resolving(); }, 90000, [thenPlay]() {
                                            QTimer::singleShot(1500, qApp, thenPlay);
                                        });
                                    });
                                });
                            });
                        });
                    });
                });
            });
        });
        QTimer::singleShot(150000, &app, []() {
            qWarning("selftest: timed out");
            QCoreApplication::exit(2);
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

            // What the page learns from, case by case: a like decided by the
            // song's newest like or unlike, song shelves seeded only by plays
            // that said yes, and weekends on the listener's own calendar.
            {
                const QDateTime now = QDateTime::currentDateTimeUtc();
                const auto mark = [](const char *kind, const char *title, const char *artist,
                                     const QDateTime &when) {
                    Rec::PlayEvent event;
                    event.kind = QString::fromLatin1(kind);
                    event.title = QString::fromUtf8(title);
                    event.artist = QString::fromUtf8(artist);
                    event.source = QStringLiteral("search");
                    event.when = when;
                    return event;
                };
                // A label below zero stands for none.
                const auto heard = [&mark](const char *title, const char *artist, const QDateTime &when,
                                           double label, qint64 listenedMs) {
                    Rec::PlayEvent event = mark("play", title, artist, when);
                    event.hasLabel = label >= 0.0;
                    event.label = qMax(0.0, label);
                    event.listenedMs = listenedMs;
                    event.trackMs = 220000;
                    return event;
                };

                // Newest first, as the database reads them.
                const QVector<Rec::PlayEvent> relike = {
                    mark("like", "Kyoto", "Phoebe Bridgers", now.addDays(-1)),
                    mark("unliked", "Kyoto", "Phoebe Bridgers", now.addDays(-2)),
                    mark("like", "Kyoto", "Phoebe Bridgers", now.addDays(-3)) };
                const QVector<Rec::PlayEvent> unliked = { relike.at(1), relike.at(2) };
                const QVector<Rec::PlayEvent> twice = { relike.at(0), relike.at(2) };
                qWarning("rec: likes: liked, unliked, liked again %s (must be liked); liked, unliked %s (must not be)",
                         Rec::likedSongs(relike).isEmpty() ? "not liked" : "liked",
                         Rec::likedSongs(unliked).isEmpty() ? "not liked" : "liked");
                qWarning("rec: like mass: re-liked %.3f (must be > 0), liked twice %.3f (must be the same), "
                         "unliked %.3f (must be 0)",
                         Rec::buildTaste(*catalogue, relike, now).positiveMass,
                         Rec::buildTaste(*catalogue, twice, now).positiveMass,
                         Rec::buildTaste(*catalogue, unliked, now).positiveMass);

                const QVector<Rec::PlayEvent> plays = {
                    heard("Blinding Lights", "The Weeknd", now.addSecs(-600), 0.0, 5000),
                    heard("Levitating", "Dua Lipa", now.addSecs(-1200), 0.0, 5000),
                    heard("Save Your Tears", "The Weeknd", now.addSecs(-1800), -1.0, 10000),
                    heard("Creep", "Radiohead", now.addSecs(-2400), -1.0, 45000),
                    heard("Yellow", "Coldplay", now.addSecs(-3000), 0.6, 120000),
                    mark("like", "Levitating", "Dua Lipa", now.addDays(-1)) };
                QStringList seeded;
                for (const Rec::Shelf &shelf : Rec::buildShelves(*catalogue, Rec::TasteProfile(), plays, 12)) {
                    if (shelf.kind == QLatin1String("song"))
                        seeded << shelf.title;
                }
                qWarning("rec: song shelves: %s (must be Levitating, skipped but liked; Creep, 45 s unlabelled; "
                         "Yellow, 0.6 — not the skipped Blinding Lights or the 10-second Save Your Tears)",
                         qPrintable(seeded.join(QStringLiteral(" | "))));

                // A week back, so every day is past: that week's Friday to
                // Monday, at hours that land on another day in UTC for most
                // of the world.
                const QDate today = QDate::currentDate();
                const QDate friday = today.addDays(-7 - (today.dayOfWeek() + 2) % 7);
                const auto at = [](const QDate &day, int hour) {
                    return QDateTime(day, QTime(hour, 0)).toUTC();
                };
                const QVector<Rec::PlayEvent> weekend = {
                    heard("Blinding Lights", "The Weeknd", at(friday.addDays(1), 1), 1.0, 200000),
                    heard("Levitating", "Dua Lipa", at(friday.addDays(2), 22), 1.0, 200000) };
                const QVector<Rec::PlayEvent> weekdays = {
                    heard("Blinding Lights", "The Weeknd", at(friday, 22), 1.0, 200000),
                    heard("Levitating", "Dua Lipa", at(friday.addDays(3), 1), 1.0, 200000) };
                qWarning("rec: weekend mass: Saturday 01:00 and Sunday 22:00 local %.2f (must be > 0); "
                         "Friday 22:00 and Monday 01:00 local %.2f (must be 0 where the weekend is Sat-Sun)",
                         Rec::buildTaste(*catalogue, weekend, now).weekendMass,
                         Rec::buildTaste(*catalogue, weekdays, now).weekendMass);
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

    // --rec-download [--cancel-at <MB>]
    //
    // The recommendation data fetched exactly as Settings fetches it: into
    // the data folder (MONOLIST_DATA_DIR), from MONOLIST_REC_DATA_URL when
    // that is set. Reports each step and what became of every file, then the
    // recommender loading from the download and the shelves it builds, and
    // quits. --cancel-at stops it once that many megabytes are in, as Cancel.
    //
    // --rec-remove
    //
    // Remove, as Settings does it, a few seconds after launch so the
    // recommender is holding the files: it must let go, and the folder go.
    if (args.contains(QStringLiteral("--rec-download")) || args.contains(QStringLiteral("--rec-remove"))) {
        auto clock = std::make_shared<QElapsedTimer>();
        clock->start();
        const auto say = [clock](const QString &text) {
            qWarning("recdata: +%6lld ms  %s", (long long)clock->elapsed(), qPrintable(text));
        };
        const auto onDisk = [&recData]() {
            return QStringLiteral("installed %1, partial %2, %3 on disk, in use %4")
                .arg(recData.installed() ? QStringLiteral("yes") : QStringLiteral("no"),
                     recData.partial() ? QStringLiteral("yes") : QStringLiteral("no"),
                     recData.sizeText(),
                     recData.inUse() ? QStringLiteral("yes") : QStringLiteral("no"));
        };
        say(QStringLiteral("from %1 into %2").arg(RecData::baseUrl(), recData.displayFolder()));
        say(QStringLiteral("at launch: ") + onDisk());

        auto lastStatus = std::make_shared<QString>();
        auto lastTenth = std::make_shared<int>(-1);
        const int cancelFlag = args.indexOf(QStringLiteral("--cancel-at"));
        const qint64 cancelAt = cancelFlag >= 0 && cancelFlag + 1 < args.size()
                                    ? args.at(cancelFlag + 1).toLongLong() << 20 : -1;
        QObject::connect(&recData, &RecData::changed, &app,
                         [&recData, say, lastStatus, lastTenth, cancelAt]() {
            if (recData.status() != *lastStatus) {
                *lastStatus = recData.status();
                say(QStringLiteral("status: ") + recData.status());
            }
            const int tenth = int(recData.progress() * 10);
            if (recData.busy() && tenth != *lastTenth) {
                *lastTenth = tenth;
                say(QStringLiteral("progress: ") + recData.progressText());
            }
            if (cancelAt >= 0 && recData.busy() && recData.doneBytes() >= cancelAt) {
                say(QStringLiteral("cancelling at ") + recData.progressText());
                recData.cancel();
            }
        });

        // Polls until `ready` holds or `limitMs` passes.
        const auto waitFor = [](std::function<bool()> ready, int limitMs, std::function<void()> then) {
            auto *poll = new QTimer(qApp);
            auto waited = std::make_shared<QElapsedTimer>();
            waited->start();
            QObject::connect(poll, &QTimer::timeout, qApp, [poll, waited, ready, limitMs, then]() {
                if (!ready() && waited->elapsed() < limitMs)
                    return;
                poll->stop();
                poll->deleteLater();
                then();
            });
            poll->start(100);
        };
        const auto reportRecs = [&recommender, say]() {
            QStringList titles;
            for (const QVariant &shelf : recommender.shelves())
                titles << shelf.toMap().value(QStringLiteral("title")).toString();
            say(QStringLiteral("recommender: catalogue \"%1\", available %2, graph %3, %4 shelves: %5%6")
                    .arg(recommender.dataDirectory(),
                         recommender.available() ? QStringLiteral("yes") : QStringLiteral("no"),
                         recommender.graphAvailable() ? QStringLiteral("yes") : QStringLiteral("no"))
                    .arg(recommender.shelves().size())
                    .arg(titles.join(QStringLiteral(" | ")),
                         recommender.message().isEmpty() ? QString()
                                                         : QStringLiteral("; says \"%1\"").arg(recommender.message())));
        };

        if (args.contains(QStringLiteral("--rec-download"))) {
            QObject::connect(&recData, &RecData::finished, &app,
                             [&recData, &recommender, say, onDisk, waitFor, reportRecs](bool ok) {
                say(QStringLiteral("finished %1: %2 files kept, %3 downloaded, %4 refused; %5")
                        .arg(ok ? QStringLiteral("whole") : QStringLiteral("NOT whole"))
                        .arg(recData.keptFiles()).arg(recData.fetchedFiles()).arg(recData.refusedFiles())
                        .arg(onDisk()));
                if (!ok) {
                    QTimer::singleShot(300, qApp, []() { QCoreApplication::quit(); });
                    return;
                }
                // The download points the recommender at itself; the page it
                // then builds is the proof the files are the ones it reads.
                waitFor([&recommender]() {
                            return !recommender.busy() && !recommender.shelves().isEmpty();
                        }, 240000, [reportRecs, say]() {
                            reportRecs();
                            say(QStringLiteral("done, quitting"));
                            QTimer::singleShot(200, qApp, []() { QCoreApplication::quit(); });
                        });
            });
            QTimer::singleShot(500, &app, [&recData]() { recData.download(); });
        } else {
            QTimer::singleShot(4000, &app, [&recData, say, onDisk, waitFor, reportRecs]() {
                reportRecs();
                say(QStringLiteral("removing"));
                recData.remove();
                waitFor([&recData]() { return !recData.removing(); }, 240000,
                        [&recData, say, onDisk, reportRecs]() {
                    say(QStringLiteral("after: ") + onDisk() + QStringLiteral("; folder %1")
                            .arg(QFileInfo::exists(RecData::folderPath()) ? QStringLiteral("STILL THERE")
                                                                           : QStringLiteral("gone")));
                    reportRecs();
                    say(QStringLiteral("done, quitting"));
                    QTimer::singleShot(200, qApp, []() { QCoreApplication::quit(); });
                });
            });
        }
        QTimer::singleShot(600000, &app, []() {
            qWarning("recdata: timed out");
            QCoreApplication::exit(2);
        });
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
    // filter catches instead. Then the same for "Hide explicit titles": what
    // it would hide on top, and its own cases with the switch off and on.
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

            // What "Hide explicit titles" adds on top, written after the rest
            // under a heading of its own, so every word on its list can be
            // checked for what else it catches.
            out << "# refused only with Hide explicit titles on\n";
            int hidden = 0;
            for (int row = 0; row < catalogue->count(); ++row) {
                const QString title = catalogue->title(row);
                const QString artist = catalogue->artist(row);
                if (Rec::suitableForSuggestion(title, artist)
                    && !Rec::suitableForSuggestion(title, artist, /*hideExplicit=*/true)) {
                    ++hidden;
                    out << row << '\t' << artist << '\t' << title << '\t' << catalogue->popularity(row) << '\n';
                }
            }
            qWarning("content: with Hide explicit titles on, %d more of %d rows refused",
                     hidden, catalogue->count());

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

            // "Hide explicit titles", each case twice: with the switch off,
            // where every one of these must pass, and on, where only the
            // clean ones may — the innocent words a careless list would
            // catch, the clean edit beside the explicit one, and each shape
            // of the explicit-version mark.
            struct ExplicitCase { const char *title; const char *artist; bool hiddenWhenOn; };
            const ExplicitCase explicitCases[] = {
                { "Fuck You", "CeeLo Green", true },
                { "Forget You", "CeeLo Green", false },
                { "Bitch Better Have My Money", "Rihanna", true },
                { "Motherfuckin' Hurricane", "Folk Band", true },
                { "Lose Yourself (Explicit)", "Eminem", true },
                { "HUMBLE. [Explicit]", "Kendrick Lamar", true },
                { "In Da Club - Explicit Version", "50 Cent", true },
                { "Gold Digger - Explicit", "Kanye West", true },
                { "Lose Yourself (Clean)", "Eminem", false },
                { "Explicit", "Folk Band", false },
                { "Hijo de Puta", "Asspera", true },
                { "Off With Her Tits", "Allie X", true },
                { "Pussy Cat Pussy Cat", "Nursery Rhymes", false },
                { "What's New Pussycat?", "Tom Jones", false },
                // Caught by an earlier list, and wrongly: Swedish "slut" is
                // an end, Serbo-Croatian "puta" is times, French "p'tits" small.
                { "I ett hus vid skogens slut", "Barnens favoriter", false },
                { "Sto puta", "Zdravko Čolić", false },
                { "Trois p’tits chats", "HeyKids Comptine Pour Bébé", false },
                { "Cum Sancto Spiritu", "Johann Sebastian Bach", false },
                { "Jag fick feeling", "Linnea Henriksson", false },
                { "Moby Dick", "Led Zeppelin", false },
                { "Hoe-Down", "Aaron Copland", false },
                { "Sex on Fire", "Kings of Leon", false },
                { "Scunthorpe Shuffle", "Folk Band", false },
            };
            int explicitWrong = 0;
            for (const ExplicitCase &c : explicitCases) {
                const QString title = QString::fromUtf8(c.title);
                const QString artist = QString::fromUtf8(c.artist);
                const bool off = Rec::suitableForSuggestion(title, artist, /*hideExplicit=*/false);
                const bool on = Rec::suitableForSuggestion(title, artist, /*hideExplicit=*/true);
                if (!off) {
                    ++explicitWrong;
                    qWarning("content: WRONG  %s — %s  (refused with the switch off)", c.artist, c.title);
                }
                if (on == c.hiddenWhenOn) {
                    ++explicitWrong;
                    qWarning("content: WRONG  %s — %s  (%s with the switch on)", c.artist, c.title,
                             on ? "allowed, should be hidden" : "hidden, should be allowed");
                }
            }
            qWarning("content: %d of %d explicit checks wrong (%d cases, switch off and on)", explicitWrong,
                     int(std::size(explicitCases)) * 2, int(std::size(explicitCases)));
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
