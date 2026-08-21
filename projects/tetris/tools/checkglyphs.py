#!/usr/bin/env python3
"""Fail if any non-ASCII character used in the firmware's UI strings is
missing from the generated font table. A missing glyph draws nothing, so
this must be checked at build time rather than discovered on the panel."""
import re
import sys
import glob

STRING_RE = re.compile(r'"((?:[^"\\]|\\.)*)"')


def strings_in(path):
    with open(path, encoding="utf-8") as f:
        return STRING_RE.findall(f.read())


def main():
    used = {}
    for path in glob.glob("core/*.c") + glob.glob("app_src/*.c"):
        if path.endswith("font_zh.c"):
            continue
        for s in strings_in(path):
            for ch in s:
                if ord(ch) > 0x7F:
                    used.setdefault(ch, set()).add(path)

    # Read the codepoints the generated table actually contains.
    have = set()
    try:
        with open("core/font_zh.c", encoding="utf-8") as f:
            body = f.read()
    except FileNotFoundError:
        print("core/font_zh.c missing - run tools/genfont.py first")
        return 1
    m = re.search(r"font_zh_cp\[FONT_ZH_COUNT\] = \{(.*?)\};", body, re.S)
    if not m:
        print("could not parse font_zh_cp table")
        return 1
    for hexval in re.findall(r"0x([0-9A-Fa-f]{4})", m.group(1)):
        have.add(chr(int(hexval, 16)))

    missing = sorted(set(used) - have)
    print(f"UI non-ASCII glyphs used: {len(used)}")
    print(f"present in font table:    {len(set(used) & have)}")
    if missing:
        print("MISSING GLYPHS:")
        for ch in missing:
            where = ", ".join(sorted(used[ch]))
            print(f"  U+{ord(ch):04X} {ch}  (used in {where})")
        return 1
    print("all UI glyphs present")
    return 0


if __name__ == "__main__":
    sys.exit(main())
