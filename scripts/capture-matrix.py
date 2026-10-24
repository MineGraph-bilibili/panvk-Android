#!/usr/bin/env python3
"""capture-matrix.py — UPSTREAM_MATRIX from the exact Mesa checkout (docs/features.txt).

Usage: scripts/capture-matrix.py --mesa work/mesa --out dist/<build>/
Writes upstream-feature-matrix.json + upstream-feature-matrix.md.
Runtime matrix (RUNTIME_DEVICE_CAPABILITIES) comes from on-device
vulkaninfo/probe and is stored separately — never conflate the two.
"""
import argparse, json, re, pathlib

ap = argparse.ArgumentParser()
ap.add_argument('--mesa', required=True)
ap.add_argument('--out', required=True)
a = ap.parse_args()
mesa = pathlib.Path(a.mesa)
out = pathlib.Path(a.out)
out.mkdir(parents=True, exist_ok=True)

feat = mesa / 'docs' / 'features.txt'
text = feat.read_text(errors='replace') if feat.exists() else ''
# Parse PanVK section loosely: count DONE rows under a PanVK heading.
in_panvk = False
done = total = 0
rows = []
for line in text.splitlines():
    if re.match(r'\s*PanVK\s*:?', line):
        in_panvk = True
        continue
    if in_panvk and re.match(r'\s*\w+Driver\s*:|\s*(RADV|ANV|Turnip|Asahi)', line):
        break
    if in_panvk:
        m = re.match(r'\s*(\S+)\s+(DONE|TODO|N/A|started|in progress)', line)
        if m:
            total += 1
            status = m.group(2)
            if status == 'DONE':
                done += 1
            rows.append({'name': m.group(1), 'status': status})

data = {
    'source': 'docs/features.txt of exact Mesa checkout',
    'mesaRev': None,
    'panvk': {'done': done, 'total': total,
              'coverage': round(done / total, 4) if total else 0.0},
    'rows': rows,
    'note': 'Upstream implementation matrix, NOT a per-device runtime guarantee.',
}
try:
    import subprocess
    data['mesaRev'] = subprocess.check_output(
        ['git', '-C', str(mesa), 'rev-parse', 'HEAD'], text=True).strip()
except Exception:
    pass
(out / 'upstream-feature-matrix.json').write_text(json.dumps(data, indent=2))
md = [f"# Upstream PanVK matrix (Mesa {data['mesaRev']})", '',
      f"Implemented: {done}/{total} ({data['panvk']['coverage']*100:.1f}%)", '',
      'NOT a runtime guarantee. See runtime-feature-matrix.json from device probe.', '']
(out / 'upstream-feature-matrix.md').write_text('\n'.join(md))
print(f"OK done={done} total={total} out={out}")
