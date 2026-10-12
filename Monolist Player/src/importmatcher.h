#pragma once

#include "innertube.h"

#include <QList>
#include <QString>
#include <QStringList>

// How a song from another service's playlist is found on YouTube Music, for
// the playlist import (ROADMAP X03; PlaylistImport runs it). The rule is the
// prototype's (importresearch/matcher.py), which was tried live on 66 songs
// from five playlists (Spotify, Deezer, JioSaavn): 63 sure, 2 likely, 1 to
// check, and none wrong once cover versions were refused.
//
// A title is taken apart as JioSaavn's matcher takes it (Saavn::parseTitle):
// its core, the version it names ("Live", "Remix", "Sped Up", a language),
// the names it credits ("feat. …") and asides that are none of those. Then:
//  - the titles must agree: the same core, or one inside the other;
//  - the version must agree both ways: "Yellow (Live)" is not "Yellow", nor
//    "Srivalli (Hindi)" the Telugu "Srivalli" (a language an album's name
//    gives counts too; named by one album alone, the user checks it), nor
//    one live take or remix another ("Live in Buenos Aires", "Chromatics
//    Remix"), as JioSaavn's matcher has it;
//  - cover wording on YouTube's side ("Originally By", "In the Style of",
//    "Karaoke", "Tribute"), in its title, its artists or its album's name,
//    that the source does not have refuses it;
//  - one of the source's artists must be credited (a video's title may
//    carry the name too, a song row's may not, nor an album's name);
//  - the length decides how sure: within 5 s on YouTube Music's own song or
//    the official video is sure, within 15 s likely, anyone's upload at most
//    likely; the exact title by another artist only within 2 s, and likely;
//    with no length to compare, the user checks it.
// Of what passes: sure before likely before to check, then the official song
// before the official video before an upload, then YouTube Music's own
// order. Never by the smallest length difference alone.
namespace ImportMatch {

// A song as the source lists it.
struct Wanted {
    QString title;
    QStringList artists;
    QString album;
    int durationSec = 0;      // 0 when the source does not say
    QString isrc;
    // A pasted "A - B" line: either half may be the title, and whichever
    // reads better is taken.
    bool eitherWay = false;
};

// One search result.
struct Candidate {
    QString videoId;
    QString title;
    QStringList artists;      // the names credited
    QString artistLine;       // as YouTube Music writes it
    QString album;
    QString artwork;
    int durationSec = 0;      // 0 when the row does not say
    QString videoType;        // InnerTube::Track::videoType
    bool video = false;       // from the videos filter
    bool isVideo = false;     // a real video, for the library's is_video
};

enum class Confidence { None, Check, Likely, Sure };

struct Verdict {
    Confidence confidence = Confidence::None;
    QString reason;           // why not, or what to check
    int deltaSec = -1;        // how far apart the lengths are; -1 unknown
};

// What to ask YouTube Music, in order: the ISRC (when the source gives one),
// "title lead-artist", "title album" (or the second artist when there is no
// album), then the same first query among videos.
struct Search {
    QString query;
    bool videos = false;
    bool isrc = false;
};
QList<Search> searches(const Wanted &wanted);

Verdict judge(const Wanted &wanted, const Candidate &candidate);
// An ISRC search's first result, the only one of it worth anything: the
// recording itself when its title passes and it is within 15 s.
Verdict judgeIsrc(const Wanted &wanted, const Candidate &candidate);

// The best of one search's results (its first five; an ISRC search's first
// alone), and a line for each about why it was or was not taken.
struct Pick {
    int index = -1;
    Verdict verdict;
    QStringList notes;
};
Pick pick(const Wanted &wanted, const QList<Candidate> &candidates, const Search &search);
// Whether `a` is a better choice than `b` (sure > likely > check, then the
// kind of row, then the order the searches and YouTube Music gave).
bool better(Confidence a, const QString &typeA, int orderA, Confidence b, const QString &typeB, int orderB);

Candidate fromTrack(const InnerTube::Track &track, bool video);

// Whether a song the library keeps is this one: the same title and version
// (its language and whose remix or which concert included), an artist in
// common, and within 3 s. Never without both lengths.
bool sameSong(const Wanted &wanted, const QString &title, const QString &artist, int durationSec,
              const QString &album = QString());

// A song as far as a match made for it may be used for it again, by title
// and artist (with its length beside it, PlaylistImport's "tal:" key): its
// title's core, the version it names (a language, its album's included, and
// whose remix or which concert) and its lead artist, folded. "Yellow" and
// "Yellow (Live)" differ, as do a Telugu album's song and its Hindi dub.
QString songKey(const Wanted &wanted);

// Lower case, accents off, "&" and "+" as "and", apostrophes gone, and the
// rest a single space: what both sides go through before they are compared.
QString fold(const QString &text);
// A title as far as it tells one song from another (its core), for looking
// a song up in the library by title.
QString titleKey(const QString &title, const QStringList &artists = {});
// The title as written without its asides ("Yellow - Remastered 2011",
// "Kesariya (From …)"), for a query.
QString bareTitle(const QString &title);

QString confidenceName(Confidence confidence);
Confidence confidenceFrom(const QString &name);
// Up by one whenever the rules change what is taken: matches an older rule
// made (import_matches) are then asked afresh.
int version();

} // namespace ImportMatch
