#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

// Reading a YouTube Music sign-in the user exported from their own browser.
//
// There is no way to sign in to Google from inside the app that Google allows,
// so the user signs in on Google's own page, in a private window of their own
// browser, and hands Monolist the cookies of that session in one of three
// shapes, told apart here by their look:
//
//  - a Netscape cookies.txt file, as browser extensions and yt-dlp write it:
//    seven TAB-separated columns a line. Lines starting "#HttpOnly_" are
//    cookies, not comments (curl's convention for HttpOnly ones); reading them
//    as comments silently loses the SID cookies, and with them the sign-in;
//  - a Cookie header, copied from a request in the browser's developer tools,
//    with or without its "Cookie:" name;
//  - a whole request copied as cURL, in bash's quoting or cmd.exe's, where the
//    cookies are in -H 'cookie: ...' or -b '...'.
//
// What is kept is what a browser would send to one of the three hosts the
// account's calls go to: music.youtube.com (YouTube Music's own calls),
// www.youtube.com (YouTube's, and yt-dlp's) and s.youtube.com (where a
// player reports what was heard). google.com's cookies never are. Google sets
// the same names there with different values, and a header carrying both
// copies is answered as signed out: 200 OK, the generic feed, logged_in=0. A
// header has no domains, so its cookies count as .youtube.com, path /, secure.
//
// The jar holds each cookie once, as a browser does: by name, domain and path.
// Which of them a request carries is decided per host and path, by the
// browser's own rules (header()): a cookie for www.youtube.com alone never
// goes to YouTube Music. Where two copies of a name would both go, only the
// more specific one does, since two values for one name is what YouTube
// answers as signed out.
//
// A result is a sign-in only with LOGIN_INFO and SAPISID (or its stand-in
// __Secure-3PAPISID): yt-dlp's own rule for "has auth cookies".
//
// Values are never logged, shown or put in a message; names and counts are.
class CookieImport
{
public:
    struct Cookie {
        QByteArray name;
        QByteArray value;         // never logged, never shown
        QString domain;           // "youtube.com", "music.youtube.com": no leading dot
        bool hostOnly = false;    // sent to `domain` itself only, not to its subdomains
        QString path = QStringLiteral("/");
        qint64 expires = 0;       // UTC seconds; 0 for one that ends with the browser session
        bool secure = false;
        bool httpOnly = false;
    };

    // What a signed-in page knows of its session beside the cookies, where it
    // is known; each part may not be. None of it is a password, but all of it
    // names the account, so it is kept with the cookies (in SecretStore) and
    // never logged.
    struct SessionInfo {
        // X-Goog-AuthUser: which of the browser's signed-in accounts this is
        // (SESSION_INDEX). -1 when not known, and the calls then say 0, the
        // first, which is the only one in a private window.
        int authUser = -1;
        // The account's own visitor id (VISITOR_DATA), never the anonymous one.
        QString visitorData;
        // DATASYNC_ID: "<user>||" for the account's own channel, or
        // "<delegated>||<user>" when a brand channel is the one in use.
        QString dataSyncId;

        bool isEmpty() const { return authUser < 0 && visitorData.isEmpty() && dataSyncId.isEmpty(); }
        // The brand channel's id, for context.user.onBehalfOfUser; empty for
        // the account's own channel and when not known.
        QString delegatedId() const;
        bool operator==(const SessionInfo &other) const
        {
            return authUser == other.authUser && visitorData == other.visitorData && dataSyncId == other.dataSyncId;
        }
        bool operator!=(const SessionInfo &other) const { return !(*this == other); }
    };

    enum class Format { None, Netscape, Header, Curl };

    struct Result {
        Format format = Format::None;
        QList<Cookie> cookies;    // for the account's hosts, one per name, domain and path, in the order read
        // From a copied request's own headers (x-goog-authuser,
        // x-goog-visitor-id), when it had them; a cookies.txt file has none.
        SessionInfo info;
        int read = 0;             // cookies found in the text at all
        int otherSites = 0;       // for google.com and the rest: left out
        int elsewhere = 0;        // for youtube.com, but never sent to the account's hosts: left out
        int expired = 0;
        int duplicates = 0;       // a cookie seen again (same name, domain and path); the first kept
        int unreadable = 0;       // lines or pairs that were not a cookie
        int spaced = 0;           // cookies.txt lines with spaces where the tabs should be
        QStringList missing;      // required names that are not there
        QString error;            // why this is not a sign-in, in a sentence; empty when it is
        bool ok() const { return error.isEmpty(); }
        // What was read, in counts: "18 cookies for YouTube from a
        // cookies.txt file (12 for other sites left out)". Never a value.
        QString summary() const;
    };

    // The hosts the account's calls go to, and the path YouTube's API calls
    // are under on the first two.
    static const QString kMusicHost;   // "music.youtube.com"
    static const QString kWwwHost;     // "www.youtube.com"
    static const QString kStatsHost;   // "s.youtube.com"
    static const QString kApiPath;     // "/youtubei/v1/"

    // `now` (UTC seconds) decides what has expired; 0 is the current time.
    static Result parse(const QByteArray &text, qint64 now = 0);

    // Of LOGIN_INFO and SAPISID-or-__Secure-3PAPISID, the ones not there.
    static QStringList missingRequired(const QList<Cookie> &cookies);

    // Whether a browser would send `cookie` with a request to
    // https://<host><path>, expiry aside: RFC 6265's domain match (a host-only
    // cookie to its own host alone) and path match. Every such request is
    // HTTPS, so Secure never keeps one back.
    static bool sentTo(const Cookie &cookie, const QString &host, const QString &path);
    // Whether it would go to one of the account's three hosts, on any path.
    static bool sentToAccountHosts(const Cookie &cookie);
    // youtube.com or one of its subdomains.
    static bool isYouTubeDomain(const QString &domain);

    // The cookies a request to https://<host><path> carries, as a browser
    // would send them: those sent there (sentTo) and not expired by `now` (0
    // is the current time), longer paths first and otherwise in the jar's
    // order (RFC 6265 5.4), with one exception: of two copies of a name, only
    // the more specific goes (the longer domain, then a host-only one, then
    // the longer path; the first in the jar on a tie).
    static QList<Cookie> forRequest(const QList<Cookie> &jar, const QString &host, const QString &path,
                                    qint64 now = 0);
    // Their Cookie header: name=value pairs joined by "; ".
    static QByteArray header(const QList<Cookie> &jar, const QString &host, const QString &path, qint64 now = 0);
    // The named cookie's value, or empty. Of several copies, the first in
    // `cookies`: give it forRequest's list to have the one a host is sent.
    static QByteArray value(const QList<Cookie> &cookies, const QByteArray &name);
    // The names, for the log.
    static QStringList names(const QList<Cookie> &cookies);

    // The jar as SecretStore keeps it: {"version":2,"cookies":[{name, value,
    // domain, hostOnly, path, expires, secure, httpOnly}],"session":{authUser,
    // visitorData, dataSyncId}}. A jar with nothing known beside its cookies
    // is written as version 1, without "session", as it was before there was
    // one, so an older Monolist still opens it. fromJson reads either version,
    // and is false for anything else, leaving `cookies` empty and `info`
    // unknown.
    static QByteArray toJson(const QList<Cookie> &cookies, const SessionInfo &info);
    static QByteArray toJson(const QList<Cookie> &cookies);
    static bool fromJson(const QByteArray &json, QList<Cookie> *cookies, SessionInfo *info = nullptr);

    // The jar as a Netscape cookies.txt file, for yt-dlp's --cookies: the
    // "# Netscape HTTP Cookie File" line Python's reader insists on, then one
    // line a cookie, "#HttpOnly_" before an HttpOnly one, a leading dot on
    // the domain exactly when the second column is TRUE (Python refuses a
    // file where they disagree), and 0 for a session cookie's expiry, which
    // yt-dlp reads as one. Only youtube.com's cookies are written, never
    // google.com's; nor one expired by `now`, nor one whose name or value
    // holds a control character, which would break its line or start
    // another. `written` gets how many lines were written.
    static QByteArray toNetscape(const QList<Cookie> &jar, qint64 now = 0, int *written = nullptr);

    static QString formatName(Format format);

    // The words of a command line as the shell it was copied for would split
    // them: bash's quoting ('...', "...", $'...', backslashes and line
    // continuations), or with `windows`, cmd.exe's carets and then the C
    // runtime's quotes. Public for the self-test.
    static QStringList commandWords(const QString &command, bool windows);

    // What an answer or a copied header says, checked before it is believed:
    // X-Goog-AuthUser as a number from 0 to 99 (else -1), a visitor id of
    // base64 and percent signs only, a DATASYNC_ID of two such ids around
    // "||". Anything else is empty: none of it may ever put a line break or
    // a stray character into a header. Public for the self-test.
    static int authUserFrom(const QByteArray &text);
    static QString visitorDataFrom(const QString &text);
    static QString dataSyncIdFrom(const QString &text);
};
