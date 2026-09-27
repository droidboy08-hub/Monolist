#include "lyricsquery.h"
#include "rec/matchkey.h"

#include <QRegularExpression>
#include <QSet>

#include <algorithm>

namespace {

// Words that say how a video was published rather than what is sung. A
// bracket holding one is noise, apart from any version marker beside it.
const QRegularExpression &noiseWords()
{
    static const QRegularExpression re(QStringLiteral(
        R"(\b(?:official|video|audio|lyrics?|visuali[sz]er|mv|hd|hq|4k|remaster(?:ed)?)\b)"),
        QRegularExpression::CaseInsensitiveOption);
    return re;
}

// Words that name another recording of the song: other timing, and sometimes
// other words. "Version" only counts after one of them ("Acoustic Version"),
// not on its own ("Single Version" is the same take).
const QRegularExpression &markerWords()
{
    static const QRegularExpression re(QStringLiteral(
        R"(\b(?:sped[\s-]+up|speed[\s-]+up|slowed(?:\s*(?:\+|&|and)\s*reverb(?:ed)?|\s+down)?|reverb(?:ed)?)"
        R"(|nightcore|live|re-?mix(?:ed)?|mix|acoustic|unplugged|instrumental|karaoke|a\s*cappella|acapella)"
        R"(|demo|extended|radio\s+edit|edit)(?:\s+version)?\b)"),
        QRegularExpression::CaseInsensitiveOption);
    return re;
}

// "feat." and its kin: always a credit, wherever it stands.
const QRegularExpression &featuring()
{
    static const QRegularExpression re(QStringLiteral(R"((?:^|\s)(?:feat\.?|ft\.?|featuring)(?=\s))"),
                                       QRegularExpression::CaseInsensitiveOption);
    return re;
}

// A bracket, not nested, as the old rule and most titles have them: an opener
// matches up to the first closer of either kind.
const QRegularExpression &bracket()
{
    static const QRegularExpression re(QStringLiteral(R"(\s*([\(\[])([^\)\]]*)[\)\]])"));
    return re;
}

// Letters and digits only, case and Latin accents folded (Rec::plainName), so
// "Tití" is "Titi" and "Don't" is "Dont".
QString key(const QString &text)
{
    QString k = Rec::plainName(text);
    k.remove(QLatin1Char(' '));
    return k;
}

QStringList tokens(const QString &text)
{
    return Rec::plainName(text).split(QLatin1Char(' '), Qt::SkipEmptyParts);
}

// Whether `inner` appears in `outer` as a run of whole words.
bool containsRun(const QStringList &outer, const QStringList &inner)
{
    if (inner.isEmpty() || inner.size() > outer.size())
        return false;
    for (qsizetype i = 0; i + inner.size() <= outer.size(); ++i) {
        if (std::equal(inner.cbegin(), inner.cend(), outer.cbegin() + i))
            return true;
    }
    return false;
}

// One name is the other, or one holds the other as whole words ("The
// Notorious B.I.G." and "Notorious B.I.G."); a held name must have a few
// letters, so an "X" or a "M" matches nobody by accident.
bool sameName(const QString &a, const QString &b)
{
    const QString ka = key(a);
    const QString kb = key(b);
    if (ka.isEmpty() || kb.isEmpty())
        return false;
    if (ka == kb)
        return true;
    const QStringList ta = tokens(a);
    const QStringList tb = tokens(b);
    return (kb.size() >= 4 && containsRun(ta, tb)) || (ka.size() >= 4 && containsRun(tb, ta));
}

bool namesAnArtist(const QString &text, const QStringList &artists)
{
    static const QRegularExpression joiner(QStringLiteral(R"(\s*(?:,|&|\+|\band\b|\bx\b)\s*)"),
                                           QRegularExpression::CaseInsensitiveOption);
    for (const QString &name : text.split(joiner, Qt::SkipEmptyParts)) {
        for (const QString &artist : artists) {
            if (sameName(artist, name))
                return true;
        }
    }
    return false;
}

// The version markers in a stretch of noise, as they were written: "Official
// Live Video" gives "Live", "Sped Up Audio" gives "Sped Up".
QString markersIn(const QString &text)
{
    QStringList found;
    for (auto it = markerWords().globalMatch(text); it.hasNext();)
        found << it.next().captured().simplified();
    return found.join(QLatin1Char(' '));
}

// A bracket's replacement: nothing, a shorter bracket, or itself.
QString cleanBracket(const QChar opener, QString content, const QStringList &artists)
{
    const QChar closer = opener == QLatin1Char('[') ? QLatin1Char(']') : QLatin1Char(')');
    // "(Remix feat. Guest)" -> "(Remix)"; "(feat. Guest)" -> nothing.
    const QRegularExpressionMatch credit = featuring().match(content);
    if (credit.hasMatch())
        content.truncate(credit.capturedStart());
    content = content.simplified();
    if (content.isEmpty())
        return {};
    // "(with Guest)" is a credit only when Guest is one of the artists.
    static const QRegularExpression with(QStringLiteral(R"(^with\s+(.+)$)"), QRegularExpression::CaseInsensitiveOption);
    if (const QRegularExpressionMatch m = with.match(content); m.hasMatch() && namesAnArtist(m.captured(1), artists))
        return {};
    if (noiseWords().match(content).hasMatch()) {
        const QString markers = markersIn(content);
        return markers.isEmpty() ? QString() : QStringLiteral(" ") + opener + markers + closer;
    }
    return QStringLiteral(" ") + opener + content + closer;
}

QStringList bracketContents(const QString &title)
{
    QStringList contents;
    for (auto it = bracket().globalMatch(title); it.hasNext();)
        contents << it.next().captured(2).simplified();
    return contents;
}

// The spellings a title goes by: as cleaned, without its brackets, and each
// part of it that could be the title on its own ("봄날 (Spring Day)" is filed
// under either). A bracket holding a version marker is never a title.
QStringList titleForms(const QString &cleaned)
{
    QStringList forms{ cleaned, LyricsQuery::bareTitle(cleaned) };
    for (const QString &content : bracketContents(cleaned)) {
        if (!markerWords().match(content).hasMatch())
            forms << content;
    }
    const QString bare = LyricsQuery::bareTitle(cleaned);
    if (bare.contains(QStringLiteral(" - ")))
        forms << bare.split(QStringLiteral(" - "), Qt::SkipEmptyParts);
    QStringList unique;
    QSet<QString> seen;
    for (const QString &form : std::as_const(forms)) {
        const QString k = key(form);
        if (!k.isEmpty() && !seen.contains(k)) {
            seen.insert(k);
            unique << form.trimmed();
        }
    }
    return unique;
}

// 1 when the two are one spelling; otherwise the share of words in common
// (Dice), so "Anti-Hero" against "Hero" is 0.67.
double similarity(const QString &a, const QString &b)
{
    if (key(a) == key(b))
        return 1.0;
    const QStringList ta = tokens(a);
    const QStringList tb = tokens(b);
    if (ta.isEmpty() || tb.isEmpty())
        return 0.0;
    const QSet<QString> sa(ta.cbegin(), ta.cend());
    const QSet<QString> sb(tb.cbegin(), tb.cend());
    const QSet<QString> both = QSet<QString>(sa).intersect(sb);
    return 2.0 * double(both.size()) / double(sa.size() + sb.size());
}

} // namespace

namespace LyricsQuery {

QStringList artistNames(const QString &line, const QString &primaryArtist, const QVariantList &pieces)
{
    static const QRegularExpression topic(QStringLiteral(R"(\s+-\s+Topic$)"), QRegularExpression::CaseInsensitiveOption);
    QStringList names;
    const auto add = [&names](QString name) {
        name.remove(topic);
        name = name.simplified();
        if (!name.isEmpty() && !names.contains(name, Qt::CaseInsensitive))
            names << name;
    };

    for (const QVariant &value : pieces) {
        const QVariantMap piece = value.toMap();
        if (piece.value(QStringLiteral("link")).toBool())
            add(piece.value(QStringLiteral("text")).toString());
    }
    // The first credit alone, as its own link named it, leads.
    if (!primaryArtist.trimmed().isEmpty()) {
        const QString primary = primaryArtist.simplified();
        names.removeIf([&primary](const QString &name) { return name.compare(primary, Qt::CaseInsensitive) == 0; });
        names.prepend(primary);
    }
    if (names.isEmpty()) {
        // Nothing but the line: cut only where the words say so.
        static const QRegularExpression feat(QStringLiteral(R"(\s+(?:feat\.?|ft\.?|featuring)\s+)"),
                                             QRegularExpression::CaseInsensitiveOption);
        for (const QString &part : line.split(feat, Qt::SkipEmptyParts))
            add(part);
    }
    return names;
}

QString cleanTitle(const QString &title, const QStringList &artists)
{
    QString text = title.simplified();

    // "Artist - Song", as video titles often go. The name as whole words: an
    // artist called "M" does not own "Mama - Remix".
    const int dash = int(text.indexOf(QStringLiteral(" - ")));
    if (dash > 0) {
        const QString front = text.left(dash);
        for (const QString &artist : artists) {
            if (sameName(front, artist)) {
                text = text.mid(dash + 3);
                break;
            }
        }
    }

    // Every bracket judged on its own.
    QString out;
    qsizetype at = 0;
    for (auto it = bracket().globalMatch(text); it.hasNext();) {
        const QRegularExpressionMatch m = it.next();
        out += text.mid(at, m.capturedStart() - at);
        out += cleanBracket(m.captured(1).at(0), m.captured(2), artists);
        at = m.capturedEnd();
    }
    out += text.mid(at);

    // An unbracketed "feat." runs to the next bracket, not to the end.
    static const QRegularExpression featTail(QStringLiteral(R"(\s+(?:feat\.?|ft\.?|featuring)\s+[^\(\[]*)"),
                                             QRegularExpression::CaseInsensitiveOption);
    out.replace(featTail, QStringLiteral(" "));

    // "Song - Remastered 2011" as a bracket would be; "Song - Live" and
    // "上を向いて歩こう - Sukiyaki" stay.
    const int suffix = int(out.lastIndexOf(QStringLiteral(" - ")));
    if (suffix > 0) {
        const QString tail = out.mid(suffix + 3);
        if (noiseWords().match(tail).hasMatch()) {
            const QString markers = markersIn(tail);
            out = out.left(suffix) + (markers.isEmpty() ? QString() : QStringLiteral(" (") + markers + QLatin1Char(')'));
        }
    }

    out = out.simplified();
    static const QRegularExpression danglingDash(QStringLiteral(R"(\s*-\s*$)"));
    out.remove(danglingDash);
    return out.isEmpty() ? title.simplified() : out;
}

QString bareTitle(const QString &cleanedTitle)
{
    QString bare = cleanedTitle;
    bare.remove(bracket());
    const int suffix = int(bare.lastIndexOf(QStringLiteral(" - ")));
    if (suffix > 0 && markerWords().match(bare.mid(suffix + 3)).hasMatch())
        bare.truncate(suffix);
    bare = bare.simplified();
    return bare.isEmpty() ? cleanedTitle : bare;
}

Query fromTrack(const QVariantMap &track, const QVariantList &pieces)
{
    Query query;
    const QVariantList given = pieces.isEmpty() ? track.value(QStringLiteral("credits")).toList() : pieces;
    query.artists = artistNames(track.value(QStringLiteral("artist")).toString(),
                                track.value(QStringLiteral("primaryArtist")).toString(), given);
    query.title = cleanTitle(track.value(QStringLiteral("title")).toString(), query.artists);
    query.bareTitle = bareTitle(query.title);
    query.album = track.value(QStringLiteral("album")).toString().simplified();
    query.durationS = track.value(QStringLiteral("durationMs")).toLongLong() / 1000.0;
    return query;
}

Match match(const Query &query, const QString &title, const QString &artist, double durationS)
{
    Match result;
    // The entry's title through the same cleaning, so its "(feat. ...)" or
    // " - Remastered 2015" does not count against it.
    const QStringList ours = titleForms(query.title);
    const QStringList theirs = titleForms(cleanTitle(title, query.artists));
    for (const QString &a : ours) {
        for (const QString &b : theirs)
            result.title = qMax(result.title, similarity(a, b));
    }

    static const QRegularExpression joiner(QStringLiteral(
        R"(\s*(?:,|&|;|/|\+|\(|\)|×|、|\bfeat\.?|\bft\.?|\bfeaturing\b|\bwith\b|\band\b|\bx\b)\s*)"),
        QRegularExpression::CaseInsensitiveOption);
    QStringList pieces = artist.split(joiner, Qt::SkipEmptyParts);
    pieces << artist;
    for (const QString &name : query.artists) {
        for (const QString &piece : std::as_const(pieces)) {
            if (sameName(name, piece)) {
                result.artist = true;
                break;
            }
        }
        if (result.artist)
            break;
    }

    if (query.durationS > 0 && durationS > 0)
        result.gapS = qAbs(query.durationS - durationS);

    const bool artistAgrees = query.artists.isEmpty() || result.artist || (result.gapS >= 0 && result.gapS <= 3.0);
    result.accepted = result.title >= 0.8 && artistAgrees;
    return result;
}

} // namespace LyricsQuery
