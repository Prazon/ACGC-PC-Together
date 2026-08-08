/* Checks the .pcasset model compiler, mostly its batching.
 *
 * A gSPVertex load fills a 32-entry vertex cache and triangle commands index it
 * with 5 bits, so every triangle's three vertices must land in the same batch.
 * Getting that wrong does not crash -- it draws the wrong triangles, or reads a
 * cache slot that was never loaded. So the invariants are asserted directly:
 * every emitted index is inside its batch, and every triangle survives.
 */

#include "pc_mod_arena.h"
#include "pc_mod_assets.h"
#include "pc_mod_model.h"

#include "types.h"
#include "PR/gbi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

static void expect(int condition, const char* what) {
    if (!condition) {
        printf("FAIL: %s\n", what);
        failures++;
    }
}

static void put_u16(uint8_t** p, uint16_t v) { *(*p)++ = (uint8_t)(v & 0xFF); *(*p)++ = (uint8_t)(v >> 8); }
static void put_u32(uint8_t** p, uint32_t v) {
    int i;
    for (i = 0; i < 4; i++) *(*p)++ = (uint8_t)((v >> (i * 8)) & 0xFF);
}

/* Builds a .pcasset with `verts` vertices and `tris` triangles. `strided`
 * spreads each triangle's indices far apart, which is the case that forces
 * vertex duplication across batches. */
static size_t build_asset(uint8_t* out, uint32_t verts, uint32_t tris, int strided) {
    uint8_t* p = out;
    uint8_t* vtx_len;
    uint8_t* msh_len;
    uint8_t* start;
    uint32_t i;

    put_u32(&p, PCASSET_MAGIC);
    put_u16(&p, PCASSET_VERSION);
    put_u16(&p, 2);

    put_u32(&p, 0x20585456u);        /* 'VTX ' */
    vtx_len = p; put_u32(&p, 0);
    start = p;
    put_u32(&p, verts);
    for (i = 0; i < verts; i++) {
        int b;
        for (b = 0; b < 16; b++) *p++ = (uint8_t)(i + b);
    }
    { uint8_t* save = p; p = vtx_len; put_u32(&p, (uint32_t)(save - start)); p = save; }

    put_u32(&p, 0x2048534Du);        /* 'MSH ' */
    msh_len = p; put_u32(&p, 0);
    start = p;
    put_u32(&p, tris);
    for (i = 0; i < tris; i++) {
        if (strided) {
            /* Indices spread across the whole vertex range, so consecutive
             * triangles rarely share vertices. */
            put_u16(&p, (uint16_t)((i * 7u) % verts));
            put_u16(&p, (uint16_t)((i * 13u + 1u) % verts));
            put_u16(&p, (uint16_t)((i * 29u + 2u) % verts));
        } else {
            put_u16(&p, (uint16_t)((i * 3u) % verts));
            put_u16(&p, (uint16_t)((i * 3u + 1u) % verts));
            put_u16(&p, (uint16_t)((i * 3u + 2u) % verts));
        }
    }
    { uint8_t* save = p; p = msh_len; put_u32(&p, (uint32_t)(save - start)); p = save; }

    return (size_t)(p - out);
}

/* Walks the emitted display list and checks the invariant that matters: every
 * triangle index is inside the batch most recently loaded. Returns the triangle
 * count seen, or -1 on a violation. */
static long audit_display_list(const Gfx* commands, size_t count, size_t vertex_total) {
    long triangles = 0;
    size_t i;
    int loaded = 0;         /* vertices in the current batch */
    int saw_vertex_load = 0;

    for (i = 0; i < count; i++) {
        const unsigned op = (unsigned)((commands[i].words.w0 >> 24) & 0xFF);
        if (op == (unsigned)((G_VTX >> 0) & 0xFF)) {
            /* F3DEX2 gSPVertex: w0 carries the count. Recover it the same way
             * the macro packs it. */
            loaded = (int)((commands[i].words.w0 >> 12) & 0xFF);
            saw_vertex_load = 1;
            if (loaded <= 0 || loaded > PC_MOD_MODEL_BATCH_VERTS) return -1;
        } else if (op == (unsigned)((G_TRI1 >> 0) & 0xFF)) {
            const unsigned packed = (unsigned)(commands[i].words.w0 & 0x00FFFFFFu);
            const int a = (int)((packed >> 16) & 0xFF) / 2;
            const int b = (int)((packed >> 8) & 0xFF) / 2;
            const int c = (int)(packed & 0xFF) / 2;
            if (!saw_vertex_load) return -1;               /* triangle before any load */
            if (a >= loaded || b >= loaded || c >= loaded) return -1;
            triangles++;
        }
    }
    (void)vertex_total;
    return triangles;
}

int main(void) {
    static uint8_t buffer[512 * 1024];
    PCAssetModel parsed;
    PCModModel model;
    char error[128];
    size_t size;

    expect(pc_mod_arena_init(4 * 1024 * 1024) == 1, "arena opens");

    /* --- A model that fits one batch --------------------------------- */
    size = build_asset(buffer, 12, 4, 0);
    expect(pcasset_parse(buffer, size, &parsed, error, sizeof(error)) == 1, "small model parses");
    expect(pc_mod_model_compile(&parsed, &model) == 1, "small model compiles");
    expect(model.batch_count == 1, "it needs exactly one batch");
    expect(model.vertex_count <= PC_MOD_MODEL_BATCH_VERTS, "and fits the vertex cache");
    expect(audit_display_list((const Gfx*)model.display_list, model.command_count,
                              model.vertex_count) == 4,
           "all four triangles emitted, every index inside its batch");

    /* --- A model that must span batches ------------------------------ */
    size = build_asset(buffer, 300, 120, 0);
    expect(pcasset_parse(buffer, size, &parsed, error, sizeof(error)) == 1, "large model parses");
    expect(pc_mod_model_compile(&parsed, &model) == 1, "large model compiles");
    expect(model.batch_count > 1, "it needs several batches");
    expect(audit_display_list((const Gfx*)model.display_list, model.command_count,
                              model.vertex_count) == 120,
           "all 120 triangles survive batching");

    /* --- The case that forces duplication ----------------------------- */
    size = build_asset(buffer, 400, 200, 1);
    expect(pcasset_parse(buffer, size, &parsed, error, sizeof(error)) == 1, "strided model parses");
    expect(pc_mod_model_compile(&parsed, &model) == 1, "strided model compiles");
    expect(audit_display_list((const Gfx*)model.display_list, model.command_count,
                              model.vertex_count) == 200,
           "all 200 strided triangles survive");
    /* Scattered indices mean vertices get emitted more than once. That is the
     * cost of a 5-bit index, not a bug -- but it should be visible. */
    expect(model.vertex_count >= 200, "duplication happened as expected");

    /* --- A degenerate triangle (all three the same vertex) ------------ */
    {
        uint8_t* p = buffer;
        uint8_t* len;
        uint8_t* start;
        int b;
        put_u32(&p, PCASSET_MAGIC);
        put_u16(&p, PCASSET_VERSION);
        put_u16(&p, 2);
        put_u32(&p, 0x20585456u);
        len = p; put_u32(&p, 0); start = p;
        put_u32(&p, 3);
        for (b = 0; b < 3 * 16; b++) *p++ = (uint8_t)b;
        { uint8_t* s = p; p = len; put_u32(&p, (uint32_t)(s - start)); p = s; }
        put_u32(&p, 0x2048534Du);
        len = p; put_u32(&p, 0); start = p;
        put_u32(&p, 1);
        put_u16(&p, 1); put_u16(&p, 1); put_u16(&p, 1);
        { uint8_t* s = p; p = len; put_u32(&p, (uint32_t)(s - start)); p = s; }

        expect(pcasset_parse(buffer, (size_t)(p - buffer), &parsed, error, sizeof(error)) == 1,
               "degenerate triangle parses");
        expect(pc_mod_model_compile(&parsed, &model) == 1, "and compiles");
        /* One distinct vertex, and the triangle still references it three
         * times -- the batch must not count it three times either. */
        expect(model.vertex_count == 1, "a repeated index is stored once per batch");
    }

    /* --- No arena means no model, not a crash ------------------------- */
    pc_mod_arena_shutdown();
    size = build_asset(buffer, 12, 4, 0);
    expect(pcasset_parse(buffer, size, &parsed, error, sizeof(error)) == 1, "parses without an arena");
    expect(pc_mod_model_compile(&parsed, &model) == 0, "compile fails cleanly with no arena");

    printf(failures == 0 ? "mod_model: PASS\n" : "mod_model: %d FAILURE(S)\n", failures);
    return failures == 0 ? 0 : 1;
}
