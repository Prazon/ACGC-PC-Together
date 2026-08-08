/* pc_mod_assets.h - the .pcasset container (P5).
 *
 * A mod's model bundle: vertices, one texture, an optional palette, triangle
 * indices, and bounds. Parsed into borrowed views over the caller's buffer --
 * nothing is copied and nothing is allocated, so a hostile file cannot make the
 * parser allocate on its behalf.
 *
 * This parser is the client's attack surface once P7/P8 land: it is the first
 * thing that reads bytes an arbitrary server chose. It is therefore
 * deliberately free of SDL, GL and game headers so it can be fuzzed on its own
 * (tests/fuzz/pcasset_fuzz.cpp), and every bound is checked before use rather
 * than after.
 *
 * Format (little-endian, the host order of every supported target):
 *
 *   0  u32  magic 'PCAS' (0x53414350)
 *   4  u16  version
 *   6  u16  chunk_count
 *   8  ...  chunks: u32 tag, u32 length, payload[length]
 *
 * Chunks, each at most once:
 *   'VTX ' u32 count, then count * 16 bytes { s16 x,y,z; s16 u,v; u8 r,g,b,a }
 *   'TEX ' u16 w, u16 h, u8 format, u8 reserved[3], then pixel data
 *   'PAL ' u16 entries, then entries * u16
 *   'MSH ' u32 tri_count, then tri_count * 6 bytes { u16 a, b, c }
 *   'META' f32 bounds[6], f32 suggested_scale
 *
 * Unknown chunk tags are skipped, never executed. A pack carries data, never
 * code -- that property is what bounds the worst case to a parser bug, and it
 * is the reason this header refuses to grow a "script" chunk.
 */
#ifndef PC_MOD_ASSETS_H
#define PC_MOD_ASSETS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PCASSET_MAGIC 0x53414350u   /* 'PCAS' little-endian */
#define PCASSET_VERSION 1

/* Caps. Checked before any indexing or size arithmetic. Chosen so the products
 * below cannot overflow 32 bits: 65535 verts * 16 bytes and 65535 tris * 6
 * bytes are both far inside range. */
#define PCASSET_MAX_FILE_BYTES (4u * 1024u * 1024u)
#define PCASSET_MAX_CHUNKS 32
#define PCASSET_MAX_VERTICES 65535u
#define PCASSET_MAX_TRIANGLES 65535u
#define PCASSET_MAX_TEXTURE_DIM 1024u
#define PCASSET_MAX_PALETTE 256u

/* GC texture formats the PC layer already decodes (pc_gx_texture.c). */
enum {
    PCASSET_FMT_I4 = 0,
    PCASSET_FMT_I8 = 1,
    PCASSET_FMT_IA4 = 2,
    PCASSET_FMT_IA8 = 3,
    PCASSET_FMT_RGB565 = 4,
    PCASSET_FMT_RGB5A3 = 5,
    PCASSET_FMT_RGBA8 = 6,
    PCASSET_FMT_CI4 = 7,
    PCASSET_FMT_CI8 = 8,
    PCASSET_FMT_CMPR = 9,
    PCASSET_FMT_COUNT
};

typedef struct {
    /* Borrowed pointers into the caller's buffer; valid only while it lives. */
    const uint8_t* vertices;      /* count * 16 bytes */
    uint32_t vertex_count;

    const uint8_t* texture;
    uint32_t texture_bytes;
    uint16_t texture_width;
    uint16_t texture_height;
    uint8_t texture_format;
    int has_texture;

    const uint8_t* palette;       /* entries * 2 bytes, big-endian u16 as the GC stores them */
    uint16_t palette_entries;
    int has_palette;

    const uint8_t* triangles;     /* tri_count * 6 bytes */
    uint32_t triangle_count;

    float bounds[6];              /* min xyz, max xyz */
    float suggested_scale;
    int has_meta;
} PCAssetModel;

/* Parses `data`. Returns 1 on success. On failure returns 0 and, if `error` is
 * non-NULL, writes a short reason into it (bounded by `error_len`).
 *
 * Never allocates, never reads outside [data, data + size), and never trusts a
 * declared length before checking it against what is left. */
int pcasset_parse(const uint8_t* data, size_t size, PCAssetModel* out, char* error, size_t error_len);

/* Bytes a texture of these dimensions and format occupies, or 0 if the
 * combination is invalid. Exposed for the packer and for tests. */
uint32_t pcasset_texture_size(uint16_t width, uint16_t height, uint8_t format);

#ifdef __cplusplus
}
#endif

#endif /* PC_MOD_ASSETS_H */
