#pragma once

#include <QObject>
#include <QString>

#include <functional>
#include <utility>

#include "lyricsquery.h"

// Lyrics as one provider gave them, or as the database kept them.
struct LyricsAnswer {
    QString synced;              // LRC
    QString plain;
    QString source;              // as credited: "LRCLIB", "Musixmatch via YouTube Music"
    bool instrumental = false;
    // Another recording's words: LRCLIB's search found the song only at a
    // length 3-10 s off. The words are right, the timing is not trusted, and
    // any provider's own plain text outranks them.
    bool loose = false;
    // Kept only because a provider that could have done better failed to
    // answer: shown, and asked for again (Lyrics).
    bool provisional = false;
    // The length of the entry it came from, in seconds, where the provider
    // reports one; -1 where it does not, and the race's gate then trusts the
    // provider's own match (YouTube Music's is the video itself).
    double durationS = -1;

    // How good an answer is, for the race: timed lines, or the word that
    // there are none (instrumental), above plain text above another
    // recording's words. Line and word timing rank alike: the first timed
    // answer in the user's order wins, whichever it is.
    enum Rank { Nothing = 0, Loose = 1, Plain = 2, Timed = 3 };
    Rank rank() const
    {
        if (instrumental || !synced.trimmed().isEmpty())
            return Timed;
        if (!plain.trimmed().isEmpty())
            return loose ? Loose : Plain;
        return Nothing;
    }
    bool found() const { return rank() != Nothing; }
};

// What one provider ended with for one song. A miss (it answered, and has
// none) is never a failure (it could not be asked, or did not answer): only
// every provider missing makes "no lyrics".
struct LyricsOutcome {
    enum Kind { Found, Missed, Failed };
    Kind kind = Missed;
    LyricsAnswer answer;   // Found
    QString error;         // Failed; or Found and `partial`
    // Found, but part of the lookup failed, so a better answer of this
    // provider's own may exist: it counts as a failure when the race decides
    // whether its answer is provisional, and it is not cached.
    bool partial = false;
};

// What a provider is asked.
struct LyricsRequest {
    QString videoId;
    LyricsQuery::Query query;
};

// One provider's lookup of one song, in flight. It calls `onFinished` once,
// unless cancel() comes first; after cancel() it calls nothing, and every
// request it had out has been aborted. Owned by whoever asked for it (the
// race), which deletes it once it has finished or been cancelled.
class LyricsLookup : public QObject
{
public:
    using QObject::QObject;

    std::function<void(const LyricsOutcome &)> onFinished;

    virtual void start() = 0;
    virtual void cancel() = 0;

protected:
    // Once: a second call, or one after cancel(), does nothing.
    void finish(const LyricsOutcome &outcome)
    {
        if (auto done = std::exchange(onFinished, nullptr))
            done(outcome);
    }
};

// A source of lyrics (lyrics/providers/*). Providers only look up; racing
// them, ranking what they find and keeping it is LyricsRace's and Lyrics'.
class LyricsProvider
{
public:
    // The best timing it can ever answer with. The race uses it to know
    // when nothing a provider still out could find would change the answer.
    enum class Timing { Plain, Line, Word };

    virtual ~LyricsProvider() = default;

    // The key its answers are kept under ("lrclib"), and its name in the log.
    virtual QString id() const = 0;
    virtual QString name() const = 0;
    virtual Timing bestTiming() const = 0;
    // Started only once every provider above it has answered and nothing at
    // all was found: for scrapers, which cost bandwidth and invite rate
    // limits. None of today's two is.
    virtual bool lazy() const { return false; }
    // May look songs up before anyone looks at them (Lyrics' background
    // lookups). Only today's two do; a newer third-party provider is asked
    // only while the lyrics are on screen, unless it is added here on
    // purpose.
    virtual bool background() const { return false; }

    // A lookup of `request`, not started yet, owned by `parent`.
    virtual LyricsLookup *lookUp(const LyricsRequest &request, QObject *parent) = 0;
};
