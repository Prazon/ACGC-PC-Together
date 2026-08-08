#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace acserver {

/* Transcodes mod-authored UTF-8 into the game's own 256-entry codepage.
 *
 * The game does not use ASCII. Its font atlas is a 16x16 grid of 12x16 glyphs
 * indexed by a custom encoding (include/m_font.h): ASCII-ish in 32..126 and
 * divergent everywhere else -- 127 is the control-code escape, 205 is newline,
 * 176..212 are symbols. The mapping below mirrors pc_utf8_to_game_code() in
 * pc/src/pc_typing.c, which does the same job for keyboard input.
 *
 * The table is duplicated rather than shared because server/ must not include
 * game headers: the server builds with no -Iinclude, and pulling the decomp
 * header set into it would couple the headless binary to the game tree. The
 * values are a fixed ROM font layout, so duplication is safe -- but
 * include/m_font.h remains the source of truth, and a change there must be
 * mirrored here.
 *
 * Failure is deliberately loud. An unmappable character is a load-time error
 * naming the offending byte, never a silent substitution: a mod author who
 * types a curly quote should be told at startup, not ship a town whose holiday
 * is called "Lantern?Night".
 */

/* Game codepage bytes with no terminator. Records that reach the client are
 * fixed-width and space-padded; see pad_to(). */
using GameString = std::vector<std::uint8_t>;

/* Returns false and fills `error` if any character has no game-codepage
 * equivalent, or if `utf8` is not valid UTF-8. */
bool transcode_utf8(const std::string& utf8, GameString& out, std::string& error);

/* Space-pads (or rejects, if too long) to an exact record width -- 16 bytes for
 * item names, mIN_ITEM_NAME_LEN. */
bool pad_to(GameString& value, std::size_t width, std::string& error);

} // namespace acserver
