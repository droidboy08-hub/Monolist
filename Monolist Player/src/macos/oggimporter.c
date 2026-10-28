/*
 * Monolist's Spotlight importer for Ogg Opus and Vorbis files (.opus, .ogg,
 * .oga): their tags (oggtags.c) as Spotlight's attributes, the ones Apple's
 * own importer sets for an M4A, so that the Finder's Get Info shows the
 * title, the artists, the album and the length, and Spotlight finds the
 * song by them. macOS has no importer of its own for Ogg: it imports these
 * files "with no plugIn", from their names alone.
 *
 * A Spotlight importer bundle (Contents/Library/Spotlight/
 * MonolistOgg.mdimporter), which Spotlight finds when macOS registers the
 * app and loads into its own worker, whether Monolist runs or not. A Core
 * Spotlight import extension (CSImportExtension) was tried first: on macOS
 * 26 neither indexing nor `mdimport -t` ever called it for these files.
 *
 * Plain C, CoreFoundation's plug-in model: a factory that makes an
 * importer, which answers for the MDImporter interface.
 */

#include <CoreFoundation/CoreFoundation.h>
#include <CoreFoundation/CFPlugInCOM.h>
#include <CoreServices/CoreServices.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "oggtags.h"

/* The factory's id, as the bundle's Info.plist names it (CFPlugInFactories). */
#define FACTORY_ID CFUUIDGetConstantUUIDWithBytes(kCFAllocatorDefault, \
    0xD8, 0x46, 0x4D, 0xC8, 0xF3, 0xD9, 0x4E, 0x35, 0xBB, 0x1C, 0x59, 0x53, 0x80, 0xC5, 0xC9, 0xFD)

typedef struct Importer {
    MDImporterInterfaceStruct *interface;
    CFUUIDRef factory;
    UInt32 references;
} Importer;

/* The index-th value of a comment, trimmed, or NULL (none, empty, or not
 * UTF-8). The caller releases it. */
static CFStringRef copy_value(const OggTags *tags, const char *name, size_t index)
{
    const char *text = ogg_tags_value(tags, name, index);
    if (!text)
        return NULL;
    CFStringRef value = CFStringCreateWithCString(kCFAllocatorDefault, text, kCFStringEncodingUTF8);
    if (!value)
        return NULL;
    CFMutableStringRef trimmed = CFStringCreateMutableCopy(kCFAllocatorDefault, 0, value);
    CFRelease(value);
    if (!trimmed)
        return NULL;
    CFStringTrimWhitespace(trimmed);
    if (CFStringGetLength(trimmed) == 0) {
        CFRelease(trimmed);
        return NULL;
    }
    return trimmed;
}

/* Sets key to the first value of the comment called name, if it has one. */
static void set_text(CFMutableDictionaryRef attributes, CFStringRef key, const OggTags *tags, const char *name)
{
    CFStringRef value = copy_value(tags, name, 0);
    if (value) {
        CFDictionarySetValue(attributes, key, value);
        CFRelease(value);
    }
}

static void set_number(CFMutableDictionaryRef attributes, CFStringRef key, CFNumberType type, const void *value)
{
    CFNumberRef number = CFNumberCreate(kCFAllocatorDefault, type, value);
    if (number) {
        CFDictionarySetValue(attributes, key, number);
        CFRelease(number);
    }
}

/* Every value of a comment that may come more than once (one ARTIST each),
 * without repeats; NULL when there is none. */
static CFArrayRef copy_values(const OggTags *tags, const char *name)
{
    CFMutableArrayRef all = CFArrayCreateMutable(kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks);
    if (!all)
        return NULL;
    for (size_t i = 0; ogg_tags_value(tags, name, i); i++) {
        CFStringRef value = copy_value(tags, name, i);
        if (!value)
            continue;
        if (!CFArrayContainsValue(all, CFRangeMake(0, CFArrayGetCount(all)), value))
            CFArrayAppendValue(all, value);
        CFRelease(value);
    }
    if (CFArrayGetCount(all) == 0) {
        CFRelease(all);
        return NULL;
    }
    return all;
}

/* The number a comment starts with ("3" or "3/12" for TRACKNUMBER, "2015",
 * "2015-11-11" or yt-dlp's "20151111" for DATE, of which the first four
 * digits), or 0. */
static long leading_number(const OggTags *tags, const char *name, int digits)
{
    const char *text = ogg_tags_value(tags, name, 0);
    if (!text)
        return 0;
    while (*text == ' ')
        text++;
    long number = 0;
    int count = 0;
    while (*text >= '0' && *text <= '9' && (digits == 0 || count < digits)) {
        number = number * 10 + (*text++ - '0');
        if (++count > 9)
            return 0;
    }
    return digits && count != digits ? 0 : number;
}

static Boolean import_file(void *self, CFMutableDictionaryRef attributes, CFStringRef type, CFStringRef path)
{
    (void)self;
    (void)type;
    char file[PATH_MAX];
    if (!CFStringGetFileSystemRepresentation(path, file, sizeof file))
        return false;
    OggTags tags;
    if (ogg_tags_read(file, OGG_TAGS_DURATION, &tags) != 0) {
        ogg_tags_free(&tags);
        return false;
    }

    /* The title is also the name Spotlight shows, as for an M4A. */
    CFStringRef title = copy_value(&tags, "TITLE", 0);
    if (title) {
        CFDictionarySetValue(attributes, kMDItemTitle, title);
        CFDictionarySetValue(attributes, kMDItemDisplayName, title);
        CFRelease(title);
    }
    CFArrayRef artists = copy_values(&tags, "ARTIST");
    if (!artists)
        artists = copy_values(&tags, "ALBUMARTIST");
    if (!artists)
        artists = copy_values(&tags, "PERFORMER");
    if (artists) {
        CFDictionarySetValue(attributes, kMDItemAuthors, artists);
        CFRelease(artists);
    }
    set_text(attributes, kMDItemAlbum, &tags, "ALBUM");
    set_text(attributes, kMDItemMusicalGenre, &tags, "GENRE");
    set_text(attributes, kMDItemComposer, &tags, "COMPOSER");
    set_text(attributes, kMDItemLyricist, &tags, "LYRICIST");
    set_text(attributes, kMDItemCopyright, &tags, "COPYRIGHT");
    set_text(attributes, kMDItemAudioEncodingApplication, &tags, "ENCODER");
    int track = (int)leading_number(&tags, "TRACKNUMBER", 0);
    if (track > 0)
        set_number(attributes, kMDItemAudioTrackNumber, kCFNumberIntType, &track);
    int year = (int)leading_number(&tags, "DATE", 4);
    if (year >= 1000)
        set_number(attributes, kMDItemRecordingYear, kCFNumberIntType, &year);

    CFStringRef sound = CFSTR("Sound");
    CFArrayRef media = CFArrayCreate(kCFAllocatorDefault, (const void **)&sound, 1, &kCFTypeArrayCallBacks);
    if (media) {
        CFDictionarySetValue(attributes, kMDItemMediaTypes, media);
        CFRelease(media);
    }
    CFStringRef codec = tags.codec == OGG_TAGS_OPUS ? CFSTR("Opus") : CFSTR("Vorbis");
    CFArrayRef codecs = CFArrayCreate(kCFAllocatorDefault, (const void **)&codec, 1, &kCFTypeArrayCallBacks);
    if (codecs) {
        CFDictionarySetValue(attributes, kMDItemCodecs, codecs);
        CFRelease(codecs);
    }
    int channels = (int)tags.channels;
    if (channels > 0)
        set_number(attributes, kMDItemAudioChannelCount, kCFNumberIntType, &channels);
    /* Opus always plays at 48 kHz, whatever rate its source had. */
    double rate = tags.codec == OGG_TAGS_OPUS ? 48000 : tags.sample_rate;
    if (rate > 0)
        set_number(attributes, kMDItemAudioSampleRate, kCFNumberDoubleType, &rate);
    if (tags.duration > 0) {
        set_number(attributes, kMDItemDurationSeconds, kCFNumberDoubleType, &tags.duration);
        /* The audio's own rate: the cover and the tags left out. */
        long long audio = tags.file_size - tags.header_size;
        if (audio > 0) {
            long long bits = llround((double)audio * 8.0 / tags.duration);
            set_number(attributes, kMDItemAudioBitRate, kCFNumberLongLongType, &bits);
            set_number(attributes, kMDItemTotalBitRate, kCFNumberLongLongType, &bits);
        }
    }

    ogg_tags_free(&tags);
    return true;
}

static ULONG add_reference(void *self)
{
    return ++((Importer *)self)->references;
}

static ULONG release(void *self)
{
    Importer *importer = self;
    if (--importer->references > 0)
        return importer->references;
    CFPlugInRemoveInstanceForFactory(importer->factory);
    CFRelease(importer->factory);
    free(importer);
    return 0;
}

static HRESULT query_interface(void *self, REFIID iid, LPVOID *out)
{
    CFUUIDRef wanted = CFUUIDCreateFromUUIDBytes(kCFAllocatorDefault, iid);
    Boolean ours = wanted && (CFEqual(wanted, kMDImporterInterfaceID) || CFEqual(wanted, IUnknownUUID));
    if (wanted)
        CFRelease(wanted);
    if (!ours) {
        *out = NULL;
        return E_NOINTERFACE;
    }
    add_reference(self);
    *out = self;
    return S_OK;
}

static MDImporterInterfaceStruct interface = {
    NULL, query_interface, add_reference, release, import_file
};

/* Named by the bundle's Info.plist (CFPlugInFactories). */
void *MonolistOggImporterFactory(CFAllocatorRef allocator, CFUUIDRef type);

void *MonolistOggImporterFactory(CFAllocatorRef allocator, CFUUIDRef type)
{
    (void)allocator;
    if (!CFEqual(type, kMDImporterTypeID))
        return NULL;
    Importer *importer = malloc(sizeof *importer);
    if (!importer)
        return NULL;
    importer->interface = &interface;
    importer->factory = CFRetain(FACTORY_ID);
    importer->references = 1;
    CFPlugInAddInstanceForFactory(importer->factory);
    return importer;
}
