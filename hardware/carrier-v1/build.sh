#!/bin/sh
# Rebuild everything from gen/design.py: schematic, checks, board, routing, fabrication files.
#   ./build.sh            full build, about 8 minutes of autorouting
#   ./build.sh --no-route stop after placement
set -e
cd "$(dirname "$0")"
K=/Applications/KiCad/KiCad.app/Contents
CLI=$K/MacOS/kicad-cli
KPY=$K/Frameworks/Python.framework/Versions/Current/bin/python3
# never write bytecode into KiCad.app: an added .pyc breaks the bundle's code signature
# and macOS then refuses to open KiCad as "damaged"
export PYTHONDONTWRITEBYTECODE=1
mkdir -p out
quiet() { grep -v -i -E 'fontconfig|wxapp|traits' || true; }

python3 gen/gen_sch.py
$CLI sch erc -o out/erc.rpt carrier-v1.kicad_sch 2>&1 | quiet >/dev/null
grep "ERC messages" out/erc.rpt
$CLI sch export netlist --format kicadsexpr -o out/carrier-v1.net carrier-v1.kicad_sch 2>&1 | quiet >/dev/null
python3 gen/check_net.py out/carrier-v1.net
$CLI sch export pdf -o out/carrier-v1-schematic.pdf carrier-v1.kicad_sch 2>&1 | quiet >/dev/null

$KPY gen/gen_pcb.py 2>&1 | quiet
[ "$1" = "--no-route" ] && exit 0
$KPY gen/route.py > out/route.log 2>&1
grep -o -E 'score of [0-9.]+ \([0-9]+ unrouted' out/route.log | tail -1
$KPY gen/stitch.py 2>&1 | quiet
$CLI pcb drc --schematic-parity -o out/drc.rpt carrier-v1.kicad_pcb 2>&1 | quiet

for side in top bottom; do
  $CLI pcb render -o out/render-$side.png -w 1600 -h 1000 --side $side --quality high carrier-v1.kicad_pcb 2>&1 | quiet >/dev/null
done
$CLI pcb render -o out/render-iso.png -w 1600 -h 1000 --rotate "-40,0,-30" --quality high --perspective --zoom 0.9 carrier-v1.kicad_pcb 2>&1 | quiet >/dev/null
python3 gen/jlc.py
