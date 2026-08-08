#include <QFontDatabase>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QTimer>

#include "appdatabase.h"
#include "artworkcache.h"
#include "downloadmanager.h"
#include "library.h"
#include "mediaextractor.h"
#include "mpvengine.h"
#include "playbackcontroller.h"
#include "streamresolver.h"
#include "ytdlp.h"

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
    player.setQueue(library.tracks());
    player.loadIndex(0);

    MediaExtractor extractor;

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
    qmlRegisterUncreatableType<SearchResultModel>(
        "Monolist.Backend", 1, 0, "SearchResultModel",
        QStringLiteral("Obtained from Extractor.results"));

    QQmlApplicationEngine qmlEngine;
    qmlEngine.addImageProvider(QStringLiteral("artwork"), new ArtworkCache(&artworkFetcher));

    QObject::connect(&qmlEngine, &QQmlApplicationEngine::objectCreationFailed,
                     &app, []() { QCoreApplication::exit(-1); },
                     Qt::QueuedConnection);
    qmlEngine.loadFromModule("Monolist", "Main");

    // --play <videoId> [seconds]
    //
    // Drives one playback attempt without anyone touching the interface, so the
    // resolve -> mpv -> artwork path can be exercised under a debugger or in a
    // script. Quits on its own so it cannot hang a test run.
    const QStringList args = app.arguments();
    const int flag = args.indexOf(QStringLiteral("--play"));
    if (flag >= 0 && flag + 1 < args.size()) {
        const QString videoId = args.at(flag + 1);
        const int seconds = (flag + 2 < args.size()) ? args.at(flag + 2).toInt() : 20;

        QObject::connect(&player, &PlaybackController::playbackError, &app,
                         [](const QString &reason) {
                             qWarning("selftest: playback error: %s", qPrintable(reason));
                         });
        QObject::connect(&player, &PlaybackController::statusChanged, &app, [&player]() {
            qWarning("selftest: status=%s source=%s",
                     qPrintable(player.statusText()), qPrintable(player.sourceLabel()));
        });

        QTimer::singleShot(500, &app, [&player, videoId]() {
            qWarning("selftest: playing %s", qPrintable(videoId));
            player.playSource(videoId, QStringLiteral("Selftest"), QStringLiteral("Selftest"));
        });
        QTimer::singleShot(seconds * 1000, &app, []() {
            qWarning("selftest: done, quitting");
            QCoreApplication::quit();
        });
    }

    return app.exec();
}
