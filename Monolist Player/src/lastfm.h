#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QPair>
#include <QString>
#include <QUrl>

#include <functional>

class QNetworkAccessManager;

// Last.fm's web service: the signature every call needs, the body it is sent
// in, what an answer means, and the four calls Monolist makes.
//
// The first three are where a scrobbler goes wrong quietly. A parameter signed
// in the wrong order, or a '+' sent unencoded (which the server reads as a
// space), fails with error 13 on some titles and not others; an answer read by
// its HTTP status instead of its error number drops scrobbles that should have
// waited. So each is a function of its own, tested against fixed answers
// (--lastfm-test), before anything is sent anywhere.
//
// The calls are the sign-in (auth.getToken, then auth.getSession once the user
// has approved the token in their browser; never auth.getMobileSession, which
// takes a password) and the two that report listening (track.updateNowPlaying,
// track.scrobble). Every one is signed and POSTed; what to do with the answer
// is the caller's business (Scrobbler).
//
// The build's API key and shared secret come from apicredentials.h, generated
// at build time; they are private to this class and never logged. Neither is
// a session key, which lives in SecretStore and is only ever passed in.
//
// Exposed to QML as the "LastFm" singleton, for whether this build can
// connect at all.
class LastFmApi : public QObject
{
    Q_OBJECT
    // Whether this build was given an API account.
    Q_PROPERTY(bool hasKey READ hasKey CONSTANT)
    // Whether connecting can work here: a key, and somewhere safe to keep
    // the session it would bring back.
    Q_PROPERTY(bool available READ available CONSTANT)
    // Why not, in a sentence for the Settings row; empty when it can.
    Q_PROPERTY(QString unavailableReason READ unavailableReason CONSTANT)

public:
    // A call's parameters in the order given; signing sorts a copy. Names are
    // ASCII ("artist[3]"), values are text, sent as UTF-8.
    using Params = QList<QPair<QString, QString>>;

    // What to do about an answer. Named for the action rather than the error
    // number, so that what the scrobbler does with each reads as policy.
    enum class Outcome {
        Ok,              // done; track.scrobble's items may still each be ignored
        Retry,           // no answer, HTTP 5xx, an unreadable body, 11 offline,
                         // 16 temporarily unavailable: back off and send again
        RateLimited,     // 29: wait a quarter of an hour
        Reauthenticate,  // 9: the session key was revoked; the user must reconnect
        Hold,            // 10 invalid key, 13 bad signature, 26 key suspended:
                         // stop and keep everything, since dropping on these
                         // is how other scrobblers have lost whole queues
        KeepWaiting,     // 14: asked for a session before the user approved
        RestartSignIn,   // 15: the sign-in token expired (they last an hour)
        Rejected         // 6, 8 and the rest: this request was wrong; a batch
                         // is resent an item at a time to find the bad one
    };
    Q_ENUM(Outcome)

    // One scrobble's fate, from its ignoredMessage code.
    enum class ItemOutcome {
        Accepted,        // 0
        Dropped,         // 1 artist ignored, 2 track ignored, 3 too old, 4 too new:
                         // it will never be accepted, so it is not kept
        TryTomorrow      // 5 the daily limit: keep it for the next UTC day
    };
    Q_ENUM(ItemOutcome)

    // An answer, read. `body` can hold a session key (auth.getSession), so a
    // Reply is never logged whole; `message` never holds one.
    struct Reply {
        Outcome outcome = Outcome::Retry;
        int httpStatus = 0;
        int error = 0;          // Last.fm's error number; 0 when it sent none
        QString message;        // Last.fm's message, or what was wrong with the answer
        QJsonObject body;
        // track.scrobble's own count of what it took...
        int accepted = 0;
        int ignored = 0;
        // ...and each item's ignoredMessage code, in the order sent. One
        // entry for track.updateNowPlaying.
        QList<int> ignoredCodes;
    };

    // One scrobble as sent. track.scrobble takes up to 50 in one request.
    struct Scrobble {
        QString artist;
        QString track;
        QString album;
        QString albumArtist;
        int durationS = 0;
        qint64 timestamp = 0;      // when it started, UTC seconds
        bool chosenByUser = true;  // false for what autoplay picked
    };
    static constexpr int kMaxBatch = 50;

    using Done = std::function<void(const Reply &reply)>;

    explicit LastFmApi(QObject *parent = nullptr);

    bool hasKey() const;
    bool available() const;
    QString unavailableReason() const;

    // Where calls go: https://ws.audioscrobbler.com/2.0/, or the address in
    // MONOLIST_LASTFM_URL, for trying the sending against a stand-in server.
    static QUrl endpoint();
    // The page, in the user's browser, where a token is approved.
    QUrl authPageUrl(const QString &token) const;

    // The calls. `done` always runs, once, with the answer read; a call
    // made with no key answers at once as a request error.
    void getToken(Done done);
    void getSession(const QString &token, Done done);
    void updateNowPlaying(const QByteArray &sessionKey, const Scrobble &item, Done done);
    void scrobble(const QByteArray &sessionKey, const QList<Scrobble> &items, Done done);

    // — for the self-tests only —
    // An invented account in place of the build's, so the calls can be
    // signed and sent to a stand-in server.
    void setTestAccount(const QByteArray &key, const QByteArray &secret);
    // Answers in place of the network: given what would have been sent
    // (signed, format included), the HTTP status and body to answer with; a
    // status of 0 is no answer at all. Answered on the next turn of the
    // event loop, as a real reply would be.
    using Responder = std::function<QPair<int, QByteArray>(const Params &sent)>;
    void setTestResponder(Responder responder);

    // api_sig: the lower-case hex MD5 of every parameter but format and
    // callback, sorted by name byte for byte ("artist[10]" before
    // "artist[1]"), each written as its name then its UTF-8 value, with the
    // shared secret on the end (last.fm/api/authspec).
    static QByteArray signature(const Params &params, const QByteArray &secret);
    // The parameters as sent: api_sig added, then format=json, which is
    // never signed. Any api_sig or format already there is replaced.
    static Params sign(Params params, const QByteArray &secret);
    // application/x-www-form-urlencoded, every name and value through
    // QUrl::toPercentEncoding. Not QUrlQuery, which leaves '+' as it is: the
    // server reads that as a space, so "Simon + Garfunkel" would arrive as
    // "Simon   Garfunkel" and fail its signature.
    static QByteArray formBody(const Params &params);

    // Reads any answer. Last.fm's error number decides, whatever the HTTP
    // status says (it sends error 9 with a 403); with no error number, only a
    // 2xx carrying a JSON object counts as an answer at all.
    static Reply parseReply(int httpStatus, const QByteArray &body, bool transportFailed = false);
    static Outcome outcomeForError(int error);
    static ItemOutcome itemOutcome(int ignoredCode);

    // auth.getToken's token, or empty.
    static QString token(const Reply &reply);
    // auth.getSession's user name and session key; false unless both are there.
    static bool session(const Reply &reply, QString *user, QByteArray *key);

    static QString outcomeName(Outcome outcome);
    static QString itemOutcomeName(ItemOutcome outcome);

private:
    // This build's account. Private, so the only code that could ever leak
    // them is in lastfm.cpp.
    static QByteArray apiKey();
    static QByteArray sharedSecret();

    // Adds api_key, signs, and sends.
    void call(Params params, Done done);
    QNetworkAccessManager *network();

    QByteArray m_key;
    QByteArray m_secret;
    QNetworkAccessManager *m_network = nullptr;
    Responder m_responder;
};
