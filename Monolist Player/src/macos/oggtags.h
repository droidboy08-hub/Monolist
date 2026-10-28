#pragma once

/*
 * The tags, the cover and the length of an Ogg Opus or Ogg Vorbis file
 * (.opus, .ogg, .oga), which macOS has no reader for: Spotlight imports them
 * with no plug-in, so the Finder shows no title, artist or album, and Quick
 * Look no cover. Read here for the Mac's two extensions inside Monolist.app,
 * the thumbnails (oggthumbnails.m) and the Spotlight importer
 * (oggspotlight.m). Plain C, no dependencies.
 *
 * What is read: the first Opus or Vorbis stream in the file (others beside
 * it, a video's, are passed over), its identification header (channels,
 * rate, Opus's pre-skip), its comment header (OpusTags, or Vorbis's), the
 * cover among its comments (METADATA_BLOCK_PICTURE, FLAC's picture block in
 * base64, as mutagen and FFmpeg write it; the old COVERART too), and the
 * length from the stream's last page. Every page's checksum is checked, and
 * every length against what is left, so a damaged or hostile file is
 * refused rather than read past its end.
 */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum OggTagsCodec { OGG_TAGS_OPUS = 1, OGG_TAGS_VORBIS = 2 };

enum OggTagsFlags {
    OGG_TAGS_PICTURE = 1,   /* decode the cover too (Quick Look) */
    OGG_TAGS_DURATION = 2   /* read the last page for the length (Spotlight) */
};

typedef struct OggTagsComment {
    char *name;    /* as written; compared without case, as Vorbis says */
    char *value;   /* UTF-8, as written (not checked) */
} OggTagsComment;

typedef struct OggTags {
    int codec;                    /* OggTagsCodec */
    unsigned channels;
    unsigned sample_rate;         /* Opus: the input's rate; it plays at 48 kHz */
    double duration;              /* seconds; 0 when not asked for or unknown */
    long long file_size;
    long long header_size;        /* up to the end of the comment header's last page */

    OggTagsComment *comments;     /* every comment but the pictures */
    size_t comment_count;

    unsigned char *picture;       /* the cover's image file (JPEG, PNG), or NULL */
    size_t picture_size;
    char *picture_mime;           /* as the file says, possibly empty */
} OggTags;

/* Reads the file at path into tags (zeroed first). Returns 0 when it is an
 * Ogg Opus or Vorbis file whose headers were read, -1 otherwise; tags must be
 * freed either way. */
int ogg_tags_read(const char *path, unsigned flags, OggTags *tags);

/* The index-th value (from 0) of the comment called name (ARTIST, TITLE),
 * any case, or NULL. A name may appear more than once: one ARTIST each. */
const char *ogg_tags_value(const OggTags *tags, const char *name, size_t index);

void ogg_tags_free(OggTags *tags);

#ifdef __cplusplus
}
#endif
