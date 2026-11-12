"""Write carrier-v1.kicad_sch from design.py.

The schematic is label style: every connected pin gets a short wire and a net label, and
every unused pin a no-connect flag. Nothing is drawn as a long wire, so the layout cannot
create a connection the netlist does not contain.
"""

import os
import sys
import uuid
from collections import defaultdict

sys.path.insert(0, os.path.dirname(__file__))
import design  # noqa: E402
import sexpr  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.dirname(HERE)
STOCK = "/Applications/KiCad/KiCad.app/Contents/SharedSupport/symbols/"
LIBS = {"carrier": os.path.join(PROJ, "lib", "carrier.kicad_sym")}
for name in ("Device", "Connector", "power"):
    LIBS[name] = STOCK + name + ".kicad_sym"

GRID = 2.54
STUB = 2.54
PAPER_W, PAPER_H = 594.0, 420.0  # A2
MARGIN = 20.0


def uid():
    return str(uuid.uuid4())


def snap(v):
    return round(v / GRID) * GRID


def q(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


_libcache = {}


def symbol(lib_id):
    lib, name = lib_id.split(":", 1)
    if lib not in _libcache:
        _libcache[lib] = sexpr.top_symbols(open(LIBS[lib], encoding="utf-8").read())
    blk = _libcache[lib][name]
    return blk.replace(f'(symbol "{name}"', f'(symbol "{lib_id}"', 1), sexpr.pins(blk)


# outward direction of the wire stub, from the pin's lib angle (y up in the library)
OUT = {0: (-1, 0), 180: (1, 0), 90: (0, 1), 270: (0, -1)}   # schematic coords, y down
LABEL = {(-1, 0): (180, "right bottom"), (1, 0): (0, "left bottom"),
         (0, -1): (90, "left bottom"), (0, 1): (270, "right bottom")}


def global_label(net, x, y, angle):
    """Global, so the net is named NET rather than /NET in the board as well."""
    just = "left" if angle in (0, 90) else "right"
    return (f'(global_label {q(net)} (shape passive) (at {x:.2f} {y:.2f} {angle}) (fields_autoplaced yes) '
            f'(effects (font (size 1.27 1.27)) (justify {just})) (uuid {q(uid())}) '
            f'(property "Intersheetrefs" "${{INTERSHEET_REFS}}" (at {x:.2f} {y:.2f} 0) '
            f'(effects (font (size 1.27 1.27)) (justify {just}) (hide yes))))')


def main():
    root = uid()
    out = []
    lib_blocks = {}
    nets = defaultdict(list)
    placed = []

    # first pass: sizes
    entries = []
    for title, group in design.GROUPS:
        row = []
        for ref, lib_id, fp, value, lcsc, pinmap in group:
            blk, pins = symbol(lib_id)
            lib_blocks[lib_id] = blk
            known = {p[0] for p in pins}
            for pnum in pinmap:
                if pnum not in known:
                    sys.exit(f"{ref}: symbol {lib_id} has no pin {pnum!r} (has {sorted(known)})")
            # extent of the pins plus the stub and label that grows out of each one
            left = right = top = bottom = 0.0
            for num, pname, px, py, ang in pins:
                dx, dy = OUT[ang % 360]
                reach = STUB + len(pinmap.get(num, "")) * 1.05 + 1
                ex, ey = px + dx * reach, -py + dy * reach
                left, right = min(left, px, ex), max(right, px, ex)
                top, bottom = min(top, -py, ey), max(bottom, -py, ey)
            left -= 2; right += 2; top -= 4; bottom += 4
            row.append((ref, lib_id, fp, value, lcsc, pinmap, pins, left, right, top, bottom))
        entries.append((title, row))

    # second pass: place. Groups flow into two columns, each into whichever is shorter,
    # and wrap inside their column.
    colw = (PAPER_W - 3 * MARGIN) / 2
    cols = [[MARGIN, MARGIN], [2 * MARGIN + colw, MARGIN]]   # [x0, y]
    for title, row in entries:
        col = min(cols, key=lambda c: c[1])
        x0, y = col
        out.append(f'(text {q(title)} (exclude_from_sim no) (at {x0} {y + 5} 0) '
                   f'(effects (font (size 3 3) (bold yes)) (justify left bottom)) (uuid {q(uid())}))')
        y += 10
        x = x0
        band_h = 0
        for e in row:
            ref, lib_id, fp, value, lcsc, pinmap, pins, left, right, top, bottom = e
            w, h = right - left, bottom - top
            if x + w > x0 + colw and x > x0:
                x = x0
                y += band_h + 4
                band_h = 0
            ox, oy = snap(x - left), snap(y - top)
            placed.append((ref, lib_id, fp, value, lcsc, pinmap, pins, ox, oy, top, bottom))
            x += w + 3
            band_h = max(band_h, h)
        col[1] = y + band_h + 8
    y = max(c[1] for c in cols)
    if y > PAPER_H:
        print(f"warning: layout runs to y={y:.0f} mm, past the sheet", file=sys.stderr)

    for ref, lib_id, fp, value, lcsc, pinmap, pins, ox, oy, top, bottom in placed:
        props = [
            ("Reference", ref, ox, oy + top + 4, False),
            ("Value", value, ox, oy + bottom - 3, False),
            ("Footprint", fp, ox, oy, True),
            ("Datasheet", "", ox, oy, True),
            ("LCSC", lcsc, ox, oy, True),
        ]
        s = [f'(symbol (lib_id {q(lib_id)}) (at {ox:.2f} {oy:.2f} 0) (unit 1) (exclude_from_sim no) '
             f'(in_bom {"no" if ref.startswith("TP") else "yes"}) (on_board yes) (dnp no) (uuid {q(uid())})']
        for name, val, px, py, hide in props:
            h = " (hide yes)" if hide else ""
            s.append(f'  (property {q(name)} {q(val)} (at {px:.2f} {py:.2f} 0) '
                     f'(effects (font (size 1.27 1.27)){h}))')
        seen = set()
        for num, pname, px, py, ang in pins:
            if num not in seen:
                s.append(f'  (pin {q(num)} (uuid {q(uid())}))')
                seen.add(num)
        s.append(f'  (instances (project "carrier-v1" (path {q("/" + root)} (reference {q(ref)}) (unit 1))))')
        s.append(")")
        out.append("\n".join(s))

        for num, pname, px, py, ang in pins:
            ex, ey = ox + px, oy - py
            net = pinmap.get(num)
            if net is None:
                out.append(f'(no_connect (at {ex:.2f} {ey:.2f}) (uuid {q(uid())}))')
                continue
            dx, dy = OUT[ang % 360]
            sx, sy = ex + dx * STUB, ey + dy * STUB
            out.append(f'(wire (pts (xy {ex:.2f} {ey:.2f}) (xy {sx:.2f} {sy:.2f})) '
                       f'(stroke (width 0) (type default)) (uuid {q(uid())}))')
            la, just = LABEL[(dx, dy)]
            out.append(global_label(net, sx, sy, la))
            nets[net].append(f"{ref}.{num}")

    # power flags, in a row at the bottom
    blk, pins = symbol("power:PWR_FLAG")
    lib_blocks["power:PWR_FLAG"] = blk
    fx = MARGIN
    fy = snap(min(PAPER_H - 15, y + 10))
    for i, net in enumerate(design.POWER_FLAGS):
        ox = snap(fx + i * 25)
        s = (f'(symbol (lib_id "power:PWR_FLAG") (at {ox:.2f} {fy:.2f} 0) (unit 1) (exclude_from_sim no) '
             f'(in_bom no) (on_board no) (dnp no) (uuid {q(uid())})\n'
             f'  (property "Reference" "#FLG0{i + 1}" (at {ox:.2f} {fy - 4:.2f} 0) (effects (font (size 1.27 1.27)) (hide yes)))\n'
             f'  (property "Value" "PWR_FLAG" (at {ox:.2f} {fy - 6:.2f} 0) (effects (font (size 1.27 1.27))))\n'
             f'  (property "Footprint" "" (at {ox:.2f} {fy:.2f} 0) (effects (font (size 1.27 1.27)) (hide yes)))\n'
             f'  (property "Datasheet" "" (at {ox:.2f} {fy:.2f} 0) (effects (font (size 1.27 1.27)) (hide yes)))\n'
             f'  (pin "1" (uuid {q(uid())}))\n'
             f'  (instances (project "carrier-v1" (path {q("/" + root)} (reference "#FLG0{i + 1}") (unit 1)))))')
        out.append(s)
        out.append(f'(wire (pts (xy {ox:.2f} {fy:.2f}) (xy {ox:.2f} {fy + STUB:.2f})) '
                   f'(stroke (width 0) (type default)) (uuid {q(uid())}))')
        out.append(global_label(net, ox, fy + STUB, 270))

    for net, members in sorted(nets.items()):
        if len(members) < 2:
            print(f"warning: net {net} has one connection: {members}", file=sys.stderr)

    head = [f'(kicad_sch (version 20231120) (generator "eeschema") (generator_version "8.0")',
            f'  (uuid {q(root)})', '  (paper "A2")',
            '  (title_block (title "rtlsdr-iphone-bridge carrier v1") (rev "1") '
            '(comment 1 "Generated by gen/gen_sch.py from gen/design.py - edit those, not this file"))',
            "  (lib_symbols"]
    head += ["    " + b for b in lib_blocks.values()]
    head.append("  )")
    body = ["  " + o for o in out]
    tail = ['  (sheet_instances (path "/" (page "1")))', ")"]
    path = os.path.join(PROJ, "carrier-v1.kicad_sch")
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(head + body + tail) + "\n")
    print(f"wrote {path}: {len(placed)} parts, {len(nets)} nets")


if __name__ == "__main__":
    main()
