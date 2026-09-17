#!/bin/bash
# Fetches a corpus of freely-licensed animated GIFs from Wikimedia Commons.
#
# Why this exists. ROADMAP.md's "Verify: is .apxl/APNG actually finished?"
# flagged that indexed animation is out of .apxl v1 not because it was weighed
# and rejected, but because the format was frozen (2026-07-25/26) two days
# before indexed-still support existed (314bb18, 2026-07-27) -- it was simply
# never revisited. The PSP/GE work since (pxl_convert_palette, GU_PSM_T4/T8)
# gives it a real motive now: an animated UI icon/sprite is the natural next
# beneficiary of the same "no per-frame conversion" win stills already have.
# But no corpus of indexed/palette animation exists locally (Anita is RGBA
# hand-drawn, CLIC and Kodak are photographs) -- nothing to measure a decision
# against. GIF is always <=256 colours per frame by format definition, so any
# animated GIF corpus is exactly the indexed content this format lacks.
#
#   bench/gif_corpus.sh            # fetch into tests/data/Animated-GIFs
#   bench/gif_corpus.sh --check    # verify an existing fetch, download nothing
#   GIF_BUDGET_MB=100 bench/...    # raise the total download budget
#   GIF_DELAY=2 bench/...          # be even gentler on the API
#
# Same shape as bench/synthetic_png.sh and bench/usc_png.sh: the corpus is
# gitignored, only this script is committed, and MANIFEST.tsv records licence
# and author per file (several free licences on Commons, e.g. CC BY-SA,
# require attribution if the files are ever redistributed rather than only
# measured -- see tests/data/README.md's "Adding assets" rule).
#
# Categories chosen for the content classes an indexed-animation decision
# actually needs, not for volume:
#   Animated pixel art  -- game sprites/icons, the PSP UI use case directly.
#   Throbbers            -- loading-icon UI animation, small and looped, the
#                            other common indexed-animation UI case.
#   Animated diagrams    -- technical/scientific animation, usually denser and
#                            more dithered, a harder case than flat pixel art.
# Photographic categories are deliberately excluded: GIF's 256-colour cap
# already tells a photographic source nothing an indexed .apxl couldn't do
# worse than the RGBA path, so it would not inform this decision.
#
# A GIF's category membership only proves Commons considers it free-licensed
# and animated in some sense; it does not prove multi-frame (a mislabelled
# still can sit in an "animated" category) or a real GIF signature despite the
# MIME type. Both are checked locally after download, the same way
# synthetic_png.sh checks the PNG signature rather than trusting the API.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
repo_root=$(cd "$here/.." && pwd)
cd "$repo_root"

dest=tests/data/Animated-GIFs
budget_mb=${GIF_BUDGET_MB:-100}
max_file_mb=${GIF_MAX_FILE_MB:-15}
check_only=0
[ "${1:-}" = "--check" ] && check_only=1

UA="PXL-benchmark-corpus/1.0 (https://github.com/RedMoth/PXL; lossless image format research)"

log() { echo "# $*" >&2; }

if [ "$check_only" = 1 ]; then
    [ -d "$dest" ] || { echo "error: $dest does not exist; run without --check first" >&2; exit 1; }
    n=0; bad=0
    while IFS= read -r f; do
        n=$((n + 1))
        magick identify -quiet "$f" >/dev/null 2>&1 || { echo "corrupt: $(basename "$f")" >&2; bad=$((bad + 1)); }
    done < <(find "$dest" -name '*.gif')
    log "checked $n files, $bad unreadable"
    [ "$bad" = 0 ]
    exit $?
fi

mkdir -p "$dest"

CATEGORIES=(
    "Animated pixel art"
    "Throbbers"
    "Animated diagrams"
)

python3 - "$dest" "$budget_mb" "$max_file_mb" "$UA" "${CATEGORIES[@]}" <<'PY'
import hashlib, json, os, re, sys, time, urllib.parse, urllib.request

dest, budget_mb, max_file_mb, ua = sys.argv[1:5]
delay = float(os.environ.get("GIF_DELAY", "1.0"))
categories = sys.argv[5:]
budget = int(budget_mb) * 1048576
max_file = int(max_file_mb) * 1048576
API = "https://commons.wikimedia.org/w/api.php"

OK_LICENCE = ("cc0", "cc by", "cc-by", "public domain", "pd", "gpl", "lgpl",
              "apache", "mit", "bsd", "wtfpl", "fal")

def api(params):
    q = urllib.parse.urlencode(params)
    req = urllib.request.Request(API + "?" + q, headers={"User-Agent": ua})
    with urllib.request.urlopen(req, timeout=40) as r:
        return json.load(r)

def plain(html):
    t = re.sub(r"<[^>]+>", " ", html or "")
    t = t.replace("&amp;", "&").replace("&quot;", '"').replace("&#039;", "'")
    return re.sub(r"\s+", " ", t).strip()[:160]

def licence_ok(name):
    n = (name or "").lower()
    return any(k in n for k in OK_LICENCE)

seen, seen_names, chosen = set(), set(), []
for cat in categories:
    cont = {}
    while True:
        p = {"action": "query", "format": "json", "generator": "categorymembers",
             "gcmtitle": "Category:" + cat, "gcmtype": "file", "gcmlimit": "200",
             "prop": "imageinfo", "iiprop": "url|size|mime|extmetadata"}
        p.update(cont)
        try:
            d = api(p)
        except Exception as e:
            print("# WARNING: %s: %s" % (cat, e), file=sys.stderr)
            break
        for page in d.get("query", {}).get("pages", {}).values():
            ii = (page.get("imageinfo") or [{}])[0]
            if ii.get("mime") != "image/gif":
                continue
            if ii.get("size", 0) > max_file:
                continue
            em = ii.get("extmetadata", {})
            lic = em.get("LicenseShortName", {}).get("value", "")
            if not licence_ok(lic):
                continue
            title = page["title"]
            if title in seen:
                continue
            outname = title[5:].replace(" ", "_")
            if outname.lower().endswith(".gif"):
                outname = outname[:-4] + ".gif"
            if outname in seen_names:
                continue
            seen.add(title)
            seen_names.add(outname)
            chosen.append({
                "title": title, "url": ii["url"], "size": ii["size"],
                "w": ii.get("width"), "h": ii.get("height"), "licence": lic,
                "artist": plain(em.get("Artist", {}).get("value", "")),
                "descurl": ii.get("descriptionurl", ""), "category": cat,
            })
        cont = d.get("continue", {})
        if not cont:
            break
        time.sleep(0.2)
    print("# %-20s %4d candidates so far" % (cat, len(chosen)), file=sys.stderr)

chosen.sort(key=lambda c: c["title"])
man = os.path.join(dest, "MANIFEST.tsv")
total, kept = 0, 0
with open(man, "w", encoding="utf-8") as mf:
    mf.write("# filename\tsha256\tbytes\twidth\theight\tframes\tlicence\tsource\tartist\n")
    for c in chosen:
        if total + c["size"] > budget:
            continue
        name = c["title"][5:].replace(" ", "_")
        if name.lower().endswith(".gif"):
            name = name[:-4] + ".gif"
        out = os.path.join(dest, name)
        if os.path.exists(out) and os.path.getsize(out) == c["size"]:
            pass
        else:
            for attempt in range(6):
                try:
                    req = urllib.request.Request(c["url"], headers={"User-Agent": ua})
                    with urllib.request.urlopen(req, timeout=90) as r:
                        blob = r.read()
                    with open(out, "wb") as f:
                        f.write(blob)
                    time.sleep(delay)
                    break
                except urllib.error.HTTPError as e:
                    if e.code == 429:
                        wait = float(e.headers.get("Retry-After") or 0) or delay * (2 ** attempt) * 5
                        print("# rate-limited, waiting %.0fs" % wait, file=sys.stderr)
                        time.sleep(wait)
                        continue
                    print("# skip %s: %s" % (name[:50], e), file=sys.stderr)
                    break
                except Exception as e:
                    print("# skip %s: %s" % (name[:50], e), file=sys.stderr)
                    break
            if not os.path.exists(out):
                continue
        data = open(out, "rb").read()
        if not (data[:6] == b"GIF87a" or data[:6] == b"GIF89a"):
            os.remove(out)
            print("# skip %s: not a GIF despite the MIME" % name[:50], file=sys.stderr)
            continue
        # Multi-frame check: a category can hold a mislabelled still. PIL's
        # n_frames forces a full parse, which is also a cheap corruption check.
        try:
            from PIL import Image
            im = Image.open(out)
            frames = getattr(im, "n_frames", 1)
        except Exception as e:
            os.remove(out)
            print("# skip %s: unreadable (%s)" % (name[:50], e), file=sys.stderr)
            continue
        if frames < 2:
            os.remove(out)
            print("# skip %s: single frame despite category" % name[:50], file=sys.stderr)
            continue
        mf.write("%s\t%s\t%d\t%s\t%s\t%d\t%s\t%s\t%s\n" % (
            name, hashlib.sha256(data).hexdigest(), len(data), c["w"], c["h"],
            frames, plain(c["licence"]), c["descurl"], c["artist"]))
        total += len(data)
        kept += 1

listed = set()
with open(man, encoding="utf-8") as mf:
    for line in mf:
        if not line.startswith("#"):
            listed.add(line.split("\t")[0])
orphans = 0
for f in os.listdir(dest):
    if f != "MANIFEST.tsv" and f not in listed:
        os.remove(os.path.join(dest, f))
        orphans += 1
if orphans:
    print("# removed %d file(s) not in the manifest (leftovers from an earlier run)"
          % orphans, file=sys.stderr)

print("# done: %d files, %.1f MB, manifest at %s" % (kept, total / 1048576, man),
      file=sys.stderr)
PY

log "corpus ready at $dest"
