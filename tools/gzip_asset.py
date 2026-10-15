"""Gzip a web asset for embedding in the firmware: gzip_asset.py <input> <output>."""

import gzip
import sys

src, dst = sys.argv[1], sys.argv[2]
with open(src, "rb") as f:
    data = f.read()
# mtime=0 keeps the output byte-identical across builds, so unchanged pages don't trigger relinks.
with open(dst, "wb") as f:
    f.write(gzip.compress(data, compresslevel=9, mtime=0))
