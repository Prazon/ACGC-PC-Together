/* pc_mod_assets.c - see pc_mod_assets.h */

#include "pc_mod_assets.h"

#include <string.h>

#define TAG_VTX 0x20585456u   /* 'VTX ' */
#define TAG_TEX 0x20584554u   /* 'TEX ' */
#define TAG_PAL 0x204C4150u   /* 'PAL ' */
#define TAG_MSH 0x2048534Du   /* 'MSH ' */
#define TAG_META 0x4154454Du  /* 'META' */

/* A bounds-checked cursor. Every read goes through it, so "did we check the
 * length first" is answered once here rather than at each call site. */
typedef struct {
    const uint8_t* data;
    size_t size;
    size_t offset;
} Cursor;

static int take(Cursor* c, size_t count, const uint8_t** out) {
    if (count > c->size - c->offset) return 0;   /* no overflow: offset <= size */
    if (out) *out = c->data + c->offset;
    c->offset += count;
    return 1;
}

static int read_u16(Cursor* c, uint16_t* out) {
    const uint8_t* p;
    if (!take(c, 2, &p)) return 0;
    *out = (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
    return 1;
}

static int read_u32(Cursor* c, uint32_t* out) {
    const uint8_t* p;
    if (!take(c, 4, &p)) return 0;
    *out = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    return 1;
}

static int read_f32(Cursor* c, float* out) {
    uint32_t bits;
    float value;
    if (!read_u32(c, &bits)) return 0;
    memcpy(&value, &bits, sizeof(value));
    /* Reject NaN and infinity: these reach transform maths, where a non-finite
     * value propagates into geometry that is very hard to trace back here. */
    if (value != value) return 0;
    if (value > 3.0e38f || value < -3.0e38f) return 0;
    *out = value;
    return 1;
}

static void fail(char* error, size_t error_len, const char* reason) {
    if (error && error_len > 0) {
        size_t n = strlen(reason);
        if (n >= error_len) n = error_len - 1;
        memcpy(error, reason, n);
        error[n] = '\0';
    }
}

uint32_t pcasset_texture_size(uint16_t width, uint16_t height, uint8_t format) {
    uint32_t pixels;
    if (width == 0 || height == 0) return 0;
    if (width > PCASSET_MAX_TEXTURE_DIM || height > PCASSET_MAX_TEXTURE_DIM) return 0;
    /* Power of two only: the GC texture path and every mip assumption below it
     * expect it, and a packer that cannot honour it should fail offline. */
    if ((width & (width - 1)) != 0 || (height & (height - 1)) != 0) return 0;

    pixels = (uint32_t)width * (uint32_t)height;   /* <= 1024*1024, no overflow */
    switch (format) {
        case PCASSET_FMT_I4:
        case PCASSET_FMT_CI4:
        case PCASSET_FMT_CMPR:
            return pixels / 2u;
        case PCASSET_FMT_I8:
        case PCASSET_FMT_IA4:
        case PCASSET_FMT_CI8:
            return pixels;
        case PCASSET_FMT_IA8:
        case PCASSET_FMT_RGB565:
        case PCASSET_FMT_RGB5A3:
            return pixels * 2u;
        case PCASSET_FMT_RGBA8:
            return pixels * 4u;
        default:
            return 0;
    }
}

int pcasset_parse(const uint8_t* data, size_t size, PCAssetModel* out, char* error, size_t error_len) {
    Cursor cursor;
    uint32_t magic = 0;
    uint16_t version = 0;
    uint16_t chunk_count = 0;
    uint16_t i;
    int seen_vtx = 0;
    int seen_tex = 0;
    int seen_pal = 0;
    int seen_msh = 0;

    if (!data || !out) { fail(error, error_len, "no data"); return 0; }
    memset(out, 0, sizeof(*out));
    out->suggested_scale = 1.0f;

    if (size < 8) { fail(error, error_len, "shorter than a header"); return 0; }
    if (size > PCASSET_MAX_FILE_BYTES) { fail(error, error_len, "larger than the 4 MB cap"); return 0; }

    cursor.data = data;
    cursor.size = size;
    cursor.offset = 0;

    if (!read_u32(&cursor, &magic) || magic != PCASSET_MAGIC) {
        fail(error, error_len, "not a .pcasset file");
        return 0;
    }
    if (!read_u16(&cursor, &version) || version != PCASSET_VERSION) {
        fail(error, error_len, "unsupported .pcasset version");
        return 0;
    }
    if (!read_u16(&cursor, &chunk_count) || chunk_count > PCASSET_MAX_CHUNKS) {
        fail(error, error_len, "too many chunks");
        return 0;
    }

    for (i = 0; i < chunk_count; i++) {
        uint32_t tag = 0;
        uint32_t length = 0;
        const uint8_t* body = NULL;
        Cursor chunk;

        if (!read_u32(&cursor, &tag) || !read_u32(&cursor, &length)) {
            fail(error, error_len, "truncated chunk header");
            return 0;
        }
        /* The declared length is checked against what is actually left before
         * it is used for anything. */
        if (!take(&cursor, length, &body)) {
            fail(error, error_len, "chunk runs past the end of the file");
            return 0;
        }

        chunk.data = body;
        chunk.size = length;
        chunk.offset = 0;

        switch (tag) {
            case TAG_VTX: {
                uint32_t count = 0;
                const uint8_t* payload = NULL;
                if (seen_vtx) { fail(error, error_len, "duplicate VTX chunk"); return 0; }
                seen_vtx = 1;
                if (!read_u32(&chunk, &count) || count == 0 || count > PCASSET_MAX_VERTICES) {
                    fail(error, error_len, "bad vertex count");
                    return 0;
                }
                if (!take(&chunk, (size_t)count * 16u, &payload)) {
                    fail(error, error_len, "VTX chunk is shorter than its count claims");
                    return 0;
                }
                out->vertices = payload;
                out->vertex_count = count;
                break;
            }
            case TAG_TEX: {
                uint16_t width = 0;
                uint16_t height = 0;
                const uint8_t* header = NULL;
                const uint8_t* pixels = NULL;
                uint32_t expected;
                if (seen_tex) { fail(error, error_len, "duplicate TEX chunk"); return 0; }
                seen_tex = 1;
                if (!read_u16(&chunk, &width) || !read_u16(&chunk, &height) ||
                    !take(&chunk, 4, &header)) {
                    fail(error, error_len, "truncated TEX header");
                    return 0;
                }
                out->texture_format = header[0];
                if (out->texture_format >= PCASSET_FMT_COUNT) {
                    fail(error, error_len, "unknown texture format");
                    return 0;
                }
                expected = pcasset_texture_size(width, height, out->texture_format);
                if (expected == 0) {
                    fail(error, error_len, "texture dimensions must be powers of two within 1024");
                    return 0;
                }
                if (!take(&chunk, expected, &pixels)) {
                    fail(error, error_len, "TEX chunk is shorter than its dimensions require");
                    return 0;
                }
                out->texture = pixels;
                out->texture_bytes = expected;
                out->texture_width = width;
                out->texture_height = height;
                out->has_texture = 1;
                break;
            }
            case TAG_PAL: {
                uint16_t entries = 0;
                const uint8_t* payload = NULL;
                if (seen_pal) { fail(error, error_len, "duplicate PAL chunk"); return 0; }
                seen_pal = 1;
                if (!read_u16(&chunk, &entries) || entries == 0 || entries > PCASSET_MAX_PALETTE) {
                    fail(error, error_len, "bad palette entry count");
                    return 0;
                }
                if (!take(&chunk, (size_t)entries * 2u, &payload)) {
                    fail(error, error_len, "PAL chunk is shorter than its count claims");
                    return 0;
                }
                out->palette = payload;
                out->palette_entries = entries;
                out->has_palette = 1;
                break;
            }
            case TAG_MSH: {
                uint32_t count = 0;
                const uint8_t* payload = NULL;
                if (seen_msh) { fail(error, error_len, "duplicate MSH chunk"); return 0; }
                seen_msh = 1;
                if (!read_u32(&chunk, &count) || count == 0 || count > PCASSET_MAX_TRIANGLES) {
                    fail(error, error_len, "bad triangle count");
                    return 0;
                }
                if (!take(&chunk, (size_t)count * 6u, &payload)) {
                    fail(error, error_len, "MSH chunk is shorter than its count claims");
                    return 0;
                }
                out->triangles = payload;
                out->triangle_count = count;
                break;
            }
            case TAG_META: {
                int axis;
                for (axis = 0; axis < 6; axis++) {
                    if (!read_f32(&chunk, &out->bounds[axis])) {
                        fail(error, error_len, "bad META bounds");
                        return 0;
                    }
                }
                if (!read_f32(&chunk, &out->suggested_scale) || out->suggested_scale <= 0.0f ||
                    out->suggested_scale > 1000.0f) {
                    fail(error, error_len, "bad META scale");
                    return 0;
                }
                out->has_meta = 1;
                break;
            }
            default:
                /* Unknown chunks are skipped, never executed. This is what lets
                 * a later format version add data an older client ignores. */
                break;
        }
    }

    if (!seen_vtx || !seen_msh) {
        fail(error, error_len, "a model needs both VTX and MSH chunks");
        return 0;
    }

    /* Every triangle index must address a vertex that arrived. Checked after
     * both chunks are known rather than during MSH, since chunk order is not
     * fixed by the format. */
    {
        uint32_t t;
        for (t = 0; t < out->triangle_count; t++) {
            const uint8_t* tri = out->triangles + (size_t)t * 6u;
            uint32_t v;
            for (v = 0; v < 3; v++) {
                const uint16_t index = (uint16_t)(tri[v * 2] | ((uint16_t)tri[v * 2 + 1] << 8));
                if (index >= out->vertex_count) {
                    fail(error, error_len, "triangle references a vertex that does not exist");
                    return 0;
                }
            }
        }
    }

    /* A CI texture without a palette would index nothing. */
    if (out->has_texture && !out->has_palette &&
        (out->texture_format == PCASSET_FMT_CI4 || out->texture_format == PCASSET_FMT_CI8)) {
        fail(error, error_len, "paletted texture with no PAL chunk");
        return 0;
    }
    /* CI4 addresses 16 entries, CI8 addresses 256. A palette shorter than the
     * format can index is a file that would read past it at draw time. */
    if (out->has_texture && out->has_palette) {
        if (out->texture_format == PCASSET_FMT_CI4 && out->palette_entries < 16) {
            fail(error, error_len, "CI4 texture needs at least 16 palette entries");
            return 0;
        }
        if (out->texture_format == PCASSET_FMT_CI8 && out->palette_entries < 256) {
            fail(error, error_len, "CI8 texture needs 256 palette entries");
            return 0;
        }
    }
    return 1;
}
