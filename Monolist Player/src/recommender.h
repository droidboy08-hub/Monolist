#pragma once

#include <QObject>
#include <QPointer>
#include <QThread>
#include <QVariantList>

#include "innertube.h"
#include "rec/taste.h"

class PlaybackController;

namespace Rec { class Catalog; class Graph; }

// Does the scanning, on a thread of its own. A shelf costs a full pass over
// 400,000 rows — about 120 ms each, and a page is several — which is an
// eternity to hold the interface still for.
class RecommenderWorker : public QObject
{
    Q_OBJECT
public:
    explicit RecommenderWorker(QObject *parent = nullptr);
    ~RecommenderWorker() override;

public Q_SLOTS:
    // Both on this thread: a Qt SQL connection belongs to the thread that
    // opened it, and the graph shards are SQLite.
    void load(const QString &catalogueDirectory, const QString &graphDirectory);
    void build(const QVector<Rec::PlayEvent> &history, const QString &region,
               const QString &regionName, int perShelf);

Q_SIGNALS:
    void loaded(bool ok, int rows, int graphShards, const QString &message);
    void built(const QVariantList &shelves, bool personal);

private:
    Rec::Catalog *m_catalog = nullptr;
    Rec::Graph *m_graph = nullptr;
};

// The Search tab's recommendations, and everything behind them.
//
// Exposed to QML as "Recs". Shelves are plain data — a title, a reason and a
// list of names — because a catalogue row is a name and nothing else. Nothing
// is resolved to a playable stream until somebody presses one, which is what
// keeps a page of suggestions free.
class Recommender : public QObject
{
    Q_OBJECT
    // Whether there is a catalogue to recommend from at all.
    Q_PROPERTY(bool available READ available NOTIFY stateChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    // Empty when all is well; otherwise why there is nothing to show.
    Q_PROPERTY(QString message READ message NOTIFY stateChanged)
    Q_PROPERTY(QVariantList shelves READ shelves NOTIFY shelvesChanged)
    // False while the page is only the cold-start shelf, so the interface can
    // say so rather than implying these are someone's own recommendations.
    Q_PROPERTY(bool personal READ personal NOTIFY shelvesChanged)
    Q_PROPERTY(QString dataDirectory READ dataDirectory WRITE setDataDirectory NOTIFY stateChanged)
    // The regional graph shards. Empty means "look beside the catalogue",
    // which is where the iOS project keeps them.
    Q_PROPERTY(QString graphDirectory READ graphDirectory WRITE setGraphDirectory NOTIFY stateChanged)
    Q_PROPERTY(bool graphAvailable READ graphAvailable NOTIFY stateChanged)

public:
    explicit Recommender(QObject *parent = nullptr);
    ~Recommender() override;

    void setPlayer(PlaybackController *player);

    bool available() const { return m_rows > 0 || m_graphShards > 0; }
    bool busy() const { return m_busy; }
    QString message() const { return m_message; }
    QVariantList shelves() const { return m_shelves; }
    bool personal() const { return m_personal; }
    QString dataDirectory() const;
    void setDataDirectory(const QString &path);
    QString graphDirectory() const;
    void setGraphDirectory(const QString &path);
    bool graphAvailable() const { return m_graphShards > 0; }

public Q_SLOTS:
    // Brings the page up to date. Nothing at all when neither the listening
    // history nor the country has changed since the last build, so it is safe
    // to call every time Search opens; queued if a build is already running.
    void refresh();
    // The same, but whether or not anything has changed: Settings' button.
    void rebuild();
    // Plays one suggestion. This is where a name becomes a song: one search,
    // then the ordinary playback path, so it behaves exactly like pressing a
    // search result, because that is what it becomes.
    void play(int shelfIndex, int rowIndex);

Q_SIGNALS:
    void stateChanged();
    void shelvesChanged();
    // Something worth a line in the toast: a suggestion that could not be
    // found, usually.
    void notice(const QString &text);

private:
    void setState(bool busy, const QString &message);
    void reload();
    // The folder the graph is actually read from: the setting, or the
    // GraphData folder beside the catalogue when the setting is empty.
    QString resolvedGraphDirectory() const;

    QThread m_thread;
    RecommenderWorker *m_worker = nullptr;
    QPointer<PlaybackController> m_player;
    InnerTube m_innerTube;

    QVariantList m_shelves;
    bool m_personal = false;
    bool m_busy = false;
    int m_rows = 0;
    int m_graphShards = 0;
    // A refresh asked for while a build was running — a country changed in
    // Settings mid-scan, say. Dropping it would leave the page showing the old
    // country until something else happened to rebuild it.
    bool m_refreshQueued = false;
    // What the page on screen was built from: the newest event and the country.
    QString m_builtFrom;
    QString m_message;
    // The suggestion waiting on a search, so its answer is not mistaken for
    // an older one.
    QString m_pendingQuery;
    QString m_pendingTitle;
    qint64 m_pendingLengthMs = -1;
};
