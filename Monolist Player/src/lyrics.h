#pragma once

#include <QAbstractListModel>
#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QTimer>
#include <QVariantMap>

#include <memory>

#include "innertube.h"
#include "lyricsquery.h"
#include "lyrics/lyricsprovider.h"
#include "lyrics/lyricsrace.h"

class QNetworkAccessManager;
class ArtistLinks;
class LrclibProvider;
class PlaybackController;
class YtmLyricsProvider;

// The lines of the lyrics on show. A line of plain lyrics has no time.
class LyricsModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)
public:
    struct Line {
        qint64 timeMs = -1;
        QString text;
    };
    enum Roles { TextRole = Qt::UserRole + 1, TimeRole };

    explicit LyricsModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void replace(const QList<Line> &lines);
    const QList<Line> &lines() const { return m_lines; }

Q_SIGNALS:
    void countChanged();

private:
    QList<Line> m_lines;
};

// Lyrics for the song playing, and which line of them is being sung.
//
// Two providers today, in this order (lyrics/providers/*): LRCLIB, a free,
// open database of time-synced lyrics, matched by title, artist and length so
// the timing fits this recording; and YouTube Music's own lyrics, plain text,
// for the video itself. Both are asked at once and the answers taken in that
// order (LyricsRace): LRCLIB's timed lines show the moment they arrive and
// YouTube Music is called off; YouTube Music's text shows once LRCLIB has
// none, or, when LRCLIB is slow or down, after the race's patience window
// (1.2 s), to be replaced by LRCLIB's timed lines if they still come.
// lyrics.race=serial is the switch back to one after the other.
//
// What was found, or that nothing was, is kept in the database, per song and
// per provider (LyricsStore), so a song is looked up once. Only an answer is
// kept for good. One shown because a better provider failed to answer (LRCLIB
// down, so YouTube Music's plain text) is kept as provisional: shown at once
// the next time, and asked for again behind it, until the better provider has
// answered. "None" is kept only when every provider answered that it has
// none; a failure is never taken for a miss. Found lyrics are checked again
// after 60 days, "none" after 3.
//
// The lines are only put on show while someone is looking at them (`active`).
// The lookups themselves also run in the background (`background`, on by
// default): the song playing is looked up once its sound starts, and the song
// after it once that is done, so opening the lyrics is a read of the database.
// One lookup per song at a time, whoever asked for it (L4).
class Lyrics : public QObject
{
    Q_OBJECT
    // "idle", "loading", "synced", "plain", "instrumental", "none" or "error"
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    // Where the lines came from: "LRCLIB", or "Musixmatch via YouTube Music".
    Q_PROPERTY(QString source READ source NOTIFY stateChanged)
    Q_PROPERTY(QString error READ error NOTIFY stateChanged)
    Q_PROPERTY(LyricsModel *lines READ lines CONSTANT)
    // The line being sung, for synced lyrics; -1 before the first.
    Q_PROPERTY(int currentLine READ currentLine NOTIFY currentLineChanged)
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)
    // "Look up lyrics in the background" (Settings): the song playing and the
    // one after it are looked up whether or not anyone is looking. Off, a
    // song is looked up only once its lyrics are on screen.
    Q_PROPERTY(bool background READ background WRITE setBackground NOTIFY backgroundChanged)
public:
    explicit Lyrics(PlaybackController *player, QObject *parent = nullptr);
    ~Lyrics() override;

    QString state() const { return m_state; }
    QString source() const { return m_source; }
    QString error() const { return m_error; }
    LyricsModel *lines() { return &m_lines; }
    int currentLine() const { return m_current; }
    bool active() const { return m_active; }
    void setActive(bool active);
    bool background() const { return m_background; }
    void setBackground(bool on);

    // LRCLIB is open source and can be run anywhere; this is where to ask.
    void setLrclibUrl(const QString &url);
    // On (the default): LRCLIB's exact lookup first, and its search's
    // entries checked against the song (LyricsQuery::match). Off
    // (lyrics.lrclib=search): its search alone, taken by length, as before.
    void setLrclibExact(bool on);
    // The names an artist line splits into, for songs kept without their
    // credits (Liked songs, the history). Unset, such a line stays whole.
    void setArtistLinks(ArtistLinks *links) { m_artistLinks = links; }
    // How the providers are raced (lyrics.race, lyrics.patience).
    void setRaceOptions(const LyricsRace::Options &options) { m_options = options; }
    LyricsRace::Options raceOptions() const { return m_options; }

    // Looks the queue's songs up as they play: the song playing once its
    // sound starts, and the one after it once that lookup is done (main.cpp
    // turns it on). Not on a metered connection for the song after, and
    // never for a song whose length is not known yet (L8).
    void followQueue();

    // A stored answer is on show while it is asked for again. For the
    // self-tests, which wait for what that ends with.
    bool checking() const { return m_checking; }
    // The best answer in hand is on show while a better provider is still
    // out: it may yet be replaced. For the self-tests, likewise.
    bool interim() const { return m_interim; }
    // A lookup is out for this song, whoever asked for it.
    bool lookingUp(const QString &videoId) const { return m_jobs.contains(videoId); }

    // For the self-tests: the providers to race, in order, in place of the
    // app's own two (not owned); followQueue() with a player that has no
    // engine, which never plays; and the connection taken as metered or not.
    void setProviders(const QList<LyricsProvider *> &providers);
    void setFollowWithoutSound(bool on) { m_followWithoutSound = on; }
    void setMeteredForTest(int metered) { m_meteredForTest = metered; }

    static QList<LyricsModel::Line> parseLrc(const QString &lrc);
    static QList<LyricsModel::Line> plainLines(const QString &text);

public Q_SLOTS:
    // Asks again, past the stored answer: after an error, or for a song whose
    // lyrics were not found before.
    void retry();
    // Plays from the start of a line.
    void seekToLine(int row);
    // Looks up one song without it playing. For the self-test.
    void lookup(const QVariantMap &track);

Q_SIGNALS:
    void stateChanged();
    void currentLineChanged();
    void activeChanged();
    void backgroundChanged();

private:
    // A lookup in flight, and the stored answer it is checking, if any: what
    // stays when asking again finds nothing better.
    struct Job {
        LyricsRace *race = nullptr;
        LyricsAnswer kept;
        bool hasKept = false;
        bool background = false;
    };
    // What a finished lookup leaves on show, once kept.
    struct Settled {
        enum Kind { Show, Error } kind = Show;
        LyricsAnswer answer;
        QString error;
        bool keptStays = false;   // the kept answer is the answer
    };

    void trackChanged();
    void load(const QVariantMap &track, bool askAgain);
    void checkAgain();
    LyricsQuery::Query queryFor(const QVariantMap &track) const;
    QList<LyricsProvider *> providers(bool background) const;

    // The lookups: one per song, the view's and the queue's alike.
    void startRace(const QString &videoId, const LyricsQuery::Query &query, const LyricsAnswer &kept,
                   bool hasKept, bool askAgain, bool background);
    void finishRace(LyricsRace *race);
    Settled commit(const QString &videoId, const LyricsRace &race, const Job &job);
    void dropJob(const QString &videoId);
    void prune();

    // The queue's lookups (followQueue).
    void prefetchQueue();
    bool prefetch(const QVariantMap &track);
    bool metered() const;

    // What is on show.
    void showOffer(LyricsRace *race);
    void display(const Settled &settled);
    void show(const LyricsAnswer &answer);
    void setState(const QString &state);
    void detach();
    void updateCurrentLine();

    PlaybackController *m_player;
    QNetworkAccessManager *m_network;
    ArtistLinks *m_artistLinks = nullptr;
    InnerTube m_innerTube;
    LyricsModel m_lines;

    std::unique_ptr<LrclibProvider> m_lrclib;
    std::unique_ptr<YtmLyricsProvider> m_ytm;
    QList<LyricsProvider *> m_order;   // enabled, in the user's order
    LyricsRace::Options m_options;

    QVariantMap m_track;         // the song being looked up or shown
    QString m_videoId;           // its id
    QString m_state = QStringLiteral("idle");
    QString m_source;
    QString m_error;
    int m_current = -1;
    bool m_active = false;

    // Every lookup in flight, by song, and the one whose answer the view
    // waits on (for m_videoId), if any. A race's answer reaches the view only
    // through m_race: an answer for an earlier song, or an earlier lookup of
    // the same one, is kept but never shown.
    QHash<QString, Job> m_jobs;
    LyricsRace *m_race = nullptr;

    // The answer on show, when there is one: what stays if asking again
    // finds nothing better. `m_checking` while it is being asked for again;
    // `m_interim` while it is the best in hand of a race still running.
    LyricsAnswer m_stored;
    bool m_hasStored = false;
    bool m_checking = false;
    bool m_interim = false;
    QTimer m_recheck;            // a provisional answer on show, asked again

    // The queue's lookups.
    bool m_background = true;
    bool m_following = false;
    bool m_followWithoutSound = false;
    int m_meteredForTest = -1;
    QString m_heard;             // the song whose sound has started
    QVariantMap m_heardTrack;    // ... with the length the engine found
    QSet<QString> m_queueSongs;  // the songs playing and next, whose lookups stay
    // When the queue last asked about a song and it did not end in an
    // answer kept for good: asked again only after a while, so a provider
    // that is down is not asked at every change of the queue.
    QHash<QString, qint64> m_queueTried;
    QElapsedTimer m_sessionClock;
};
