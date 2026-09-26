#include "suitable.h"

#include <QRegularExpression>
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

// The words "Hide explicit titles" hides, for explicitTitle().
//
// The test for a word is whether a clean edit would bleep it or retitle the
// song around it — "Fuck You" going out as "Forget You" — and whether it means
// nothing else. Each form is spelled out, since matching is on whole words:
// " fuck " finds "Fuck You" and never "Scunthorpe", but not "fucking" either.
//
// Run against all 400,000 rows, the list below hides about a thousand, and
// every group was read through. What that reading changed is recorded here.
//
// In, although borderline:
// - "bitch": crude whatever the song, and bleeped on the radio; of 169
//   titles, not one meant a dog.
// - "pussy": a nursery rhyme ("Pussy Cat, Pussy Cat") and "Pussy Willow" are
//   the innocent uses, so those phrases are let back in (see cleanPhrases);
//   "Pussycat", one word, never matched.
// - "tits": French writes "petits" as "p'tits" — "Trois p'tits chats" is a
//   children's song — so "p'tits" is let back in the same way.
// - "nigga": not on the slur list, which refuses a title whatever the
//   listener's settings, because it is used within hip-hop rather than thrown
//   at anyone — but a clean edit bleeps it, which is the test here.
// - "mierda", "verga", "joder", "scheisse", "ficken", "merde", "putain",
//   "salope": Spanish, German and French words that mean nothing else, in
//   the languages reggaeton, German rap and French rap are sung in.
// - "putas", and the idioms "hijo de puta", "de puta madre", "puta que
//   pariu", "filho da puta": but not "puta" alone, which is Serbo-Croatian
//   for "times" and "of the road" — "Sto puta", "Na pola puta".
//
// Out, because they mean something else too often:
// - "slut": Swedish and Danish for "end". "I ett hus vid skogens slut" is a
//   children's song, and seven of the 27 titles it caught were like it.
// - "ass" (the animal, and "kick-ass" is not what anyone hides), "dick" (a
//   name: Moby Dick, Dick Dale), "cock" (a rooster; "Who Killed Cock Robin"),
//   "hoe" (the tool; "Hoe-Down"), "tit" (the bird), "cum" (Latin: "Cum Sancto
//   Spiritu" closes the Gloria of the mass).
// - "fick", the German imperative, is Swedish for "got": "Jag fick feeling".
// - "puto" is a boy in Portugal; "culo" and "damn" are words radio plays.
// - "sex", "sexy", "porn", "horny": they name the subject rather than swear
//   about it, and radio plays "Sex on Fire" under that title.
const QStringList &crude()
{
    static const QStringList terms = {
        QStringLiteral(" fuck "), QStringLiteral(" fucks "), QStringLiteral(" fucked "),
        QStringLiteral(" fucker "), QStringLiteral(" fuckers "), QStringLiteral(" fucking "),
        QStringLiteral(" fuckin "), QStringLiteral(" fuckboy "), QStringLiteral(" fuckboys "),
        QStringLiteral(" motherfucker "), QStringLiteral(" motherfuckers "),
        QStringLiteral(" motherfucking "), QStringLiteral(" motherfuckin "),
        QStringLiteral(" mothafucka "), QStringLiteral(" mothafuckas "),
        QStringLiteral(" muthafucka "), QStringLiteral(" muthafuckas "),
        QStringLiteral(" shit "), QStringLiteral(" shits "), QStringLiteral(" shitty "),
        QStringLiteral(" shitting "), QStringLiteral(" bullshit "),
        QStringLiteral(" cunt "), QStringLiteral(" cunts "),
        QStringLiteral(" pussy "), QStringLiteral(" pussies "),
        QStringLiteral(" bitch "), QStringLiteral(" bitches "),
        QStringLiteral(" whore "), QStringLiteral(" whores "),
        QStringLiteral(" asshole "), QStringLiteral(" assholes "),
        QStringLiteral(" cocksucker "), QStringLiteral(" cocksuckers "),
        QStringLiteral(" dickhead "), QStringLiteral(" blowjob "), QStringLiteral(" dildo "),
        QStringLiteral(" tits "), QStringLiteral(" titties "),
        QStringLiteral(" wanker "), QStringLiteral(" wankers "), QStringLiteral(" twat "),
        QStringLiteral(" nigga "), QStringLiteral(" niggas "),
        QStringLiteral(" putas "), QStringLiteral(" hijo de puta "), QStringLiteral(" hija de puta "),
        QStringLiteral(" de puta madre "), QStringLiteral(" puta que pariu "),
        QStringLiteral(" filho da puta "), QStringLiteral(" filha da puta "),
        QStringLiteral(" mierda "), QStringLiteral(" verga "), QStringLiteral(" joder "),
        QStringLiteral(" scheisse "), QStringLiteral(" scheiße "), QStringLiteral(" ficken "),
        QStringLiteral(" merde "), QStringLiteral(" putain "), QStringLiteral(" salope "),
    };
    return terms;
}

// The innocent phrases a crude word is part of, taken out of a title before
// the words are checked. "p tits" is "p'tits" once the apostrophe is a space.
const QStringList &cleanPhrases()
{
    static const QStringList phrases = {
        QStringLiteral(" pussy cat "), QStringLiteral(" pussy willow "), QStringLiteral(" p tits "),
    };
    return phrases;
}

// A title that says it is the explicit version: "(Explicit)", "[Explicit]",
// "(Explicit Version)", "Explicit Version", "Dirty Version", "Song - Explicit".
// Only as a mark — in brackets, before "version" or "edit", or trailing after
// a dash — so a song that is merely called "Explicit" is not one.
bool explicitMark(const QString &title)
{
    static const QRegularExpression mark(
        QStringLiteral(R"([(\[][^)\]]*\bexplicit\b[^)\]]*[)\]])"
                       R"(|\b(?:explicit|dirty)\s+(?:album\s+)?(?:version|edit)\b)"
                       R"(|[-–—]\s*explicit\s*$)"),
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::UseUnicodePropertiesOption);
    return mark.match(title).hasMatch();
}

} // namespace

bool suitableForSuggestion(const QString &title, const QString &artist, bool hideExplicit)
{
    const QString titleWords = words(title);
    const QString artistWords = words(artist);
    if (hateAct(artistWords))
        return false;
    if (containsAny(titleWords, anywhere()) || containsAny(artistWords, anywhere()))
        return false;
    if (containsAny(titleWords, inTitlesOnly()))
        return false;
    if (hideExplicit && explicitTitle(title))
        return false;
    return true;
}

bool explicitTitle(const QString &title)
{
    QString titleWords = words(title);
    // Replaced with a single space, which keeps the words either side of the
    // phrase whole for the check that follows. Until none is left, because a
    // phrase said twice ("pussy cat pussy cat") shares the space between its
    // two copies, and one pass over it takes only the first.
    for (const QString &phrase : cleanPhrases()) {
        while (titleWords.contains(phrase))
            titleWords.replace(phrase, QStringLiteral(" "));
    }
    return containsAny(titleWords, crude()) || explicitMark(title);
}

} // namespace Rec
