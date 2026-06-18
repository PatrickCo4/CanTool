#!/usr/bin/env python3
"""Bake index.html into webpage.h as a gzipped PROGMEM byte array.

Usage:  python3 gen_webpage.py [index.html] [webpage.h]
The CI workflow runs this before compiling, so webpage.h always matches
the current index.html. You can also run it locally after editing the page.
"""
import sys, gzip

src = sys.argv[1] if len(sys.argv) > 1 else "CanTool/index.html"
dst = sys.argv[2] if len(sys.argv) > 2 else "CanTool/webpage.h"

html = open(src, "rb").read()
gz = gzip.compress(html, 9)

lines = ["// Auto-generated from index.html by gen_webpage.py - do not edit by hand.",
         "#pragma once",
         "#include <stddef.h>",
         "#include <stdint.h>",
         "",
         "static const size_t webpage_gz_len = %d;" % len(gz),
         "static const uint8_t webpage_gz[] PROGMEM = {"]
for i in range(0, len(gz), 16):
    chunk = gz[i:i+16]
    lines.append("  " + ", ".join("0x%02X" % b for b in chunk) + ",")
lines.append("};")

open(dst, "w").write("\n".join(lines) + "\n")
print("Wrote %s: %d bytes HTML -> %d bytes gzip (%d%% of original)"
      % (dst, len(html), len(gz), round(100 * len(gz) / len(html))))
