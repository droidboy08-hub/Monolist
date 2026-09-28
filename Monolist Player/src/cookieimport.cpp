#include "cookieimport.h"

#include <QDateTime>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

using Cookie = CookieImport::Cookie;
using Result = CookieImport::Result;

// 9999-12-31 23:59:59 UTC: the latest expiry a cookies.txt line may carry.
constexpr double kLastExpiry = 253402300799.0;

// A cookies.txt TRUE/FALSE column: 1, 0, or -1 for neither.
int flag(const QByteArray &field)
{
    const QByteArray value = field.trimmed().toUpper();
    return value == "TRUE" ? 1 : value == "FALSE" ? 0 : -1;
}

// A cookie name is an HTTP token: printable ASCII, no separators.
bool isToken(const QByteArray &name)
{
    if (name.isEmpty())
        return false;
    for (const char c : name) {
        const uchar u = uchar(c);
        if (u <= 32 || u >= 127 || std::strchr("()<>@,;:\\\"/[]?={}", c))
            return false;
    }
    return true;
}

// Which of two copies of a name a browser treats as the more specific: the
// longer domain, then a host-only cookie over a domain one, then the longer
// path. What is left is a tie, and the copy met first stays.
bool moreSpecific(const Cookie &a, const Cookie &b)
{
    if (a.domain.size() != b.domain.size())
        return a.domain.size() > b.domain.size();
    if (a.hostOnly != b.hostOnly)
        return a.hostOnly;
    return a.path.size() > b.path.size();
}

// RFC 6265 5.1.3, as a browser applies it: a host-only cookie goes to its own
// host alone, a domain cookie to its domain and every host under it.
bool domainMatches(const Cookie &cookie, const QString &host)
{
    return cookie.hostOnly ? cookie.domain == host
                           : cookie.domain == host || host.endsWith(QLatin1Char('.') + cookie.domain);
}

// What makes a cookie the same cookie to a browser: a second one with all of
// these the same replaces the first, where one differing in any of them is
// another cookie.
QString identity(const Cookie &cookie)
{
    return QString::fromLatin1(cookie.name) + QLatin1Char('\n') + cookie.domain + QLatin1Char('\n')
           + (cookie.hostOnly ? QLatin1Char('h') : QLatin1Char('d')) + QLatin1Char('\n') + cookie.path;
}

// Where every cookie read goes: counted, then kept only if it would go to one
// of the account's hosts, has not expired, and is not a second copy of a
// cookie already kept.
struct Collector {
    Result &result;
    qint64 now;
    QSet<QString> kept;   // identity()

    void add(const Cookie &cookie)
    {
        ++result.read;
        if (!CookieImport::isYouTubeDomain(cookie.domain)) {
            ++result.otherSites;
            return;
        }
        if (!CookieImport::sentToAccountHosts(cookie)) {
            ++result.elsewhere;
            return;
        }
        if (cookie.expires > 0 && cookie.expires <= now) {
            ++result.expired;
            return;
        }
        // The same cookie twice (a header naming it twice): the first stays,
        // as it always has. Two copies of a name on different domains are
        // two cookies, and header() chooses between them per host.
        const QString key = identity(cookie);
        if (kept.contains(key)) {
            ++result.duplicates;
            return;
        }
        kept.insert(key);
        result.cookies.append(cookie);
    }
};

// A request header copied with the cookies, for what it says of the session:
// which of the browser's accounts (x-goog-authuser) and its visitor id
// (x-goog-visitor-id). Anything that does not look right is ignored.
void readSessionHeader(const QByteArray &line, CookieImport::SessionInfo &info)
{
    const int colon = int(line.indexOf(':', line.startsWith(':') ? 1 : 0));
    if (colon <= 0)
        return;
    const QByteArray name = line.left(colon).trimmed().toLower();
    const QByteArray value = line.mid(colon + 1).trimmed();
    if (name == "x-goog-authuser") {
        const int index = CookieImport::authUserFrom(value);
        if (index >= 0)
            info.authUser = index;
    } else if (name == "x-goog-visitor-id") {
        const QString id = CookieImport::visitorDataFrom(QString::fromLatin1(value));
        if (!id.isEmpty())
            info.visitorData = id;
    }
}

// A byte that would end a line of a cookies.txt file, or start a new one.
bool hasControl(const QByteArray &bytes)
{
    for (const char c : bytes) {
        const uchar u = uchar(c);
        if (u < 0x20 || u == 0x7f)
            return true;
    }
    return false;
}

// A cookies.txt line whose tabs became spaces somewhere (an editor, a chat
// window): six or seven words with TRUE/FALSE where the flags go. Worth
// telling apart from nonsense, because the remedy is so specific.
bool looksSpaced(const QByteArray &line)
{
    const QList<QByteArray> words = line.simplified().split(' ');
    return (words.size() == 6 || words.size() == 7) && !line.contains('\t')
           && flag(words.at(1)) >= 0 && flag(words.at(3)) >= 0;
}

QList<QByteArray> lines(const QByteArray &text)
{
    QList<QByteArray> result = text.split('\n');
    for (QByteArray &line : result) {
        if (line.endsWith('\r'))
            line.chop(1);
    }
    return result;
}

bool looksNetscape(const QByteArray &text)
{
    for (const QByteArray &line : lines(text)) {
        const QByteArray trimmed = line.trimmed();
        if (trimmed.startsWith("# Netscape HTTP Cookie File") || trimmed.startsWith("# HTTP Cookie File")
            || line.startsWith("#HttpOnly_"))
            return true;
        if (trimmed.isEmpty() || line.startsWith('#'))
            continue;
        const QList<QByteArray> fields = line.split('\t');
        if ((fields.size() == 6 || fields.size() == 7) && flag(fields.at(1)) >= 0 && flag(fields.at(3)) >= 0)
            return true;
        if (looksSpaced(line))
            return true;
    }
    return false;
}

// domain, include-subdomains, path, secure, expires, name, value: one cookie a
// line, TAB-separated. "#HttpOnly_" before the domain marks an HttpOnly
// cookie; every other line starting '#' is a comment.
void readNetscape(const QByteArray &text, Collector &collector)
{
    Result &result = collector.result;
    for (QByteArray line : lines(text)) {
        if (line.trimmed().isEmpty())
            continue;
        bool httpOnly = false;
        if (line.startsWith("#HttpOnly_")) {
            httpOnly = true;
            line.remove(0, int(std::strlen("#HttpOnly_")));
        } else if (line.startsWith('#')) {
            continue;
        }
        QList<QByteArray> fields = line.split('\t');
        if (fields.size() == 6)
            fields.append(QByteArray());   // a cookie with an empty value, as curl reads it
        if (fields.size() != 7) {
            if (looksSpaced(line))
                ++result.spaced;
            else
                ++result.unreadable;
            continue;
        }

        QByteArray domain = fields.at(0).trimmed().toLower();
        const bool dotted = domain.startsWith('.');
        if (dotted)
            domain.remove(0, 1);
        const int subdomains = flag(fields.at(1));
        const int secure = flag(fields.at(3));
        bool numeric = false;
        // Some exporters write the expiry with a fraction of a second. It is
        // turned into a whole number below, which for a value no qint64 can
        // hold (1e300, inf, nan: this is a file from outside) is undefined,
        // so anything past the end of the year 9999 is refused first. An
        // empty one is a session cookie, as Python's cookie jar writes it,
        // and so as yt-dlp writes its jar back (curl writes 0).
        const QByteArray expiry = fields.at(4).trimmed();
        const double expires = expiry.isEmpty() ? 0.0 : expiry.toDouble(&numeric);
        if (expiry.isEmpty())
            numeric = true;
        const QByteArray name = fields.at(5).trimmed();
        if (domain.isEmpty() || domain.contains(' ') || subdomains < 0 || secure < 0 || !numeric
            || !std::isfinite(expires) || expires < 0 || expires > kLastExpiry || name.isEmpty()) {
            ++result.unreadable;
            continue;
        }

        Cookie cookie;
        cookie.domain = QString::fromLatin1(domain);
        // A leading dot means the same as the flag; a file that disagrees
        // with itself is read the way that sends the cookie.
        cookie.hostOnly = !(dotted || subdomains == 1);
        cookie.path = fields.at(2).isEmpty() ? QStringLiteral("/") : QString::fromLatin1(fields.at(2));
        cookie.secure = secure == 1;
        cookie.expires = qint64(expires);
        cookie.name = name;
        cookie.value = fields.at(6);
        cookie.httpOnly = httpOnly;
        collector.add(cookie);
    }
}

// "a=b; c=d", as a Cookie header carries them. A header says nothing of
// domains, so every one counts as a .youtube.com cookie for path /, sent
// only over HTTPS: what the browser sent to YouTube Music, it had for it.
void readPairs(const QByteArray &pairs, Collector &collector)
{
    for (QByteArray part : pairs.split(';')) {
        part = part.trimmed();
        if (part.isEmpty())
            continue;
        const int equals = int(part.indexOf('='));
        const QByteArray name = equals > 0 ? part.left(equals).trimmed() : QByteArray();
        if (!isToken(name)) {
            ++collector.result.unreadable;
            continue;
        }
        Cookie cookie;
        cookie.name = name;
        cookie.value = part.mid(equals + 1).trimmed();
        cookie.domain = QStringLiteral("youtube.com");
        cookie.secure = true;
        collector.add(cookie);
    }
}

// The value of a "cookie: ..." header line, if that is what it is.
bool cookieHeaderValue(const QByteArray &line, QByteArray *value)
{
    const int colon = int(line.indexOf(':'));
    if (colon <= 0 || line.left(colon).trimmed().toLower() != "cookie")
        return false;
    *value = line.mid(colon + 1);
    return true;
}

// A Cookie header on its own, with or without its name, or one line among a
// block of request headers copied whole.
void readHeader(const QByteArray &text, Collector &collector)
{
    bool found = false;
    bool otherHeaders = false;
    static const QRegularExpression headerLine(QStringLiteral(R"(^\s*:?[A-Za-z][A-Za-z0-9-]*\s*:\s)"));
    CookieImport::SessionInfo info;
    for (const QByteArray &line : lines(text)) {
        QByteArray value;
        if (cookieHeaderValue(line, &value)) {
            found = true;
            readPairs(value, collector);
        } else if (headerLine.match(QString::fromLatin1(line)).hasMatch()) {
            otherHeaders = true;
            readSessionHeader(line.trimmed(), info);
        }
    }
    if (found) {
        collector.result.info = info;
        return;
    }
    if (otherHeaders) {
        collector.result.error = QStringLiteral(
            "These request headers have no Cookie header among them. Copy them from a request to "
            "music.youtube.com made while signed in.");
        return;
    }
    // The value alone, as the developer tools copy it; a line break is as
    // good as a semicolon between pairs.
    QByteArray joined = text;
    joined.replace('\r', ';').replace('\n', ';');
    readPairs(joined, collector);
}

// "Copy as cURL": the cookies are in -H 'cookie: ...' or in -b '...'.
void readCurl(const QString &command, Collector &collector)
{
    // Chrome's and Firefox's "(cmd)" copies wrap every argument in ^"...^".
    const bool windows = command.contains(QLatin1String("^\""));
    const QStringList words = CookieImport::commandWords(command, windows);
    bool fromFile = false;
    bool found = false;
    CookieImport::SessionInfo info;
    for (int i = 1; i < words.size(); ++i) {
        const QString &word = words.at(i);
        QString header;
        QString cookies;
        if ((word == QLatin1String("-H") || word == QLatin1String("--header")) && i + 1 < words.size())
            header = words.at(++i);
        else if (word.startsWith(QLatin1String("-H")) && !word.startsWith(QLatin1String("--")))
            header = word.mid(2);
        else if ((word == QLatin1String("-b") || word == QLatin1String("--cookie")) && i + 1 < words.size())
            cookies = words.at(++i);
        else if (word.startsWith(QLatin1String("-b")) && !word.startsWith(QLatin1String("--")))
            cookies = word.mid(2);

        QByteArray value;
        if (!header.isEmpty() && cookieHeaderValue(header.toUtf8(), &value)) {
            found = true;
            readPairs(value, collector);
        } else if (!header.isEmpty()) {
            readSessionHeader(header.toUtf8().trimmed(), info);
        } else if (!cookies.isEmpty()) {
            // Without '=', -b names a file to read cookies from.
            if (cookies.contains(QLatin1Char('='))) {
                found = true;
                readPairs(cookies.toUtf8(), collector);
            } else {
                fromFile = true;
            }
        }
    }
    if (found) {
        collector.result.info = info;
        return;
    }
    collector.result.error = fromFile
        ? QStringLiteral("That cURL command reads its cookies from a file (-b with a file name), so they are "
                         "not in it. Copy the request from the browser's developer tools instead.")
        : QStringLiteral("That cURL command carries no cookies. Copy a request to music.youtube.com made "
                         "while signed in, such as a browse request.");
}

bool startsWithWord(const QByteArray &text, const char *word)
{
    const int length = int(std::strlen(word));
    return text.size() > length && text.left(length).toLower() == word
           && QByteArray(" \t\r\n").contains(text.at(length));
}

// bash's ANSI-C quoting, $'...', from just after the opening quote. Returns
// where the word goes on.
int ansiC(const QString &s, int i, QString *word)
{
    const int n = int(s.size());
    const auto hex = [&](int maxDigits) {
        uint code = 0;
        int digits = 0;
        while (digits < maxDigits && i < n && QStringLiteral("0123456789abcdefABCDEF").contains(s.at(i))) {
            code = code * 16 + uint(QString(s.at(i)).toUInt(nullptr, 16));
            ++i;
            ++digits;
        }
        return digits > 0 ? code : 0xFFFFFFFFu;
    };
    while (i < n) {
        const QChar c = s.at(i);
        if (c == QLatin1Char('\''))
            return i + 1;
        if (c != QLatin1Char('\\') || i + 1 >= n) {
            *word += c;
            ++i;
            continue;
        }
        const QChar e = s.at(i + 1);
        i += 2;
        switch (e.unicode()) {
        case 'n': *word += QLatin1Char('\n'); break;
        case 't': *word += QLatin1Char('\t'); break;
        case 'r': *word += QLatin1Char('\r'); break;
        case 'a': *word += QChar(0x07); break;
        case 'b': *word += QChar(0x08); break;
        case 'e': case 'E': *word += QChar(0x1B); break;
        case 'f': *word += QChar(0x0C); break;
        case 'v': *word += QChar(0x0B); break;
        case '\\': case '\'': case '"': case '?': *word += e; break;
        case 'x': case 'u': case 'U': {
            const uint code = hex(e == QLatin1Char('x') ? 2 : e == QLatin1Char('u') ? 4 : 8);
            if (code == 0xFFFFFFFFu) {
                *word += QLatin1Char('\\');
                *word += e;
            } else {
                const char32_t point = char32_t(code);
                *word += QString::fromUcs4(&point, 1);
            }
            break;
        }
        default:
            if (e >= QLatin1Char('0') && e <= QLatin1Char('7')) {
                uint code = uint(e.unicode() - '0');
                for (int digits = 1; digits < 3 && i < n && s.at(i) >= QLatin1Char('0') && s.at(i) <= QLatin1Char('7'); ++digits)
                    code = code * 8 + uint(s.at(i++).unicode() - '0');
                *word += QChar(code);
            } else {
                *word += QLatin1Char('\\');
                *word += e;
            }
            break;
        }
    }
    return n;
}

QStringList posixWords(const QString &s)
{
    QStringList words;
    QString word;
    bool inWord = false;
    const int n = int(s.size());
    const auto endWord = [&]() {
        if (inWord)
            words << word;
        word.clear();
        inWord = false;
    };
    int i = 0;
    while (i < n) {
        const QChar c = s.at(i);
        if (c == QLatin1Char(' ') || c == QLatin1Char('\t') || c == QLatin1Char('\r') || c == QLatin1Char('\n')) {
            endWord();
            ++i;
        } else if (c == QLatin1Char('\\')) {
            // A backslash at the end of a line continues the command.
            if (i + 1 < n && s.at(i + 1) == QLatin1Char('\n')) {
                i += 2;
            } else if (i + 2 < n && s.at(i + 1) == QLatin1Char('\r') && s.at(i + 2) == QLatin1Char('\n')) {
                i += 3;
            } else {
                if (i + 1 < n) {
                    word += s.at(i + 1);
                    inWord = true;
                }
                i += 2;
            }
        } else if (c == QLatin1Char('\'')) {
            int end = int(s.indexOf(QLatin1Char('\''), i + 1));
            if (end < 0)
                end = n;
            word += s.mid(i + 1, end - i - 1);
            inWord = true;
            i = end + 1;
        } else if (c == QLatin1Char('$') && i + 1 < n && s.at(i + 1) == QLatin1Char('\'')) {
            i = ansiC(s, i + 2, &word);
            inWord = true;
        } else if (c == QLatin1Char('"')) {
            ++i;
            while (i < n && s.at(i) != QLatin1Char('"')) {
                if (s.at(i) == QLatin1Char('\\') && i + 1 < n
                    && QStringLiteral("\"\\$`\n").contains(s.at(i + 1))) {
                    if (s.at(i + 1) != QLatin1Char('\n'))
                        word += s.at(i + 1);
                    i += 2;
                    continue;
                }
                word += s.at(i);
                ++i;
            }
            ++i;
            inWord = true;
        } else {
            word += c;
            inWord = true;
            ++i;
        }
    }
    endWord();
    return words;
}

QStringList windowsWords(const QString &s)
{
    // cmd.exe first. The copy escapes every quote as ^" so cmd.exe never
    // enters a quoted run, and so every caret is its escape: the character
    // after it is taken as it is, and a caret at the end of a line joins the
    // next line on.
    QString t;
    t.reserve(s.size());
    for (int i = 0; i < s.size(); ++i) {
        if (s.at(i) == QLatin1Char('^') && i + 1 < s.size()) {
            if (s.at(i + 1) == QLatin1Char('\r') && i + 2 < s.size() && s.at(i + 2) == QLatin1Char('\n'))
                i += 2;
            else if (s.at(i + 1) == QLatin1Char('\n'))
                i += 1;
            else
                t += s.at(++i);
            continue;
        }
        t += s.at(i);
    }

    // Then the C runtime's argv rules: quotes group, and backslashes are
    // literal except before a quote, where each pair is one backslash and an
    // odd one out makes the quote literal.
    QStringList words;
    QString word;
    bool inWord = false;
    bool quoted = false;
    const int n = int(t.size());
    int i = 0;
    while (i < n) {
        const QChar c = t.at(i);
        if (!quoted && (c == QLatin1Char(' ') || c == QLatin1Char('\t') || c == QLatin1Char('\r')
                        || c == QLatin1Char('\n'))) {
            if (inWord)
                words << word;
            word.clear();
            inWord = false;
            ++i;
        } else if (c == QLatin1Char('\\')) {
            int k = 0;
            while (i + k < n && t.at(i + k) == QLatin1Char('\\'))
                ++k;
            if (i + k < n && t.at(i + k) == QLatin1Char('"')) {
                word += QString(k / 2, QLatin1Char('\\'));
                if (k % 2) {
                    word += QLatin1Char('"');
                    i += k + 1;
                } else {
                    i += k;   // the quote groups; the next turn takes it
                }
            } else {
                word += QString(k, QLatin1Char('\\'));
                i += k;
            }
            inWord = true;
        } else if (c == QLatin1Char('"')) {
            if (quoted && i + 1 < n && t.at(i + 1) == QLatin1Char('"')) {
                word += QLatin1Char('"');
                i += 2;
            } else {
                quoted = !quoted;
                ++i;
            }
            inWord = true;
        } else {
            word += c;
            inWord = true;
            ++i;
        }
    }
    if (inWord)
        words << word;
    return words;
}

QString plural(int count, const char *one, const char *many)
{
    return count == 1 ? QString::fromLatin1(one) : QString::fromLatin1(many).arg(count);
}

}

QStringList CookieImport::commandWords(const QString &command, bool windows)
{
    return windows ? windowsWords(command) : posixWords(command);
}

const QString CookieImport::kMusicHost = QStringLiteral("music.youtube.com");
const QString CookieImport::kWwwHost = QStringLiteral("www.youtube.com");
const QString CookieImport::kStatsHost = QStringLiteral("s.youtube.com");
const QString CookieImport::kApiPath = QStringLiteral("/youtubei/v1/");

bool CookieImport::isYouTubeDomain(const QString &domain)
{
    return domain == QLatin1String("youtube.com") || domain.endsWith(QLatin1String(".youtube.com"));
}

bool CookieImport::sentTo(const Cookie &cookie, const QString &host, const QString &path)
{
    if (!domainMatches(cookie, host))
        return false;
    // RFC 6265 5.1.4: the cookie's path is the request's, or a directory of it.
    const QString &cookiePath = cookie.path;
    if (cookiePath.isEmpty() || cookiePath == QLatin1String("/") || cookiePath == path)
        return true;
    return path.startsWith(cookiePath)
           && (cookiePath.endsWith(QLatin1Char('/')) || path.at(cookiePath.size()) == QLatin1Char('/'));
}

bool CookieImport::sentToAccountHosts(const Cookie &cookie)
{
    // On any path: a cookie for /watch alone still goes with yt-dlp's page
    // fetch, and header() leaves it off every call it does not belong on.
    return domainMatches(cookie, kMusicHost) || domainMatches(cookie, kWwwHost) || domainMatches(cookie, kStatsHost);
}

QList<CookieImport::Cookie> CookieImport::forRequest(const QList<Cookie> &jar, const QString &host,
                                                     const QString &path, qint64 now)
{
    if (now <= 0)
        now = QDateTime::currentSecsSinceEpoch();
    QList<Cookie> sent;
    QHash<QByteArray, int> byName;
    for (const Cookie &cookie : jar) {
        if (!sentTo(cookie, host, path) || (cookie.expires > 0 && cookie.expires <= now))
            continue;
        // A browser would send both copies of a name. YouTube answers such a
        // header as signed out, so only the more specific goes, in the place
        // of the first: what a single-host jar has always sent.
        const auto found = byName.constFind(cookie.name);
        if (found != byName.cend()) {
            Cookie &kept = sent[*found];
            if (moreSpecific(cookie, kept))
                kept = cookie;
            continue;
        }
        byName.insert(cookie.name, int(sent.size()));
        sent.append(cookie);
    }
    // Longer paths first; otherwise the jar's order, which is the order the
    // cookies were first had in (a rotated one keeps its place, as a browser
    // keeps a replaced cookie's creation time).
    std::stable_sort(sent.begin(), sent.end(), [](const Cookie &a, const Cookie &b) {
        return a.path.size() > b.path.size();
    });
    return sent;
}

QByteArray CookieImport::header(const QList<Cookie> &jar, const QString &host, const QString &path, qint64 now)
{
    QList<QByteArray> pairs;
    for (const Cookie &cookie : forRequest(jar, host, path, now))
        pairs << cookie.name + '=' + cookie.value;
    return pairs.join("; ");
}

QByteArray CookieImport::toNetscape(const QList<Cookie> &jar, qint64 now, int *written)
{
    if (now <= 0)
        now = QDateTime::currentSecsSinceEpoch();
    // Python's reader refuses a file whose first line is not this one.
    QByteArray out = "# Netscape HTTP Cookie File\n"
                     "# A signed-in YouTube session, written by Monolist for yt-dlp.\n\n";
    int lines = 0;
    for (const Cookie &cookie : jar) {
        const QByteArray domain = cookie.domain.toLatin1();
        const QByteArray path = (cookie.path.isEmpty() ? QStringLiteral("/") : cookie.path).toLatin1();
        if (!isYouTubeDomain(cookie.domain) || cookie.name.isEmpty() || (cookie.expires > 0 && cookie.expires <= now)
            || hasControl(cookie.name) || hasControl(cookie.value) || hasControl(domain) || hasControl(path)
            || domain.contains(' '))
            continue;
        if (cookie.httpOnly)
            out += "#HttpOnly_";
        out += (cookie.hostOnly ? domain : '.' + domain) + '\t' + (cookie.hostOnly ? "FALSE" : "TRUE") + '\t'
               + path + '\t' + (cookie.secure ? "TRUE" : "FALSE") + '\t' + QByteArray::number(cookie.expires)
               + '\t' + cookie.name + '\t' + cookie.value + '\n';
        ++lines;
    }
    if (written)
        *written = lines;
    return out;
}

int CookieImport::authUserFrom(const QByteArray &text)
{
    const QByteArray digits = text.trimmed();
    if (digits.isEmpty() || digits.size() > 2)
        return -1;
    for (const char c : digits) {
        if (c < '0' || c > '9')
            return -1;
    }
    return digits.toInt();
}

QString CookieImport::visitorDataFrom(const QString &text)
{
    static const QRegularExpression shape(QStringLiteral(R"(^[A-Za-z0-9_\-%=+/]{8,512}$)"));
    const QString id = text.trimmed();
    return shape.match(id).hasMatch() ? id : QString();
}

QString CookieImport::dataSyncIdFrom(const QString &text)
{
    static const QRegularExpression shape(QStringLiteral(R"(^[A-Za-z0-9_\-]{1,64}\|\|[A-Za-z0-9_\-]{0,64}$)"));
    const QString id = text.trimmed();
    return shape.match(id).hasMatch() ? id : QString();
}

QString CookieImport::SessionInfo::delegatedId() const
{
    // yt-dlp's reading of it: "<delegated>||<user>" on a brand channel,
    // "<user>||" on the account's own.
    const int bars = int(dataSyncId.indexOf(QLatin1String("||")));
    if (bars <= 0)
        return QString();
    const QString second = dataSyncId.mid(bars + 2);
    return second.isEmpty() ? QString() : dataSyncId.left(bars);
}

QByteArray CookieImport::value(const QList<Cookie> &cookies, const QByteArray &name)
{
    for (const Cookie &cookie : cookies) {
        if (cookie.name == name)
            return cookie.value;
    }
    return QByteArray();
}

QStringList CookieImport::names(const QList<Cookie> &cookies)
{
    QStringList list;
    list.reserve(cookies.size());
    for (const Cookie &cookie : cookies)
        list << QString::fromLatin1(cookie.name);
    return list;
}

QStringList CookieImport::missingRequired(const QList<Cookie> &cookies)
{
    QStringList missing;
    if (value(cookies, "LOGIN_INFO").isEmpty())
        missing << QStringLiteral("LOGIN_INFO");
    // YouTube's own pages fall back to __Secure-3PAPISID when there is no
    // SAPISID, and yt-dlp does the same.
    if (value(cookies, "SAPISID").isEmpty() && value(cookies, "__Secure-3PAPISID").isEmpty())
        missing << QStringLiteral("SAPISID");
    return missing;
}

QString CookieImport::formatName(Format format)
{
    switch (format) {
    case Format::Netscape: return QStringLiteral("cookies.txt file");
    case Format::Header:   return QStringLiteral("Cookie header");
    case Format::Curl:     return QStringLiteral("cURL command");
    case Format::None:     break;
    }
    return QStringLiteral("nothing");
}

QString CookieImport::Result::summary() const
{
    QStringList aside;
    if (otherSites > 0)
        aside << QStringLiteral("%1 for other sites left out").arg(otherSites);
    if (elsewhere > 0)
        aside << QStringLiteral("%1 for other parts of YouTube left out").arg(elsewhere);
    if (duplicates > 0)
        aside << plural(duplicates, "1 duplicate dropped", "%1 duplicates dropped");
    if (expired > 0)
        aside << QStringLiteral("%1 expired").arg(expired);
    if (unreadable + spaced > 0)
        aside << plural(unreadable + spaced, "1 unreadable line skipped", "%1 unreadable lines skipped");
    const QString count = plural(int(cookies.size()), "1 cookie", "%1 cookies");
    return QStringLiteral("%1 for YouTube from a %2%3")
        .arg(count, formatName(format),
             aside.isEmpty() ? QString() : QStringLiteral(" (") + aside.join(QStringLiteral(", ")) + QLatin1Char(')'));
}

CookieImport::Result CookieImport::parse(const QByteArray &input, qint64 now)
{
    Result result;
    if (now <= 0)
        now = QDateTime::currentSecsSinceEpoch();
    QByteArray text = input;
    if (text.startsWith("\xEF\xBB\xBF"))
        text.remove(0, 3);
    const QByteArray trimmed = text.trimmed();

    if (trimmed.isEmpty()) {
        result.error = QStringLiteral("There is nothing to import: it is empty.");
        return result;
    }
    if (text.contains('\0')) {
        result.error = QStringLiteral("That is not a text file. Choose the cookies.txt file the browser "
                                      "extension saved.");
        return result;
    }

    Collector collector{ result, now, {} };
    if (startsWithWord(trimmed, "curl") || startsWithWord(trimmed, "curl.exe")) {
        result.format = Format::Curl;
        readCurl(QString::fromUtf8(trimmed), collector);
    } else if (trimmed.startsWith('[') || trimmed.startsWith('{')) {
        result.error = QStringLiteral("That looks like a JSON cookie export. Monolist reads the cookies.txt "
                                      "(Netscape) format: export again in that format, or paste the Cookie "
                                      "header instead.");
        return result;
    } else if (trimmed.contains("Invoke-WebRequest") || trimmed.contains("System.Net.Cookie")) {
        result.error = QStringLiteral("That is a PowerShell command. Copy the request as cURL (bash) instead, "
                                      "or paste just its Cookie header.");
        return result;
    } else if (looksNetscape(text)) {
        result.format = Format::Netscape;
        readNetscape(text, collector);
    } else {
        result.format = Format::Header;
        readHeader(text, collector);
    }
    if (!result.error.isEmpty()) {
        result.cookies.clear();
        result.info = {};
        return result;
    }

    if (result.read == 0) {
        switch (result.format) {
        case Format::Netscape:
            result.error = result.spaced > 0
                ? QStringLiteral("The file's columns are separated by spaces, not tabs, so it cannot be read. "
                                 "Export it again with the browser extension rather than copying it through "
                                 "an editor.")
                : QStringLiteral("The file has no cookies in it.");
            break;
        default:
            result.error = QStringLiteral("Nothing here looks like a Cookie header: that is name=value pairs "
                                          "separated by semicolons.");
            break;
        }
    } else if (result.cookies.isEmpty()) {
        if (result.otherSites + result.elsewhere == 0 && result.expired > 0) {
            result.error = QStringLiteral("Every YouTube cookie here has expired. Sign in again in the private "
                                          "window and export afresh.");
        } else {
            result.error = QStringLiteral("None of these cookies is for YouTube Music (%1 for other sites). "
                                          "Export them from a youtube.com tab, or copy a request to "
                                          "music.youtube.com.").arg(result.otherSites + result.elsewhere);
        }
    } else {
        result.missing = missingRequired(result.cookies);
        const QString expiredNote = result.expired > 0
            ? QStringLiteral(" (%1 had already expired.)").arg(result.expired) : QString();
        if (result.missing.contains(QStringLiteral("LOGIN_INFO"))) {
            result.error = QStringLiteral("There is no LOGIN_INFO cookie, so this is not a signed-in session. "
                                          "Sign in at music.youtube.com in the private window first, then "
                                          "export or copy again.") + expiredNote;
        } else if (!result.missing.isEmpty()) {
            result.error = QStringLiteral("There is no SAPISID or __Secure-3PAPISID cookie, which YouTube needs "
                                          "to accept a signed-in request. Export again from the same private "
                                          "window, after signing in.") + expiredNote;
        }
    }
    if (!result.error.isEmpty()) {
        result.cookies.clear();
        result.info = {};
    }
    return result;
}

QByteArray CookieImport::toJson(const QList<Cookie> &cookies, const SessionInfo &info)
{
    QJsonArray list;
    for (const Cookie &cookie : cookies) {
        // Latin-1 both ways, so every byte of a value comes back as it was.
        list.append(QJsonObject{
            { QStringLiteral("name"), QString::fromLatin1(cookie.name) },
            { QStringLiteral("value"), QString::fromLatin1(cookie.value) },
            { QStringLiteral("domain"), cookie.domain },
            { QStringLiteral("hostOnly"), cookie.hostOnly },
            { QStringLiteral("path"), cookie.path },
            { QStringLiteral("expires"), QJsonValue(cookie.expires) },
            { QStringLiteral("secure"), cookie.secure },
            { QStringLiteral("httpOnly"), cookie.httpOnly } });
    }
    QJsonObject jar{ { QStringLiteral("version"), 1 }, { QStringLiteral("cookies"), list } };
    if (!info.isEmpty()) {
        QJsonObject session;
        if (info.authUser >= 0)
            session.insert(QStringLiteral("authUser"), info.authUser);
        if (!info.visitorData.isEmpty())
            session.insert(QStringLiteral("visitorData"), info.visitorData);
        if (!info.dataSyncId.isEmpty())
            session.insert(QStringLiteral("dataSyncId"), info.dataSyncId);
        jar.insert(QStringLiteral("version"), 2);
        jar.insert(QStringLiteral("session"), session);
    }
    return QJsonDocument(jar).toJson(QJsonDocument::Compact);
}

QByteArray CookieImport::toJson(const QList<Cookie> &cookies)
{
    return toJson(cookies, SessionInfo());
}

bool CookieImport::fromJson(const QByteArray &json, QList<Cookie> *cookies, SessionInfo *info)
{
    cookies->clear();
    if (info)
        *info = SessionInfo();
    const QJsonDocument document = QJsonDocument::fromJson(json);
    const QJsonObject jar = document.object();
    const int version = jar.value(QStringLiteral("version")).toInt();
    if (!document.isObject() || (version != 1 && version != 2) || !jar.value(QStringLiteral("cookies")).isArray())
        return false;
    // Checked as an answer's would be; a part that does not pass is simply
    // not known, and the calls do without it.
    SessionInfo known;
    if (version == 2) {
        const QJsonObject session = jar.value(QStringLiteral("session")).toObject();
        const QJsonValue authUser = session.value(QStringLiteral("authUser"));
        if (authUser.isDouble())
            known.authUser = authUserFrom(QByteArray::number(authUser.toInt(-1)));
        known.visitorData = visitorDataFrom(session.value(QStringLiteral("visitorData")).toString());
        known.dataSyncId = dataSyncIdFrom(session.value(QStringLiteral("dataSyncId")).toString());
    }
    QList<Cookie> read;
    for (const QJsonValue &entry : jar.value(QStringLiteral("cookies")).toArray()) {
        const QJsonObject object = entry.toObject();
        Cookie cookie;
        cookie.name = object.value(QStringLiteral("name")).toString().toLatin1();
        cookie.value = object.value(QStringLiteral("value")).toString().toLatin1();
        cookie.domain = object.value(QStringLiteral("domain")).toString();
        cookie.hostOnly = object.value(QStringLiteral("hostOnly")).toBool();
        cookie.path = object.value(QStringLiteral("path")).toString(QStringLiteral("/"));
        cookie.expires = object.value(QStringLiteral("expires")).toInteger();
        cookie.secure = object.value(QStringLiteral("secure")).toBool();
        cookie.httpOnly = object.value(QStringLiteral("httpOnly")).toBool();
        if (!entry.isObject() || cookie.name.isEmpty() || !isYouTubeDomain(cookie.domain))
            return false;
        read.append(cookie);
    }
    *cookies = read;
    if (info)
        *info = known;
    return true;
}
