#!/bin/bash
# PXL baseline: size and timings across the dataset. Output is TSV.
# Usage: bench/baseline.sh [-l LEVEL] > bench/baseline_<date>.tsv
export LC_ALL=C
T=${PXLTOOL:-build/pxltool}
LEVEL=12
[ "$1" = "-l" ] && { LEVEL=$2; shift 2; }
D=tests/data

printf "set\tfile\tw\th\tdepth\tctype\tpng\tpxl\tratio\tenc_ms\tdec_ms\tfilter\tstatus\n"

run_one(){
  local set=$1 f=$2
  local o=/tmp/bl.pxl r=/tmp/bl.png
  rm -f $o $r
  local s=$(date +%s%N)
  if ! $T c "$f" $o -l $LEVEL >/dev/null 2>&1; then
    printf "%s\t%s\t\t\t\t\t%d\t\t\t\t\t\tENC_REFUSED\n" "$set" "$(basename "$f")" "$(stat -c%s "$f")"
    return
  fi
  local enc=$(( ($(date +%s%N)-s)/1000000 ))
  s=$(date +%s%N)
  if ! $T d $o $r >/dev/null 2>&1; then
    printf "%s\t%s\t\t\t\t\t%d\t%d\t\t%d\t\t\tDEC_FAIL\n" "$set" "$(basename "$f")" "$(stat -c%s "$f")" "$(stat -c%s $o)" "$enc"
    return
  fi
  local dec=$(( ($(date +%s%N)-s)/1000000 ))
  local png=$(stat -c%s "$f") pxl=$(stat -c%s $o)
  local flt=$($T info $o 2>/dev/null | sed -n 's/.*color filter *: *//p' | cut -d' ' -f1)
  # bit-exact pixel comparison, original vs decoded.
  # Via bench/pngcmp.py rather than PIL: PIL ignores tRNS on grayscale images
  # and truncates 16 bits to 8, which makes correct files look broken.
  local st=OK
  python3 -W ignore "$(dirname "$0")/pngcmp.py" "$f" "$r" >/dev/null 2>&1 || st=MISMATCH
  read w h d c <<<"$(python3 -c "
import struct,sys
x=open('$f','rb').read()
w,h,bd,ct=struct.unpack('>IIBB',x[16:26]); print(w,h,bd,ct)" 2>/dev/null || echo '. . . .')"
  awk -v s="$set" -v f="$(basename "$f")" -v w=$w -v h=$h -v d=$d -v c=$c \
      -v p=$png -v x=$pxl -v e=$enc -v dd=$dec -v fl="$flt" -v st=$st \
      'BEGIN{printf "%s\t%s\t%s\t%s\t%s\t%s\t%d\t%d\t%.2f\t%d\t%d\t%s\t%s\n",s,f,w,h,d,c,p,x,100*x/p,e,dd,fl,st}'
}

for f in $D/Kodak-Lossless-True-Color-Image-Suite/PhotoCD_PCD0992/*.png; do run_one kodak "$f"; done
for f in $D/The-official-test-suite-for-PNG/*.png;                     do run_one pngsuite "$f"; done
for f in $D/*.png;                                                     do run_one misc "$f"; done
