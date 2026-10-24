#!/usr/bin/env python3
"""dev tool: split a unified diff into hunks, keep those matching a pattern.
Usage: dev-split-hunks.py <diff-file> <pattern> [out-file]
Prints hunk headers + sizes; writes kept hunks (with ---/+++ labels) to out-file.
"""
import re, sys

diff_file, pattern = sys.argv[1], sys.argv[2]
out_file = sys.argv[3] if len(sys.argv) > 3 else None
text = open(diff_file, errors='replace').read()
lines = text.splitlines(keepends=True)
hdr = [l for l in lines if l.startswith(('--- ', '+++ '))]
body = ''.join(l for l in lines if not l.startswith(('--- ', '+++ ')))
hunks = [h for h in re.split(r'(?=@@ )', body) if h.strip()]
keep = [h for h in hunks if re.search(pattern, h, re.I)]
print(f"total={len(hunks)} kept={len(keep)}")
for h in keep:
    first = h.splitlines()[0]
    print(f"HUNK {first[:120]} ({len(h.splitlines())} lines)")
if out_file:
    with open(out_file, 'w') as f:
        f.writelines(hdr)
        for h in keep:
            f.write(h)
    print(f"wrote {out_file}")
