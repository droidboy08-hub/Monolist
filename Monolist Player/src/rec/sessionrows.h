#pragma once

#include <QHash>
#include <QSet>
#include <QString>
#include <QVariantList>
#include <QVector>

#include <functional>

// Home's just-played rows ("More like <song>", "Based on your recent
// listening"), composed from the songs just heard and YouTube Music's own
// radio for each of them (JustPlayed, justplayed.h, asks and keeps those).
//
// Pure: no SQL, no network, and never the catalogue. Everything is keyed by
// video id and answered by the radio, so a lyric upload, a fan's remix or a
// phonk edit the catalogue has never heard of seeds a row like any other
// song. What sets the rows apart is the radios themselves: two seeds whose
// radios share their artists are one cluster, so a page after phonk and then
// Punjabi keeps both, side by side, without anything knowing what either is.
namespace Rec {

// One song just heard, as it seeds: every play of one video id is one seed,
// at its newest time, `listens` the plays it stands for.
struct SessionSeed {
    QString videoId;
    QString title;
    QString artist;
    QString leadKey;       // suggestionArtistKey of its first credit
    QString artistName;    // that first credit as written
    QString artistId;      // its page, where known
    bool chosen = true;    // false for what autoplay added
    bool liked = false;
    int listens = 1;
    qint64 lastAt = 0;     // UTC seconds
    bool live = false;     // the song playing now
};

// One song of a radio, as YouTube Music answered it.
struct RadioTrack {
    QString videoId;
    QString title;
    QString artist;
    QString primaryArtist;
    QString album;
    QString albumId;
    QString artwork;
    qint64 durationMs = 0;
    bool isVideo = false;
    QVariantList credits;  // [{ text, id, link }], as InnerTube::creditsToVariant
};

struct SessionOptions {
    int perRow = 12;
    int mixRow = 15;
    int minRow = 6;        // a seed row with fewer is not shown
    int minMix = 8;
    int perArtist = 3;     // cards of one lead artist in one row at most
    // Songs heard these last 48 hours: never offered again so soon.
    QSet<QString> recentHeard;
    bool hideExplicit = false;
    // Which version of the rows (rotationFor); 0 is the rows as ranked.
    quint64 rotation = 0;
};

struct SessionRow {
    QString key;           // kind + ":" + seedVideoId
    QString kind;          // "s1", "mix", "s2" or "s3"
    QString title;
    QString seedVideoId;
    QVector<RadioTrack> items;
};

// Whether a song was turned down (Recommender::unwanted): by its id, its
// title and artist, or its artist.
using SessionFilter = std::function<bool(const QString &videoId, const QString &title, const QString &artist)>;

// A title as a row's heading says it: "BbY WOW - KAROL G (Lyrics)" by KAROL
// G is "BbY WOW". Only what says how it was uploaded goes ((Lyrics), [Official
// Video], (Audio), [4K] and the like, and a leading "<artist> - "); Slowed,
// Sped Up, Remix, Live and Reverb are the song and stay. The title as it came
// when nothing would be left.
QString cleanSeedTitle(const QString &title, const QString &artist);

// The first name of a credit as written, without YouTube's " - Topic":
// "Bruno Mars & Lady Gaga" is Bruno Mars.
QString leadArtistName(const QString &artist);

// The lead artist of a radio's song, as rows compare and cap them.
QString radioLeadKey(const RadioTrack &track);

// How much two radios share their artists: the distinct lead artists of each
// one's first 25 songs, |A ∩ B| / min(|A|, |B|). 0 when either has none.
double artistOverlap(const QVector<RadioTrack> &a, const QVector<RadioTrack> &b);

// Which seeds go together, as a cluster number per seed (the index of its
// cluster's first seed). Two seeds join when they share a lead artist, or
// when their radios overlap by 0.30 or more; joins are transitive. A seed
// with no radio joins by its lead artist alone.
QVector<int> clusterSeeds(const QVector<SessionSeed> &seeds, const QHash<QString, QVector<RadioTrack>> &radios);

// exp(-age / halfLife) × (chosen ? 1 : 0.5) × (1 + 0.25 ln listens) ×
// (liked ? 1.3 : 1): a loop of 116 plays weighs about 2.2 plays.
double seedWeight(const SessionSeed &seed, qint64 nowSecs, double halfLifeHours);

// Plays (newest first, one seed each) collapsed as rows want them: one seed
// per video id at its newest time with `listens` added up, then one per
// song, so a lyric upload and the official audio of it are one seed.
QVector<SessionSeed> collapseSeeds(const QVector<SessionSeed> &plays);

// What one seed's radio could offer, in its order: not the seed itself nor
// any seed's song, nothing heard these 48 hours, nothing turned down or
// unsuitable, and no more than `perArtist` of one lead artist. `seeds` may
// be empty (the probe's); `seen` (video ids and song keys) is left out too.
QVector<RadioTrack> eligibleItems(const QVector<RadioTrack> &radio, const QVector<SessionSeed> &seeds,
                                  const SessionOptions &options, const SessionFilter &unwanted,
                                  const QSet<QString> &seen = {});

// The rows, from the seeds (newest first; the live seed, if any, first) and
// the radios kept for them (by seed video id; a seed with none is passed
// over). S1 is the newest seed with a radio of 6 usable songs or more; S2
// the newest of another cluster within 48 hours, a song the user chose
// first, the rotation drawing between the top two; S3 the newest of the
// heaviest cluster neither covers. The mix takes what they left, from up to
// five seeds, two a cluster. No song is in two rows; each row's first four
// stay put across rotations, a seed row leaving out all that one before it
// could have drawn (its first 24), not only what the draw took. Rows in
// reading order: s1, mix, s2, s3.
QVector<SessionRow> buildSessionRows(const QVector<SessionSeed> &seeds,
                                     const QHash<QString, QVector<RadioTrack>> &radios, qint64 nowSecs,
                                     const SessionOptions &options, const SessionFilter &unwanted);

// The artists Home's More like <artist> could follow: the lead credits of
// the seeds of the last 48 hours, by their summed seedWeight with a half-life
// of 24 hours, heaviest first, one seed each (its newest), and only those
// whose page is an artist's own. `weights`, when given, gets each one's.
QVector<SessionSeed> artistCandidates(const QVector<SessionSeed> &seeds, qint64 nowSecs,
                                      const std::function<bool(const QString &name)> &isArtistPage,
                                      QVector<double> *weights = nullptr);

} // namespace Rec
