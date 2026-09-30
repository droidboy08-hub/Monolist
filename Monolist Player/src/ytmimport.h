#pragma once

#include "albummodel.h"
#include "innertube.h"
#include "mediaextractor.h"

#include <QObject>
#include <QPointer>
#include <QSet>
#include <QTimer>

#include <memory>

class Library;
class YtmSession;

// The YouTube Music account's library, read into Monolist and never written
// back (ROADMAP C04): its liked songs, as the playlist "Liked on YouTube
// Music"; its own playlists, private ones among them; and its history.
//
// Read-only, and kept apart. What is read goes into tables of its own
// (ytm_*, appdatabase.cpp), never into the user's own likes, playlists, saves
// or history, so a sync never changes those, never feeds the recommendations
// and is never scrobbled; and all of it goes when the user signs out. The
// account's playlists are kept as references: their songs are read when one
// is opened (Catalog, with the account), not every playlist at every sync.
//
// Gentle with the account, by the owner's rule (docs/research/
// account-safety.md). Every call goes through the account's guard like any
// other (AccountGuard), and on top of that:
//  - one list at a time, a page at a time, 3-6 s between pages and a longer
//    pause every ten, with Auth::Required, so nothing is ever read signed
//    out in its place;
//  - at most 50 pages of liked songs (5,000) and 20 of playlists; the
//    history is one call;
//  - a list replaces what was kept only when it was read to its end (or to
//    its cap) with no error; an error, a signed-out answer or a rest stops
//    the sync and keeps everything as it was;
//  - on its own at most once in 12 hours, a minute or two after YouTube
//    Music confirms the session, never at the moment of a launch; SYNC NOW
//    at most once in 15 minutes.
//
// Exposed to QML as the "AccountLibrary" singleton.
class YtmImport : public QObject
{
    Q_OBJECT
    // "Import my YouTube Music library" (ytmusic.import_library, "0" for
    // off; on by default). Off, nothing is read, and what was is deleted.
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY enabledChanged)
    // There is an imported library to show: the switch on, a session held
    // for the account it came from, and something read.
    Q_PROPERTY(bool shown READ shown NOTIFY changed)
    // The session it came from has ended: what is shown is what the last
    // sync read, and cannot be read again until the user signs back in.
    Q_PROPERTY(bool stale READ stale NOTIFY changed)
    Q_PROPERTY(bool syncing READ syncing NOTIFY changed)
    Q_PROPERTY(bool canSyncNow READ canSyncNow NOTIFY changed)
    // Where things stand, in a sentence, plain text.
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(int likedCount READ likedCount NOTIFY changed)
    Q_PROPERTY(int playlistCount READ playlistCount NOTIFY changed)
    Q_PROPERTY(int historyCount READ historyCount NOTIFY changed)
    Q_PROPERTY(SearchResultModel *liked READ liked CONSTANT)
    Q_PROPERTY(AlbumModel *playlists READ playlists CONSTANT)
    Q_PROPERTY(SearchResultModel *history READ history CONSTANT)

public:
    struct Timing {
        // After YouTube Music confirms the session, before an automatic sync.
        int startDelayMs = 60 * 1000;
        int startJitterMs = 60 * 1000;
        // Between the pages of a list, and between lists.
        int pageGapMs = 3000;
        int pageJitterMs = 3000;
        // And a longer pause every so many pages.
        int restEveryPages = 10;
        int restMs = 30 * 1000;
        int restJitterMs = 30 * 1000;
        qint64 autoEveryMs = 12LL * 60 * 60 * 1000;
        qint64 manualCooldownMs = 15LL * 60 * 1000;
        int likedPages = 50;       // the first page and 49 more: 5,000 songs
        int playlistPages = 20;
        int historyRows = 200;
    };

    YtmImport(Library *library, YtmSession *session, QObject *parent = nullptr);
    ~YtmImport() override;

    void setTiming(const Timing &timing);

    bool enabled() const { return m_enabled; }
    void setEnabled(bool enabled);
    bool shown() const;
    bool stale() const { return m_stale && shown(); }
    bool syncing() const { return m_run != nullptr; }
    bool canSyncNow() const;
    QString status() const;
    int likedCount() const { return int(m_likedAll.size()); }
    int playlistCount() const { return m_playlists.rowCount(); }
    int historyCount() const { return m_history.rowCount(); }
    SearchResultModel *liked() { return &m_liked; }
    AlbumModel *playlists() { return &m_playlists; }
    SearchResultModel *history() { return &m_history; }

    // Read the library now, as the user asked (Settings' SYNC NOW, the
    // playlist's menu). Refused, with a toast saying why, while there is no
    // confirmed session, the account rests, a sync runs, or one ran in the
    // last 15 minutes.
    Q_INVOKABLE void syncNow();
    // Every song of Liked on YouTube Music, and of the history, in order:
    // what Play, Shuffle and Download take (the table fills a few rows a
    // frame, and may be behind).
    Q_INVOKABLE QVariantList likedTrackList() const;
    Q_INVOKABLE QVariantList historyTrackList() const;
    // Whether a page is one of the account's own (Liked music, or one of its
    // playlists), which Catalog opens with the account so a private one opens.
    Q_INVOKABLE bool isAccountPage(const QString &browseId) const;

    // For a look or a screenshot (--ytm-demo active+library): an invented
    // library, shown as if read, and nothing ever asked. Only under
    // MONOLIST_DATA_DIR, since it is written to the tables it would be read
    // into.
    bool showDemo();

Q_SIGNALS:
    void changed();
    void enabledChanged();
    // A short confirmation, for the toast.
    void notice(const QString &text);
    // How a sync ended: "done", "failed", "stopped" (signed out, the switch
    // off, the account resting). For the self-test and the log.
    void synced(const QString &outcome);

private:
    struct Run;

    void sessionChanged();
    void load();
    void forget(const QString &why);
    void scheduleAuto(qint64 inMs);
    void start(bool manual);
    void stop(const QString &outcome, const QString &why);
    void fail(const QString &why);
    bool current(quint64 run) const;
    // Waits the gap between pages (and the longer pause every ten), then
    // `next`, for the run that asked.
    void after(std::function<void()> next);
    void readLiked(const QString &token);
    void readPlaylists(const QString &token);
    void readHistory();
    void commit();
    void feedLiked();
    QString storedAccount() const;
    qint64 syncedAt() const;
    static QString countText(int n, const char *one, const char *many);

    Library *m_library = nullptr;
    QPointer<YtmSession> m_session;
    Timing m_timing;
    InnerTube m_innerTube{ this, /*warmUp=*/false };
    bool m_enabled = true;
    bool m_demo = false;
    bool m_stale = false;
    bool m_hasData = false;
    QString m_lastError;
    bool m_lastIncomplete = false;
    qint64 m_lastSyncedAt = 0;   // UTC ms of the sync the lists shown come from

    std::unique_ptr<Run> m_run;
    quint64 m_runs = 0;
    QTimer m_autoTimer;

    QList<SearchResultModel::Item> m_likedAll;
    QList<SearchResultModel::Item> m_likedFeed;   // waiting to be shown
    bool m_feedScheduled = false;
    SearchResultModel m_liked;
    SearchResultModel m_history;
    AlbumModel m_playlists{ AlbumModel::AccountPlaylists };
    QSet<QString> m_playlistIds;
};
