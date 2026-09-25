#include "matchkey.h"

#include <QChar>
#include <QList>
#include <QSet>
#include <QStringList>

namespace {

// Everything here works on CODE POINTS, never on QString's UTF-16 units.
//
// The Python indexes and slices Python strings, where an astral character
// counts as one position. Titles do carry them - musical symbols, emoji in
// YouTube-sourced names - and a single one would shift every offset after it by
// one if we sliced QStrings directly, producing keys that look plausible and
// match nothing.
using Text = QList<char32_t>;

// ------------------------------------------------------------------ character
// classes
//
// These mirror CPython's, not Qt's defaults, because the regexes and the
// isalnum() test in build_embeddings.py are what decided the shipped keys.

// Python's \w for str patterns: str.isalnum() plus underscore.
bool isWordChar(char32_t cp)
{
    return cp == U'_' || QChar::isLetterOrNumber(cp);
}

// Python's \s for str patterns (Py_UNICODE_ISSPACE). Deliberately spelled out:
// it is not QChar::isSpace(), which differs at the edges.
bool isSpaceChar(char32_t cp)
{
    switch (cp) {
    case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D:
    case 0x1C: case 0x1D: case 0x1E: case 0x1F: case 0x20:
    case 0x85: case 0xA0: case 0x1680:
    case 0x2028: case 0x2029: case 0x202F: case 0x205F: case 0x3000:
        return true;
    default:
        return cp >= 0x2000 && cp <= 0x200A;
    }
}

bool isCombiningMark(char32_t cp)
{
    switch (QChar::category(cp)) {
    case QChar::Mark_NonSpacing:
    case QChar::Mark_SpacingCombining:
    case QChar::Mark_Enclosing:
        return true;
    default:
        return false;
    }
}

// The accent ranges folding is allowed to drop. Scoped to Latin on purpose:
// Devanagari vowel signs and Arabic harakat live in Mn too, and removing those
// destroys the word instead of folding an accent off it.
bool isLatinCombining(char32_t cp)
{
    return (cp >= 0x0300 && cp <= 0x036F) || (cp >= 0x1AB0 && cp <= 0x1AFF)
        || (cp >= 0x1DC0 && cp <= 0x1DFF) || (cp >= 0x20D0 && cp <= 0x20FF)
        || (cp >= 0xFE20 && cp <= 0xFE2F);
}

// Unicode's Cased and Case_Ignorable, needed only by the Final_Sigma rule
// below. Approximated from the general categories Qt exposes: this misses
// Other_Lowercase/Other_Uppercase (modifier letters, circled letters), which
// cannot appear next to a Greek sigma in any real tag.
bool isCased(char32_t cp)
{
    switch (QChar::category(cp)) {
    case QChar::Letter_Lowercase:
    case QChar::Letter_Uppercase:
    case QChar::Letter_Titlecase:
        return true;
    default:
        return false;
    }
}

bool isCaseIgnorable(char32_t cp)
{
    switch (QChar::category(cp)) {
    case QChar::Mark_NonSpacing:
    case QChar::Mark_Enclosing:
    case QChar::Other_Format:
    case QChar::Letter_Modifier:
    case QChar::Symbol_Modifier:
        return true;
    default:
        break;
    }
    // Word_Break MidLetter / MidNumLet / Single_Quote.
    switch (cp) {
    case 0x0027: case 0x002E: case 0x003A: case 0x00B7: case 0x0387:
    case 0x055F: case 0x05F4: case 0x2018: case 0x2019: case 0x2024:
    case 0x2027: case 0xFE13: case 0xFE52: case 0xFF07: case 0xFF0E:
    case 0xFF1A:
        return true;
    default:
        return false;
    }
}

// ----------------------------------------------------------------- text utils

Text toCodePoints(const QString &s)
{
    Text out;
    out.reserve(s.size());
    for (qsizetype i = 0; i < s.size(); ++i) {
        const QChar c = s.at(i);
        if (c.isHighSurrogate() && i + 1 < s.size() && s.at(i + 1).isLowSurrogate()) {
            out.append(QChar::surrogateToUcs4(c, s.at(i + 1)));
            ++i;
        } else {
            out.append(char32_t(c.unicode()));
        }
    }
    return out;
}

QString toQString(const Text &t)
{
    return QString::fromUcs4(t.constData(), t.size());
}

// Plain substring search. The terms below are all ASCII, so comparing code
// points against bytes is exact.
qsizetype lengthOf(const char *term)
{
    qsizetype n = 0;
    while (term[n] != '\0')
        ++n;
    return n;
}

bool matchesAt(const Text &hay, qsizetype pos, const char *term, qsizetype n)
{
    if (pos + n > hay.size())
        return false;
    for (qsizetype j = 0; j < n; ++j)
        if (hay.at(pos + j) != char32_t(uchar(term[j])))
            return false;
    return true;
}

qsizetype indexOf(const Text &hay, const char *needle)
{
    const qsizetype n = lengthOf(needle);
    for (qsizetype i = 0; i + n <= hay.size(); ++i)
        if (matchesAt(hay, i, needle, n))
            return i;
    return -1;
}

bool contains(const Text &hay, const char *needle)
{
    return indexOf(hay, needle) >= 0;
}

bool containsAny(const Text &hay, const char *const *terms, int count)
{
    for (int i = 0; i < count; ++i)
        if (contains(hay, terms[i]))
            return true;
    return false;
}

// `\bterm\b` anchored at `pos`. Every term here is made of word characters, so
// a word boundary is simply "the neighbour is not a word character".
bool wordAt(const Text &hay, qsizetype pos, const char *term)
{
    const qsizetype n = lengthOf(term);
    if (!matchesAt(hay, pos, term, n))
        return false;
    if (pos > 0 && isWordChar(hay.at(pos - 1)))
        return false;
    return pos + n == hay.size() || !isWordChar(hay.at(pos + n));
}

Text slice(const Text &t, qsizetype from, qsizetype to)
{
    Text out;
    out.reserve(to - from);
    for (qsizetype i = from; i < to; ++i)
        out.append(t.at(i));
    return out;
}

// ------------------------------------------------------------------- the rules

// Credits: safe to strip as bare words anywhere. No released title uses "feat"
// as ordinary language.
const char *const kCreditTerms[] = { "feat", "ft", "featuring" };
constexpr int kCreditCount = 3;

// Release/edition noise: only stripped after a " - " or inside a parenthetical,
// never bare, because every one of them is also a real title - "Clean",
// "Mono", "Deluxe", and "with", which bare would turn "Die With A Smile" into
// "die".
const char *const kReleaseTerms[] = {
    "remaster", "remastered", "deluxe", "bonus track", "album version",
    "single version", "radio edit", "mono", "stereo", "explicit", "clean",
    "expanded", "anniversary", "with", "version", "edit",
};
constexpr int kReleaseCount = 16;

// Words marking a genuinely different recording. Never stripped: a remix and
// its original are different points in the embedding space, and collapsing them
// makes the catalogue claim a match it does not have.
const char *const kKeepTerms[] = {
    "remix", "acoustic", "live", "unplugged", "instrumental",
    "demo", "reprise", "sped up", "slowed",
};
constexpr int kKeepCount = 9;

// CPython's str.lower() applies Unicode's Final_Sigma rule, so an all-capitals
// Greek title folds to a FINAL sigma (U+03C2) where a naive per-character
// lowercase gives U+03C3. Different bytes, different hash, and Greek tags come
// in capitals often enough to matter.
bool finalSigmaHere(const Text &s, qsizetype i)
{
    qsizetype j = i - 1;
    for (; j >= 0; --j)
        if (!isCaseIgnorable(s.at(j)))
            break;
    if (j < 0 || !isCased(s.at(j)))
        return false;
    for (j = i + 1; j < s.size(); ++j)
        if (!isCaseIgnorable(s.at(j)))
            break;
    return j == s.size() || !isCased(s.at(j));
}

// Reads the source rather than lowering in place, because the Final_Sigma
// context is defined over the original characters.
Text lowercased(const Text &src)
{
    Text out;
    out.reserve(src.size());
    for (qsizetype i = 0; i < src.size(); ++i) {
        const char32_t cp = src.at(i);
        if (cp == 0x03A3)
            out.append(finalSigmaHere(src, i) ? char32_t(0x03C2) : char32_t(0x03C3));
        else
            out.append(QChar::toLower(cp));
    }
    return out;
}

// NFKD, drop the Latin accents, lowercase. NFKD and not a diacritic-insensitive
// fold: the two disagree on characters like the fi ligature and 1/2, and NFKD
// is what built the catalogue.
Text fold(const QString &s)
{
    const Text decomposed = toCodePoints(s.normalized(QString::NormalizationForm_KD));
    Text kept;
    kept.reserve(decomposed.size());
    for (char32_t cp : decomposed)
        if (!isLatinCombining(cp))
            kept.append(cp);
    return lowercased(kept);
}

// Runs of characters that are neither alphanumeric nor combining marks become
// one space.
//
// Combining marks survive alongside alphanumerics because the Latin accents
// are already gone by now and what is left is load-bearing: a Devanagari matra
// is a letter's vowel, not decoration, and dropping it would collapse distinct
// titles onto each other.
Text collapse(const Text &s)
{
    Text out;
    bool pending = false;
    for (char32_t cp : s) {
        if (QChar::isLetterOrNumber(cp) || isCombiningMark(cp)) {
            if (pending && !out.isEmpty())
                out.append(U' ');
            pending = false;
            out.append(cp);
        } else {
            pending = true;
        }
    }
    return out;
}

// Python's str.strip(). collapse() cannot leave an outer space, so this only
// ever matters if the rules above change.
Text stripped(const Text &s)
{
    qsizetype from = 0;
    qsizetype to = s.size();
    while (from < to && isSpaceChar(s.at(from)))
        ++from;
    while (to > from && isSpaceChar(s.at(to - 1)))
        --to;
    return slice(s, from, to);
}

// Python's `_PAREN.sub`, which is a NON-nesting regex: "[\(\[][^\)\]]*[\)\]]".
// An opener matches up to the first closer of either kind, so "(a [b)" is one
// match and nested brackets do not pair up. The Swift twin counts depth
// instead; this follows the Python.
Text stripParentheticals(const Text &t)
{
    Text out;
    qsizetype i = 0;
    while (i < t.size()) {
        if (t.at(i) == U'(' || t.at(i) == U'[') {
            qsizetype j = i + 1;
            while (j < t.size() && t.at(j) != U')' && t.at(j) != U']')
                ++j;
            if (j < t.size()) {
                const Text inner = slice(t, i, j + 1);
                out.append(U' ');
                if (containsAny(inner, kKeepTerms, kKeepCount)) {
                    out.append(inner);
                    out.append(U' ');
                }
                i = j + 1;
                continue;
            }
            // No closer left anywhere, so no later opener can match either -
            // but the regex would still copy the rest verbatim, and so does the
            // loop, one character at a time.
        }
        out.append(t.at(i));
        ++i;
    }
    return out;
}

// Truncate at the FIRST " - " when what follows is release noise. "As It Was -
// Radio Edit" loses the suffix; "Sunday Bloody Sunday - Live" keeps it, because
// "live" names a different recording.
Text stripDashSuffix(const Text &t)
{
    const qsizetype i = indexOf(t, " - ");
    if (i < 0)
        return t;
    const Text tail = slice(t, i + 3, t.size());
    if (containsAny(tail, kKeepTerms, kKeepCount))
        return t;
    if (containsAny(tail, kReleaseTerms, kReleaseCount)
        || containsAny(tail, kCreditTerms, kCreditCount))
        return slice(t, 0, i);
    return t;
}

// Earliest whole-word credit term; everything from it onward goes.
//
// The word boundary is the whole point: "BIRDS OF A FEATHER" keeps its feather
// and "ft2 anthem" keeps its ft2, both of which a substring cut would eat.
qsizetype creditCut(const Text &t)
{
    for (qsizetype p = 0; p < t.size(); ++p)
        for (int k = 0; k < kCreditCount; ++k)
            if (wordAt(t, p, kCreditTerms[k]))
                return p;
    return t.size();
}

// Python's `re.sub(r"\s+-\s+topic$", "", a)` - YouTube's auto-generated artist
// channels. Note that Python's $ also matches before a single trailing newline.
Text stripTopicSuffix(const Text &a)
{
    const bool trailingNewline = !a.isEmpty() && a.last() == U'\n';
    for (qsizetype end : { a.size(), trailingNewline ? a.size() - 1 : qsizetype(-1) }) {
        if (end < 5 || !matchesAt(a, end - 5, "topic", 5))
            continue;
        qsizetype p = end - 5;
        const qsizetype wsEnd = p;
        while (p > 0 && isSpaceChar(a.at(p - 1)))
            --p;
        if (p == wsEnd || p == 0 || a.at(p - 1) != U'-')
            continue;
        qsizetype q = p - 1;
        const qsizetype dash = q;
        while (q > 0 && isSpaceChar(a.at(q - 1)))
            --q;
        if (q == dash)
            continue;
        Text out = slice(a, 0, q);
        out.append(slice(a, end, a.size()));
        return out;
    }
    return a;
}

// Keep the part before the first match of
// `\s*(?:,|&|;|/|\bx\b|\band\b|\bfeat\.?\b|\bft\.?\b|\bwith\b)\s*`.
//
// Word boundaries, not substrings - this is where the Swift twin went wrong.
// `\bfeat\.?\b` is equivalent to `\bfeat\b` for deciding WHETHER it matches:
// when the dot is there the trailing `\b` fails on it and the engine backtracks
// to the dotless branch, which then succeeds; when the next character is a
// letter both branches fail. Only the match's length differs, and only its
// start is wanted here.
bool separatorAt(const Text &a, qsizetype q)
{
    switch (a.at(q)) {
    case U',': case U'&': case U';': case U'/':
        return true;
    default:
        break;
    }
    return wordAt(a, q, "x") || wordAt(a, q, "and") || wordAt(a, q, "feat")
        || wordAt(a, q, "ft") || wordAt(a, q, "with");
}

Text cutAtSeparator(const Text &a)
{
    for (qsizetype q = 0; q < a.size(); ++q) {
        if (!separatorAt(a, q))
            continue;
        // The match starts at the leading `\s*`, which is greedy, so it begins
        // at the head of the whitespace run in front of the separator. No
        // earlier separator can be hiding inside that run: every alternative
        // starts with a non-space character.
        qsizetype p = q;
        while (p > 0 && isSpaceChar(a.at(p - 1)))
            --p;
        return slice(a, 0, p);
    }
    return a;
}

const QSet<QString> &weakArtistTokens()
{
    // Tokens that appear across so many stage names they identify nobody. A
    // name is mostly its distinctive half, and these are the other half:
    // without them "DJ Snake" matched "DJ Khaled" and "Lil Baby" matched "Lil
    // Nas X".
    static const QSet<QString> tokens = {
        QStringLiteral("dj"), QStringLiteral("lil"), QStringLiteral("young"),
        QStringLiteral("mc"), QStringLiteral("the"), QStringLiteral("sir"),
        QStringLiteral("dr"), QStringLiteral("big"), QStringLiteral("king"),
        QStringLiteral("queen"), QStringLiteral("official"), QStringLiteral("band"),
        QStringLiteral("prod"), QStringLiteral("feat"), QStringLiteral("ft"),
        QStringLiteral("mr"), QStringLiteral("miss"), QStringLiteral("baby"),
        QStringLiteral("yo"),
    };
    return tokens;
}

QSet<QString> tokensOf(const QString &name)
{
    QSet<QString> out;
    for (const QString &token : name.split(QLatin1Char(' '), Qt::SkipEmptyParts))
        out.insert(token);
    return out;
}

bool isSubsetOf(const QSet<QString> &smaller, const QSet<QString> &larger)
{
    for (const QString &token : smaller)
        if (!larger.contains(token))
            return false;
    return true;
}

} // namespace

namespace Rec {

QString titleCore(const QString &title)
{
    Text t = fold(title);
    t = stripParentheticals(t);
    t = stripDashSuffix(t);
    t = slice(t, 0, creditCut(t));
    return toQString(stripped(collapse(t)));
}

QString primaryArtist(const QString &artist)
{
    // A tag can carry several performers separated by NUL — ID3v2.4 and Vorbis
    // both do — and the Python never saw one, because it only ever read
    // YouTube strings. Left alone the NUL collapses to a space, so
    // "Bruno Mars\0Lady Gaga" becomes "bruno mars lady gaga", which matches no
    // catalogue row: the strict tier misses and the track drops silently to the
    // weaker title-only one. Spec 5.2 asks for the first performer.
    Text a = fold(artist.section(QChar(u'\0'), 0, 0));
    a = stripTopicSuffix(a);
    a = cutAtSeparator(a);
    return toQString(stripped(collapse(a)));
}

bool artistsAgree(const QString &a, const QString &b)
{
    const QSet<QString> x = tokensOf(primaryArtist(a));
    const QSet<QString> y = tokensOf(primaryArtist(b));
    // Two empty names do not agree. 154 catalogue rows have an empty primary
    // artist, and letting them agree would join all of them to each other.
    if (x.isEmpty() || y.isEmpty())
        return false;
    if (x == y)
        return true;

    QSet<QString> dx = x;
    QSet<QString> dy = y;
    dx.subtract(weakArtistTokens());
    dy.subtract(weakArtistTokens());
    // Nothing distinctive left on one side: the name is all filler, and the
    // equality test above already had its chance at it.
    if (dx.isEmpty() || dy.isEmpty())
        return false;

    return dx.size() <= dy.size() ? isSubsetOf(dx, dy) : isSubsetOf(dy, dx);
}

quint64 fnv1a64(const QByteArray &bytes)
{
    quint64 h = Q_UINT64_C(0xcbf29ce484222325);
    for (char c : bytes) {
        h ^= quint64(uchar(c));
        h *= Q_UINT64_C(0x100000001b3);
    }
    return h;
}

quint64 strictKey(const QString &title, const QString &artist)
{
    return fnv1a64((titleCore(title) + QLatin1Char('|') + primaryArtist(artist)).toUtf8());
}

quint64 titleKey(const QString &title)
{
    return fnv1a64(titleCore(title).toUtf8());
}

QString plainName(const QString &name)
{
    const QList<uint> points = name.normalized(QString::NormalizationForm_KD).toUcs4();
    QList<char32_t> out;
    out.reserve(points.size());
    // The script of the letter a combining mark would sit on: an accent on a
    // Latin, Greek or Cyrillic letter is a spelling variant and goes; a vowel
    // sign in Devanagari is part of the letter and stays, or different names
    // collapse into one.
    QChar::Script base = QChar::Script_Unknown;
    for (const uint point : points) {
        const char32_t c = char32_t(point);
        const QChar::Category category = QChar::category(c);
        if (category == QChar::Mark_NonSpacing || category == QChar::Mark_SpacingCombining
            || category == QChar::Mark_Enclosing) {
            if (base == QChar::Script_Latin || base == QChar::Script_Greek
                || base == QChar::Script_Cyrillic)
                continue;
            out.append(c);
            continue;
        }
        if (QChar::isLetterOrNumber(c)) {
            base = QChar::script(c);
            out.append(QChar::toLower(c));
        } else {
            base = QChar::Script_Unknown;
            out.append(U' ');
        }
    }
    return QString::fromUcs4(out.constData(), out.size()).simplified();
}

} // namespace Rec
