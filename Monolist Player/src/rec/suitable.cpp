#include "suitable.h"

#include <QSet>
#include <QStringList>

namespace Rec {
namespace {

// Lowercase, Latin accents removed, everything but letters and digits turned
// into spaces, and padded with a space either side so that a word or phrase
// can be found as " word " without ever matching inside a longer one.
QString words(const QString &text)
{
    QString out;
    out.reserve(text.size());
    for (const QChar c : text.normalized(QString::NormalizationForm_KD)) {
        if (c.category() == QChar::Mark_NonSpacing)
            continue;
        out += c.isLetterOrNumber() ? c.toLower() : QLatin1Char(' ');
    }
    return QLatin1Char(' ') + out.simplified() + QLatin1Char(' ');
}

// The words themselves have to be written out: a filter cannot refuse what it
// does not name.
//
// The rule is slurs, as written, and two slogans that exist only to endorse.
// A slur is refused whatever the song around it means, because putting the
// word in front of someone is the harm; that costs Clawfinger's anti-racist
// song of the same name, and it is the right trade. A reference is not
// refused, because one word cannot tell mention from endorsement. Tested
// against every row in the catalogue, the lone word "hitler" refused eight
// songs of which three were hateful — two of them already refused by the act
// list below — and the rest were an anti-Nazi reggae song ("Hitler muss immer
// wieder sterben"), two comedy rap battles, a 1930s satire and a Punjabi film
// song. "white pride" refused the anti-fascist "Good Night White Pride". Both
// are out, for the same reason "nazi" was never in.
const QStringList &anywhere()
{
    static const QStringList terms = {
        QStringLiteral(" nigger "), QStringLiteral(" niggers "),
        QStringLiteral(" faggot "), QStringLiteral(" faggots "),
        QStringLiteral(" chink "), QStringLiteral(" chinks "),
        QStringLiteral(" gook "), QStringLiteral(" gooks "),
        QStringLiteral(" wetback "), QStringLiteral(" wetbacks "),
        QStringLiteral(" raghead "), QStringLiteral(" ragheads "),
        QStringLiteral(" towelhead "), QStringLiteral(" towelheads "),
        QStringLiteral(" tranny "), QStringLiteral(" trannies "),
        QStringLiteral(" sieg heil "), QStringLiteral(" white power "),
    };
    return terms;
}

// Slurs that are also someone's name, so only a title is held to them. The
// antisemitic one was tried here and refused two songs, both wrongly: it is
// the Spanish short form of Enrique, and it turns up in "(feat. Kike & Manu)".
const QStringList &inTitlesOnly()
{
    static const QStringList terms = {
        QStringLiteral(" spic "), QStringLiteral(" spics "),
    };
    return terms;
}

// Acts whose output is predominantly hate music: white-power rock, national
// socialist black metal, and a band that built its catalogue on slurs. Named
// in full and matched as the credit's opening words, so "Landser & …" is
// caught while no other artist can be: every name ends on a word boundary.
bool hateAct(const QString &artistWords)
{
    static const QStringList acts = {
        QStringLiteral(" anal cunt "),
        QStringLiteral(" skrewdriver "),
        QStringLiteral(" landser "),
        QStringLiteral(" nokturnal mortum "),
        QStringLiteral(" satanic warmaster "),
        QStringLiteral(" adolf hitler "),
    };
    for (const QString &act : acts) {
        if (artistWords.startsWith(act))
            return true;
    }
    return false;
}

bool containsAny(const QString &text, const QStringList &terms)
{
    for (const QString &term : terms) {
        if (text.contains(term))
            return true;
    }
    return false;
}

} // namespace

bool suitableForSuggestion(const QString &title, const QString &artist)
{
    const QString titleWords = words(title);
    const QString artistWords = words(artist);
    if (hateAct(artistWords))
        return false;
    if (containsAny(titleWords, anywhere()) || containsAny(artistWords, anywhere()))
        return false;
    if (containsAny(titleWords, inTitlesOnly()))
        return false;
    return true;
}

} // namespace Rec
