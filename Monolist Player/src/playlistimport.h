#pragma once

#include "importmatcher.h"
#include "importsource.h"

#include <QAbstractListModel>
#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

#include <memory>

class InnerTube;
class Library;

// The rows of one import, as the dialog's live list and the review show
// them: each song as the source listed it, and what became of it.
class ImportRowModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
public:
    enum Role {
        RowIdRole = Qt::UserRole + 1,
        PositionRole,
        TitleRole,
        ArtistRole,
        AlbumRole,
        DurationMsRole,
        StatusRole,
        VideoIdRole,
        ReasonRole,
        CandidateRole,
    };
    struct Item {
        int rowId = 0;
        int position = 0;
        QString title;
        QString artist;
        QString album;
        qint64 durationMs = 0;
        QString status;
        QString videoId;
        QString reason;
        QVariantMap candidate;
    };

    explicit ImportRowModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    int count() const { return int(m_items.size()); }
    // A row as a map with the role names as keys; empty out of range.
    Q_INVOKABLE QVariantMap get(int index) const;

    void reset(const QList<Item> &items);
    void append(const QList<Item> &items);
    void update(const Item &item);

Q_SIGNALS:
    void countChanged();

private:
    QList<Item> m_items;
    QHash<int, int> m_index;   // row id -> index
};

// Importing a playlist from a link, a pasted list or a CSV file (ROADMAP
// X03): the playlist is made at once, and its songs are found on YouTube
// Music one at a time and added in the source's order as they are found.
// Exposed to QML as the "Importer" singleton.
//
// A job, by the owner's rules (importresearch/SPEC.md):
//  - never the user's account: every request anonymous, through a
//    cookieless InnerTube, never counted against AccountGuard, and no
//    Cookie, Authorization or X-Goog-AuthUser header ever, signed in or not;
//  - gentle: one search at a time, 2-3 s apart, at most 600 an hour and
//    2,000 a day; the source's pages 2-3 s apart; the recommender's cover
//    lookups paused while it runs (busy);
//  - any refusal (an error status, an empty answer, a page that is not an
//    answer): a rest of a quarter of an hour, then the same step again; a
//    second in a row pauses the job, and says when it goes on by itself.
//    YouTube Music's refusals are the computer's, not one job's: the rest
//    holds every import (one stopped, or one started meanwhile, waits it out
//    too), and the recommender's covers with them, and the count goes on
//    from one import to the next until YouTube Music answers again;
//  - songs already in the library, and songs matched before by any import
//    (import_matches), are not searched for again;
//  - kept in the database (import_jobs, import_rows), so a job goes on
//    after a restart, a little after the launch; Cancel keeps what was
//    found.
// Sure and likely matches go into the playlist as they are found; one to
// check waits for the user, as does one not found, in the review.
class PlaylistImport : public QObject
{
    Q_OBJECT
    // An import is running, resting or waiting to go on by itself.
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    // The import shown: the one started last, or the one running. See job().
    Q_PROPERTY(QVariantMap job READ job NOTIFY jobChanged)
    // The import going on, or next to go on, whichever is shown: for the
    // sidebar. See activeJob().
    Q_PROPERTY(QVariantMap activeJob READ activeJob NOTIFY activeJobChanged)
    Q_PROPERTY(ImportRowModel *rows READ rows CONSTANT)
    // What preview() read. See previewed().
    Q_PROPERTY(QVariantMap previewed READ previewed NOTIFY previewedChanged)
    // Goes up whenever any import's rows change, so bindings that call
    // review() know to ask again.
    Q_PROPERTY(int revision READ revision NOTIFY revisionChanged)

public:
    struct Timing {
        int searchGapMs = 2000;          // between two searches...
        int searchJitterMs = 1000;       // ...and up to this much more
        int pageGapMs = 2000;            // between two of the source's pages
        int pageJitterMs = 1000;
        qint64 restMs = 15LL * 60 * 1000;    // after a first refusal
        qint64 pauseMs = 60LL * 60 * 1000;   // after a second, doubling to a day
        int perHour = 600;
        int perDay = 2000;
        int resumeDelayMs = 20 * 1000;   // after a launch, before a job left running goes on
    };

    explicit PlaylistImport(Library *library, QObject *parent = nullptr);
    ~PlaylistImport() override;

    void setTiming(const Timing &timing);
    // The jobs a launch finds unfinished go on, resumeDelayMs later.
    void load();
    // For the self-test only: Spotify, JioSaavn, Deezer and Apple Music on
    // this computer (Import::Reader::setTestServer).
    static void setTestServer(const QString &base);

    bool busy() const { return m_busy; }
    // { id, playlistId, name, platform, type, label, url, state, note,
    //   resumeAt, resumeText, total, read, found, likely, check, notFound,
    //   notSearched, dismissed, pending, done, playlistGone, secondsLeft }.
    //   `state` is reading, finding, queued, resting, waiting, paused, done
    //   or cancelled; `note` says why it rests, waits or is paused, and until
    //   when; `resumeAt` is that time (ms since the epoch, 0 for none), and
    //   `resumeText` the same as the note writes it ("14:35", "Sun 14:35");
    //   `total` is what the source says it holds while it is being read, and
    //   the songs read once it is all read; `found` counts the sure and the
    //   user's choices, `likely` the likely; `notFound` includes
    //   `notSearched`, the songs a stop left unasked; `playlistGone` is set
    //   for an import whose playlist was deleted; `secondsLeft` is a rough
    //   guess, the hour's and the day's searches counted.
    QVariantMap job() const { return m_jobMap; }
    // As job(), for the import going on (or resting, or waiting), else the
    // first one paused by hand; empty for none. `others`: how many more
    // imports are not yet done.
    QVariantMap activeJob() const { return m_activeMap; }
    ImportRowModel *rows() { return &m_rows; }
    // { state: "" | "loading" | "ready" | "failed", link, platform, type,
    //   label, name, owner, artwork, count, readable, warnings: [..],
    //   error, sample: [{ title, artist, durationMs }] }. `count` is what
    //   the source says it holds, `readable` how many can be read.
    QVariantMap previewed() const { return m_previewed; }
    int revision() const { return m_revision; }

    // What a link is, as it is typed: { ok, platform, type, kind, label,
    // reason, looksLikeList }. Asks nothing.
    Q_INVOKABLE QVariantMap parse(const QString &text) const;
    // Reads a link's first page (one request; a Spotify list of 100 also
    // asks the public page for its real total) into `previewed`.
    Q_INVOKABLE void preview(const QString &link);
    // A pasted list, or a CSV file (a file: URL or a path), read at once
    // into `previewed`.
    Q_INVOKABLE void previewList(const QString &text);
    Q_INVOKABLE void previewFile(const QString &fileUrl);
    Q_INVOKABLE void clearPreview();
    // Imports what was previewed into a new playlist called `name` (the
    // source's own name when empty). The job's id, 0 when nothing was
    // previewed.
    Q_INVOKABLE int start(const QString &name = QString());
    // The running job, or the one named.
    Q_INVOKABLE void pause(int jobId = 0);
    Q_INVOKABLE void resume(int jobId = 0);
    Q_INVOKABLE void cancel(int jobId = 0);
    // `job` and `rows` show this one.
    Q_INVOKABLE void showJob(int jobId);
    // The latest import into a playlist, 0 for none.
    Q_INVOKABLE int jobForPlaylist(int playlistId) const;
    // Any import as `job` has the one shown, without showing it: a
    // playlist's page, and the toast for one that ends behind another.
    // Empty for none.
    Q_INVOKABLE QVariantMap jobInfo(int jobId) const;
    // What is left to review on a playlist's page: { jobId, name, state,
    // check: [rows], notFound: [rows], notSearched: [rows] }, each row
    // { rowId, position, title, artist, album, durationMs, status, videoId,
    // reason, candidate: { videoId, title, artist, album, artwork,
    // durationMs, isVideo, confidence, reason } }. `notSearched` holds the
    // songs a stop left unasked, apart from those YouTube Music had nothing
    // for. Empty lists when there is nothing.
    Q_INVOKABLE QVariantMap review(int playlistId) const;
    // A stopped import's songs never asked for: looked for after all, as
    // the import would have (the import goes on); or out of the review.
    Q_INVOKABLE void lookForStopped(int jobId);
    Q_INVOKABLE void dismissStopped(int jobId);
    // About how long finding this many songs takes, in seconds: a few
    // seconds a song, and no faster than the hour's and the day's searches
    // allow.
    Q_INVOKABLE int estimateSeconds(int songs) const;
    // Review: the candidate taken, into the playlist at the song's place in
    // the source's order; or a song the user found (a map with the app's
    // track roles: sourceId, title, artist, album, artwork, durationMs).
    Q_INVOKABLE bool useCandidate(int rowId);
    Q_INVOKABLE bool useSong(int rowId, const QVariantMap &track);
    // Not this song: never offered for it again, and the row goes to Not found.
    Q_INVOKABLE void notThis(int rowId);
    // Out of the review.
    Q_INVOKABLE void dismiss(int rowId);
    // What to search for, for Search prefilled: "Title Artist".
    Q_INVOKABLE QString searchTextFor(int rowId) const;
    // The songs not found, one a line ("Artist - Title (3:45)"), on the
    // clipboard; how many. Not those a stop left unasked (notSearched).
    Q_INVOKABLE int copyNotFound(int jobId);

Q_SIGNALS:
    void busyChanged();
    void jobChanged();
    void activeJobChanged();
    void previewedChanged();
    void revisionChanged();
    // A short confirmation, for the toast.
    void notice(const QString &text);
    // An import came to its end: "Imported 'X': 97 of 100 — 2 to check,
    // 1 not found". For the toast with REVIEW.
    void finished(int jobId, const QString &summary);
    // For the self-test and the log: one song settled ("found", "likely",
    // "check", "notfound"), and one search asked.
    void rowSettled(int jobId, int position, const QString &status);
    void searched(const QString &query, bool videos);

private:
    struct Job;
    struct RowData;
    struct Run;
    struct LibrarySong {
        QString title;
        QString artist;
        QString album;
        QString artwork;
        QString videoId;
        int durationSec = 0;
        bool isVideo = false;
    };

    void setBusy(bool busy);
    void scheduleStep(qint64 inMs);
    void step();
    bool nextJob(Job *job);
    void readMore(Job &job);
    void findNext(Job &job);
    void runSearch();
    void conclude();
    void settle(const Job &job, const RowData &row, const QString &status, const QString &videoId,
                const QString &reason, const QVariantMap &candidate);
    void settleFromSource(const Job &job);
    // A refusal: by YouTube Music (`youtube`), for every import; by the
    // source, for this job alone.
    void failed(Job &job, const QString &why, bool youtube);
    void complete(Job &job);
    void finishJob(Job &job, const QString &state, const QString &note);
    bool budgetAllows(Job &job);
    void countSearch();
    void loadBudget();
    // YouTube Music's refusals: until when every import rests (0 for no
    // rest), what a job held by it says, and YouTube Music answering again.
    qint64 restingUntil();
    QString restNote() const;
    qint64 refusedBy(const QString &why);
    void answered();
    void loadRest();
    void saveRest();
    qint64 pauseFor(int refusals) const;
    void hold(Job &job, qint64 until);
    qint64 estimate(qint64 songs, double perSong) const;
    // A warning for a list imported before, or being imported: empty if not.
    QString importedBefore(const Import::Link &link) const;
    void refreshActive();

    bool loadJob(int jobId, Job *job) const;
    void saveJob(const Job &job);
    // A job's rows in the source's order, those `where` says, `limit` at
    // most (0 for all).
    QList<RowData> rowsOf(int jobId, const QString &where = QString(), int limit = 0) const;
    bool rowById(int rowId, RowData *row, Job *job) const;
    int insertRows(Job &job, const QList<Import::Row> &rows, int from);
    void refreshJob(int jobId);
    void touch();
    void buildLibraryIndex();
    bool inLibrary(const ImportMatch::Wanted &wanted, LibrarySong *song) const;
    void noteInLibrary(const LibrarySong &song);
    QStringList cacheKeys(const Job &job, const RowData &row) const;
    bool cached(const QStringList &keys, QVariantMap *candidate, QString *confidence) const;
    void remember(const QStringList &keys, const QVariantMap &candidate, const QString &confidence);
    bool rejected(const QStringList &keys, const QString &videoId) const;
    bool addAt(const Job &job, const RowData &row, const QVariantMap &track);
    QVariantMap jobMap(const Job &job) const;
    static ImportMatch::Wanted wantedOf(const RowData &row);
    static ImportRowModel::Item itemOf(const RowData &row);
    static QVariantMap rowMap(const RowData &row);
    static QVariantMap trackOf(const QVariantMap &candidate);

    Library *m_library = nullptr;
    Timing m_timing;
    InnerTube *m_innerTube = nullptr;
    Import::Reader *m_reader = nullptr;
    QTimer m_timer;
    QElapsedTimer m_clock;
    bool m_busy = false;
    bool m_inFlight = false;     // a request is out
    quint64 m_epoch = 0;         // moved on by a pause or a cancel: what was out is dropped
    int m_activeJob = 0;
    std::unique_ptr<Run> m_run;  // the row being searched for
    // m_clock: no search before this, the gap after the last one answered.
    qint64 m_nextSearchAtMs = -1;
    // Searches made this hour and this day (UTC seconds when each began),
    // kept in settings so a restart does not reset them.
    qint64 m_hourStart = 0;
    int m_hourCount = 0;
    qint64 m_dayStart = 0;
    int m_dayCount = 0;
    // How long each song took to settle, on average, for secondsLeft.
    qint64 m_searchedRows = 0;
    qint64 m_searchedMs = 0;

    // YouTube Music's refusals, for every import at once: until when none
    // asks it (UTC ms; 0 for no rest), how many came in a row with no
    // answer between, and what was said. Kept in settings, beside the
    // search budget, so neither a restart nor a stop forgets them.
    qint64 m_restUntilMs = 0;
    int m_refusals = 0;
    QString m_restWhy;
    bool m_restLoaded = false;

    int m_shownJob = 0;
    QVariantMap m_jobMap;
    QVariantMap m_activeMap;
    ImportRowModel m_rows;
    int m_revision = 0;

    QVariantMap m_previewed;
    Import::Link m_previewLink;
    Import::Page m_previewPage;
    bool m_previewReady = false;
    quint64 m_previewToken = 0;

    QHash<QString, QList<LibrarySong>> m_libraryIndex;   // by title key
    int m_indexedFor = 0;        // the job the index was read for
    bool m_budgetLoaded = false;
};
