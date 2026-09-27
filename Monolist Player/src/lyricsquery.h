#pragma once

#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

// What a lyrics database is asked for a song, and whether an entry it answers
// with is that song. One set of rules for every provider, so a second cleaning
// pass inside a provider can never undo the first.
//
// The title is cleaned the way databases file songs: "(Official Video)",
// "[Remastered 2011]" and "(feat. ...)" go, and "Artist - " in front goes. A
// version marker stays, even when it shares a bracket with noise: "(Official
// Live Video)" becomes "(Live)", because a live take or a remix is another
// recording with other timing, sometimes other words. "(with X)" goes only when
// X is one of the song's artists — "To Sir (With Love)" is a title — and an
// unbracketed "feat." stops at the next bracket, so "Song feat. Guest (Remix)"
// keeps its "(Remix)".
//
// The artists are the song's credits as YouTube Music linked them, never a
// guess made by cutting the joined line at "&" or ",": "Simon & Garfunkel" and
// "Earth, Wind & Fire" are one name each. Without credits, the caller passes
// what ArtistLinks could split the line into (names it has seen linked), and a
// line nobody has linked stays whole, cut only at an unambiguous "feat.".
namespace LyricsQuery {

struct Query {
    QString title;        // "Song (Live)": as databases file it, markers kept
    QString bareTitle;    // "Song": without the markers, for a database that files only the song
    QStringList artists;  // every credited name, the lead first; one whole line when it could not be split
    QString album;
    double durationS = 0; // 0 when not known

    QString leadArtist() const { return artists.value(0); }
};

// `pieces` is the artist line as ArtistLinks::credits gives it,
// [{ text, id, link }]; empty, the track's own "credits" are used, then its
// "primaryArtist", then the line itself.
Query fromTrack(const QVariantMap &track, const QVariantList &pieces = {});

QStringList artistNames(const QString &line, const QString &primaryArtist, const QVariantList &pieces);
QString cleanTitle(const QString &title, const QStringList &artists);
// A cleaned title without its version markers or anything else in brackets.
QString bareTitle(const QString &cleanedTitle);

// How well an entry a database answered with fits the song asked for.
struct Match {
    double title = 0;      // 0..1: 1 when one spelling of it is the other's
    bool artist = false;   // a credited name is (in) the entry's artist
    double gapS = -1;      // |length difference| in seconds; -1 when either is unknown
    bool accepted = false;
};
// Accepted when the titles agree (0.8 or better) and so do the artists — or,
// for an artist written in another script, the lengths are within 3 s. A hit
// on the artist alone is never enough, and neither is a title that only
// contains the one asked for: "Anti-Hero" is not "Hero".
Match match(const Query &query, const QString &title, const QString &artist, double durationS);

} // namespace LyricsQuery
