#pragma once

#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrlQuery>

#include <functional>
#include <memory>

class QNetworkAccessManager;

// JioSaavn as a second source of sound: the same song, where JioSaavn has it,
// as AAC at up to 320 kbps, which is more than YouTube offers anyone who is not
// paying it. YouTube stays the source of every song and every id; JioSaavn is
// only ever asked "do you have exactly this recording?", and its answer is used
// only when it is certainly yes (see Saavn::choose). A wrong song at 320 kbps
// is far worse than the right one at 128, so everything below leans towards
// refusing.
//
// Written from the protocol as documented (BITCHORD_ENGINE_RESEARCH.md 3.3 and
// 3.6), not from anyone's code. The endpoint, its parameters, the headers and
// the DES key are simply what JioSaavn's own clients send.
namespace Saavn {

// A song in the queue, with as much as matching needs.
struct Target {
    QString videoId;
    QString title;
    QString artist;          // the whole artist line; may join several names
    QString album;
    qint64 durationMs = 0;   // 0 when unknown
};

// One song in a JioSaavn answer.
struct Row {
    QString id;
    QString title;           // entities decoded
    QString album;
    QStringList artists;     // primary and featured, entities decoded
    QString language;        // JioSaavn's own "language", lower case; empty when not given
    int durationSec = 0;     // 0 when not given
    bool explicitContent = false;
    bool has320 = false;     // more_info["320kbps"] == "true"
    QString encryptedUrl;    // more_info.encrypted_media_url
};

// A title taken apart: what must match, what must match both ways, and what
// only breaks ties.
struct Title {
    QString core;            // normalised, without asides, credits or noise
    QStringList versions;    // canonical version markers, sorted: "live", "remix", "part 1 5", "telugu"
    QStringList credits;     // names credited in the title (feat., and "with" inside brackets)
    QStringList versionContext;  // other words beside a version marker: a remixer, a venue
    // Words of an aside that is none of the above ("(Synthwave)", "(Encore)",
    // "(Kehte Hain Khuda)", a bare "(2012)"): nothing says what they mean, so
    // the other side must have them somewhere — its title, artists or album.
    QStringList unknown;
};

// What a match was judged against, as a short hash: a remembered answer is
// used again only for the song as it was when it was asked about, and only
// by the matcher that gave it (matcherVersion goes up whenever the rules
// change, so a wrong match an older build made is not replayed).
QString signature(const Target &target);
int matcherVersion();

// &quot; &amp; &#039; and the rest, which JioSaavn leaves in its titles.
QString decodeEntities(const QString &text);

// more_info.encrypted_media_url: Base64, then DES-ECB under the key every
// JioSaavn client uses, then PKCS#5 unpadding. Empty when it does not decrypt
// to a link on JioSaavn's own CDN (isCdnLink); a plain-HTTP link there is
// moved to HTTPS. encryptMediaUrl is the reverse, for the self-test's fixtures.
QString decryptMediaUrl(const QString &encrypted);
QString encryptMediaUrl(const QString &url);
// HTTPS, on saavncdn.com or a host under it: the only links handed to mpv,
// and the only ones taken back from the saavn_matches table.
bool isCdnLink(const QString &url);

// The rendition a link names at the end of its path ("..._96.mp4"), moved up
// to 320 kbps where the row says that exists.
struct Stream {
    QString url;
    int kbps = 0;            // 0 when the link does not say
    bool readable = false;   // whether the path named a bitrate at all
};
Stream streamFor(const QString &decryptedUrl, bool has320);
// 96 kbps and below is refused: YouTube's own stream is better than that.
// A link that names no bitrate is let through, and the log says so.
bool acceptable(const Stream &stream, QString *why = nullptr);

Row rowFrom(const QJsonObject &song);
// Whether a search answer is one at all: it has a "results" list, empty or
// not. An object without one is an error or a throttle sent as HTTP 200, and
// says nothing about whether JioSaavn has the song.
bool isSearchAnswer(const QJsonObject &root);
QList<Row> rowsFromSearch(const QJsonObject &root);
// song.getDetails answers either {"songs": [...]} or {"<id>": {...}}.
bool rowFromDetails(const QJsonObject &root, const QString &id, Row *row);

QStringList splitArtists(const QString &artistLine);
Title parseTitle(const QString &title, const QStringList &artistNames = {});
// "<title> <lead artist>", cleaned: one search per song.
QString searchQuery(const Target &target);

struct Judgement {
    bool accepted = false;
    QString reason;          // why not
};
Judgement judge(const Target &target, const Row &row);

struct Choice {
    QList<int> accepted;     // indexes into the rows, best first
    QStringList refusals;    // one line per row turned down
    QString reason;          // why nothing, when `accepted` is empty
};
Choice choose(const Target &target, const QList<Row> &rows);

} // namespace Saavn

class JioSaavn : public QObject
{
    Q_OBJECT
public:
    struct Result {
        enum Kind { Match, NoMatch, Failed };
        Kind kind = Failed;
        QString url;             // the stream, for a match
        int kbps = 0;            // 0 when the link does not say
        QString saavnId;
        QString title;           // the row accepted, for the log
        QString artists;
        int durationSec = 0;
        QString reason;          // why not, for NoMatch and Failed
        QStringList refusals;    // every row turned down, and why
        int rows = 0;            // how many rows the search returned
        qint64 elapsedMs = 0;
    };
    using Callback = std::function<void(const Result &)>;

    explicit JioSaavn(QObject *parent = nullptr);

    // X-Forwarded-For and X-Real-IP naming an Indian address, which JioSaavn
    // reads as where the request comes from: some songs are only offered in
    // India. On unless the listener turns it off in Settings.
    void setIndiaHeaders(bool on) { m_indiaHeaders = on; }
    bool indiaHeaders() const { return m_indiaHeaders; }

    // One search, and a song.getDetails only for a row that came without its
    // link. `done` is called exactly once, on this object's thread.
    void lookup(const Saavn::Target &target, Callback done);

private:
    struct Lookup;
    using Answer = std::function<void(const QJsonObject &root, const QString &error)>;
    void get(const QUrlQuery &query, Answer done);
    void tryNext(const std::shared_ptr<Lookup> &state);
    void finish(const std::shared_ptr<Lookup> &state, Result result);

    QNetworkAccessManager *m_network;
    bool m_indiaHeaders = true;
};
