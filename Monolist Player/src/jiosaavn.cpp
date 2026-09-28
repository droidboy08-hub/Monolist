#include "jiosaavn.h"
#include "des.h"
#include "rec/matchkey.h"

#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSet>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <utility>

namespace {

const QString kEndpoint = QStringLiteral("https://www.jiosaavn.com/api.php");
// The key every JioSaavn client decrypts its links with. Not a secret: it is
// in each copy of their apps, which is how the documentation came by it.
const QByteArray kLinkKey = QByteArrayLiteral("38346591");
// A desktop Chrome, as JioSaavn's own web player is used.
const QByteArray kChromeUserAgent = QByteArrayLiteral(
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/126.0.0.0 Safari/537.36");
// An address in a block Reliance Jio holds, so JioSaavn takes the request for
// one from India.
const QByteArray kIndianAddress = QByteArrayLiteral("49.36.0.1");

// Short on purpose: the lookup runs beside YouTube's, and a JioSaavn that is
// slow to answer is one the listener should not be kept waiting for.
constexpr int kConnectTimeoutMs = 4000;
constexpr int kRequestTimeoutMs = 6000;
// Fifteen rows: the same recording is often listed several times (the album,
// a compilation, the film's soundtrack), and the one worth having may not be
// the first.
constexpr int kSearchRows = 15;
// A row without its link costs a second request. Two at most per lookup, so a
// search full of such rows cannot turn into a queue of requests.
constexpr int kMaxDetailsCalls = 2;
// The same recording: within three seconds. Different masters of one
// recording differ by a second or two; a radio edit, a live take or an intro
// on a video differ by more.
constexpr qint64 kSameRecordingMs = 3000;
// Up by one whenever the rules below change what is taken: an answer that an
// earlier matcher remembered is then asked about afresh (StreamResolver).
constexpr int kMatcherVersion = 2;

QString field(const QJsonObject &object, const QString &key)
{
    const QJsonValue value = object.value(key);
    if (value.isString())
        return value.toString();
    if (value.isDouble())
        return QString::number(value.toInteger());
    if (value.isBool())
        return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    return {};
}

// ---------------------------------------------------------------- normalising

// Lower case, Latin, Greek and Cyrillic accents off (Rec::plainName), "&" and
// "+" spelt "and", apostrophes gone ("don't" is "dont", not "don t"), and
// everything that is not a letter or a digit a single space. Both sides of a
// comparison go through this, so what matters is only that it is the same.
QString normalise(const QString &text)
{
    QString prepared;
    prepared.reserve(text.size() + 8);
    for (const QChar c : text) {
        switch (c.unicode()) {
        case u'\'': case u'`': case 0x2018: case 0x2019: case 0x00B4: case 0x02BC:
            continue;
        case u'&': case u'+':
            prepared += QStringLiteral(" and ");
            continue;
        default:
            prepared += c;
        }
    }
    return Rec::plainName(prepared);
}

QStringList words(const QString &text)
{
    return normalise(text).split(QLatin1Char(' '), Qt::SkipEmptyParts);
}

bool startsWith(const QStringList &list, qsizetype at, const QStringList &phrase)
{
    if (at + phrase.size() > list.size())
        return false;
    for (qsizetype i = 0; i < phrase.size(); ++i) {
        if (list.at(at + i) != phrase.at(i))
            return false;
    }
    return true;
}

// ------------------------------------------------------------------- artists

// Names that identify nobody on their own: filler words, compilations, and the
// labels that upload most of India's music videos under their own name.
const QSet<QString> &weakKeys()
{
    static const QSet<QString> keys = {
        QStringLiteral("the"), QStringLiteral("and"), QStringLiteral("various"),
        QStringLiteral("variousartists"), QStringLiteral("va"), QStringLiteral("unknown"),
        QStringLiteral("unknownartist"), QStringLiteral("traditional"), QStringLiteral("anonymous"),
        QStringLiteral("dj"), QStringLiteral("mc"), QStringLiteral("lil"), QStringLiteral("band"),
        QStringLiteral("orchestra"), QStringLiteral("choir"), QStringLiteral("official"),
        QStringLiteral("music"), QStringLiteral("records"), QStringLiteral("tseries"),
        QStringLiteral("sonymusicindia"), QStringLiteral("zeemusiccompany"), QStringLiteral("tipsofficial"),
    };
    return keys;
}

// The words that join two credits inside one name as written: "Simon &
// Garfunkel", "A x B", "A feat. B". A name is kept whole AND in its parts,
// so "Simon & Garfunkel" agrees with "Simon and Garfunkel" and with itself
// split at the ampersand, but never with "Paul Simon".
const QSet<QString> &joinWords()
{
    static const QSet<QString> joins = {
        QStringLiteral("and"), QStringLiteral("x"), QStringLiteral("with"), QStringLiteral("feat"),
        QStringLiteral("ft"), QStringLiteral("featuring"), QStringLiteral("vs"),
    };
    return joins;
}

bool usableKey(const QString &key)
{
    return key.size() >= 2 && !weakKeys().contains(key);
}

// One credited name, as the keys it can be found by: the whole of it, and,
// where it joins several ("Simon & Garfunkel", "A x B"), each part.
struct NameKey {
    QString label;           // as written, for the log
    QString whole;           // empty when the name identifies nobody on its own
    QStringList pieces;      // where it joins several; empty otherwise
};

// A name as its letters with the spaces taken out, so "A. R. Rahman" and
// "A.R. Rahman" are one key; and a channel's "- Topic", "VEVO" or "Official"
// taken off, since that is YouTube's way of writing the artist's own name.
NameKey nameKey(const QString &name)
{
    NameKey key;
    key.label = name.trimmed();
    QStringList parts = words(name);
    if (parts.size() > 1 && (parts.last() == QLatin1String("topic") || parts.last() == QLatin1String("official")))
        parts.removeLast();
    if (!parts.isEmpty() && parts.last().endsWith(QLatin1String("vevo"))) {
        if (parts.last() == QLatin1String("vevo"))
            parts.removeLast();
        else
            parts.last().chop(4);
    }
    if (parts.isEmpty())
        return key;
    if (const QString whole = parts.join(QString()); usableKey(whole))
        key.whole = whole;

    QStringList pieces;
    QStringList piece;
    for (qsizetype i = 0; i < parts.size(); ++i) {
        if (joinWords().contains(parts.at(i)) && !piece.isEmpty() && i + 1 < parts.size()) {
            pieces.append(piece.join(QString()));
            piece.clear();
            continue;
        }
        piece.append(parts.at(i));
    }
    pieces.append(piece.join(QString()));
    if (pieces.size() > 1) {
        for (const QString &part : std::as_const(pieces)) {
            if (usableKey(part))
                key.pieces.append(part);
        }
    }
    return key;
}

bool identifies(const NameKey &key)
{
    return !key.whole.isEmpty() || !key.pieces.isEmpty();
}

// Every key of every name, for "is anyone credited on both sides".
QSet<QString> artistKeys(const QStringList &names)
{
    QSet<QString> keys;
    for (const QString &name : names) {
        const NameKey key = nameKey(name);
        if (!key.whole.isEmpty())
            keys.insert(key.whole);
        for (const QString &piece : key.pieces)
            keys.insert(piece);
    }
    return keys;
}

// Whether a name is among the keys: whole, or every one of its parts.
bool credits(const QSet<QString> &keys, const NameKey &name)
{
    if (!name.whole.isEmpty() && keys.contains(name.whole))
        return true;
    if (name.pieces.isEmpty())
        return false;
    for (const QString &piece : name.pieces) {
        if (!keys.contains(piece))
            return false;
    }
    return true;
}

bool intersects(const QSet<QString> &a, const QSet<QString> &b)
{
    for (const QString &key : a) {
        if (b.contains(key))
            return true;
    }
    return false;
}

// -------------------------------------------------------------------- titles

struct Pieces {
    QString main;
    QStringList asides;
};

// Takes the bracketed asides out of a title, nested ones and all: "Kesariya
// (From "Brahmastra (Hindi)")" is "Kesariya" and one aside.
Pieces splitBrackets(const QString &title)
{
    static const QString openers = QStringLiteral("([{\uFF08\u3010\u300C");
    static const QString closers = QStringLiteral(")]}\uFF09\u3011\u300D");
    Pieces out;
    QString current;
    int depth = 0;
    for (const QChar c : title) {
        if (openers.contains(c)) {
            if (depth++ == 0) {
                out.main += QLatin1Char(' ');
                current.clear();
                continue;
            }
        } else if (closers.contains(c) && depth > 0) {
            if (--depth == 0) {
                out.asides.append(current);
                current.clear();
                continue;
            }
        } else if (depth == 0) {
            out.main += c;
            continue;
        }
        current += c;
    }
    if (depth > 0 && !current.trimmed().isEmpty())
        out.asides.append(current);
    return out;
}

// " - ", " | " and their kin: what follows is an aside too ("Yellow - Live in
// Buenos Aires"), unless what comes before is the artist ("Coldplay -
// Yellow"). A hyphen with no spaces round it is part of the title, and so is
// one inside quotes: 'Srivalli - From "Pushpa - The Rise"' has one aside.
QStringList splitDashes(const QString &text)
{
    static const QRegularExpression dash(QStringLiteral("\\s+[-\\x{2013}\\x{2014}|~]\\s+|\\s+//\\s+"));
    const auto quoted = [&text](qsizetype end) {
        qsizetype quotes = 0;
        for (qsizetype i = 0; i < end; ++i) {
            const char16_t c = text.at(i).unicode();
            if (c == u'"' || c == 0x201C || c == 0x201D)
                ++quotes;
        }
        return quotes % 2 == 1;
    };
    QStringList out;
    qsizetype from = 0;
    QRegularExpressionMatchIterator it = dash.globalMatch(text);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        if (quoted(match.capturedStart()))
            continue;
        out.append(text.mid(from, match.capturedStart() - from).trimmed());
        from = match.capturedEnd();
    }
    out.append(text.mid(from).trimmed());
    out.removeAll(QString());
    return out;
}

// What a version marker is called, whichever way it was written. An empty
// name marks release noise: words that say nothing about the recording.
struct Phrase {
    QStringList words;
    QString canonical;
};

const QList<Phrase> &phrases()
{
    static const QList<Phrase> list = [] {
        const QList<std::pair<const char *, const char *>> table = {
            // Different recordings, or the same one changed: must agree.
            { "live", "live" }, { "unplugged", "unplugged" }, { "mtv unplugged", "unplugged" },
            { "remix", "remix" }, { "rmx", "remix" }, { "remixed", "remix" }, { "re mix", "remix" },
            { "mix", "mix" }, { "megamix", "mix" }, { "edit", "edit" }, { "extended", "extended" },
            { "dub", "dub" }, { "vip", "vip" }, { "rework", "rework" }, { "reworked", "rework" },
            { "bootleg", "bootleg" }, { "mashup", "mashup" }, { "medley", "medley" },
            { "acoustic", "acoustic" }, { "stripped", "stripped" }, { "piano", "piano" },
            { "orchestral", "orchestral" }, { "symphonic", "orchestral" },
            { "instrumental", "instrumental" }, { "karaoke", "karaoke" }, { "backing track", "karaoke" },
            { "acapella", "acapella" }, { "acappella", "acapella" }, { "a cappella", "acapella" },
            { "a capella", "acapella" },
            { "sped up", "sped up" }, { "speed up", "sped up" }, { "spedup", "sped up" },
            { "nightcore", "sped up" }, { "slowed", "slowed" }, { "slowed down", "slowed" },
            { "reverb", "reverb" }, { "reverbed", "reverb" }, { "lofi", "lofi" }, { "lo fi", "lofi" },
            { "8d", "8d" }, { "8d audio", "8d" }, { "bass boosted", "bass boosted" },
            { "cover", "cover" }, { "demo", "demo" }, { "reprise", "reprise" }, { "reprised", "reprise" },
            { "recreated", "recreated" }, { "rerecorded", "rerecorded" }, { "re recorded", "rerecorded" },
            { "rerecording", "rerecorded" }, { "re recording", "rerecorded" }, { "re record", "rerecorded" },
            { "rerecord", "rerecorded" }, { "new recording", "rerecorded" }, { "taylors version", "rerecorded" },
            { "encore", "encore" }, { "revisited", "revisited" }, { "flip", "flip" }, { "slow", "slow" },
            { "jhankar", "jhankar" }, { "jhankar beats", "jhankar" }, { "sufi", "sufi" },
            { "synthwave", "synthwave" },
            { "female", "female" }, { "female version", "female" }, { "male", "male" },
            { "male version", "male" }, { "duet", "duet" }, { "sad", "sad" }, { "sad version", "sad" },
            { "session", "session" }, { "sessions", "session" },
            { "chopped", "chopped" }, { "screwed", "chopped" },
            // A song redone in another style: "Manchild (Garage)",
            // "(Techno Version)". JioSaavn is full of them, and one credited
            // to the original artist would otherwise pass as the song.
            { "garage", "garage" }, { "drill", "drill" }, { "hardstyle", "hardstyle" },
            { "techno", "techno" }, { "hypertechno", "techno" }, { "house", "house" },
            { "dubstep", "dubstep" }, { "phonk", "phonk" }, { "jersey club", "jersey club" },
            { "amapiano", "amapiano" }, { "trap", "trap" }, { "edm", "edm" }, { "dnb", "dnb" },
            { "drum and bass", "dnb" }, { "chill", "chill" }, { "chillout", "chill" },
            { "lounge", "lounge" }, { "country", "country" }, { "rock", "rock" }, { "metal", "metal" },
            { "jazz", "jazz" }, { "reggae", "reggae" }, { "rap", "rap" }, { "club", "club" },
            { "dance", "dance" }, { "rave", "rave" }, { "funk", "funk" },
            // A song sung again in another language is another recording:
            // "Srivalli (Hindi)" is not the Telugu original. (The same names
            // are in languages() below.)
            { "hindi", "hindi" }, { "tamil", "tamil" }, { "telugu", "telugu" }, { "kannada", "kannada" },
            { "malayalam", "malayalam" }, { "bengali", "bengali" }, { "marathi", "marathi" },
            { "punjabi", "punjabi" }, { "gujarati", "gujarati" }, { "english", "english" },
            { "spanish", "spanish" }, { "korean", "korean" }, { "japanese", "japanese" },
            { "bhojpuri", "bhojpuri" }, { "urdu", "urdu" }, { "odia", "odia" }, { "assamese", "assamese" },
            { "haryanvi", "haryanvi" }, { "rajasthani", "rajasthani" },
            // Release noise: the same recording under another label.
            { "radio edit", "" }, { "single edit", "" }, { "album edit", "" }, { "clean edit", "" },
            { "explicit edit", "" }, { "radio version", "" }, { "single version", "" },
            { "album version", "" }, { "original version", "" }, { "original mix", "" },
            { "main version", "" }, { "lp version", "" }, { "remaster", "" }, { "remastered", "" },
            { "remastering", "" }, { "digitally remastered", "" }, { "version", "" }, { "mono", "" },
            { "stereo", "" }, { "explicit", "" }, { "clean", "" }, { "censored", "" }, { "uncensored", "" },
            { "bonus track", "" }, { "bonus", "" }, { "deluxe", "" }, { "edition", "" },
            { "expanded", "" }, { "anniversary", "" }, { "official", "" }, { "audio", "" }, { "video", "" },
            { "music video", "" }, { "lyric", "" }, { "lyrics", "" }, { "lyrical", "" }, { "letra", "" },
            { "visualizer", "" }, { "visualiser", "" }, { "hd", "" }, { "hq", "" }, { "4k", "" },
            { "1080p", "" }, { "720p", "" }, { "mv", "" }, { "full song", "" }, { "full video", "" },
            { "full", "" }, { "song", "" }, { "songs", "" }, { "single", "" }, { "ep", "" },
            { "original", "" }, { "studio version", "" }, { "dirty", "" },
            { "soundtrack", "" }, { "ost", "" },
            { "original motion picture soundtrack", "" }, { "from the motion picture", "" },
        };
        QList<Phrase> out;
        for (const auto &[text, canonical] : table)
            out.append({ QString::fromLatin1(text).split(QLatin1Char(' ')), QString::fromLatin1(canonical) });
        // Longest first, so "radio edit" is read before "edit" and "sped up"
        // before anything shorter.
        std::stable_sort(out.begin(), out.end(), [](const Phrase &a, const Phrase &b) {
            return a.words.size() > b.words.size();
        });
        return out;
    }();
    return list;
}

// The version markers that are languages. Kept apart from the rest because a
// language can also come from an album's name, and because JioSaavn says
// which language each row is in.
const QSet<QString> &languages()
{
    static const QSet<QString> set = {
        QStringLiteral("hindi"), QStringLiteral("tamil"), QStringLiteral("telugu"), QStringLiteral("kannada"),
        QStringLiteral("malayalam"), QStringLiteral("bengali"), QStringLiteral("marathi"),
        QStringLiteral("punjabi"), QStringLiteral("gujarati"), QStringLiteral("english"),
        QStringLiteral("spanish"), QStringLiteral("korean"), QStringLiteral("japanese"),
        QStringLiteral("bhojpuri"), QStringLiteral("urdu"), QStringLiteral("odia"), QStringLiteral("assamese"),
        QStringLiteral("haryanvi"), QStringLiteral("rajasthani"),
    };
    return set;
}

// Trailing words a video title carries and the song does not: "Kesariya Full
// Video Song", "Yellow Official Video". Only ever taken off the end, and
// never all of a title.
void stripTrailingNoise(QStringList &list)
{
    static const QList<QStringList> noise = {
        { "official", "music", "video" }, { "official", "lyric", "video" }, { "official", "video" },
        { "official", "audio" }, { "lyric", "video" }, { "lyrical", "video" },
        { "full", "video", "song" }, { "full", "audio", "song" }, { "full", "video" }, { "full", "song" },
        { "video", "song" }, { "audio", "song" }, { "music", "video" }, { "with", "lyrics" },
        { "lyrics" }, { "lyrical" }, { "official" }, { "hd" }, { "hq" }, { "4k" },
    };
    for (bool again = true; again;) {
        again = false;
        for (const QStringList &phrase : noise) {
            if (list.size() > phrase.size() && startsWith(list, list.size() - phrase.size(), phrase)) {
                list.resize(list.size() - phrase.size());
                again = true;
                break;
            }
        }
    }
}

bool isYear(const QString &word)
{
    bool ok = false;
    const int year = word.toInt(&ok);
    return ok && word.size() == 4 && year >= 1900 && year <= 2100;
}

// "1", "iv", "two": a part's number, as a digit.
QString partNumber(const QString &word)
{
    static const QHash<QString, QString> spelt = {
        { QStringLiteral("i"), QStringLiteral("1") }, { QStringLiteral("ii"), QStringLiteral("2") },
        { QStringLiteral("iii"), QStringLiteral("3") }, { QStringLiteral("iv"), QStringLiteral("4") },
        { QStringLiteral("v"), QStringLiteral("5") }, { QStringLiteral("vi"), QStringLiteral("6") },
        { QStringLiteral("vii"), QStringLiteral("7") }, { QStringLiteral("viii"), QStringLiteral("8") },
        { QStringLiteral("ix"), QStringLiteral("9") }, { QStringLiteral("x"), QStringLiteral("10") },
        { QStringLiteral("one"), QStringLiteral("1") }, { QStringLiteral("two"), QStringLiteral("2") },
        { QStringLiteral("three"), QStringLiteral("3") }, { QStringLiteral("four"), QStringLiteral("4") },
        { QStringLiteral("five"), QStringLiteral("5") }, { QStringLiteral("six"), QStringLiteral("6") },
        { QStringLiteral("seven"), QStringLiteral("7") }, { QStringLiteral("eight"), QStringLiteral("8") },
        { QStringLiteral("nine"), QStringLiteral("9") }, { QStringLiteral("ten"), QStringLiteral("10") },
    };
    bool ok = false;
    const int number = word.toInt(&ok);
    if (ok && number >= 0 && word.size() <= 3)
        return QString::number(number);
    return spelt.value(word);
}

// "Pt. 2", "Part II", "Pts. 1-5": which part is a version that must agree —
// part one and part two are different tracks, however alike their titles.
void takeParts(QStringList &list, QStringList &versions)
{
    static const QSet<QString> partWords = {
        QStringLiteral("pt"), QStringLiteral("pts"), QStringLiteral("part"), QStringLiteral("parts"),
    };
    for (qsizetype i = 0; i < list.size(); ++i) {
        if (!partWords.contains(list.at(i)))
            continue;
        QStringList numbers;
        qsizetype end = i + 1;
        // "Part 1 - Part 5" is "Pts. 1-5" written out.
        while (end < list.size()) {
            if (!partNumber(list.at(end)).isEmpty()) {
                numbers.append(partNumber(list.at(end++)));
            } else if (!numbers.isEmpty() && partWords.contains(list.at(end)) && end + 1 < list.size()
                       && !partNumber(list.at(end + 1)).isEmpty()) {
                ++end;
            } else {
                break;
            }
        }
        if (numbers.isEmpty())
            continue;
        versions.append(QStringLiteral("part ") + numbers.join(QLatin1Char(' ')));
        list.remove(i, end - i);
        --i;
    }
}

const QSet<QString> &creditWords()
{
    static const QSet<QString> set = {
        QStringLiteral("feat"), QStringLiteral("ft"), QStringLiteral("featuring"),
    };
    return set;
}

// Words that carry nothing when they sit beside a version marker: "Live *at*
// Wembley" names Wembley.
const QSet<QString> &stopWords()
{
    static const QSet<QString> set = {
        QStringLiteral("in"), QStringLiteral("at"), QStringLiteral("from"), QStringLiteral("the"),
        QStringLiteral("a"), QStringLiteral("an"), QStringLiteral("of"), QStringLiteral("on"),
        QStringLiteral("by"), QStringLiteral("and"), QStringLiteral("for"), QStringLiteral("to"),
        QStringLiteral("with"), QStringLiteral("version"),
    };
    return set;
}

// Whatever an aside says, in the parts of `title` it belongs in. Leans
// towards "this is another recording": words it cannot place are kept, to be
// found on the other side, rather than dropped.
void readAside(const QString &aside, Saavn::Title &title)
{
    QStringList list = words(aside);
    if (list.isEmpty())
        return;
    const QString first = list.first();
    // A credit. "with" counts here, inside brackets or after a dash, and
    // nowhere else: bare, it is "Dancing with a Stranger".
    if (creditWords().contains(first) || first == QLatin1String("with")) {
        if (list.size() == 2 && list.at(1) == QLatin1String("lyrics"))
            return;
        if (list.size() > 1)
            title.credits.append(list.mid(1).join(QLatin1Char(' ')));
        return;
    }
    // The film or the album it is from: its name says nothing about which
    // recording this is, and can be anything at all — including "Live". A
    // language in brackets inside it does: that is how JioSaavn names a dub,
    // 'Deva Deva (From "Brahmastra (Telugu)")', and a film's own name is
    // never bracketed there.
    if (first == QLatin1String("from")) {
        for (const QString &inner : splitBrackets(aside).asides) {
            for (const QString &word : words(inner)) {
                if (languages().contains(word))
                    title.versions.append(word);
            }
        }
        return;
    }
    // Who produced it: says nothing either.
    if (first == QLatin1String("prod")
        || startsWith(list, 0, { QStringLiteral("produced"), QStringLiteral("by") })
        || startsWith(list, 0, { QStringLiteral("music"), QStringLiteral("by") }))
        return;

    // A credit after the marker: "(Chromatics Remix feat. X)".
    for (qsizetype i = 1; i < list.size(); ++i) {
        if (creditWords().contains(list.at(i)) || list.at(i) == QLatin1String("with")) {
            if (i + 1 < list.size())
                title.credits.append(list.mid(i + 1).join(QLatin1Char(' ')));
            list.resize(i);
            break;
        }
    }

    takeParts(list, title.versions);
    const bool saysVersion = list.contains(QLatin1String("version"));
    bool marked = false;
    bool noise = false;
    QStringList rest;
    QStringList years;
    for (qsizetype i = 0; i < list.size();) {
        bool matched = false;
        for (const Phrase &phrase : phrases()) {
            if (startsWith(list, i, phrase.words)) {
                if (!phrase.canonical.isEmpty()) {
                    title.versions.append(phrase.canonical);
                    marked = true;
                } else {
                    noise = true;
                }
                i += phrase.words.size();
                matched = true;
                break;
            }
        }
        if (matched)
            continue;
        if (isYear(list.at(i)))
            years.append(list.at(i));
        else if (!stopWords().contains(list.at(i)))
            rest.append(list.at(i));
        ++i;
    }
    // Beside a marker: whose remix, which concert.
    if (marked) {
        title.versionContext.append(rest);
        return;
    }
    // Noise and a year ("Remastered 2011") is noise. A year on its own,
    // "(2012)", dates a recording as much as a release, so it is kept.
    if (rest.isEmpty()) {
        if (!noise)
            title.unknown.append(years);
        return;
    }
    // "(Arijit Singh Version)", "(Film Version)", "(Version 2)": a version
    // that names itself, which the other side must name the same way.
    if (saysVersion) {
        title.versions.append(rest.join(QLatin1Char(' ')) + QStringLiteral(" version"));
        return;
    }
    title.unknown.append(rest);
}

// Everything the comparison needs about one side, worked out once.
struct Side {
    Saavn::Title title;
    QStringList versions;        // title.versions without the languages
    QSet<QString> languages;     // named in the title, or in the album's
    QString rowLanguage;         // JioSaavn's own field; rows only
    QList<NameKey> names;        // who is credited: the artist line and the title's credits
    QSet<QString> keys;
    QString album;               // albumKey
    QSet<QString> vocabulary;    // every word of the title, the artists and the album
    qint64 durationMs = 0;
};

// An album's name as far as it tells one album from another: its core title
// and whatever its asides say ("Part One", "- The Rise"), without release
// noise or a language, which is compared on its own.
QString albumKey(const Saavn::Title &album)
{
    QStringList parts{ album.core };
    for (const QString &version : album.versions) {
        if (!languages().contains(version))
            parts.append(version);
    }
    parts.append(album.unknown);
    parts.append(album.versionContext);
    return parts.join(QLatin1Char(' ')).simplified();
}

Side makeSide(const QString &title, const QStringList &artists, const QString &album)
{
    Side side;
    side.title = Saavn::parseTitle(title, artists);
    for (const QString &version : std::as_const(side.title.versions)) {
        if (languages().contains(version))
            side.languages.insert(version);
        else
            side.versions.append(version);
    }
    const Saavn::Title albumTitle = Saavn::parseTitle(album);
    for (const QString &version : albumTitle.versions) {
        if (languages().contains(version))
            side.languages.insert(version);
    }
    side.album = albumKey(albumTitle);
    for (const QString &name : artists + side.title.credits) {
        const NameKey key = nameKey(name);
        if (identifies(key))
            side.names.append(key);
    }
    side.keys = artistKeys(artists + side.title.credits);
    for (const QString &text : QStringList{ title, artists.join(QLatin1Char(' ')), album }) {
        for (const QString &word : words(text))
            side.vocabulary.insert(word);
    }
    return side;
}

Side ourSide(const Saavn::Target &target)
{
    Side side = makeSide(target.title, Saavn::splitArtists(target.artist), target.album);
    side.durationMs = qMax<qint64>(0, target.durationMs);
    return side;
}

Side rowSide(const Saavn::Row &row)
{
    Side side = makeSide(row.title, row.artists, row.album);
    side.rowLanguage = row.language;
    side.durationMs = qint64(qMax(0, row.durationSec)) * 1000;
    return side;
}

QString describe(const QStringList &versions)
{
    return versions.isEmpty() ? QStringLiteral("the studio version") : versions.join(QStringLiteral(", "));
}

QString describe(const QSet<QString> &languages)
{
    QStringList list(languages.cbegin(), languages.cend());
    list.sort();
    return list.isEmpty() ? QStringLiteral("no language named") : list.join(QStringLiteral(", "));
}

// The words beside a version marker that could name someone or somewhere:
// "DJ" names nobody.
QStringList context(const Side &side)
{
    QStringList out;
    for (const QString &word : side.title.versionContext) {
        if (!weakKeys().contains(word))
            out.append(word);
    }
    return out;
}

// Empty when `theirs` is this recording; otherwise why not.
QString refusal(const Side &ours, const Side &theirs)
{
    if (ours.title.core.isEmpty() || ours.title.core != theirs.title.core) {
        return QStringLiteral("title \"%1\" is not \"%2\"").arg(theirs.title.core, ours.title.core);
    }
    if (ours.versions != theirs.versions)
        return QStringLiteral("%1, where this is %2").arg(describe(theirs.versions), describe(ours.versions));

    // A language named on one side is a dub on the other unless it names the
    // same. Where only ours names one, the row's own language field counts
    // for the row: "Srivalli" in JioSaavn's Telugu is the Telugu Srivalli.
    QSet<QString> theirLanguages = theirs.languages;
    if (theirLanguages.isEmpty() && !ours.languages.isEmpty() && !theirs.rowLanguage.isEmpty())
        theirLanguages.insert(theirs.rowLanguage);
    if (ours.languages != theirLanguages) {
        return QStringLiteral("%1, where this is %2")
            .arg(describe(theirLanguages), describe(ours.languages));
    }

    // Words neither side can place ("(Synthwave)", "(Encore)", an alternate
    // title) must be somewhere on the other side.
    const auto missing = [](const Side &from, const Side &in) {
        QStringList out;
        for (const QString &word : from.title.unknown) {
            if (!in.vocabulary.contains(word))
                out.append(word);
        }
        return out;
    };
    if (const QStringList words = missing(theirs, ours); !words.isEmpty())
        return QStringLiteral("\"%1\" in the title, which this does not say").arg(words.join(QLatin1Char(' ')));
    if (const QStringList words = missing(ours, theirs); !words.isEmpty())
        return QStringLiteral("does not say \"%1\", which this does").arg(words.join(QLatin1Char(' ')));

    if (!intersects(ours.keys, theirs.keys))
        return QStringLiteral("no artist in common");
    if (ours.durationMs > 0) {
        // Our length known and theirs not: nothing ties the row to it.
        if (theirs.durationMs <= 0)
            return QStringLiteral("JioSaavn gives no length to compare");
        const qint64 delta = theirs.durationMs - ours.durationMs;
        if (qAbs(delta) > kSameRecordingMs) {
            return QStringLiteral("%1 s %2").arg(qRound(qAbs(delta) / 1000.0))
                .arg(delta > 0 ? QStringLiteral("longer") : QStringLiteral("shorter"));
        }
    }

    // Both a remix, or both live: whose remix, and where, must not disagree.
    // Named on one side only, the name must be found on the other ("Yellow
    // (Live)" from the album "Live in Buenos Aires" is the Buenos Aires take;
    // from "Live 2003" it is not).
    const QStringList a = context(ours);
    const QStringList b = context(theirs);
    const QString kind = describe(ours.title.versions);
    if (!a.isEmpty() && !b.isEmpty()) {
        if (!intersects(QSet<QString>(a.cbegin(), a.cend()), QSet<QString>(b.cbegin(), b.cend()))) {
            return QStringLiteral("a different %1 (%2, where this is %3)")
                .arg(kind, b.join(QLatin1Char(' ')), a.join(QLatin1Char(' ')));
        }
    } else if (!a.isEmpty() || !b.isEmpty()) {
        const QStringList &named = a.isEmpty() ? b : a;
        const Side &other = a.isEmpty() ? ours : theirs;
        const bool found = std::any_of(named.cbegin(), named.cend(),
                                       [&other](const QString &word) { return other.vocabulary.contains(word); });
        if (!found) {
            return a.isEmpty()
                ? QStringLiteral("%1 \"%2\", where this names none").arg(kind, b.join(QLatin1Char(' ')))
                : QStringLiteral("names no \"%2\", as this %1 does").arg(kind, a.join(QLatin1Char(' ')));
        }
    }
    return {};
}

QString rowLabel(const Saavn::Row &row)
{
    return QStringLiteral("%1 — %2").arg(row.title, row.artists.join(QStringLiteral(", ")));
}

} // namespace

namespace Saavn {

QString decodeEntities(const QString &text)
{
    static const QHash<QString, QString> named = {
        { QStringLiteral("quot"), QStringLiteral("\"") }, { QStringLiteral("amp"), QStringLiteral("&") },
        { QStringLiteral("apos"), QStringLiteral("'") }, { QStringLiteral("lt"), QStringLiteral("<") },
        { QStringLiteral("gt"), QStringLiteral(">") }, { QStringLiteral("nbsp"), QStringLiteral(" ") },
    };
    QString current = text;
    // Twice: JioSaavn now and then encodes an entity again ("&amp;quot;").
    for (int pass = 0; pass < 2 && current.contains(QLatin1Char('&')); ++pass) {
        QString out;
        out.reserve(current.size());
        for (qsizetype i = 0; i < current.size(); ++i) {
            const QChar c = current.at(i);
            if (c == QLatin1Char('&')) {
                const qsizetype semi = current.indexOf(QLatin1Char(';'), i + 1);
                if (semi > i + 1 && semi - i <= 10) {
                    const QString name = current.mid(i + 1, semi - i - 1);
                    QString replacement;
                    if (name.startsWith(QLatin1Char('#'))) {
                        bool ok = false;
                        const bool hex = name.size() > 1 && (name.at(1) == QLatin1Char('x') || name.at(1) == QLatin1Char('X'));
                        const uint code = hex ? name.mid(2).toUInt(&ok, 16) : name.mid(1).toUInt(&ok, 10);
                        if (ok && code > 0 && code <= 0x10FFFF && (code < 0xD800 || code > 0xDFFF)) {
                            const char32_t point = code;
                            replacement = QString::fromUcs4(&point, 1);
                        }
                    } else {
                        replacement = named.value(name);
                    }
                    if (!replacement.isEmpty()) {
                        out += replacement;
                        i = semi;
                        continue;
                    }
                }
            }
            out += c;
        }
        current = out;
    }
    return current;
}

QString decryptMediaUrl(const QString &encrypted)
{
    const auto decoded = QByteArray::fromBase64Encoding(encrypted.trimmed().toLatin1(),
                                                        QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded)
        return {};
    bool ok = false;
    const QByteArray plain = Des::decryptEcb(kLinkKey, *decoded, &ok);
    if (!ok)
        return {};
    const QString text = QString::fromUtf8(plain).trimmed();
    // Some of JioSaavn's links have been plain HTTP. Their CDN answers the
    // same path over HTTPS, and a stream fetched in the clear is one anyone
    // on the way can read or replace.
    QUrl url(text, QUrl::StrictMode);
    if (url.scheme() == QLatin1String("http"))
        url.setScheme(QStringLiteral("https"));
    const QString link = url.toString(QUrl::FullyEncoded);
    return isCdnLink(link) ? link : QString();
}

QString encryptMediaUrl(const QString &url)
{
    return QString::fromLatin1(Des::encryptEcb(kLinkKey, url.toUtf8()).toBase64());
}

bool isCdnLink(const QString &link)
{
    const QUrl url(link, QUrl::StrictMode);
    if (!url.isValid() || url.scheme() != QLatin1String("https") || !url.userInfo().isEmpty())
        return false;
    const QString host = url.host().toLower();
    return host == QLatin1String("saavncdn.com") || host.endsWith(QLatin1String(".saavncdn.com"));
}

QString signature(const Target &target)
{
    const QString text = QStringList{ normalise(target.title), normalise(target.artist), normalise(target.album),
                                      QString::number(qRound(qMax<qint64>(0, target.durationMs) / 1000.0)) }
                             .join(QLatin1Char('\n'));
    return QString::fromLatin1(
        QCryptographicHash::hash(text.toUtf8(), QCryptographicHash::Sha256).toHex().left(32));
}

int matcherVersion()
{
    return kMatcherVersion;
}

Stream streamFor(const QString &decryptedUrl, bool has320)
{
    Stream stream;
    stream.url = decryptedUrl;
    QUrl url(decryptedUrl);
    const QString path = url.path();
    static const QRegularExpression rendition(QStringLiteral("_(48|96|160|320)\\.(mp4|aac|mp3)$"));
    const QRegularExpressionMatch match = rendition.match(path);
    if (!match.hasMatch())
        return stream;
    stream.readable = true;
    stream.kbps = match.captured(1).toInt();
    // Only where the row says 320 exists: asked for a rendition it does not
    // have, the CDN answers 403.
    if (has320 && stream.kbps != 320) {
        QString upgraded = path;
        upgraded.replace(match.capturedStart(1), match.capturedLength(1), QStringLiteral("320"));
        url.setPath(upgraded);
        stream.url = url.toString(QUrl::FullyEncoded);
        stream.kbps = 320;
    }
    return stream;
}

bool acceptable(const Stream &stream, QString *why)
{
    if (stream.url.isEmpty()) {
        if (why)
            *why = QStringLiteral("no link");
        return false;
    }
    if (stream.readable && stream.kbps <= 96) {
        if (why)
            *why = QStringLiteral("only %1 kbps, which YouTube beats").arg(stream.kbps);
        return false;
    }
    return true;
}

Row rowFrom(const QJsonObject &song)
{
    Row row;
    const QJsonObject more = song.value(QStringLiteral("more_info")).toObject();
    // Search rows keep most of this under more_info; older details answers
    // keep it at the top.
    const auto either = [&song, &more](const QString &key) {
        const QString value = field(more, key);
        return value.isEmpty() ? field(song, key) : value;
    };
    row.id = field(song, QStringLiteral("id"));
    row.title = decodeEntities(field(song, QStringLiteral("title"))).trimmed();
    if (row.title.isEmpty())
        row.title = decodeEntities(field(song, QStringLiteral("song"))).trimmed();
    row.album = decodeEntities(either(QStringLiteral("album"))).trimmed();
    row.language = either(QStringLiteral("language")).trimmed().toLower();
    row.durationSec = either(QStringLiteral("duration")).toInt();
    row.explicitContent = either(QStringLiteral("explicit_content")) == QLatin1String("1");
    row.has320 = either(QStringLiteral("320kbps")) == QLatin1String("true");
    row.encryptedUrl = either(QStringLiteral("encrypted_media_url"));

    const auto add = [&row](const QString &raw) {
        const QString name = decodeEntities(raw).trimmed();
        if (!name.isEmpty() && !row.artists.contains(name))
            row.artists.append(name);
    };
    QJsonObject map = more.value(QStringLiteral("artistMap")).toObject();
    if (map.isEmpty())
        map = song.value(QStringLiteral("artistMap")).toObject();
    for (const QString &role : { QStringLiteral("primary_artists"), QStringLiteral("featured_artists") }) {
        const QJsonArray artists = map.value(role).toArray();
        for (const QJsonValue &artist : artists)
            add(artist.toObject().value(QStringLiteral("name")).toString());
    }
    // Older answers give the names as one string each.
    if (row.artists.isEmpty()) {
        for (const QString &key : { QStringLiteral("primary_artists"), QStringLiteral("featured_artists") }) {
            for (const QString &name : either(key).split(QLatin1Char(','), Qt::SkipEmptyParts))
                add(name);
        }
    }
    if (row.artists.isEmpty()) {
        for (const QString &name : either(QStringLiteral("singers")).split(QLatin1Char(','), Qt::SkipEmptyParts))
            add(name);
    }
    // Last, the subtitle: "Artist, Artist - Album".
    if (row.artists.isEmpty()) {
        const QString subtitle = decodeEntities(field(song, QStringLiteral("subtitle")));
        for (const QString &name : subtitle.section(QStringLiteral(" - "), 0, 0).split(QLatin1Char(','), Qt::SkipEmptyParts))
            add(name);
    }
    return row;
}

bool isSearchAnswer(const QJsonObject &root)
{
    return root.value(QStringLiteral("results")).isArray();
}

QList<Row> rowsFromSearch(const QJsonObject &root)
{
    QList<Row> rows;
    const QJsonArray results = root.value(QStringLiteral("results")).toArray();
    for (const QJsonValue &value : results) {
        const QJsonObject song = value.toObject();
        const QString type = field(song, QStringLiteral("type"));
        if (!type.isEmpty() && type != QLatin1String("song"))
            continue;
        const Row row = rowFrom(song);
        if (!row.id.isEmpty() && !row.title.isEmpty())
            rows.append(row);
    }
    return rows;
}

bool rowFromDetails(const QJsonObject &root, const QString &id, Row *row)
{
    QJsonObject song;
    const QJsonValue songs = root.value(QStringLiteral("songs"));
    if (songs.isArray()) {
        // The object for the id asked about, never simply the first: a
        // details answer for one song must not hand over another's link.
        const QJsonArray list = songs.toArray();
        for (const QJsonValue &value : list) {
            const QJsonObject object = value.toObject();
            const QString objectId = field(object, QStringLiteral("id"));
            if (objectId == id || (objectId.isEmpty() && list.size() == 1)) {
                song = object;
                break;
            }
        }
    } else if (root.value(id).isObject()) {
        song = root.value(id).toObject();
    }
    if (song.isEmpty())
        return false;
    Row read = rowFrom(song);
    if (read.id.isEmpty())
        read.id = id;
    if (read.id != id)
        return false;
    *row = read;
    return true;
}

// Split where a line of credits separates names outright. "&" and "and" are
// not among them: they are also inside names ("Simon & Garfunkel"), and
// artistKeys keeps such a name whole as well as in parts.
QStringList splitArtists(const QString &artistLine)
{
    static const QRegularExpression separators(
        QStringLiteral("\\s*[,;/|\\x{3001}]\\s*|\\s+(?:feat\\.?|ft\\.?|featuring|vs\\.?)\\s+|\\s+[xX]\\s+"),
        QRegularExpression::CaseInsensitiveOption);
    QStringList names;
    for (const QString &part : artistLine.split(separators, Qt::SkipEmptyParts)) {
        const QString name = part.trimmed();
        if (!name.isEmpty())
            names.append(name);
    }
    return names;
}

Title parseTitle(const QString &title, const QStringList &artistNames)
{
    Title result;
    const Pieces pieces = splitBrackets(decodeEntities(title));
    QStringList segments = splitDashes(pieces.main);
    if (segments.isEmpty())
        segments.append(QString());

    // "Coldplay - Yellow": the head is the artist, the song comes after.
    qsizetype start = 0;
    if (segments.size() >= 2) {
        const QSet<QString> ours = artistKeys(artistNames);
        if (!ours.isEmpty() && intersects(artistKeys(splitArtists(segments.first())), ours))
            start = 1;
    }
    QStringList asides = pieces.asides;
    for (qsizetype i = start + 1; i < segments.size(); ++i)
        asides.append(segments.at(i));

    QStringList main = words(segments.at(start));
    // A bare credit ends the title: "Song feat. Someone". Never a bare
    // "with", which is too often the title itself.
    for (qsizetype i = 0; i < main.size(); ++i) {
        if (creditWords().contains(main.at(i))) {
            if (i + 1 < main.size())
                result.credits.append(main.mid(i + 1).join(QLatin1Char(' ')));
            main.resize(i);
            break;
        }
    }
    stripTrailingNoise(main);
    takeParts(main, result.versions);
    result.core = main.join(QLatin1Char(' '));

    for (const QString &aside : std::as_const(asides))
        readAside(aside, result);

    std::sort(result.versions.begin(), result.versions.end());
    result.versions.erase(std::unique(result.versions.begin(), result.versions.end()), result.versions.end());
    return result;
}

QString searchQuery(const Target &target)
{
    const QStringList names = splitArtists(target.artist);
    const Title title = parseTitle(target.title, names);
    QStringList query;
    if (!title.core.isEmpty())
        query.append(title.core);
    // The version belongs in the search, so the live take or the remix is
    // among the rows at all.
    for (const QString &version : title.versions)
        query.append(version);
    if (!names.isEmpty()) {
        QStringList lead = words(names.first());
        if (!lead.isEmpty() && lead.last().endsWith(QLatin1String("vevo")))
            lead.last().chop(4);
        if (!lead.isEmpty() && lead.last() == QLatin1String("topic"))
            lead.removeLast();
        query.append(lead.join(QLatin1Char(' ')));
    }
    return query.join(QLatin1Char(' ')).simplified();
}

Judgement judge(const Target &target, const Row &row)
{
    const QString why = refusal(ourSide(target), rowSide(row));
    return { why.isEmpty(), why };
}

Choice choose(const Target &target, const QList<Row> &rows)
{
    Choice choice;
    const Side ours = ourSide(target);
    if (ours.title.core.isEmpty()) {
        choice.reason = QStringLiteral("the title has nothing to match on");
        return choice;
    }
    if (rows.isEmpty()) {
        choice.reason = QStringLiteral("JioSaavn found nothing");
        return choice;
    }

    // Every row once, and its side worked out once.
    QList<int> listed;
    QList<Side> sides(rows.size());
    QList<int> plausible;
    QSet<QString> seen;
    for (int i = 0; i < rows.size(); ++i) {
        const Row &row = rows.at(i);
        if (!row.id.isEmpty()) {
            if (seen.contains(row.id))
                continue;
            seen.insert(row.id);
        }
        listed.append(i);
        sides[i] = rowSide(row);
        const QString why = refusal(ours, sides.at(i));
        if (!why.isEmpty()) {
            choice.refusals.append(rowLabel(row) + QStringLiteral(": ") + why);
            continue;
        }
        plausible.append(i);
    }

    // A name of ours that this row does not credit and another listing of
    // the same song does (the same title and version, in any language, at a
    // length that fits): that listing is likelier ours, and this one may be
    // the song sung by someone else. The Hindi "Deva Deva" credits every
    // name the Telugu one does but its singer, and the Telugu one is refused
    // for its language: neither is taken, which is right for a Telugu song.
    const auto fits = [&ours](const Side &side) {
        return ours.durationMs <= 0 || side.durationMs <= 0
               || qAbs(side.durationMs - ours.durationMs) <= kSameRecordingMs;
    };
    for (auto it = plausible.begin(); it != plausible.end();) {
        const Side &side = sides.at(*it);
        QString lacking;
        QString elsewhere;
        for (const NameKey &name : ours.names) {
            if (credits(side.keys, name))
                continue;
            for (const int j : std::as_const(listed)) {
                const Side &other = sides.at(j);
                if (j != *it && other.title.core == ours.title.core && other.versions == ours.versions
                    && fits(other) && credits(other.keys, name)) {
                    lacking = name.label;
                    elsewhere = rowLabel(rows.at(j));
                    break;
                }
            }
            if (!lacking.isEmpty())
                break;
        }
        if (lacking.isEmpty()) {
            ++it;
            continue;
        }
        choice.refusals.append(QStringLiteral("%1: does not credit %2, and %3 does")
                                   .arg(rowLabel(rows.at(*it)), lacking, elsewhere));
        it = plausible.erase(it);
    }

    if (plausible.isEmpty()) {
        choice.reason = QStringLiteral("none of the %1 rows is this recording").arg(rows.size());
        return choice;
    }
    const auto refuseAll = [&choice, &rows](const QList<int> &which, const QString &reason, const QString &each) {
        choice.reason = reason;
        for (const int i : which)
            choice.refusals.append(rowLabel(rows.at(i)) + QStringLiteral(": ") + each);
    };
    // Without the song's own length, two rows that both fit could be two
    // recordings, and nothing is left to tell them apart.
    if (ours.durationMs <= 0 && plausible.size() > 1) {
        refuseAll(plausible,
                  QStringLiteral("the song's length is unknown and %1 rows fit, so none is taken").arg(plausible.size()),
                  QStringLiteral("one of several that fit, with no length to choose by"));
        return choice;
    }

    // Rows that fit and disagree about where they come from may be two
    // recordings — the film's song and its dub, which share a backing track
    // and so a length. Our album, where it names one of theirs, settles it,
    // before anything about bitrates.
    QList<int> pool = plausible;
    if (!ours.album.isEmpty()) {
        QList<int> same;
        for (const int i : std::as_const(pool)) {
            if (sides.at(i).album == ours.album)
                same.append(i);
        }
        if (!same.isEmpty() && same.size() < pool.size()) {
            for (const int i : std::as_const(pool)) {
                if (!same.contains(i))
                    choice.refusals.append(rowLabel(rows.at(i)) + QStringLiteral(": from another album than this"));
            }
            pool = same;
        }
    }
    // Otherwise rows in two languages cannot be told apart at all. (Rows on
    // two albums can: nearly every well-known song is on a dozen
    // compilations, credited a little differently on each, and refusing
    // them all, as the paper's rule in 3.3 would, refuses the song. What
    // tells a dub from its original is the language, and a performer of
    // ours missing — both checked.)
    if (ours.languages.isEmpty()) {
        QSet<QString> spoken;
        for (const int i : std::as_const(pool)) {
            if (!sides.at(i).rowLanguage.isEmpty())
                spoken.insert(sides.at(i).rowLanguage);
        }
        if (spoken.size() > 1) {
            refuseAll(pool,
                      QStringLiteral("rows in %1 fit, and nothing says which language this is").arg(describe(spoken)),
                      QStringLiteral("one of several languages that fit"));
            return choice;
        }
    }
    // Uncensored first, then the one offered at 320 kbps, then the closest
    // in length; JioSaavn's own order (most played first) breaks what is
    // left. Our album and our names have already done their part: every row
    // left is from the album ours names, where one does, and credits the
    // same of our names.
    const auto distance = [&ours, &sides](int i) {
        return ours.durationMs > 0 && sides.at(i).durationMs > 0 ? qAbs(sides.at(i).durationMs - ours.durationMs)
                                                                : qint64(-1);
    };
    std::stable_sort(pool.begin(), pool.end(), [&](int a, int b) {
        const Row &x = rows.at(a);
        const Row &y = rows.at(b);
        if (x.explicitContent != y.explicitContent)
            return x.explicitContent;
        if (x.has320 != y.has320)
            return x.has320;
        const qint64 dx = distance(a);
        const qint64 dy = distance(b);
        if ((dx < 0) != (dy < 0))
            return dx >= 0;
        return dx < dy;
    });
    choice.accepted = pool;
    return choice;
}

} // namespace Saavn

// ------------------------------------------------------------------ the client

struct JioSaavn::Lookup {
    Saavn::Target target;
    Callback done;
    QElapsedTimer clock;
    QList<Saavn::Row> rows;
    QList<int> order;          // rows accepted by the matcher, best first
    qsizetype next = 0;        // the next of them to try for a link
    int detailsCalls = 0;
    QString detailsError;      // a song.getDetails that JioSaavn did not answer
    QStringList refusals;
    QString noMatchReason;
    bool finished = false;
};

JioSaavn::JioSaavn(QObject *parent)
    : QObject(parent)
    , m_network(new QNetworkAccessManager(this))
{
}

void JioSaavn::lookup(const Saavn::Target &target, Callback done)
{
    auto state = std::make_shared<Lookup>();
    state->target = target;
    state->done = std::move(done);
    state->clock.start();

    const QString query = Saavn::searchQuery(target);
    if (query.isEmpty()) {
        Result result;
        result.kind = Result::NoMatch;
        result.reason = QStringLiteral("nothing to search for");
        // Queued, so the caller always hears back after lookup() returns.
        QMetaObject::invokeMethod(this, [this, state, result]() { finish(state, result); },
                                  Qt::QueuedConnection);
        return;
    }

    QUrlQuery params;
    params.addQueryItem(QStringLiteral("__call"), QStringLiteral("search.getResults"));
    params.addQueryItem(QStringLiteral("_format"), QStringLiteral("json"));
    params.addQueryItem(QStringLiteral("_marker"), QStringLiteral("0"));
    params.addQueryItem(QStringLiteral("api_version"), QStringLiteral("4"));
    params.addQueryItem(QStringLiteral("ctx"), QStringLiteral("android"));
    params.addQueryItem(QStringLiteral("q"), query);
    params.addQueryItem(QStringLiteral("p"), QStringLiteral("1"));
    params.addQueryItem(QStringLiteral("n"), QString::number(kSearchRows));

    get(params, [this, state](const QJsonObject &root, const QString &error) {
        if (!error.isEmpty()) {
            Result result;
            result.kind = Result::Failed;
            result.reason = error;
            finish(state, result);
            return;
        }
        // An error or a throttle, sent as HTTP 200: not heard from, rather
        // than "not there", which would be remembered for a day.
        if (!Saavn::isSearchAnswer(root)) {
            Result result;
            result.kind = Result::Failed;
            result.reason = QStringLiteral("JioSaavn answered without any results list");
            finish(state, result);
            return;
        }
        state->rows = Saavn::rowsFromSearch(root);
        const Saavn::Choice choice = Saavn::choose(state->target, state->rows);
        state->order = choice.accepted;
        state->refusals = choice.refusals;
        state->noMatchReason = choice.reason;
        tryNext(state);
    });
}

// The link comes with the search row (one request per lookup, D11); a row
// that came without one is asked about on its own, and only then.
void JioSaavn::tryNext(const std::shared_ptr<Lookup> &state)
{
    while (state->next < state->order.size()) {
        const int index = state->order.at(state->next);
        Saavn::Row &row = state->rows[index];
        const QString label = QStringLiteral("%1 — %2").arg(row.title, row.artists.join(QStringLiteral(", ")));

        if (row.encryptedUrl.isEmpty()) {
            if (state->detailsCalls >= kMaxDetailsCalls) {
                state->refusals.append(label + QStringLiteral(": came without a link"));
                ++state->next;
                continue;
            }
            ++state->detailsCalls;
            QUrlQuery params;
            params.addQueryItem(QStringLiteral("__call"), QStringLiteral("song.getDetails"));
            params.addQueryItem(QStringLiteral("_format"), QStringLiteral("json"));
            params.addQueryItem(QStringLiteral("_marker"), QStringLiteral("0"));
            params.addQueryItem(QStringLiteral("api_version"), QStringLiteral("4"));
            params.addQueryItem(QStringLiteral("ctx"), QStringLiteral("android"));
            params.addQueryItem(QStringLiteral("pids"), row.id);
            get(params, [this, state, index, label](const QJsonObject &root, const QString &error) {
                Saavn::Row &row = state->rows[index];
                Saavn::Row details;
                if (error.isEmpty() && Saavn::rowFromDetails(root, row.id, &details)) {
                    row.encryptedUrl = details.encryptedUrl;
                    row.has320 = row.has320 || details.has320;
                }
                if (row.encryptedUrl.isEmpty()) {
                    state->refusals.append(label + (error.isEmpty() ? QStringLiteral(": no link in its details")
                                                                    : QStringLiteral(": ") + error));
                    if (!error.isEmpty())
                        state->detailsError = error;
                    ++state->next;
                }
                tryNext(state);
            });
            return;
        }

        const QString url = Saavn::decryptMediaUrl(row.encryptedUrl);
        if (url.isEmpty()) {
            state->refusals.append(label + QStringLiteral(": its link did not decrypt"));
            ++state->next;
            continue;
        }
        const Saavn::Stream stream = Saavn::streamFor(url, row.has320);
        QString why;
        if (!Saavn::acceptable(stream, &why)) {
            state->refusals.append(label + QStringLiteral(": ") + why);
            ++state->next;
            continue;
        }
        if (!stream.readable) {
            qInfo("jiosaavn: the link for %s names no bitrate; taken as it is",
                  qPrintable(state->target.videoId.isEmpty() ? row.id : state->target.videoId));
        }

        Result result;
        result.kind = Result::Match;
        result.url = stream.url;
        result.kbps = stream.kbps;
        result.saavnId = row.id;
        result.title = row.title;
        result.artists = row.artists.join(QStringLiteral(", "));
        result.album = row.album;
        result.durationSec = row.durationSec;
        finish(state, result);
        return;
    }

    Result result;
    result.kind = Result::NoMatch;
    if (!state->detailsError.isEmpty()) {
        // A row that fits, whose link JioSaavn did not give when asked: it
        // may well have one, so nothing is remembered.
        result.kind = Result::Failed;
        result.reason = QStringLiteral("a row that fits could not be asked about (%1)").arg(state->detailsError);
    } else if (!state->order.isEmpty()) {
        result.reason = QStringLiteral("no row that fits has a usable stream");
    } else {
        result.reason = state->noMatchReason;
    }
    finish(state, result);
}

void JioSaavn::finish(const std::shared_ptr<Lookup> &state, Result result)
{
    if (state->finished)
        return;
    state->finished = true;
    result.elapsedMs = state->clock.elapsed();
    result.rows = int(state->rows.size());
    result.refusals = state->refusals;
    if (state->done)
        state->done(result);
}

void JioSaavn::get(const QUrlQuery &query, Answer done)
{
    QUrl url(kEndpoint);
    url.setQuery(query);
    QNetworkRequest request(url);
    request.setRawHeader("User-Agent", kChromeUserAgent);
    request.setRawHeader("Accept", "application/json");
    request.setRawHeader("Accept-Language", "en-IN,en;q=0.9");
    request.setRawHeader("Cookie", "explicit_content=1");
    if (m_indiaHeaders) {
        request.setRawHeader("X-Forwarded-For", kIndianAddress);
        request.setRawHeader("X-Real-IP", kIndianAddress);
    }
    // Redirects are followed only to JioSaavn's own hosts, over HTTPS: Qt
    // carries every header above into a redirected request, and the Indian
    // address, the cookie and the query are for JioSaavn alone.
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::UserVerifiedRedirectPolicy);
    request.setTransferTimeout(kRequestTimeoutMs);
    QNetworkReply *reply = m_network->get(request);
    connect(reply, &QNetworkReply::redirected, reply, [reply](const QUrl &to) {
        const QUrl target = reply->url().resolved(to);
        const QString host = target.host().toLower();
        if (target.scheme() == QLatin1String("https")
            && (host == QLatin1String("jiosaavn.com") || host.endsWith(QLatin1String(".jiosaavn.com")))) {
            Q_EMIT reply->redirectAllowed();
            return;
        }
        reply->setProperty("monolistRedirectedAway", true);
        reply->abort();
    });

    // Qt has one timeout, for silence. The connection gets its own, shorter
    // one: a host that cannot even be reached is not worth the whole wait.
    // Any sign of a connection counts — the handshake done, the request
    // written, or an answer begun.
    auto connected = std::make_shared<bool>(false);
    const auto markConnected = [connected]() { *connected = true; };
    connect(reply, &QNetworkReply::encrypted, reply, markConnected);
    connect(reply, &QNetworkReply::requestSent, reply, markConnected);
    connect(reply, &QNetworkReply::metaDataChanged, reply, markConnected);
    QTimer::singleShot(kConnectTimeoutMs, reply, [reply, connected]() {
        if (!*connected && reply->isRunning()) {
            reply->setProperty("monolistTimedOut", QStringLiteral("connecting"));
            reply->abort();
        }
    });
    QTimer::singleShot(kRequestTimeoutMs, reply, [reply]() {
        if (reply->isRunning()) {
            reply->setProperty("monolistTimedOut", QStringLiteral("answering"));
            reply->abort();
        }
    });

    connect(reply, &QNetworkReply::finished, this, [reply, done = std::move(done)]() {
        reply->deleteLater();
        const QString timedOut = reply->property("monolistTimedOut").toString();
        if (!timedOut.isEmpty()) {
            done({}, QStringLiteral("JioSaavn timed out %1").arg(timedOut));
            return;
        }
        if (reply->property("monolistRedirectedAway").toBool()) {
            done({}, QStringLiteral("JioSaavn redirected away from its own hosts; not followed"));
            return;
        }
        if (reply->error() != QNetworkReply::NoError) {
            done({}, QStringLiteral("JioSaavn: %1").arg(reply->errorString()));
            return;
        }
        QByteArray body = reply->readAll();
        // Now and then a stray line comes before the JSON.
        const qsizetype brace = body.indexOf('{');
        if (brace > 0)
            body.remove(0, brace);
        QJsonParseError error{};
        const QJsonDocument document = QJsonDocument::fromJson(body, &error);
        if (error.error != QJsonParseError::NoError || !document.isObject()) {
            done({}, QStringLiteral("JioSaavn answered with something that is not JSON"));
            return;
        }
        done(document.object(), QString());
    });
}
