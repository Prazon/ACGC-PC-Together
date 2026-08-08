#!/usr/bin/env python3
"""Count top-level entries in a C array initialiser.

Brace-aware and comment/string-aware, so it is reliable for the decomp's
furniture tables where entries are themselves brace groups. Used to establish
the real length of every FTR-indexed table before writing _Static_asserts.

Usage: count_init_entries.py FILE SYMBOL
"""
import re
import sys


def strip_noise(text):
    """Remove comments and string/char literals so braces inside them do not count."""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if c == "/" and nxt == "/":
            i = text.find("\n", i)
            if i < 0:
                break
            continue
        if c == "/" and nxt == "*":
            j = text.find("*/", i + 2)
            i = n if j < 0 else j + 2
            continue
        if c in "\"'":
            quote = c
            i += 1
            while i < n:
                if text[i] == "\\":
                    i += 2
                    continue
                if text[i] == quote:
                    i += 1
                    break
                i += 1
            out.append(" ")
            continue
        out.append(c)
        i += 1
    return "".join(out)


def count_entries(path, symbol):
    text = strip_noise(open(path, encoding="utf-8", errors="replace").read())
    m = re.search(r"\b" + re.escape(symbol) + r"\s*\[[^\]]*\]\s*=\s*\{", text)
    if not m:
        return None
    i = m.end()          # just past the opening brace of the initialiser
    depth = 1
    entries = 0
    seen_content = False
    while i < len(text):
        c = text[i]
        if c == "{":
            depth += 1
            seen_content = True
        elif c == "}":
            depth -= 1
            if depth == 0:
                # A trailing comma leaves no content after it; only count a
                # final partial entry when something followed the last comma.
                return entries + (1 if seen_content else 0)
        elif c == "," and depth == 1:
            entries += 1
            seen_content = False
        elif not c.isspace():
            seen_content = True
        i += 1
    return None


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    result = count_entries(sys.argv[1], sys.argv[2])
    if result is None:
        sys.exit("symbol %r not found as an array initialiser in %s" % (sys.argv[2], sys.argv[1]))
    print(result)
