/* Bounded-garbage fuzzing for the .pcasset parser.
 *
 * This parser is the client's attack surface for server-delivered content: once
 * P7/P8 land it is the first thing to read bytes an arbitrary town chose. A
 * crash here is a crash in every player's client, so net/CLAUDE.md's rule for
 * protocol parsers applies to it too.
 *
 * Two strategies, because they find different things:
 *   - pure random bytes, which mostly bounce off the magic check
 *   - a *valid* file with one byte corrupted, which gets past the header and
 *     exercises the length and index checks that actually matter
 */

#include "pc_mod_assets.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace {

void put_u16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFF));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
}

void put_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>((value >> (i * 8)) & 0xFF));
}

/* A minimal but complete model: two triangles over four vertices, a 4x4 RGBA8
 * texture, and bounds. Small enough that a single corrupted byte is likely to
 * land somewhere meaningful. */
std::vector<std::uint8_t> valid_model() {
    std::vector<std::uint8_t> body;

    std::vector<std::uint8_t> vtx;
    put_u32(vtx, 4);
    for (int i = 0; i < 4; ++i) {
        for (int b = 0; b < 16; ++b) vtx.push_back(static_cast<std::uint8_t>(i * 16 + b));
    }

    std::vector<std::uint8_t> tex;
    put_u16(tex, 4);
    put_u16(tex, 4);
    tex.push_back(PCASSET_FMT_RGBA8);
    tex.push_back(0);
    tex.push_back(0);
    tex.push_back(0);
    tex.resize(tex.size() + 4u * 4u * 4u, 0x7F);

    std::vector<std::uint8_t> msh;
    put_u32(msh, 2);
    const std::uint16_t indices[6] = { 0, 1, 2, 0, 2, 3 };
    for (const std::uint16_t index : indices) put_u16(msh, index);

    std::vector<std::uint8_t> meta;
    for (int i = 0; i < 7; ++i) put_u32(meta, 0x3F800000u);   /* 1.0f */

    std::vector<std::uint8_t> file;
    put_u32(file, PCASSET_MAGIC);
    put_u16(file, PCASSET_VERSION);
    put_u16(file, 4);

    const std::pair<std::uint32_t, const std::vector<std::uint8_t>*> chunks[] = {
        { 0x20585456u, &vtx }, { 0x20584554u, &tex }, { 0x2048534Du, &msh }, { 0x4154454Du, &meta },
    };
    for (const auto& entry : chunks) {
        put_u32(file, entry.first);
        put_u32(file, static_cast<std::uint32_t>(entry.second->size()));
        file.insert(file.end(), entry.second->begin(), entry.second->end());
    }
    return file;
}

} // namespace

int main(int argc, char** argv) {
    const long iterations = argc > 1 ? std::strtol(argv[1], nullptr, 10) : 50000;
    std::random_device seed_source;
    const std::uint32_t seed = seed_source();
    std::mt19937 random(seed);

    const std::vector<std::uint8_t> base = valid_model();
    PCAssetModel model;
    char error[128];

    /* The clean file must parse, or the corruption pass below is testing
     * nothing. */
    if (!pcasset_parse(base.data(), base.size(), &model, error, sizeof(error))) {
        std::printf("{\"pcasset_fuzz\":\"fail\",\"reason\":\"baseline rejected: %s\"}\n", error);
        return 1;
    }
    if (model.vertex_count != 4 || model.triangle_count != 2 || !model.has_texture) {
        std::printf("{\"pcasset_fuzz\":\"fail\",\"reason\":\"baseline parsed wrong\"}\n");
        return 1;
    }

    for (long i = 0; i < iterations; ++i) {
        if ((i & 1) == 0) {
            /* Random bytes, random length. */
            const std::size_t length = random() % 512;
            std::vector<std::uint8_t> noise(length);
            for (std::uint8_t& byte : noise) byte = static_cast<std::uint8_t>(random() & 0xFF);
            (void)pcasset_parse(noise.empty() ? nullptr : noise.data(), noise.size(), &model, error,
                                sizeof(error));
        } else {
            /* One byte of a valid file flipped, so the header passes and the
             * interesting checks run. */
            std::vector<std::uint8_t> corrupted = base;
            const std::size_t index = random() % corrupted.size();
            corrupted[index] = static_cast<std::uint8_t>(random() & 0xFF);
            /* Occasionally truncate as well: a short file with a valid header
             * is the shape most likely to walk off the end. */
            if ((random() & 7) == 0) corrupted.resize(random() % corrupted.size());
            (void)pcasset_parse(corrupted.empty() ? nullptr : corrupted.data(), corrupted.size(), &model,
                                error, sizeof(error));
        }
    }

    std::printf("{\"pcasset_fuzz\":\"pass\",\"iterations\":%ld,\"seed\":%u}\n", iterations, seed);
    return 0;
}
