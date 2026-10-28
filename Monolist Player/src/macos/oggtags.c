#include "oggtags.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>

/* The largest comment header read into memory: far more than any cover
 * (a 720 px JPEG is about 100 KB, 140 KB in base64), and a bound on what a
 * file claiming more can make the extensions allocate. */
#define MAX_PACKET (64u * 1024 * 1024)
/* An Ogg page: a 27-byte header, up to 255 lacing values, up to 255 x 255
 * bytes of body. */
#define MAX_PAGE (27 + 255 + 255 * 255)
/* Bytes that are not a page, between pages, before the reading stops. */
#define MAX_SKIP (1024 * 1024)
/* The last page is looked for in the file's tail: 128 KB, then 512 KB and so
 * on up to this (a chained file's first stream ends far from the end). */
#define MAX_TAIL (16 * 1024 * 1024)

typedef struct Page {
    unsigned flags;                 /* 1 continued, 2 first, 4 last */
    int64_t granule;
    uint32_t serial;
    unsigned segment_count;
    const unsigned char *segments;  /* the lacing values */
    const unsigned char *body;
} Page;

typedef struct Reader {
    FILE *file;
    uint32_t crc[256];
    unsigned char page[MAX_PAGE];
} Reader;

typedef struct Stream {
    uint32_t serial;
    int codec;
    unsigned pre_skip;              /* Opus */
} Stream;

static uint32_t le32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint32_t be32(const unsigned char *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | (uint32_t)p[3];
}

/* Ogg's CRC-32: polynomial 0x04c11db7, not reflected, starting at 0. */
static void crc_init(uint32_t table[256])
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t r = i << 24;
        for (int k = 0; k < 8; k++)
            r = (r & 0x80000000u) ? (r << 1) ^ 0x04c11db7u : r << 1;
        table[i] = r;
    }
}

static uint32_t crc_add(const uint32_t table[256], uint32_t crc, const unsigned char *p, size_t n)
{
    while (n--)
        crc = (crc << 8) ^ table[((crc >> 24) ^ *p++) & 0xff];
    return crc;
}

/* The page at p, if one starts there, fits in avail bytes and its checksum
 * is right: its length, and *page filled in. 0 otherwise. */
static size_t parse_page(const uint32_t crc[256], const unsigned char *p, size_t avail, Page *page)
{
    if (avail < 27 || memcmp(p, "OggS", 4) != 0 || p[4] != 0)
        return 0;
    size_t segments = p[26];
    if (avail < 27 + segments)
        return 0;
    size_t body = 0;
    for (size_t i = 0; i < segments; i++)
        body += p[27 + i];
    size_t size = 27 + segments + body;
    if (avail < size)
        return 0;
    static const unsigned char zero[4] = { 0, 0, 0, 0 };
    uint32_t sum = crc_add(crc, 0, p, 22);
    sum = crc_add(crc, sum, zero, 4);
    sum = crc_add(crc, sum, p + 26, size - 26);
    if (sum != le32(p + 22))
        return 0;
    page->flags = p[5];
    page->granule = (int64_t)((uint64_t)le32(p + 6) | (uint64_t)le32(p + 10) << 32);
    page->serial = le32(p + 14);
    page->segment_count = (unsigned)segments;
    page->segments = p + 27;
    page->body = p + 27 + segments;
    return size;
}

/* The next page in the file, read into r->page. 1 when there is one; 0 at
 * the end, or after MAX_SKIP bytes that are not one. */
static int next_page(Reader *r, Page *page)
{
    size_t skipped = 0;
    for (;;) {
        off_t start = ftello(r->file);
        unsigned char *p = r->page;
        if (start < 0 || fread(p, 1, 27, r->file) != 27)
            return 0;
        if (memcmp(p, "OggS", 4) == 0) {
            size_t segments = p[26], body = 0;
            if (fread(p + 27, 1, segments, r->file) == segments) {
                for (size_t i = 0; i < segments; i++)
                    body += p[27 + i];
                if (fread(p + 27 + segments, 1, body, r->file) == body
                    && parse_page(r->crc, p, 27 + segments + body, page))
                    return 1;
            }
        }
        /* Not a page here: the next "OggS", from a byte on. */
        if (fseeko(r->file, start + 1, SEEK_SET) != 0)
            return 0;
        uint32_t window = 0;
        for (;;) {
            int c = getc(r->file);
            if (c == EOF || ++skipped > MAX_SKIP)
                return 0;
            window = window << 8 | (uint32_t)c;
            if (window == 0x4f676753u) {   /* "OggS" */
                if (fseeko(r->file, -4, SEEK_CUR) != 0)
                    return 0;
                break;
            }
        }
    }
}

static int name_is(const char *name, size_t length, const char *wanted)
{
    return strlen(wanted) == length && strncasecmp(name, wanted, length) == 0;
}

static char *copy_text(const char *text, size_t length)
{
    char *copy = malloc(length + 1);
    if (copy) {
        memcpy(copy, text, length);
        copy[length] = '\0';
    }
    return copy;
}

/* Standard base64, padded or not, into a new buffer; NULL if it is not. */
static unsigned char *base64_decode(const char *text, size_t length, size_t *out_size)
{
    unsigned char *out = malloc(length / 4 * 3 + 3);
    if (!out)
        return NULL;
    size_t n = 0;
    uint32_t bits = 0;
    int count = 0;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)text[i];
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+') v = 62;
        else if (c == '/') v = 63;
        else if (c == '=') break;
        else if (c == '\r' || c == '\n' || c == ' ' || c == '\t') continue;
        else { free(out); return NULL; }
        bits = bits << 6 | (uint32_t)v;
        if (++count == 4) {
            out[n++] = (unsigned char)(bits >> 16);
            out[n++] = (unsigned char)(bits >> 8);
            out[n++] = (unsigned char)bits;
            bits = 0;
            count = 0;
        }
    }
    if (count == 2) {
        out[n++] = (unsigned char)(bits >> 4);
    } else if (count == 3) {
        out[n++] = (unsigned char)(bits >> 10);
        out[n++] = (unsigned char)(bits >> 2);
    } else if (count == 1) {
        free(out);
        return NULL;
    }
    *out_size = n;
    return out;
}

/* How much a picture of this FLAC type is the cover: the front cover first,
 * then "other" (what a tagger writes when it does not say), anything else,
 * the 32 px file icons last. An old COVERART comment, which has no type,
 * ranks under all of them. */
static int picture_rank(uint32_t type)
{
    switch (type) {
    case 3: return 4;
    case 0: return 3;
    case 1: case 2: return 1;
    default: return 2;
    }
}

static void keep_picture(OggTags *tags, int *best, int rank,
                         const unsigned char *data, size_t size, const char *mime, size_t mime_length)
{
    if (rank <= *best || size == 0)
        return;
    unsigned char *copy = malloc(size);
    char *mime_copy = copy_text(mime, mime_length);
    if (!copy || !mime_copy) {
        free(copy);
        free(mime_copy);
        return;
    }
    memcpy(copy, data, size);
    free(tags->picture);
    free(tags->picture_mime);
    tags->picture = copy;
    tags->picture_size = size;
    tags->picture_mime = mime_copy;
    *best = rank;
}

/* METADATA_BLOCK_PICTURE's value: FLAC's picture block (all big-endian:
 * type, MIME type, description, width, height, depth, colours, the image),
 * in base64. */
static void read_picture_block(OggTags *tags, int *best, const char *value, size_t length)
{
    size_t size = 0;
    unsigned char *block = base64_decode(value, length, &size);
    if (!block)
        return;
    size_t at = 0;
    if (size - at < 8)
        goto done;
    uint32_t type = be32(block + at);
    uint32_t mime_length = be32(block + at + 4);
    at += 8;
    if (mime_length > size - at)
        goto done;
    const char *mime = (const char *)block + at;
    at += mime_length;
    if (size - at < 4)
        goto done;
    uint32_t description_length = be32(block + at);
    at += 4;
    if (description_length > size - at)
        goto done;
    at += description_length;
    if (size - at < 20)
        goto done;
    at += 16;   /* width, height, depth, colours: the image says them itself */
    uint32_t data_length = be32(block + at);
    at += 4;
    if (data_length > size - at)
        goto done;
    /* "-->" is a link to the picture, not the picture. */
    if (!(mime_length == 3 && memcmp(mime, "-->", 3) == 0))
        keep_picture(tags, best, picture_rank(type), block + at, data_length, mime, mime_length);
done:
    free(block);
}

static int add_comment(OggTags *tags, size_t *capacity, const char *name, size_t name_length,
                       const char *value, size_t value_length)
{
    if (tags->comment_count == *capacity) {
        size_t grown = *capacity ? *capacity * 2 : 16;
        OggTagsComment *comments = realloc(tags->comments, grown * sizeof *comments);
        if (!comments)
            return -1;
        tags->comments = comments;
        *capacity = grown;
    }
    char *n = copy_text(name, name_length);
    char *v = copy_text(value, value_length);
    if (!n || !v) {
        free(n);
        free(v);
        return -1;
    }
    tags->comments[tags->comment_count].name = n;
    tags->comments[tags->comment_count].value = v;
    tags->comment_count++;
    return 0;
}

/* The comment header after its magic: the vendor, then the comments, each
 * a little-endian length and NAME=value. A list cut short keeps what came
 * before the cut. */
static int read_comments(OggTags *tags, unsigned flags, const unsigned char *p, size_t n)
{
    size_t at = 0, capacity = 0;
    int best = -1;
    if (n < 4)
        return -1;
    uint32_t vendor = le32(p);
    at = 4;
    if (vendor > n - at || n - at - vendor < 4)
        return -1;
    at += vendor;
    uint32_t count = le32(p + at);
    at += 4;
    for (uint32_t k = 0; k < count && n - at >= 4; k++) {
        uint32_t length = le32(p + at);
        at += 4;
        if (length > n - at)
            break;
        const char *comment = (const char *)p + at;
        at += length;
        const char *equals = memchr(comment, '=', length);
        if (!equals || equals == comment)
            continue;
        size_t name_length = (size_t)(equals - comment);
        const char *value = equals + 1;
        size_t value_length = length - name_length - 1;
        if (name_is(comment, name_length, "METADATA_BLOCK_PICTURE")) {
            if (flags & OGG_TAGS_PICTURE)
                read_picture_block(tags, &best, value, value_length);
            continue;
        }
        if (name_is(comment, name_length, "COVERART")) {
            if (flags & OGG_TAGS_PICTURE) {
                size_t size = 0;
                unsigned char *image = base64_decode(value, value_length, &size);
                if (image)
                    keep_picture(tags, &best, 0, image, size, "", 0);
                free(image);
            }
            continue;
        }
        if (add_comment(tags, &capacity, comment, name_length, value, value_length) != 0)
            return -1;
    }
    return 0;
}

/* A stream's first packet, alone on its first page: Opus's or Vorbis's
 * identification header, or something else (0). */
static int read_identification(const unsigned char *p, size_t n, OggTags *tags, Stream *stream)
{
    if (n >= 19 && memcmp(p, "OpusHead", 8) == 0 && (p[8] >> 4) == 0 && p[9] > 0) {
        stream->codec = OGG_TAGS_OPUS;
        stream->pre_skip = (unsigned)p[10] | (unsigned)p[11] << 8;
        tags->channels = p[9];
        tags->sample_rate = le32(p + 12);
        return 1;
    }
    if (n >= 30 && p[0] == 1 && memcmp(p + 1, "vorbis", 6) == 0 && le32(p + 7) == 0 && p[11] > 0
        && le32(p + 12) > 0) {
        stream->codec = OGG_TAGS_VORBIS;
        tags->channels = p[11];
        tags->sample_rate = le32(p + 12);
        return 1;
    }
    return 0;
}

/* The first Opus or Vorbis stream's identification and comment headers. */
static int read_headers(Reader *r, unsigned flags, OggTags *tags, Stream *stream)
{
    Page page;
    int found = 0;
    unsigned char *packet = NULL;
    size_t size = 0, capacity = 0;
    int skipping = 0;   /* the rest of a packet begun before what was read */
    int result = -1;

    while (next_page(r, &page)) {
        if (page.flags & 2) {
            /* All the streams' first pages come before any other page. */
            if (found)
                continue;
            size_t length = 0;
            unsigned i = 0;
            while (i < page.segment_count && page.segments[i] == 255)
                length += page.segments[i++];
            if (i == page.segment_count)
                continue;   /* not a whole packet: neither Opus nor Vorbis */
            length += page.segments[i];
            if (read_identification(page.body, length, tags, stream)) {
                found = 1;
                stream->serial = page.serial;
            }
            continue;
        }
        if (!found)
            break;
        if (page.serial != stream->serial)
            continue;
        if (!(page.flags & 1) && size > 0)
            break;          /* the header was cut off */
        if ((page.flags & 1) && size == 0)
            skipping = 1;
        const unsigned char *body = page.body;
        for (unsigned i = 0; i < page.segment_count; i++) {
            unsigned lacing = page.segments[i];
            if (!skipping && lacing > 0) {
                if (size + lacing > MAX_PACKET)
                    goto done;
                if (size + lacing > capacity) {
                    size_t grown = capacity ? capacity * 2 : 65536;
                    while (grown < size + lacing)
                        grown *= 2;
                    unsigned char *bigger = realloc(packet, grown);
                    if (!bigger)
                        goto done;
                    packet = bigger;
                    capacity = grown;
                }
                memcpy(packet + size, body, lacing);
                size += lacing;
            }
            body += lacing;
            if (lacing < 255) {
                if (skipping) {
                    skipping = 0;
                    continue;
                }
                /* The second packet: the comment header. */
                tags->header_size = (long long)ftello(r->file);
                if (stream->codec == OGG_TAGS_OPUS && size >= 8 && memcmp(packet, "OpusTags", 8) == 0)
                    result = read_comments(tags, flags, packet + 8, size - 8);
                else if (stream->codec == OGG_TAGS_VORBIS && size >= 7 && packet[0] == 3
                         && memcmp(packet + 1, "vorbis", 6) == 0)
                    result = read_comments(tags, flags, packet + 7, size - 7);
                goto done;
            }
        }
    }
done:
    free(packet);
    if (found && result == 0)
        tags->codec = stream->codec;
    return result;
}

/* The length: the granule position of the stream's last page, in the
 * file's tail. Opus counts at 48 kHz, after its pre-skip. */
static double read_duration(Reader *r, const OggTags *tags, const Stream *stream)
{
    long long file_size = tags->file_size;
    size_t window = 128 * 1024;
    unsigned char *tail = NULL;
    int64_t granule = -1;
    while (file_size >= 27) {
        if ((long long)window > file_size)
            window = (size_t)file_size;
        unsigned char *bigger = realloc(tail, window);
        if (!bigger)
            break;
        tail = bigger;
        if (fseeko(r->file, (off_t)(file_size - (long long)window), SEEK_SET) != 0
            || fread(tail, 1, window, r->file) != window)
            break;
        for (size_t at = window - 27 + 1; at-- > 0;) {
            Page page;
            if (tail[at] == 'O' && parse_page(r->crc, tail + at, window - at, &page)
                && page.serial == stream->serial && page.granule != -1) {
                granule = page.granule;
                break;
            }
        }
        if (granule != -1 || (long long)window == file_size || window >= MAX_TAIL)
            break;
        window *= 4;
    }
    free(tail);
    if (granule <= 0)
        return 0;
    if (stream->codec == OGG_TAGS_OPUS)
        return granule > (int64_t)stream->pre_skip ? (double)(granule - stream->pre_skip) / 48000.0 : 0;
    return tags->sample_rate ? (double)granule / tags->sample_rate : 0;
}

int ogg_tags_read(const char *path, unsigned flags, OggTags *tags)
{
    memset(tags, 0, sizeof *tags);
    Reader *r = calloc(1, sizeof *r);
    if (!r)
        return -1;
    crc_init(r->crc);
    r->file = fopen(path, "rb");
    int result = -1;
    if (r->file) {
        if (fseeko(r->file, 0, SEEK_END) == 0) {
            tags->file_size = ftello(r->file);
            rewind(r->file);
        }
        Stream stream = { 0, 0, 0 };
        result = read_headers(r, flags, tags, &stream);
        if (result == 0 && (flags & OGG_TAGS_DURATION))
            tags->duration = read_duration(r, tags, &stream);
        fclose(r->file);
    }
    free(r);
    return result;
}

const char *ogg_tags_value(const OggTags *tags, const char *name, size_t index)
{
    for (size_t i = 0; i < tags->comment_count; i++) {
        if (strcasecmp(tags->comments[i].name, name) == 0 && index-- == 0)
            return tags->comments[i].value;
    }
    return NULL;
}

void ogg_tags_free(OggTags *tags)
{
    for (size_t i = 0; i < tags->comment_count; i++) {
        free(tags->comments[i].name);
        free(tags->comments[i].value);
    }
    free(tags->comments);
    free(tags->picture);
    free(tags->picture_mime);
    memset(tags, 0, sizeof *tags);
}
