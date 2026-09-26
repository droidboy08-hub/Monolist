#include "cookieimport.h"

#include <QDateTime>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

#include <cmath>
#include <cstring>

namespace {

using Cookie = CookieImport::Cookie;
using Result = CookieImport::Result;

// 9999-12-31 23:59:59 UTC: the latest expiry a cookies.txt line may carry.
constexpr double kLastExpiry = 253402300799.0;

const QString kMusicHost = QStringLiteral("music.youtube.com");
// Every call Monolist makes with the account is under this path, so a cookie
// that would not be sent here is of no use to it.
const QString kCallPath = QStringLiteral("/youtubei/v1/");

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
// path. What is left is a tie, and the first copy read stays.
bool moreSpecific(const Cookie &a, const Cookie &b)
{
    if (a.domain.size() != b.domain.size())
        return a.domain.size() > b.domain.size();
    if (a.hostOnly != b.hostOnly)
        return a.hostOnly;
    return a.path.size() > b.path.size();
}

// Where every cookie read goes: counted, then kept only if it is for YouTube
// Music, has not expired, and is the most specific copy of its name.
struct Collector {
    Result &result;
    qint64 now;
    QHash<QByteArray, int> byName;

    void add(const Cookie &cookie)
    {
        ++result.read;
        if (!CookieImport::isYouTubeDomain(cookie.domain)) {
            ++result.otherSites;
            return;
        }
        if (!CookieImport::sentToMusic(cookie)) {
            ++result.elsewhere;
            return;
        }
        if (cookie.expires > 0 && cookie.expires <= now) {
            ++result.expired;
            return;
        }
        const auto found = byName.constFind(cookie.name);
        if (found != byName.cend()) {
            ++result.duplicates;
            Cookie &kept = result.cookies[*found];
            if (moreSpecific(cookie, kept))
                kept = cookie;
            return;
        }
        byName.insert(cookie.name, int(result.cookies.size()));
        result.cookies.append(cookie);
    }
};

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
        // so anything past the end of the year 9999 is refused first.
        const double expires = fields.at(4).trimmed().toDouble(&numeric);
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
    for (const QByteArray &line : lines(text)) {
        QByteArray value;
        if (cookieHeaderValue(line, &value)) {
            found = true;
            readPairs(value, collector);
        } else if (headerLine.match(QString::fromLatin1(line)).hasMatch()) {
            otherHeaders = true;
        }
    }
    if (found)
        return;
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
    if (found)
        return;
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

bool CookieImport::isYouTubeDomain(const QString &domain)
{
    return domain == QLatin1String("youtube.com") || domain.endsWith(QLatin1String(".youtube.com"));
}

bool CookieImport::sentToMusic(const Cookie &cookie)
{
    const bool domain = cookie.hostOnly
        ? cookie.domain == kMusicHost
        : cookie.domain == kMusicHost || kMusicHost.endsWith(QLatin1Char('.') + cookie.domain);
    if (!domain)
        return false;
    // RFC 6265's path match, against the one path the calls are under.
    const QString &path = cookie.path;
    if (path.isEmpty() || path == QLatin1String("/"))
        return true;
    return kCallPath.startsWith(path)
           && (path.endsWith(QLatin1Char('/')) || kCallPath.at(path.size()) == QLatin1Char('/'));
}

QByteArray CookieImport::header(const QList<Cookie> &cookies, qint64 now)
{
    if (now <= 0)
        now = QDateTime::currentSecsSinceEpoch();
    QList<QByteArray> pairs;
    for (const Cookie &cookie : cookies) {
        if (!sentToMusic(cookie) || (cookie.expires > 0 && cookie.expires <= now))
            continue;
        pairs << cookie.name + '=' + cookie.value;
    }
    return pairs.join("; ");
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
    return QStringLiteral("%1 for YouTube Music from a %2%3")
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
    if (!result.error.isEmpty())
        result.cookies.clear();
    return result;
}

QByteArray CookieImport::toJson(const QList<Cookie> &cookies)
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
    const QJsonObject jar{ { QStringLiteral("version"), 1 }, { QStringLiteral("cookies"), list } };
    return QJsonDocument(jar).toJson(QJsonDocument::Compact);
}

bool CookieImport::fromJson(const QByteArray &json, QList<Cookie> *cookies)
{
    cookies->clear();
    const QJsonDocument document = QJsonDocument::fromJson(json);
    const QJsonObject jar = document.object();
    if (!document.isObject() || jar.value(QStringLiteral("version")).toInt() != 1
        || !jar.value(QStringLiteral("cookies")).isArray())
        return false;
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
    return true;
}
