#pragma once

#include <QObject>
#include <QPointer>
#include <QSet>
#include <QThread>
#include <QVariantList>
#include <QVariantMap>

#include <functional>

#include "innertube.h"
#include "rec/shelves.h"
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
    // One page. `generation` is handed back with it, so See all can later
    // name which page it is continuing a shelf of.
    void build(quint64 generation, const QVector<Rec::PlayEvent> &history, const Rec::Exclusions &exclude,
               quint64 rotation, const QString &region, const QString &regionName, int perShelf,
               bool hideExplicit);
    // More of one shelf of page `generation`, for See all: `count` rows past
    // `already` (the list so far, as row maps). Nothing when that page is no
    // longer the one held, whose shelves' anchors are gone.
    void more(quint64 generation, int shelf, const QVariantList &already, int count,
              const Rec::Exclusions &exclude, bool hideExplicit);

Q_SIGNALS:
    void loaded(bool ok, int rows, int graphShards, const QString &message);
    void built(quint64 generation, const QVariantList &shelves, bool personal);
    void moreReady(quint64 generation, int shelf, const QVariantList &rows, bool exhausted);

private:
    Rec::Catalog *m_catalog = nullptr;
    Rec::Graph *m_graph = nullptr;
    // The last page built, with each shelf's anchor, and what it was built
    // from: what See all carries on from.
    QVector<Rec::Shelf> m_page;
    QVector<Rec::PlayEvent> m_pageHistory;
    quint64 m_pageGeneration = 0;
};

// The Search tab's recommendations, and everything behind them.
//
// Exposed to QML as "Recs". Shelves are plain data — a title, a reason and a
// list of names — because a catalogue row is a name and nothing else. Nothing
// is resolved to a playable stream until somebody presses one, which is what
// keeps a page of suggestions free.
//
// A row, wherever QML hands one back, is a map with at least `title` and
// `artist` (and `lengthMs` where the graph knew it). A shelf is named by its
// index in `shelves`, and -1 names the See all list that is open.
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
    // The one or two of `shelves` Home shows at its top while its feed is
    // not the YouTube Music account's, as indices into `shelves`, in Home's
    // order (Rec::homePicks); empty when nothing personal was built.
    Q_PROPERTY(QVariantList homeShelves READ homeShelves NOTIFY shelvesChanged)
    Q_PROPERTY(QString dataDirectory READ dataDirectory WRITE setDataDirectory NOTIFY stateChanged)
    // The regional graph shards. Empty means "look beside the catalogue",
    // which is where the iOS project keeps them.
    Q_PROPERTY(QString graphDirectory READ graphDirectory WRITE setGraphDirectory NOTIFY stateChanged)
    Q_PROPERTY(bool graphAvailable READ graphAvailable NOTIFY stateChanged)
    // Settings' "Hide explicit titles": crude or sexual titles, and songs
    // marked as the explicit version, kept off every shelf (Rec::explicitTitle).
    // Off until the listener turns it on; kept in the settings table.
    Q_PROPERTY(bool hideExplicit READ hideExplicit WRITE setHideExplicit NOTIFY hideExplicitChanged)
    // The suggestion being looked up for a press, a menu or Play all — its
    // title and artist joined by a line break — so its row can show that
    // something is happening. Empty when nothing is.
    Q_PROPERTY(QString pendingKey READ pendingKey NOTIFY pendingChanged)
    // The See all list that is open: its shelf's `title`, `reason`, `kind`
    // and `shelf` index, and the rows so far. More comes with loadMore().
    Q_PROPERTY(QVariantMap more READ more NOTIFY moreChanged)
    Q_PROPERTY(QVariantList moreRows READ moreRows NOTIFY moreChanged)
    Q_PROPERTY(bool moreLoading READ moreLoading NOTIFY moreStateChanged)
    // The shelf's anchor has nothing more worth showing.
    Q_PROPERTY(bool moreExhausted READ moreExhausted NOTIFY moreStateChanged)

public:
    explicit Recommender(QObject *parent = nullptr);
    ~Recommender() override;

    // Also where autoplay's radio learns what it may not add.
    void setPlayer(PlaybackController *player);

    bool available() const { return m_rows > 0 || m_graphShards > 0; }
    bool busy() const { return m_busy; }
    QString message() const { return m_message; }
    QVariantList shelves() const { return m_shelves; }
    QVariantList homeShelves() const;
    bool personal() const { return m_personal; }
    QString dataDirectory() const;
    void setDataDirectory(const QString &path);
    QString graphDirectory() const;
    void setGraphDirectory(const QString &path);
    bool graphAvailable() const { return m_graphShards > 0; }
    bool hideExplicit() const { return m_hideExplicit; }
    // Rebuilds the page straight away, so the switch is seen to work.
    void setHideExplicit(bool hide);
    QString pendingKey() const { return m_pendingKey; }
    QVariantMap more() const { return m_more; }
    QVariantList moreRows() const { return m_moreRows; }
    bool moreLoading() const { return m_moreLoading; }
    bool moreExhausted() const { return m_moreExhausted; }

    // Both folders at once, with one reload rather than one each: how the
    // downloaded data (RecData) is put to use. An empty graph means "beside
    // the catalogue".
    void useDirectories(const QString &catalogue, const QString &graph);
    // Stops reading anything inside `folder`: whichever setting points into
    // it is cleared, and the worker lets go of the files. True when there was
    // something to let go, and dataLoaded() will say when it has.
    bool release(const QString &folder);
    // A load is asked of the worker and not yet done: until it is, the worker
    // may still hold whatever it read before, whatever the settings say now.
    // dataLoaded() says when it is done.
    bool loadPending() const { return m_loadsPending > 0; }
    // Whether `path` is `folder` or somewhere under it.
    static bool isInside(const QString &path, const QString &folder);

    // Whether a song was turned down — by id, by name, or by its artist.
    // Autoplay's radio asks this of every song it would add.
    bool unwanted(const QString &videoId, const QString &title, const QString &artist) const;

    // A page changes every this many seconds of the clock (see rotationFor):
    // come back to Search after it and the shelves are drawn afresh, while
    // opening Search twice within it shows exactly the page left.
    static constexpr qint64 kRotationPeriodSecs = 45 * 60;

public Q_SLOTS:
    // Brings the page up to date. Nothing at all when nothing it is built
    // from has changed since the last build — the listening, the country,
    // the explicit switch, the songs kept and turned down, and the rotation —
    // so it is safe to call every time Search opens; queued if a build is
    // already running.
    void refresh();
    // A fresh page, whether or not anything has changed: the next rotation.
    // Search's REFRESH and Settings' REBUILD.
    void rebuild();
    // Plays one suggestion. This is where a name becomes a song: one search,
    // then the ordinary playback path, so it behaves exactly like pressing a
    // search result, because that is what it becomes. The latest press wins.
    void play(const QVariantMap &row);
    // The same search, for a row's menu rather than for playing it at once:
    // Play next, Add to playlist and the rest need a song, and a suggestion
    // is only a name until it is looked up. `purpose` is handed back with the
    // song in resolved(), untouched. As with play(), the latest ask wins.
    void resolve(const QVariantMap &row, const QString &purpose);
    // Plays a whole shelf in order: the first song as soon as it is found,
    // and each of the rest looked up in turn behind it and added to the
    // queue, ahead of anything autoplay finds. Stops the moment the listener
    // plays something else.
    void playAll(int shelf);
    // Opens See all for the shelf `key` names (moreKey): its rows, then more
    // from the same anchor. Kept as it is when that list is already open.
    // After the page has been rebuilt, the same shelf is found by its kind
    // and title; one no longer there is shown as gone (more.gone).
    void openMore(const QString &key);
    void loadMore();
    // "Not interested": the song is recorded as turned down, taken off every
    // shelf and the See all list at once, and never suggested again —
    // Search, See all or autoplay's radio. `track` needs a title and artist;
    // a `sourceId` is kept too, for the radio.
    void notInterested(const QVariantMap &track);
    // "Don't suggest <artist>": the same, for everything by them.
    void dontSuggestArtist(const QString &artist);
    // Takes the last of those two back, while the toast still offers it:
    // the record goes, and the rows come back where they were.
    void undoNotInterested();

public:
    // A shelf's rows as they are now, after anything turned down.
    Q_INVOKABLE QVariantList rowsOf(int shelf) const;
    // What names a shelf's See all in the view history, for openMore: the
    // index alone would name another shelf once the page is drawn again.
    Q_INVOKABLE QString moreKey(int shelf) const;

Q_SIGNALS:
    // What resolve() found, as a track map with the app's track roles.
    void resolved(const QString &purpose, const QVariantMap &track);
    void stateChanged();
    void shelvesChanged();
    void hideExplicitChanged();
    void pendingChanged();
    void moreChanged();
    void moreStateChanged();
    // Rows added to the open See all list, at its end.
    void moreAppended(const QVariantList &rows);
    // A shelf's rows changed in place (-1: the See all list): something
    // turned down taken out, or put back. The page is not rebuilt for it.
    void rowsEdited(int shelf);
    // Something worth a line in the toast: a suggestion that could not be
    // found, usually.
    void notice(const QString &text);
    // A line for the toast that can be taken back with undoNotInterested().
    void undoable(const QString &text);
    // The worker holds exactly what the settings name, with no other load
    // queued behind: any file read before is closed and unmapped.
    void dataLoaded();

private:
    // Rows taken off the page by a "Not interested", and where each stood.
    struct Taken {
        int shelf = 0;
        int index = 0;
        QVariantMap row;
    };
    struct Dismissal {
        qint64 eventId = 0;
        quint64 generation = 0;    // the page the rows were taken from
        quint64 moreSerial = 0;    // and the See all list
        QVector<Taken> taken;
    };
    // Play all, while it is still looking songs up.
    struct Feed {
        quint64 token = 0;
        QVariantList rows;
        int next = 0;
        bool started = false;
        quint64 queue = 0;         // the player's queue it feeds
    };
    using Found = std::function<void(const InnerTube::Track *track, const QString &error)>;

    void setState(bool busy, const QString &message);
    void reload();
    // One suggestion's search, and the result pickResult() chooses.
    void lookUp(const QVariantMap &row, Found done);
    void feedNext();
    void setPending(const QString &key);
    void clearPending(const QString &key);
    // The folder the graph is actually read from: the setting, or the
    // GraphData folder beside the catalogue when the setting is empty.
    QString resolvedGraphDirectory() const;
    // What the listener has turned down, read back from play_events.
    void loadTurnedDown();
    qint64 recordTurnDown(const QString &kind, const QString &videoId, const QString &title,
                          const QString &artist);
    void takeRows(Dismissal &dismissal, const std::function<bool(const QVariantMap &row)> &matches);
    Rec::Exclusions exclusions() const;
    // The rotation for now: this stretch of the clock, and this many
    // REFRESHes since launch.
    quint64 currentRotation() const;

    QThread m_thread;
    RecommenderWorker *m_worker = nullptr;
    QPointer<PlaybackController> m_player;
    InnerTube m_innerTube;

    QVariantList m_shelves;
    bool m_personal = false;
    bool m_busy = false;
    bool m_hideExplicit = false;
    int m_rows = 0;
    int m_graphShards = 0;
    // Loads asked of the worker and not yet answered.
    int m_loadsPending = 0;
    // A refresh asked for while a build was running — a country changed in
    // Settings mid-scan, say. Dropping it would leave the page showing the old
    // country until something else happened to rebuild it.
    bool m_refreshQueued = false;
    // What the page on screen was built from (see refresh()).
    QString m_builtFrom;
    QString m_message;
    // Builds asked for, and the one on screen.
    quint64 m_generation = 0;
    quint64 m_shownGeneration = 0;
    // REFRESHes asked for since launch.
    int m_turn = 0;

    // The songs the library keeps, as of the last build.
    QSet<quint64> m_owned;
    // Everything turned down, by strict key, by video id, and by artist key.
    QSet<quint64> m_turnedDownSongs;
    QSet<QString> m_turnedDownIds;
    QSet<QString> m_turnedDownArtists;
    Dismissal m_lastDismissal;

    // Which press, menu look-up and Play all are the latest.
    quint64 m_playToken = 0;
    quint64 m_resolveToken = 0;
    Feed m_feed;
    QString m_pendingKey;

    QVariantMap m_more;
    QVariantList m_moreRows;
    quint64 m_moreGeneration = 0;
    quint64 m_moreSerial = 0;
    bool m_moreLoading = false;
    bool m_moreExhausted = false;
};
