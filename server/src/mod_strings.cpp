#include "acserver/mod_strings.hpp"

#include <array>

namespace acserver {
namespace {

constexpr int kNoMapping = -1;

/* ASCII range. Mirrors the ascii_map[] in pc/src/pc_typing.c: most of 32..126
 * is identity, but '#', '$', '*', '[', ']', '^', '`', '{'..'~' have no glyph. */
constexpr std::array<int, 128> kAscii = {
    kNoMapping, kNoMapping, kNoMapping, kNoMapping, kNoMapping, kNoMapping, kNoMapping, kNoMapping,
    kNoMapping, kNoMapping, kNoMapping, kNoMapping, kNoMapping, kNoMapping, kNoMapping, kNoMapping,
    kNoMapping, kNoMapping, kNoMapping, kNoMapping, kNoMapping, kNoMapping, kNoMapping, kNoMapping,
    kNoMapping, kNoMapping, kNoMapping, kNoMapping, kNoMapping, kNoMapping, kNoMapping, kNoMapping,
    ' ', '!', '"', kNoMapping, kNoMapping, '%', '&', '\'',
    '(', ')', '*', '+', ',', '-', '.', '/',
    '0', '1', '2', '3', '4', '5', '6', '7',
    '8', '9', ':', ';', '<', '=', '>', '?',
    '@', 'A', 'B', 'C', 'D', 'E', 'F', 'G',
    'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O',
    'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W',
    'X', 'Y', 'Z', kNoMapping, 222 /* CHAR_BACKSLASH */, kNoMapping, kNoMapping, '_',
    kNoMapping, 'a', 'b', 'c', 'd', 'e', 'f', 'g',
    'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o',
    'p', 'q', 'r', 's', 't', 'u', 'v', 'w',
    'x', 'y', 'z', kNoMapping, kNoMapping, kNoMapping, kNoMapping, kNoMapping,
};

/* U+00C0..U+00DF, uppercase accented. CHAR_* names from include/m_font.h. */
constexpr std::array<int, 32> kUpperLatin1 = {
    3, 4, 5, 6, 2, 7, 162, 8,          /* Grave/Acute/Circumflex/Tilde/Diaeresis A, Angstrom, Ash, Cedilla */
    9, 10, 11, 12,                     /* E family */
    13, 14, 15, 16,                    /* I family */
    17, 18,                            /* Eth, Tilde N */
    19, 20, 21, 22, 23, kNoMapping,    /* O family; U+00D7 multiplication sign has no glyph here */
    24, 25, 26, 27, 28, 150, 30, 29,   /* OE, U family, Acute Y, Thorn, sharp s */
};

/* U+00E0..U+00FF, lowercase accented. */
constexpr std::array<int, 32> kLowerLatin1 = {
    31, 35, 36, 91, 93, 94, 163, 96,
    123, 124, 125, 126,
    129, 130, 131, 132,
    134, 135,
    136, 137, 138, 139, 140, kNoMapping,
    141, 142, 143, 145, 146, 147, 149, 148,
};

int map_codepoint(std::uint32_t cp) {
    if (cp < 128) return kAscii[cp];
    if (cp == 0xA1) return 0;   /* CHAR_INVERT_EXCLAMATION */
    if (cp == 0xBF) return 1;   /* CHAR_INVERT_QUESTIONMARK */
    if (cp >= 0xC0 && cp <= 0xDF) return kUpperLatin1[cp - 0xC0];
    if (cp >= 0xE0 && cp <= 0xFF) return kLowerLatin1[cp - 0xE0];
    return kNoMapping;
}

/* Minimal strict UTF-8 decoder. Rejects overlong forms, surrogates and
 * out-of-range values rather than substituting -- a mod manifest is authored
 * text, so anything malformed is a mistake worth reporting. */
bool next_codepoint(const std::string& s, std::size_t& i, std::uint32_t& cp) {
    const auto byte = [&s](std::size_t k) { return static_cast<unsigned char>(s[k]); };
    const unsigned char lead = byte(i);
    std::size_t extra = 0;
    if (lead < 0x80) { cp = lead; i += 1; return true; }
    if ((lead & 0xE0) == 0xC0) { cp = lead & 0x1Fu; extra = 1; }
    else if ((lead & 0xF0) == 0xE0) { cp = lead & 0x0Fu; extra = 2; }
    else if ((lead & 0xF8) == 0xF0) { cp = lead & 0x07u; extra = 3; }
    else return false;
    if (i + extra >= s.size()) return false;   /* truncated continuation */
    for (std::size_t k = 1; k <= extra; ++k) {
        const unsigned char c = byte(i + k);
        if ((c & 0xC0) != 0x80) return false;
        cp = (cp << 6) | (c & 0x3Fu);
    }
    if ((extra == 1 && cp < 0x80) || (extra == 2 && cp < 0x800) || (extra == 3 && cp < 0x10000))
        return false;                                   /* overlong */
    if (cp >= 0xD800 && cp <= 0xDFFF) return false;      /* surrogate */
    if (cp > 0x10FFFF) return false;
    i += extra + 1;
    return true;
}

std::string quote_codepoint(std::uint32_t cp) {
    static const char* kHex = "0123456789ABCDEF";
    std::string digits;
    do {
        digits.insert(digits.begin(), kHex[cp & 0xFu]);
        cp >>= 4;
    } while (cp != 0);
    while (digits.size() < 4) digits.insert(digits.begin(), '0');
    return "U+" + digits;
}

} // namespace

bool transcode_utf8(const std::string& utf8, GameString& out, std::string& error) {
    out.clear();
    out.reserve(utf8.size());
    std::size_t i = 0;
    while (i < utf8.size()) {
        const std::size_t start = i;
        std::uint32_t cp = 0;
        if (!next_codepoint(utf8, i, cp)) {
            error = "invalid UTF-8 at byte " + std::to_string(start);
            return false;
        }
        const int mapped = map_codepoint(cp);
        if (mapped == kNoMapping) {
            error = "character " + quote_codepoint(cp) + " at byte " + std::to_string(start) +
                    " has no glyph in the game font";
            return false;
        }
        out.push_back(static_cast<std::uint8_t>(mapped));
    }
    return true;
}

bool pad_to(GameString& value, std::size_t width, std::string& error) {
    if (value.size() > width) {
        error = "string is " + std::to_string(value.size()) + " glyphs but the field holds " +
                std::to_string(width);
        return false;
    }
    value.resize(width, static_cast<std::uint8_t>(' '));
    return true;
}

} // namespace acserver
