#pragma once

#include <QAbstractListModel>
#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QVariantMap>

#include "innertube.h"
#include "lyricsquery.h"

class QNetworkAccessManager;
class QNetworkReply;
class ArtistLinks;
class PlaybackController;

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
// Time-synced lyrics come from LRCLIB, a free, open database of them (the
// same one many desktop players use), matched by title, artist and length so
// the timing fits this recording: first its exact lookup, then its search.
// Where LRCLIB has nothing, YouTube Music's own lyrics fill in, as plain text.
// What was found, or that nothing was, is kept in the database, so a song is
// looked up once.
//
// Only an answer is kept for good. One shown because a better source failed
// to answer (LRCLIB down, so YouTube Music's plain text) is kept as
// provisional: shown at once the next time, and asked for again behind it,
// until the better source has answered. "None" is kept only when every source
// answered that it has none; a failure is never taken for a miss. Found lyrics
// are checked again after 60 days, "none" after 3.
//
// Lyrics are only asked for while someone is looking at them (`active`), not
// for every song that plays.
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
public:
    explicit Lyrics(PlaybackController *player, QObject *parent = nullptr);

    QString state() const { return m_state; }
    QString source() const { return m_source; }
    QString error() const { return m_error; }
    LyricsModel *lines() { return &m_lines; }
    int currentLine() const { return m_current; }
    bool active() const { return m_active; }
    void setActive(bool active);

    // LRCLIB is open source and can be run anywhere; this is where to ask.
    void setLrclibUrl(const QString &url);
    // On (the default): LRCLIB's exact lookup first, and its search's
    // entries checked against the song (LyricsQuery::match). Off
    // (lyrics.lrclib=search): its search alone, taken by length, as before.
    void setLrclibExact(bool on) { m_lrclibExact = on; }
    // The names an artist line splits into, for songs kept without their
    // credits (Liked songs, the history). Unset, such a line stays whole.
    void setArtistLinks(ArtistLinks *links) { m_artistLinks = links; }

    // A stored answer is on show while it is asked for again. For the
    // self-tests, which wait for what that ends with.
    bool checking() const { return m_checking; }

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

private:
    // Lyrics as a source gave them, or as the database kept them.
    struct Answer {
        QString synced;
        QString plain;
        QString source;
        bool instrumental = false;
        bool provisional = false;   // kept only because a better source failed
    };
    // Where one LRCLIB request has got to.
    enum class Step { Idle, Pending, Found, Missed, Failed };

    void trackChanged();
    void load(const QVariantMap &track, bool askAgain);
    void checkAgain();

    // The database: what is kept for the song, and keeping an answer.
    enum class Stored { Nothing, Show, ShowAndCheck };
    Stored readStored(Answer *answer) const;
    void store(const Answer &answer);
    void touchStored();

    // LRCLIB: /api/get, then /api/search, under one deadline.
    void startLrclib();
    void askExact();
    void askSearch(bool loose);
    void countBytes(QNetworkReply *reply);
    void handleExact(QNetworkReply *reply);
    void handleSearch(QNetworkReply *reply);
    void settleLrclib();
    void endLrclib(const QString &outcome);
    void askYouTube();

    // How a lookup ends: found, nothing anywhere, or not known.
    void settle(const Answer &answer, const QString &why = QString());
    void settleNone();
    void settleError(const QString &error);
    void keepStored();

    void show(const Answer &answer);
    void setState(const QString &state);
    void cancelRequest();
    void updateCurrentLine();

    PlaybackController *m_player;
    QNetworkAccessManager *m_network;
    ArtistLinks *m_artistLinks = nullptr;
    InnerTube m_innerTube;
    LyricsModel m_lines;

    QString m_lrclib = QStringLiteral("https://lrclib.net");
    bool m_lrclibExact = true;
    QVariantMap m_track;         // the song being looked up or shown
    QString m_videoId;           // its id: answers for any other are dropped
    LyricsQuery::Query m_query;  // what it is looked up as
    // Counts lookups, so an answer to an earlier one, even for the same song,
    // is dropped.
    quint64 m_generation = 0;
    QString m_state = QStringLiteral("idle");
    QString m_source;
    QString m_error;
    int m_current = -1;
    bool m_active = false;

    // The kept answer on show, when there is one: what stays if asking again
    // finds nothing better. `m_checking` while it is being asked for again.
    Answer m_stored;
    bool m_hasStored = false;
    bool m_checking = false;
    QTimer m_recheck;            // a provisional answer on show, asked again

    // This lookup's LRCLIB leg.
    QPointer<QNetworkReply> m_exactRequest;
    QPointer<QNetworkReply> m_searchRequest;
    Step m_exact = Step::Idle;
    Step m_search = Step::Idle;
    bool m_exactUnreachable = false;  // no HTTP answer at all: search would fare no better
    bool m_searchLoose = false;
    bool m_searchEmpty = false;       // the strict search had no entries at all
    bool m_exactSent = false;
    bool m_exactOverdue = false;      // no longer waited for, once the search has an answer
    Answer m_exactAnswer;
    Answer m_searchAnswer;
    QTimer m_hedge;                   // the exact lookup is slow: search as well
    QTimer m_exactWait;               // the search's answer held for the exact one
    QTimer m_deadline;                // LRCLIB as a whole
    QElapsedTimer m_lrclibClock;
    QElapsedTimer m_exactSentClock;
    qint64 m_lrclibBytes = 0;
    QStringList m_lrclibTrail;        // each request's outcome, for the log

    // An LRCLIB match that was not close enough to trust its timing, kept as
    // plain text in case nothing better turns up; and why LRCLIB failed.
    QString m_loosePlain;
    QString m_lrclibError;
};
