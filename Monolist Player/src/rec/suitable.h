#pragma once

#include <QString>

namespace Rec {

// Whether a song may be offered to someone who did not ask for it.
//
// Every shelf in the Search tab is unsolicited, which sets a higher bar than
// search does: a listener who searches for something gets what they searched
// for, but a recommendation is the app putting a title in front of them. The
// catalogue is 400,000 songs with nothing filtering what they are called, and
// review found an ordinary grindcore seed putting a title about voting for
// Hitler at the top of a shelf, and others surfacing racial slurs.
//
// So this refuses slurs and explicit hate references in a title, and a short
// list of acts whose output is predominantly hate music. It is deliberately
// narrow, for reasons that are all measured rather than supposed:
//
// - Whole words only. Substring matching flags "Musarrat Nazir", "Maria
//   Nazionale" and "Aaryan Shah", which is worse than no filter.
// - Slurs, not references. "nazi", "hitler" and "white pride" all appear in
//   this catalogue mostly in songs AGAINST what they name — Dead Kennedys,
//   "Hitler muss immer wieder sterben", "Good Night White Pride" — and a
//   filter that silences those has the matter backwards. A slur is different:
//   showing the word is the harm, whatever the song means.
// - Words that are also ordinary names are checked in titles only: "Kike" is
//   the Spanish short form of Enrique, and there is an artist called that.
// - Not crude or sexual titles in general. Whether to hide explicit songs is
//   a choice for the listener, not a safety question, and a grindcore fan's
//   shelf being full of grindcore is the shelf working. That choice is
//   `hideExplicit` — Settings' "Hide explicit titles", off unless the listener
//   turns it on — which adds explicitTitle() below to the checks.
//
// Nothing here stops anyone searching for anything.
bool suitableForSuggestion(const QString &title, const QString &artist, bool hideExplicit = false);

// Whether a title is one a listener who asked to hide explicit songs would not
// want put in front of them: a crude or sexual word in it, whole and in any
// case, or a mark that it is the explicit version — "(Explicit)", "[Explicit]",
// "Explicit Version".
//
// Titles only, because that is all there is: the catalogue and the graph
// carry no explicit flag, so a song whose words are explicit under a clean
// title passes. The word list is short on purpose, and every word on it was
// run against the whole catalogue (--content-test) before it went in; a word
// that also means something innocent stays off it, because hiding a Latin
// mass or a Swedish pop song for someone else's swearing is the filter
// failing, not erring on the safe side.
bool explicitTitle(const QString &title);

} // namespace Rec
