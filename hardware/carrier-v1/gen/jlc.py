"""Write the three files JLCPCB's PCB + SMT order asks for, into out/jlc/.

    gerbers.zip   Gerber and drill files
    bom.csv       Comment, Designator, Footprint, LCSC Part #
    cpl.csv       Designator, Mid X, Mid Y, Layer, Rotation

Plain Python: it drives kicad-cli and reads the board file's footprints as text.
"""

import csv
import os
import re
import shutil
import subprocess
import zipfile
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.dirname(HERE)
CLI = "/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli"
PCB = os.path.join(PROJ, "carrier-v1.kicad_pcb")
OUT = os.path.join(PROJ, "out", "jlc")


def run(*args):
    subprocess.run([CLI, *args], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def footprints():
    """(ref, value, footprint, lcsc) for every placed footprint, from the board file."""
    text = open(PCB, encoding="utf-8").read()
    out = []
    for m in re.finditer(r'\(footprint "([^"]+)"', text):
        start = m.start()
        depth = 0
        for i in range(start, len(text)):
            if text[i] == "(":
                depth += 1
            elif text[i] == ")":
                depth -= 1
                if depth == 0:
                    break
        blk = text[start:i]
        ref = re.search(r'\(property "Reference" "([^"]*)"', blk).group(1)
        val = re.search(r'\(property "Value" "([^"]*)"', blk).group(1)
        lcsc = re.search(r'\(property "LCSC" "([^"]*)"', blk)
        out.append((ref, val, m.group(1).split(":")[-1], lcsc.group(1) if lcsc else ""))
    return out


def main():
    if os.path.isdir(OUT):
        shutil.rmtree(OUT)
    os.makedirs(os.path.join(OUT, "gerbers"))
    g = os.path.join(OUT, "gerbers") + "/"
    layers = "F.Cu,In1.Cu,In2.Cu,B.Cu,F.Paste,B.Paste,F.Silkscreen,B.Silkscreen,F.Mask,B.Mask,Edge.Cuts"
    run("pcb", "export", "gerbers", "--layers", layers, "--subtract-soldermask", "-o", g, PCB)
    run("pcb", "export", "drill", "--format", "excellon", "--excellon-separate-th",
        "--generate-map", "--map-format", "gerberx2", "-o", g, PCB)
    with zipfile.ZipFile(os.path.join(OUT, "gerbers.zip"), "w", zipfile.ZIP_DEFLATED) as z:
        for f in sorted(os.listdir(g)):
            z.write(os.path.join(g, f), f)

    # mounting holes and bare test pads are not parts
    parts = [p for p in footprints() if not p[0].startswith(("H", "TP"))]
    groups = defaultdict(list)
    for ref, val, fp, lcsc in parts:
        groups[(val, fp, lcsc)].append(ref)
    with open(os.path.join(OUT, "bom.csv"), "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["Comment", "Designator", "Footprint", "LCSC Part #"])
        for (val, fp, lcsc), refs in sorted(groups.items(), key=lambda kv: kv[1][0]):
            w.writerow([val, ",".join(sorted(refs, key=lambda r: (re.sub(r"\d", "", r), int(re.sub(r"\D", "", r) or 0)))), fp, lcsc])

    pos = os.path.join(OUT, "pos.csv")
    run("pcb", "export", "pos", "--format", "csv", "--units", "mm", "--side", "both",
        "--exclude-dnp", "-o", pos, PCB)
    keep = {p[0] for p in parts}
    with open(pos, encoding="utf-8") as f, open(os.path.join(OUT, "cpl.csv"), "w", newline="", encoding="utf-8") as o:
        w = csv.writer(o)
        w.writerow(["Designator", "Mid X", "Mid Y", "Layer", "Rotation"])
        for row in csv.DictReader(f):
            if row["Ref"] in keep:
                w.writerow([row["Ref"], row["PosX"] + "mm", row["PosY"] + "mm",
                            "Top" if row["Side"].lower().startswith("top") else "Bottom", row["Rot"]])
    os.remove(pos)
    missing = [p[0] for p in parts if not p[3]]
    print(f"wrote {OUT}: {len(groups)} BOM lines, {len(parts)} placements"
          + (f"; no LCSC number: {missing}" if missing else ""))


if __name__ == "__main__":
    main()
