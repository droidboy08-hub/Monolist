#pragma once

// The Ogg reader (oggtags.c) on files the test writes itself, page by page:
// Opus and Vorbis, a comment header over several pages, the front cover
// chosen over an icon, another stream beside the audio, damaged and cut
// files, and lengths that point past the end. Then the app's own Spotlight
// importer (Contents/Library/Spotlight/MonolistOgg.mdimporter) loaded as
// Spotlight loads it, and the attributes it gives; and a cover set as a
// file's Finder icon (finder.mm), the audio left as it was. In a folder
// inside MONOLIST_DATA_DIR, which it insists on. Mac only. Prints a line per
// check; returns how many failed.
int runOggSelfTest();
