#include "importmatcher.h"

#include "jiosaavn.h"
#include "rec/matchkey.h"

#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <cstdlib>

namespace {

// 2: an album's cover wording and language, whose remix or which concert,
// and "(Movie: …)" read as "(From …)"; the artist no longer found in the
// album's name.
constexpr int kVersion = 2;
// The first five results of a search: past them YouTube Music's ranking has
// already said what it thinks, and a match that low is more often a cover.
constexpr int kResultsLooked = 5;
// How close two lengths must be, in seconds (matcher.py).
constexpr int kSureSecs = 5;
constexpr int kLikelySecs = 15;
constexpr int kOtherArtistSecs = 2;
constexpr int kLibrarySecs = 3;

// What a cover, a karaoke track or a tribute says of itself, folded. Only
// counted when the source's own title does not say it too ("Cover Me").
const QStringList &coverMarks()
{
    static const QStringList marks = {
        QStringLiteral("originally by"), QStringLiteral("originally performed"), QStringLiteral("in the style of"),
        QStringLiteral("made famous"), QStringLiteral("tribute"), QStringLiteral("karaoke"),
        QStringLiteral("cover"), QStringLiteral("backing track"),
    };
    return marks;
}

bool containsPhrase(const QString &text, const QString &phrase)
{
    if (phrase.isEmpty())
        return false;
    return (QLatin1Char(' ') + text + QLatin1Char(' ')).contains(QLatin1Char(' ') + phrase + QLatin1Char(' '));
}

QStringList foldedWords(const QString &text)
{
    return ImportMatch::fold(text).split(QLatin1Char(' '), Qt::SkipEmptyParts);
}

// YouTube Music names a film song's film "(Movie: Saiyaara)" where JioSaavn
// and Spotify say '(From "Saiyaara")': the same aside, which says nothing
// about the recording (but a language bracketed inside it, as "From" does).
QString prepared(const QString &title)
{
    static const QRegularExpression movie(
        QStringLiteral("([\\(\\[\\x{FF08}]\\s*|\\s[-\\x{2013}\\x{2014}]\\s*)(?:movie|film)\\s*:\\s*"),
        QRegularExpression::CaseInsensitiveOption);
    QString out = title;
    out.replace(movie, QStringLiteral("\\1From "));
    return out;
}

// One side of a comparison, worked out once.
struct Side {
    Saavn::Title title;
    // title.versions without "edit" or a language: "Edit" alone is release
    // noise here, as in matcher.py (Spotify's "Song - Edit" is YouTube
    // Music's "Song" as often as not, and the length tells the edit from the
    // album take better than the word does).
    QStringList versions;
    QStringList languages;         // a language the title names: a dub
    QStringList albumLanguages;    // a language the album's name gives
    // The words beside a version marker that could name someone or
    // somewhere: whose remix, which concert ("Live in Buenos Aires").
    QStringList context;
    QSet<QString> vocabulary;      // every word of the title, the artists and the album
};

void addOnce(QStringList &list, const QString &word)
{
    if (!list.contains(word))
        list.append(word);
}

Side sideOf(const QString &title, const QStringList &artists, const QString &album,
            const QStringList &artistNames)
{
    Side side;
    side.title = Saavn::parseTitle(prepared(title), artistNames);
    for (const QString &version : std::as_const(side.title.versions)) {
        if (Saavn::isLanguage(version))
            addOnce(side.languages, version);
        else if (version != QLatin1String("edit"))
            addOnce(side.versions, version);
    }
    if (!album.trimmed().isEmpty()) {
        for (const QString &version : Saavn::parseTitle(prepared(album)).versions) {
            if (Saavn::isLanguage(version))
                addOnce(side.albumLanguages, version);
        }
    }
    for (const QString &word : std::as_const(side.title.versionContext)) {
        if (!Saavn::namesNobody(word))
            addOnce(side.context, word);
    }
    side.versions.sort();
    side.languages.sort();
    side.albumLanguages.sort();
    side.context.sort();
    for (const QString &text : QStringList{ title, artists.join(QLatin1Char(' ')), album }) {
        for (const QString &word : foldedWords(text))
            side.vocabulary.insert(word);
    }
    return side;
}

// Every language a side names, its title's and its album's.
QStringList allLanguages(const Side &side)
{
    QStringList all = side.languages;
    for (const QString &language : side.albumLanguages)
        addOnce(all, language);
    all.sort();
    return all;
}

QString languageNames(const QStringList &languages)
{
    QStringList names;
    for (const QString &language : languages)
        names.append(language.left(1).toUpper() + language.mid(1));
    return names.join(QStringLiteral(" and "));
}

// matcher.py's title_sim, with "one inside the other" taken word by word
// ("love" is not inside "lovely day").
double titleSimilarity(const QString &a, const QString &b)
{
    if (a.isEmpty() || b.isEmpty())
        return 0.0;
    if (a == b)
        return 1.0;
    if (std::min(a.size(), b.size()) >= 3 && (containsPhrase(a, b) || containsPhrase(b, a)))
        return 0.85;
    const QStringList wa = a.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    const QStringList wb = b.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    const QSet<QString> sa(wa.cbegin(), wa.cend());
    const QSet<QString> sb(wb.cbegin(), wb.cend());
    const qsizetype common = QSet<QString>(sa).intersect(sb).size();
    const qsizetype all = QSet<QString>(sa).unite(sb).size();
    return all > 0 ? double(common) / double(all) : 0.0;
}

// A channel's "- Topic", "VEVO" or "Official" is YouTube's way of writing
// the artist's own name.
QString cleanName(const QString &name)
{
    QStringList words = foldedWords(name);
    if (words.size() > 1 && (words.last() == QLatin1String("topic") || words.last() == QLatin1String("official")
                             || words.last() == QLatin1String("vevo")))
        words.removeLast();
    if (!words.isEmpty() && words.last().size() > 4 && words.last().endsWith(QLatin1String("vevo")))
        words.last().chop(4);
    return words.join(QLatin1Char(' '));
}

// matcher.py's artist_ok: any of the source's names among the candidate's
// credits, or written in its line (and, for a video, its title).
bool artistAgrees(const QStringList &wanted, const QStringList &credited, const QString &line)
{
    QStringList theirs;
    for (const QString &name : credited) {
        const QString clean = cleanName(name);
        if (!clean.isEmpty())
            theirs.append(clean);
    }
    const QString folded = ImportMatch::fold(line);
    for (const QString &name : wanted) {
        const QString ours = cleanName(name);
        if (ours.isEmpty())
            continue;
        if (theirs.contains(ours))
            return true;
        if (ours.size() >= 3 && containsPhrase(folded, ours))
            return true;
        QString oursJoined = ours;
        oursJoined.remove(QLatin1Char(' '));
        const QStringList ourWords = ours.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        const QSet<QString> ourSet(ourWords.cbegin(), ourWords.cend());
        for (const QString &their : std::as_const(theirs)) {
            if (std::min(their.size(), ours.size()) >= 3 && (containsPhrase(their, ours) || containsPhrase(ours, their)))
                return true;
            QString theirJoined = their;
            theirJoined.remove(QLatin1Char(' '));
            if (theirJoined == oursJoined)   // "A. R. Rahman" and "AR Rahman"
                return true;
            const QStringList theirWords = their.split(QLatin1Char(' '), Qt::SkipEmptyParts);
            const QSet<QString> theirSet(theirWords.cbegin(), theirWords.cend());
            const qsizetype all = QSet<QString>(ourSet).unite(theirSet).size();
            if (all > 0 && double(QSet<QString>(ourSet).intersect(theirSet).size()) / double(all) >= 0.5)
                return true;
        }
    }
    return false;
}

bool official(const QString &videoType)
{
    return videoType == QLatin1String("MUSIC_VIDEO_TYPE_ATV") || videoType == QLatin1String("MUSIC_VIDEO_TYPE_OMV")
           || videoType == QLatin1String("MUSIC_VIDEO_TYPE_OFFICIAL_SOURCE_MUSIC");
}

int typeRank(const QString &videoType)
{
    if (videoType == QLatin1String("MUSIC_VIDEO_TYPE_ATV"))
        return 0;
    if (videoType == QLatin1String("MUSIC_VIDEO_TYPE_OMV")
        || videoType == QLatin1String("MUSIC_VIDEO_TYPE_OFFICIAL_SOURCE_MUSIC"))
        return 1;
    if (videoType == QLatin1String("MUSIC_VIDEO_TYPE_UGC"))
        return 2;
    return 3;
}

int confidenceRank(ImportMatch::Confidence confidence)
{
    switch (confidence) {
    case ImportMatch::Confidence::Sure:   return 0;
    case ImportMatch::Confidence::Likely: return 1;
    case ImportMatch::Confidence::Check:  return 2;
    case ImportMatch::Confidence::None:   break;
    }
    return 3;
}

QString describeVersions(const QStringList &versions)
{
    return versions.isEmpty() ? QStringLiteral("the studio version") : versions.join(QStringLiteral(", "));
}

// What the titles say about each other, before artists and lengths:
// empty when they agree, else why not. `similarity` is set either way.
QString titleRefusal(const ImportMatch::Wanted &wanted, const ImportMatch::Candidate &candidate,
                     const Side &ours, const Side &theirs, double *similarity)
{
    double ts = titleSimilarity(ours.title.core, theirs.title.core);
    // A video's title often carries the artist ("Artist: Title", "Title |
    // Artist"); when it names one of ours and holds our title, it passes.
    if (ts < 0.85 && candidate.video && !ours.title.core.isEmpty()) {
        const QString folded = ImportMatch::fold(candidate.title);
        for (const QString &artist : wanted.artists) {
            const QString name = ImportMatch::fold(artist);
            if (!name.isEmpty() && containsPhrase(folded, name) && containsPhrase(folded, ours.title.core)) {
                ts = 0.85;
                break;
            }
        }
    }
    *similarity = ts;
    if (ts < 0.85)
        return QStringLiteral("the title is \"%1\"").arg(candidate.title);

    // A tribute album says so in its name ("Hits Made Famous by Anne
    // Murray", "The String Quartet Tribute to Coldplay") while its titles
    // are the songs' own.
    const QString theirTitle = ImportMatch::fold(candidate.title);
    const QString ourTitle = ImportMatch::fold(wanted.title);
    const QString theirArtists = ImportMatch::fold(candidate.artistLine);
    const QString ourArtists = ImportMatch::fold(wanted.artists.join(QLatin1Char(' ')));
    const QString theirAlbum = ImportMatch::fold(candidate.album);
    const QString ourAlbum = ImportMatch::fold(wanted.album);
    for (const QString &mark : coverMarks()) {
        if ((containsPhrase(theirTitle, mark) || containsPhrase(theirArtists, mark) || containsPhrase(theirAlbum, mark))
            && !containsPhrase(ourTitle, mark) && !containsPhrase(ourArtists, mark) && !containsPhrase(ourAlbum, mark))
            return QStringLiteral("a cover (\"%1\")").arg(mark);
    }
    if (ours.versions != theirs.versions)
        return QStringLiteral("%1, where the song is %2").arg(describeVersions(theirs.versions),
                                                               describeVersions(ours.versions));

    // A language a title names is a dub ("Srivalli (Hindi)"): the other side
    // must name the same, in its title or its album's name. Two albums that
    // name languages must name the same ones. (One album alone naming one is
    // left to the user: see languageDoubt.)
    const QStringList ourLanguages = allLanguages(ours);
    const QStringList theirLanguages = allLanguages(theirs);
    if (ourLanguages != theirLanguages
        && (!ours.languages.isEmpty() || !theirs.languages.isEmpty()
            || (!ourLanguages.isEmpty() && !theirLanguages.isEmpty()))) {
        if (ourLanguages.isEmpty())
            return QStringLiteral("in %1, which the song does not say").arg(languageNames(theirLanguages));
        if (theirLanguages.isEmpty())
            return QStringLiteral("names no language, where the song is in %1").arg(languageNames(ourLanguages));
        return QStringLiteral("in %1, where the song is in %2").arg(languageNames(theirLanguages),
                                                                     languageNames(ourLanguages));
    }

    // Both a remix, or both live: whose remix, and where, must not disagree
    // (as JioSaavn's matcher has it). Named on one side only, the name must
    // be found somewhere on the other: "Yellow (Live)" from the album "Live
    // in Buenos Aires" is the Buenos Aires take; from "Live 2003" it is not.
    const QStringList &a = ours.context;
    const QStringList &b = theirs.context;
    const QString kind = describeVersions(ours.versions);
    if (!a.isEmpty() && !b.isEmpty()) {
        const QSet<QString> sa(a.cbegin(), a.cend());
        const QSet<QString> sb(b.cbegin(), b.cend());
        if (!sa.intersects(sb))
            return QStringLiteral("a different %1 (%2, where the song's is %3)")
                .arg(kind, b.join(QLatin1Char(' ')), a.join(QLatin1Char(' ')));
    } else if (!a.isEmpty() || !b.isEmpty()) {
        const QStringList &named = a.isEmpty() ? b : a;
        const Side &other = a.isEmpty() ? ours : theirs;
        const bool found = std::any_of(named.cbegin(), named.cend(),
                                       [&other](const QString &word) { return other.vocabulary.contains(word); });
        if (!found) {
            return a.isEmpty()
                ? QStringLiteral("%1 \"%2\", which the song does not name").arg(kind, b.join(QLatin1Char(' ')))
                : QStringLiteral("names no \"%2\", as the song's %1 does").arg(kind, a.join(QLatin1Char(' ')));
        }
    }
    return {};
}

// A language only one album names, and neither title: the same recording or
// its dub, which nothing else tells apart, so the user hears it. Empty when
// there is no such doubt.
QString languageDoubt(const Side &ours, const Side &theirs)
{
    const QStringList ourLanguages = allLanguages(ours);
    const QStringList theirLanguages = allLanguages(theirs);
    if (ourLanguages == theirLanguages)
        return {};
    if (theirLanguages.isEmpty())
        return QStringLiteral("its album names no language, where the song's is in %1")
            .arg(languageNames(ourLanguages));
    return QStringLiteral("its album is in %1, which the song does not say").arg(languageNames(theirLanguages));
}

// Words neither side can place ("(Encore)", an alternate title, a list of
// actors on a film song's video): found on the other side, or the match is
// only one to check. Empty when there are none.
QString unexplained(const Side &ours, const Side &theirs)
{
    const auto missing = [](const Side &from, const Side &in) {
        QStringList out;
        for (const QString &word : from.title.unknown) {
            if (word.size() >= 2 && !in.vocabulary.contains(word) && !out.contains(word))
                out.append(word);
        }
        return out;
    };
    if (const QStringList words = missing(theirs, ours); !words.isEmpty())
        return QStringLiteral("its title also says \"%1\"").arg(words.join(QLatin1Char(' ')));
    if (const QStringList words = missing(ours, theirs); !words.isEmpty())
        return QStringLiteral("its title does not say \"%1\"").arg(words.join(QLatin1Char(' ')));
    return {};
}

ImportMatch::Verdict judgeOne(const ImportMatch::Wanted &wanted, const ImportMatch::Candidate &candidate)
{
    using ImportMatch::Confidence;
    ImportMatch::Verdict verdict;
    const QStringList names = wanted.artists + candidate.artists;
    const Side ours = sideOf(wanted.title, wanted.artists, wanted.album, names);
    const Side theirs = sideOf(candidate.title, candidate.artists, candidate.album, names);
    if (ours.title.core.isEmpty()) {
        verdict.reason = QStringLiteral("the title has nothing to match on");
        return verdict;
    }
    double ts = 0.0;
    if (const QString why = titleRefusal(wanted, candidate, ours, theirs, &ts); !why.isEmpty()) {
        verdict.reason = why;
        return verdict;
    }

    // A song row credits its artists; only a video's title may carry them.
    // Never the album's name: "Hits Made Famous by Anne Murray" credits
    // nobody, least of all Anne Murray.
    QString line = candidate.artistLine;
    if (candidate.video)
        line += QLatin1Char(' ') + candidate.title;
    const bool anyArtist = !wanted.artists.isEmpty();
    const bool agrees = anyArtist && artistAgrees(wanted.artists + ours.title.credits, candidate.artists, line);
    const int delta = wanted.durationSec > 0 && candidate.durationSec > 0
                          ? std::abs(wanted.durationSec - candidate.durationSec) : -1;
    verdict.deltaSec = delta;

    if (!anyArtist) {
        // A pasted line with a title alone: the same title, and the user says.
        if (ts == 1.0 && (delta < 0 || delta <= kLikelySecs)) {
            verdict.confidence = Confidence::Check;
            verdict.reason = QStringLiteral("no artist to compare");
        } else {
            verdict.reason = QStringLiteral("no artist to compare, and not the same title");
        }
        return verdict;
    }
    if (delta < 0) {
        if (agrees) {
            verdict.confidence = Confidence::Check;
            verdict.reason = QStringLiteral("no length to compare");
        } else {
            verdict.reason = QStringLiteral("another artist, and no length to compare");
        }
        return verdict;
    }
    if (agrees && delta <= kSureSecs) {
        verdict.confidence = official(candidate.videoType) ? Confidence::Sure : Confidence::Likely;
        verdict.reason = official(candidate.videoType) ? QString() : QStringLiteral("someone's upload");
    } else if (agrees && delta <= kLikelySecs) {
        verdict.confidence = Confidence::Likely;
        verdict.reason = QStringLiteral("%1 s apart").arg(delta);
    } else if (!agrees && ts == 1.0 && delta <= kOtherArtistSecs) {
        verdict.confidence = Confidence::Likely;
        verdict.reason = QStringLiteral("the artist is named differently; the length is the same");
    } else if (agrees) {
        verdict.reason = QStringLiteral("%1 s %2").arg(delta).arg(candidate.durationSec > wanted.durationSec
                                                                       ? QStringLiteral("longer")
                                                                       : QStringLiteral("shorter"));
        return verdict;
    } else {
        verdict.reason = QStringLiteral("by %1").arg(candidate.artistLine.isEmpty() ? QStringLiteral("someone else")
                                                                                   : candidate.artistLine);
        return verdict;
    }
    if (const QString why = unexplained(ours, theirs); !why.isEmpty()) {
        verdict.confidence = Confidence::Check;
        verdict.reason = why;
    } else if (const QString doubt = languageDoubt(ours, theirs); !doubt.isEmpty()) {
        verdict.confidence = Confidence::Check;
        verdict.reason = doubt;
    }
    return verdict;
}

// The same line read the other way round: "A - B" as title B by A.
ImportMatch::Wanted swapped(const ImportMatch::Wanted &wanted)
{
    ImportMatch::Wanted other = wanted;
    other.title = wanted.artists.value(0);
    other.artists = wanted.title.isEmpty() ? QStringList() : QStringList{ wanted.title };
    other.eitherWay = false;
    return other;
}

bool betterVerdict(const ImportMatch::Verdict &a, const ImportMatch::Verdict &b)
{
    return confidenceRank(a.confidence) < confidenceRank(b.confidence);
}

} // namespace

namespace ImportMatch {

QString fold(const QString &text)
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
        case u'$':
            prepared += QLatin1Char('s');   // Ke$ha
            continue;
        default:
            prepared += c;
        }
    }
    return Rec::plainName(prepared);
}

QString titleKey(const QString &title, const QStringList &artists)
{
    return Saavn::parseTitle(title, artists).core;
}

QString bareTitle(const QString &title)
{
    // Asides in brackets, nested ones and all, then whatever follows a
    // spaced dash.
    QString out;
    int depth = 0;
    for (const QChar c : title) {
        if (c == QLatin1Char('(') || c == QLatin1Char('[') || c == QChar(0xFF08)) {
            ++depth;
            continue;
        }
        if ((c == QLatin1Char(')') || c == QLatin1Char(']') || c == QChar(0xFF09)) && depth > 0) {
            --depth;
            out += QLatin1Char(' ');
            continue;
        }
        if (depth == 0)
            out += c;
    }
    static const QRegularExpression dash(QStringLiteral("\\s+[-\\x{2013}\\x{2014}|]\\s+"));
    const QRegularExpressionMatch match = dash.match(out);
    if (match.hasMatch() && match.capturedStart() > 0)
        out.truncate(match.capturedStart());
    out = out.simplified();
    return out.isEmpty() ? title.simplified() : out;
}

QList<Search> searches(const Wanted &wanted)
{
    QList<Search> list;
    const QString title = bareTitle(wanted.title);
    const QString lead = wanted.artists.value(0).trimmed();
    const QString first = (title + QLatin1Char(' ') + lead).simplified();
    if (!wanted.isrc.trimmed().isEmpty())
        list.append({ wanted.isrc.trimmed().toUpper(), false, true });
    if (!first.isEmpty())
        list.append({ first, false, false });
    // JioSaavn often credits the composer first; the album names the film.
    const QString second = !wanted.album.trimmed().isEmpty() ? bareTitle(wanted.album)
                                                             : wanted.artists.value(1).trimmed();
    if (!second.isEmpty()) {
        const QString query = (title + QLatin1Char(' ') + second).simplified();
        if (query != first)
            list.append({ query, false, false });
    }
    if (!first.isEmpty())
        list.append({ first, true, false });
    return list;
}

Verdict judge(const Wanted &wanted, const Candidate &candidate)
{
    Verdict verdict = judgeOne(wanted, candidate);
    if (wanted.eitherWay && !wanted.artists.isEmpty()) {
        const Verdict other = judgeOne(swapped(wanted), candidate);
        if (betterVerdict(other, verdict))
            verdict = other;
    }
    return verdict;
}

Verdict judgeIsrc(const Wanted &wanted, const Candidate &candidate)
{
    Verdict verdict;
    const QStringList names = wanted.artists + candidate.artists;
    const Side ours = sideOf(wanted.title, wanted.artists, wanted.album, names);
    const Side theirs = sideOf(candidate.title, candidate.artists, candidate.album, names);
    double ts = 0.0;
    if (const QString why = titleRefusal(wanted, candidate, ours, theirs, &ts); !why.isEmpty()) {
        verdict.reason = why;
        return verdict;
    }
    const int delta = wanted.durationSec > 0 && candidate.durationSec > 0
                          ? std::abs(wanted.durationSec - candidate.durationSec) : -1;
    verdict.deltaSec = delta;
    if (delta < 0) {
        verdict.confidence = Confidence::Check;
        verdict.reason = QStringLiteral("no length to compare");
    } else if (delta <= kLikelySecs) {
        verdict.confidence = Confidence::Sure;
    } else {
        verdict.reason = QStringLiteral("%1 s apart").arg(delta);
    }
    return verdict;
}

Pick pick(const Wanted &wanted, const QList<Candidate> &candidates, const Search &search)
{
    Pick best;
    const int looked = search.isrc ? 1 : kResultsLooked;
    for (int i = 0; i < candidates.size() && i < looked; ++i) {
        const Candidate &candidate = candidates.at(i);
        const Verdict verdict = search.isrc ? judgeIsrc(wanted, candidate) : judge(wanted, candidate);
        best.notes.append(QStringLiteral("#%1 \"%2\" / %3 / %4 s: %5")
                              .arg(i + 1)
                              .arg(candidate.title, candidate.artistLine)
                              .arg(candidate.durationSec)
                              .arg(verdict.confidence == Confidence::None ? verdict.reason
                                                                          : confidenceName(verdict.confidence)));
        if (verdict.confidence == Confidence::None)
            continue;
        if (best.index < 0
            || better(verdict.confidence, candidate.videoType, i, best.verdict.confidence,
                      candidates.at(best.index).videoType, best.index)) {
            best.index = i;
            best.verdict = verdict;
        }
    }
    return best;
}

bool better(Confidence a, const QString &typeA, int orderA, Confidence b, const QString &typeB, int orderB)
{
    if (confidenceRank(a) != confidenceRank(b))
        return confidenceRank(a) < confidenceRank(b);
    if (typeRank(typeA) != typeRank(typeB))
        return typeRank(typeA) < typeRank(typeB);
    return orderA < orderB;
}

Candidate fromTrack(const InnerTube::Track &track, bool video)
{
    Candidate candidate;
    candidate.videoId = track.videoId;
    candidate.title = track.title;
    candidate.artistLine = track.artist;
    candidate.album = track.album;
    candidate.artwork = track.artwork;
    candidate.durationSec = int(qRound(double(track.durationMs) / 1000.0));
    candidate.videoType = track.videoType;
    candidate.video = video;
    candidate.isVideo = track.isVideo;
    for (const InnerTube::Credit &credit : track.credits) {
        if (credit.name && !credit.text.trimmed().isEmpty())
            candidate.artists.append(credit.text.trimmed());
    }
    if (candidate.artists.isEmpty())
        candidate.artists = Saavn::splitArtists(track.artist);
    return candidate;
}

bool sameSong(const Wanted &wanted, const QString &title, const QString &artist, int durationSec,
              const QString &album)
{
    if (wanted.durationSec <= 0 || durationSec <= 0 || std::abs(wanted.durationSec - durationSec) > kLibrarySecs)
        return false;
    const QStringList theirArtists = Saavn::splitArtists(artist);
    const auto agrees = [&](const Wanted &w) {
        if (w.artists.isEmpty())
            return false;
        const QStringList names = w.artists + theirArtists;
        const Side ours = sideOf(w.title, w.artists, w.album, names);
        const Side theirs = sideOf(title, theirArtists, album, names);
        return !ours.title.core.isEmpty() && ours.title.core == theirs.title.core && ours.versions == theirs.versions
               && allLanguages(ours) == allLanguages(theirs) && ours.context == theirs.context
               && artistAgrees(w.artists + ours.title.credits, theirArtists, artist);
    };
    return agrees(wanted) || (wanted.eitherWay && agrees(swapped(wanted)));
}

QString songKey(const Wanted &wanted)
{
    const Side side = sideOf(wanted.title, wanted.artists, wanted.album, wanted.artists);
    QStringList version = side.versions;
    for (const QString &language : allLanguages(side))
        version.append(language);
    version.append(side.context);
    return side.title.core + QLatin1Char('|') + version.join(QLatin1Char(',')) + QLatin1Char('|')
           + fold(wanted.artists.value(0));
}

QString confidenceName(Confidence confidence)
{
    switch (confidence) {
    case Confidence::Sure:   return QStringLiteral("sure");
    case Confidence::Likely: return QStringLiteral("likely");
    case Confidence::Check:  return QStringLiteral("check");
    case Confidence::None:   break;
    }
    return QStringLiteral("none");
}

Confidence confidenceFrom(const QString &name)
{
    if (name == QLatin1String("sure"))
        return Confidence::Sure;
    if (name == QLatin1String("likely"))
        return Confidence::Likely;
    if (name == QLatin1String("check"))
        return Confidence::Check;
    return Confidence::None;
}

int version()
{
    return kVersion;
}

} // namespace ImportMatch
