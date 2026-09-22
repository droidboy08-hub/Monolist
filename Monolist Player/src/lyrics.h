#pragma once

#include <QAbstractListModel>
#include <QObject>
#include <QPointer>
#include <QVariantMap>

#include "innertube.h"

class QNetworkAccessManager;
class QNetworkReply;
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
// the timing fits this recording. Where LRCLIB has nothing, YouTube Music's own
// lyrics fill in, as plain text. What was found, or that nothing was, is kept
// in the database, so a song is looked up once.
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
    void trackChanged();
    void load(const QVariantMap &track, bool askAgain);
    bool loadStored();
    void searchLrclib(bool loose);
    void handleLrclib(QNetworkReply *reply, bool loose);
    void askYouTube();
    void finish(const QString &synced, const QString &plain, const QString &source, bool instrumental);
    void show(const QString &synced, const QString &plain, const QString &source, bool instrumental);
    void setState(const QString &state);
    void cancelRequest();
    void updateCurrentLine();

    PlaybackController *m_player;
    QNetworkAccessManager *m_network;
    InnerTube m_innerTube;
    LyricsModel m_lines;
    QPointer<QNetworkReply> m_request;

    QString m_lrclib = QStringLiteral("https://lrclib.net");
    QVariantMap m_track;         // the song being looked up or shown
    QString m_videoId;           // its id: answers for any other are dropped
    QString m_state = QStringLiteral("idle");
    QString m_source;
    QString m_error;
    int m_current = -1;
    bool m_active = false;

    // An LRCLIB match that was not close enough to trust its timing, kept as
    // plain text in case nothing better turns up; and why LRCLIB failed.
    QString m_loosePlain;
    QString m_lrclibError;
};
