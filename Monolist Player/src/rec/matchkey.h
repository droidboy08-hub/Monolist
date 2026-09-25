#pragma once

#include <QByteArray>
#include <QString>

// The bridge between a track we hold and a row in the embedding catalogue.
//
// The catalogue ships as four binary files keyed by 64-bit hashes of a
// normalised title and artist; there is no id in common with anything a tag or
// a search result carries, so these keys are the only way in. They were
// computed offline by graph/build_embeddings.py, which makes THAT script the
// specification - not the Swift twin in the iOS app, which drifted from it in
// ways nobody noticed because a wrong key does not fail, it just quietly
// matches nothing. Every rule below is the Python one.
//
// Rules that look arbitrary and are not:
//   - Folding is NFKD plus removal of the LATIN combining ranges only.
//     Stripping every Mn would turn Devanagari and Arabic titles into rubble
//     rather than folding an accent off them.
//   - Lowercasing is locale-independent. A Turkish locale maps I to a dotless
//     i and changes the key for every title with a capital I in it.
//   - The artist separators are whole words. "Malcolm X" loses the X, "John
//     Featherstone" keeps all of itself, and "ft2 anthem" stays a title.
namespace Rec {

// "Blinding Lights (Remastered 2020)" -> "blinding lights". Parentheticals go
// unless they name a different recording ("(Live)"), a " - " suffix goes when
// it is release noise, and everything from a "feat" / "ft" / "featuring" word
// onward goes.
QString titleCore(const QString &title);

// "Dua Lipa feat. DaBaby" -> "dua lipa". The lead credit only, so that a
// collaboration and the lead's own songs reach the same catalogue rows.
QString primaryArtist(const QString &artist);

// Whether two artist names plausibly name the same act. Used by the title-only
// match tier, where the artist is the only evidence left, so it errs towards
// missing a match rather than inventing one. Two empty names do NOT agree.
bool artistsAgree(const QString &a, const QString &b);

// FNV-1a, 64-bit, over raw bytes: offset basis 0xcbf29ce484222325, prime
// 0x100000001b3.
quint64 fnv1a64(const QByteArray &bytes);

// K1, the strict key the catalogue is indexed on: titleCore + "|" +
// primaryArtist, hashed as UTF-8.
quint64 strictKey(const QString &title, const QString &artist);

// K2, the title-only key. The catalogue stores these only for titles unique
// across the whole of it, so a hit means something.
quint64 titleKey(const QString &title);

// Not part of the catalogue's keys, and not from the Python: a whole name
// reduced to what two spellings of it share, for comparing one artist's name
// across the catalogue, the graph shards and the listener's history, which
// each spell it their own way.
//
// Case, punctuation and accents go, so "Beyoncé" and "BEYONCE", and "Guns N'
// Roses" with a curly apostrophe and a straight one, agree. Unlike
// primaryArtist it keeps the WHOLE name: "Belle and Sebastian" is not "belle",
// and "AC/DC" is "ac dc", not "ac". Accents come off Latin, Greek and Cyrillic
// letters only — in Devanagari or Thai a mark is part of the letter — and the
// work is done in code points, so a character outside the basic plane is one
// character, not two halves.
QString plainName(const QString &name);

} // namespace Rec
