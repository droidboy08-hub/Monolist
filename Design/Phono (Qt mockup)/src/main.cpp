#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QIcon>

#include "appdatabase.h"
#include "library.h"
#include "playbackcontroller.h"
#include "mediaextractor.h"

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("Phono"));
    app.setApplicationName(QStringLiteral("Phono"));
    app.setApplicationDisplayName(QStringLiteral("Phono"));

    // Neutral control style: the design is drawn entirely by the QML components,
    // platform styles would override paddings and colors.
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    AppDatabase database;
    if (!database.open())
        qWarning("Phono: local database unavailable, running with in-memory data only.");
    database.createSchema();
    database.seedSampleDataIfEmpty();

    Library library;
    library.load();

    PlaybackController player;
    player.setQueue(library.tracks());
    player.loadIndex(0);

    MediaExtractor extractor;

    qmlRegisterSingletonInstance("Phono.Backend", 1, 0, "Library", &library);
    qmlRegisterSingletonInstance("Phono.Backend", 1, 0, "Player", &player);
    qmlRegisterSingletonInstance("Phono.Backend", 1, 0, "Extractor", &extractor);

    QQmlApplicationEngine engine;
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed,
                     &app, []() { QCoreApplication::exit(-1); },
                     Qt::QueuedConnection);
    engine.loadFromModule("Phono", "Main");

    return app.exec();
}
