#!/usr/bin/env python3
"""Import one exact tagged Vulkan Profile with immutable provenance."""
import argparse, datetime, hashlib, json, pathlib, re, urllib.request

def main():
    ap=argparse.ArgumentParser(description=__doc__); ap.add_argument("--url",required=True); ap.add_argument("--tag",required=True); ap.add_argument("--output",required=True); a=ap.parse_args()
    original=urllib.request.urlopen(a.url,timeout=30).read(); original_sha=hashlib.sha256(original).hexdigest(); normalized=original
    normalization=None
    try: document=json.loads(original)
    except json.JSONDecodeError:
        normalized=re.sub(rb",\s*([}\]])",rb"\1",original); document=json.loads(normalized)
        normalization={"kind":"remove-trailing-commas-only","originalSha256":original_sha,"normalizedSha256":hashlib.sha256(normalized).hexdigest()}
    if not isinstance(document.get("profiles"),dict) or not isinstance(document.get("capabilities"),dict): raise SystemExit("not a Vulkan Profiles document")
    payload={"provenance":{"url":a.url,"tag":a.tag,"originalSha256":original_sha,"normalizedSha256":hashlib.sha256(normalized).hexdigest(),"normalization":normalization,"importedAt":datetime.datetime.now(datetime.timezone.utc).isoformat()},"document":document}
    out=pathlib.Path(a.output); out.parent.mkdir(parents=True,exist_ok=True); out.write_text(json.dumps(payload,indent=2)+"\n"); print(f"PASS profiles={len(document['profiles'])} sha256={original_sha} output={out}")
if __name__=="__main__": main()
