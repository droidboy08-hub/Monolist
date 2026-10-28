#pragma once

class QString;

// Opus and Ogg files (.opus, .ogg, .oga) in the Finder, which reads no tags
// or cover from Ogg. Their tags come from Spotlight's importer inside the
// app (Contents/Library/Spotlight/MonolistOgg.mdimporter, oggimporter.c).
// Their cover cannot come from an app extension: Quick Look gives every
// audio file to macOS's own thumbnailer (on macOS 26 an app's thumbnail
// extension for org.xiph.ogg-audio is never asked), which draws a blank
// note for these. So the app gives each Ogg download the cover it carries
// as its own icon, which the Finder draws in every view, as it does an
// M4A's cover. It is kept in the file's resource fork, beside the audio,
// which stays exactly as it was. Mac only (finder.mm).
namespace MacFinder {

// Gives the Ogg file at path the cover it carries as its icon, 256 px
// (about 200 KB beside the song). False when it is not Ogg, carries no
// cover, or the icon could not be set; the file is then left as it was.
bool setCover(const QString &path);

// Whether the file at path has an icon of its own.
bool hasCustomIcon(const QString &path);

// The Spotlight importer inside this app, as "<its build> <its path>", or
// empty when there is none (a build that left it out).
QString spotlightImporter();

// Tells Spotlight about that importer (`mdimport -r`), which also has it read
// every Ogg file on the Mac again: songs it saw before the importer came
// show their tags too. Until told, Spotlight may go on importing Ogg "with
// no plugIn". Started, not waited for.
void announceSpotlightImporter();

} // namespace MacFinder
