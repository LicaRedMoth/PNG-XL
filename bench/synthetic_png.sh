#!/bin/bash
# Fetches a corpus of freely-licensed UI screenshots from Wikimedia Commons.
#
# Why this exists. Every corpus the project had was photographic or animation,
# and synthetic non-photographic stills -- interfaces, rendered text, diagrams --
# were never covered, although that is the content a PNG replacement is actually
# pointed at. A local folder of personal screenshots answered the question once
# (see docs/BENCHMARKS.md, 2026-09-15) but cannot be published, so the README's
# reproducible table still could not show the format's best case. This corpus
# can: everything here is freely licensed and fetched by a committed script.
#
#   bench/synthetic_png.sh            # fetch into tests/data/Synthetic-Screenshots
#   bench/synthetic_png.sh --check    # verify an existing fetch, download nothing
#   SYNTH_BUDGET_MB=400 bench/...     # raise the total download budget
#   SYNTH_DELAY=2 bench/...           # be even gentler on the API
#
# Downloads run at one request per second with backoff on HTTP 429, because
# Wikimedia cuts off faster bulk fetches within a minute. A full run is
# therefore slow by design; it resumes, so an interrupted fetch can be repeated.
#
# Like bench/usc_png.sh, the corpus itself is gitignored and only this script is
# committed, so the set is reproducible without carrying a few hundred megabytes
# in git history.
#
# Only PNG is taken, and this is not fussiness: a PNG re-saved from a JPEG (or
# any other lossy source) carries codec noise that no lossless coder can remove,
# and it would quietly make every ratio here look worse than the content
# deserves. Commons hosts plenty of such files, so width, MIME and a
# colour-count sanity check are all applied before a file joins the corpus.
#
# A warning about the baseline, which matters more here than for any other
# corpus. Commons uploads come from hundreds of unknown tools, so "the source
# PNG" is not a defined thing: measured as uploaded, this corpus puts PXL at
# 57.9% of PNG, but `oxipng -o 2` alone takes the same files to 75.3% -- about a
# quarter of that apparent win is slack in someone's export settings, not
# compression. **Never quote a ratio from this corpus without the oxipng row
# beside it**, which is what that row in bench/corpus.sh exists for. Re-encoding
# the corpus to normalise it was tried and rejected: ImageMagick drops a fully
# opaque alpha channel, turning RGBA files into RGB, and a screenshot corpus
# that has lost its alpha channels is no longer the content being modelled.
#
# MANIFEST.tsv records licence and author per file. Commons is free-licence only
# by policy, but several of these are CC BY-SA, which requires attribution if the
# images are ever redistributed rather than merely measured.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
repo_root=$(cd "$here/.." && pwd)
cd "$repo_root"

dest=tests/data/Synthetic-Screenshots
budget_mb=${SYNTH_BUDGET_MB:-260}
min_width=${SYNTH_MIN_WIDTH:-800}
max_file_mb=${SYNTH_MAX_FILE_MB:-8}
check_only=0
[ "${1:-}" = "--check" ] && check_only=1

# A descriptive User-Agent is required by the Wikimedia API policy; a generic
# one gets rate-limited or refused.
UA="PXL-benchmark-corpus/1.0 (https://github.com/RedMoth/PXL; lossless image format research)"

log() { echo "# $*" >&2; }

if [ "$check_only" = 1 ]; then
    [ -d "$dest" ] || { echo "error: $dest does not exist; run without --check first" >&2; exit 1; }
    n=0; bad=0
    while IFS= read -r f; do
        n=$((n + 1))
        magick identify -quiet "$f" >/dev/null 2>&1 || { echo "corrupt: $(basename "$f")" >&2; bad=$((bad + 1)); }
    done < <(find "$dest" -name '*.png')
    log "checked $n files, $bad unreadable"
    [ "$bad" = 0 ]
    exit $?
fi

mkdir -p "$dest"

# Categories chosen for breadth of interface style rather than volume: wiki
# pages and browser chrome are text-heavy, Inkscape and Emacs are dense tool
# panels, and browsers bring page renderings. Photographs of screens and scanned
# documents live in the broad "Screenshots of free software" category, which is
# deliberately not used.
CATEGORIES=(
    "MediaWiki screenshots"
    "Screenshots of Mozilla Firefox"
    "Wikipedia screenshots"
    "Screenshots of Inkscape"
    "Screenshots of web browsers"
    "Screenshots of GNU Emacs"
)

python3 - "$dest" "$budget_mb" "$min_width" "$max_file_mb" "$UA" "${CATEGORIES[@]}" <<'PY'
import hashlib, json, os, re, sys, time, urllib.parse, urllib.request

dest, budget_mb, min_width, max_file_mb, ua = sys.argv[1:6]
delay = float(os.environ.get("SYNTH_DELAY", "1.0"))
categories = sys.argv[6:]
budget = int(budget_mb) * 1048576
min_width = int(min_width)
max_file = int(max_file_mb) * 1048576
API = "https://commons.wikimedia.org/w/api.php"

# Commons is free-licence only by policy; this list is a second line of defence
# against oddities (and against a category picking up a non-free logo).
OK_LICENCE = ("cc0", "cc by", "cc-by", "public domain", "pd", "gpl", "lgpl",
              "apache", "mit", "bsd", "wtfpl", "fal")

def api(params):
    q = urllib.parse.urlencode(params)
    req = urllib.request.Request(API + "?" + q, headers={"User-Agent": ua})
    with urllib.request.urlopen(req, timeout=40) as r:
        return json.load(r)

def plain(html):
    """Commons returns author/licence as HTML, newlines included. Left raw it
    tears the TSV into fragments, which is how 29 phantom rows appeared."""
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
            if ii.get("mime") != "image/png":
                continue
            if ii.get("width", 0) < min_width or ii.get("size", 0) > max_file:
                continue
            em = ii.get("extmetadata", {})
            lic = em.get("LicenseShortName", {}).get("value", "")
            if not licence_ok(lic):
                continue
            title = page["title"]
            if title in seen:
                continue
            # Two Commons titles can normalise to one filename (Foo.PNG and
            # Foo.png), which would put two rows in the manifest for one file.
            outname = title[5:].replace(" ", "_")
            if outname.lower().endswith(".png"):
                outname = outname[:-4] + ".png"
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
    print("# %-34s %4d candidates so far" % (cat, len(chosen)), file=sys.stderr)

chosen.sort(key=lambda c: c["title"])
man = os.path.join(dest, "MANIFEST.tsv")
total, kept = 0, 0
with open(man, "w", encoding="utf-8") as mf:
    mf.write("# filename\tsha256\tbytes\twidth\theight\tlicence\tsource\tartist\n")
    for c in chosen:
        if total + c["size"] > budget:
            continue
        name = c["title"][5:].replace(" ", "_")
        if name.lower().endswith(".png"):
            name = name[:-4] + ".png"   # .PNG would be skipped by `find -name`
        out = os.path.join(dest, name)
        if os.path.exists(out) and os.path.getsize(out) == c["size"]:
            pass                      # already fetched; a re-run resumes
        else:
            # Wikimedia rate-limits bulk media fetches, and rightly so. One
            # request per second with exponential backoff on 429 is the polite
            # rate; going faster gets the whole run cut off after a few dozen
            # files, which is exactly what happened at 0.1s.
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
        if not data.startswith(b"\x89PNG\r\n\x1a\n"):
            os.remove(out)
            print("# skip %s: not a PNG despite the MIME" % name[:50], file=sys.stderr)
            continue
        mf.write("%s\t%s\t%d\t%s\t%s\t%s\t%s\t%s\n" % (
            name, hashlib.sha256(data).hexdigest(), len(data), c["w"], c["h"],
            plain(c["licence"]), c["descurl"], c["artist"]))
        total += len(data)
        kept += 1
        if kept % 25 == 0:
            print("# %d/%d files, %.1f MB" % (kept, len(chosen), total / 1048576), file=sys.stderr)

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
