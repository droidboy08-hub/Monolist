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
// Only what a browser would send to https://music.youtube.com is kept, one
// cookie per name. Google sets the same names on google.com with different
// values, and a header carrying both copies is answered as signed out: 200 OK,
// the generic feed, logged_in=0. So cookies are scoped to YouTube Music first,
// and only then de-duplicated, the most specific domain winning. A header
// has no domains, so its cookies count as .youtube.com, path /, secure.
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

    enum class Format { None, Netscape, Header, Curl };

    struct Result {
        Format format = Format::None;
        QList<Cookie> cookies;    // for YouTube Music, one per name, in the order read
        int read = 0;             // cookies found in the text at all
        int otherSites = 0;       // for google.com and the rest: left out
        int elsewhere = 0;        // for youtube.com, but never sent to YouTube Music: left out
        int expired = 0;
        int duplicates = 0;       // a name seen again; the less specific copy dropped
        int unreadable = 0;       // lines or pairs that were not a cookie
        int spaced = 0;           // cookies.txt lines with spaces where the tabs should be
        QStringList missing;      // required names that are not there
        QString error;            // why this is not a sign-in, in a sentence; empty when it is
        bool ok() const { return error.isEmpty(); }
        // What was read, in counts: "18 cookies for YouTube Music from a
        // cookies.txt file (12 for other sites left out)". Never a value.
        QString summary() const;
    };

    // `now` (UTC seconds) decides what has expired; 0 is the current time.
    static Result parse(const QByteArray &text, qint64 now = 0);

    // Of LOGIN_INFO and SAPISID-or-__Secure-3PAPISID, the ones not there.
    static QStringList missingRequired(const QList<Cookie> &cookies);

    // Whether a browser would send `cookie` with a call to
    // https://music.youtube.com/youtubei/v1/..., expiry aside.
    static bool sentToMusic(const Cookie &cookie);
    // youtube.com or one of its subdomains.
    static bool isYouTubeDomain(const QString &domain);

    // The Cookie header for such a call: name=value pairs joined by "; ",
    // leaving out any that has expired by `now` (0 is the current time).
    static QByteArray header(const QList<Cookie> &cookies, qint64 now = 0);
    // The named cookie's value, or empty.
    static QByteArray value(const QList<Cookie> &cookies, const QByteArray &name);
    // The names, for the log.
    static QStringList names(const QList<Cookie> &cookies);

    // The jar as SecretStore keeps it: {"version":1,"cookies":[{name, value,
    // domain, hostOnly, path, expires, secure, httpOnly}]}. fromJson is false
    // for anything else, and then leaves `cookies` empty.
    static QByteArray toJson(const QList<Cookie> &cookies);
    static bool fromJson(const QByteArray &json, QList<Cookie> *cookies);

    static QString formatName(Format format);

    // The words of a command line as the shell it was copied for would split
    // them: bash's quoting ('...', "...", $'...', backslashes and line
    // continuations), or with `windows`, cmd.exe's carets and then the C
    // runtime's quotes. Public for the self-test.
    static QStringList commandWords(const QString &command, bool windows);
};
