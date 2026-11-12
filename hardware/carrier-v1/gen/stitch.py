"""Connect plane-net pads that the router left floating, with a short track and a via.

Freerouting counts a GND or +3V3 pad as connected when the net has a plane, but an SMD
pad on the top layer only reaches In1/In2 through a via. DRC names those pads; for each
one this looks for the nearest spot where a via and the track to it clear every other
net, and adds them. Run with KiCad's Python after routing:
    python3 gen/stitch.py
"""

import json
import math
import os
import re
import subprocess

import pcbnew

HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.dirname(HERE)
PCB = os.path.join(PROJ, "carrier-v1.kicad_pcb")
CLI = "/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli"
PLANE_NETS = ("GND", "+3V3")
VIA_D, VIA_DRILL, TRACK_W, CLEAR = 0.6, 0.3, 0.3, 0.2


def mm(v):
    return pcbnew.FromMM(v)


def floating_items():
    """(description, x mm, y mm) for each plane-net pad or track DRC calls unconnected."""
    rpt = os.path.join(PROJ, "out", "drc-stitch.json")
    subprocess.run([CLI, "pcb", "drc", "--format", "json", "--units", "mm", "-o", rpt, PCB],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    found = {}
    for v in json.load(open(rpt)).get("unconnected_items", []):
        for it in v.get("items", []):
            d = it.get("description", "")
            m = re.match(r"(Pad \S+|Track) \[([^\]]+)\]", d)
            if m and m.group(2) in PLANE_NETS:
                pos = it.get("pos", {})
                found[(d, round(pos.get("x", 0), 3), round(pos.get("y", 0), 3))] = m.group(2)
    return found


def hole_too_close(board, p, drill, gap=0.5):
    """Any other drilled hole, plated or not, within gap of a new hole at p."""
    for fp in board.GetFootprints():
        for pad in fp.Pads():
            if pad.HasHole():
                d = pad.GetPosition() - p
                r = max(pad.GetDrillSize().x, pad.GetDrillSize().y) / 2
                if math.hypot(d.x, d.y) < r + mm(drill / 2 + gap):
                    return True
    for t in board.GetTracks():
        if t.GetClass() == "PCB_VIA":
            d = t.GetPosition() - p
            if 0 < math.hypot(d.x, d.y) < t.GetDrill() / 2 + mm(drill / 2 + gap):
                return True
    return False


def collides(board, shape, layers, net, clearance):
    for fp in board.GetFootprints():
        for pad in fp.Pads():
            if pad.GetNetCode() == net:
                continue
            for layer in layers:
                if pad.IsOnLayer(layer) and pad.GetEffectiveShape(layer).Collide(shape, clearance):
                    return True
    for t in board.GetTracks():
        if t.GetNetCode() == net:
            continue
        for layer in layers:
            if t.IsOnLayer(layer) and t.GetEffectiveShape(layer).Collide(shape, clearance):
                return True
    return False


def stitch(board, x, y, netname, rmax=4.0):
    """A via within rmax of (x, y) on netname, joined to it by a short top-layer track."""
    net = board.FindNet(netname).GetNetCode()
    c = pcbnew.VECTOR2I(mm(x), mm(y))
    outline = board.GetBoardEdgesBoundingBox()
    shrunk = pcbnew.BOX2I(outline.GetPosition(), outline.GetSize())
    # the box includes half the Edge.Cuts line, so 0.6 from it is 0.55 from the real edge
    shrunk.Inflate(-mm(0.7))
    outer = [pcbnew.F_Cu, board.GetLayerID("In2.Cu"), pcbnew.B_Cu]
    steps = int(rmax * 10)
    for r in [k / 10 for k in range(6, steps + 1)]:
        for k in range(32):
            a = 2 * math.pi * k / 32
            p = pcbnew.VECTOR2I(c.x + mm(r * math.cos(a)), c.y + mm(r * math.sin(a)))
            if not shrunk.Contains(p):
                continue
            via = pcbnew.SHAPE_CIRCLE(p, mm(VIA_D / 2))
            seg = pcbnew.SHAPE_SEGMENT(c, p, mm(0.25))
            if collides(board, via, outer, net, mm(CLEAR)) or hole_too_close(board, p, VIA_DRILL):
                continue
            if collides(board, seg, [pcbnew.F_Cu], net, mm(CLEAR)):
                continue
            t = pcbnew.PCB_TRACK(board)
            t.SetStart(c)
            t.SetEnd(p)
            t.SetWidth(mm(0.25))
            t.SetLayer(pcbnew.F_Cu)
            t.SetNetCode(net)
            board.Add(t)
            v = pcbnew.PCB_VIA(board)
            v.SetPosition(p)
            v.SetWidth(mm(VIA_D))
            v.SetDrill(mm(VIA_DRILL))
            v.SetNetCode(net)
            board.Add(v)
            return r
    return None


def spread_vias(board, min_gap=0.5):
    """Push apart vias of different nets whose drilled holes are closer than min_gap,
    carrying the ends of their tracks along. Freerouting knows copper clearance, not
    hole-to-hole spacing."""
    vias = [t for t in board.GetTracks() if t.GetClass() == "PCB_VIA"]
    moved = 0
    for i, a in enumerate(vias):
        for b in vias[i + 1:]:
            if a.GetNetCode() == b.GetNetCode():
                continue
            d = b.GetPosition() - a.GetPosition()
            dist = math.hypot(d.x, d.y)
            need = mm(min_gap) + (a.GetDrill() + b.GetDrill()) / 2
            if 0 < dist < need:
                push = need - dist + mm(0.02)
                old = b.GetPosition()
                new = pcbnew.VECTOR2I(int(old.x + d.x / dist * push), int(old.y + d.y / dist * push))
                for t in board.GetTracks():
                    if t.GetClass() == "PCB_TRACK" and t.GetNetCode() == b.GetNetCode():
                        if t.GetStart() == old:
                            t.SetStart(new)
                        if t.GetEnd() == old:
                            t.SetEnd(new)
                b.SetPosition(new)
                moved += 1
    return moved


def stitch_grid(board, pitch=2.5):
    """Ground vias on a grid wherever they clear everything: they join every scrap of the
    top and bottom GND pours to the In1 plane, which the pours alone do not guarantee."""
    gnd = board.FindNet("GND").GetNetCode()
    layers = [pcbnew.F_Cu, board.GetLayerID("In2.Cu"), pcbnew.B_Cu]
    edge = board.GetBoardEdgesBoundingBox()
    boxes = []
    for fp in board.GetFootprints():
        bb = fp.GetBoundingBox(False)
        bb.Inflate(mm(0.3))
        boxes.append(bb)
    keep = [z for z in board.Zones() if z.GetIsRuleArea()]
    added = 0
    y = edge.GetTop() + mm(1.5)
    while y < edge.GetBottom() - mm(1.5):
        x = edge.GetLeft() + mm(1.5)
        while x < edge.GetRight() - mm(1.5):
            p = pcbnew.VECTOR2I(x, y)
            # near_via keeps a second run from stacking a via on each one already placed
            if not any(b.Contains(p) for b in boxes) and not any(z.Outline().Contains(p) for z in keep) \
                    and not near_via(board, p, mm(VIA_D)):
                via = pcbnew.SHAPE_CIRCLE(p, mm(VIA_D / 2))
                if not collides(board, via, layers, gnd, mm(CLEAR)) and not hole_too_close(board, p, VIA_DRILL):
                    v = pcbnew.PCB_VIA(board)
                    v.SetPosition(p)
                    v.SetWidth(mm(VIA_D))
                    v.SetDrill(mm(VIA_DRILL))
                    v.SetNetCode(gnd)
                    board.Add(v)
                    added += 1
            x += mm(pitch)
        y += mm(pitch)
    return added


def stitch_islands(board):
    """A via in every piece of an outer GND pour that has none, so no piece is left
    connected to nothing. Pieces are found after a fill."""
    gnd = board.FindNet("GND").GetNetCode()
    layers = [pcbnew.F_Cu, board.GetLayerID("In2.Cu"), pcbnew.B_Cu]
    anchors = [t.GetPosition() for t in board.GetTracks() if t.GetClass() == "PCB_VIA" and t.GetNetCode() == gnd]
    anchors += [p.GetPosition() for fp in board.GetFootprints() for p in fp.Pads()
                if p.GetNetCode() == gnd and p.HasHole()]
    # a pour piece can reach the board edge, and a via there breaks edge clearance
    inside = board.GetBoardEdgesBoundingBox()
    inside.Inflate(-mm(0.7))
    added = 0
    for z in board.Zones():
        if z.GetIsRuleArea() or z.GetNetCode() != gnd:
            continue
        for layer in (pcbnew.F_Cu, pcbnew.B_Cu):
            if not z.IsOnLayer(layer):
                continue
            polys = z.GetFilledPolysList(layer)
            for i in range(polys.OutlineCount()):
                piece = pcbnew.SHAPE_POLY_SET()
                piece.AddOutline(polys.Outline(i))
                if any(piece.Contains(a) for a in anchors):
                    continue
                bb = polys.Outline(i).BBox()
                done = False
                step = mm(0.25)
                y = bb.GetTop()
                while y <= bb.GetBottom() and not done:
                    x = bb.GetLeft()
                    while x <= bb.GetRight() and not done:
                        p = pcbnew.VECTOR2I(x, y)
                        ring = pcbnew.SHAPE_CIRCLE(p, mm(VIA_D / 2))
                        if (inside.Contains(p) and piece.Contains(p) and not collides(board, ring, layers, gnd, mm(CLEAR))
                                and not hole_too_close(board, p, VIA_DRILL)):
                            v = pcbnew.PCB_VIA(board)
                            v.SetPosition(p)
                            v.SetWidth(mm(VIA_D))
                            v.SetDrill(mm(VIA_DRILL))
                            v.SetNetCode(gnd)
                            board.Add(v)
                            anchors.append(p)
                            added += 1
                            done = True
                        x += step
                    y += step
    return added


def near_via(board, p, dist):
    for t in board.GetTracks():
        if t.GetClass() == "PCB_VIA":
            d = t.GetPosition() - p
            if d.x * d.x + d.y * d.y < dist * dist:
                return True
    return False


def prune_dangling(board):
    """Delete track segments with an end that touches nothing: the leftovers where the
    router joined a locked track part-way along it."""
    removed = 0
    while True:
        tracks = [t for t in board.GetTracks() if t.GetClass() == "PCB_TRACK"]
        dead = []
        for t in tracks:
            for end in (t.GetStart(), t.GetEnd()):
                hit = False
                for o in board.GetTracks():
                    if o is t or o.GetNetCode() != t.GetNetCode():
                        continue
                    # an exact duplicate does not count as a connection: Freerouting
                    # sometimes leaves a dead-end stub drawn twice, end to end
                    if o.GetClass() == "PCB_TRACK" and {(o.GetStart().x, o.GetStart().y), (o.GetEnd().x, o.GetEnd().y)} == \
                            {(t.GetStart().x, t.GetStart().y), (t.GetEnd().x, t.GetEnd().y)}:
                        continue
                    if o.GetClass() == "PCB_VIA":
                        if (o.GetPosition() - end).EuclideanNorm() <= o.GetWidth(pcbnew.F_Cu) / 2:
                            hit = True
                            break
                    elif o.IsOnLayer(t.GetLayer()) and o.GetEffectiveShape(t.GetLayer()).Collide(
                            pcbnew.SHAPE_CIRCLE(end, 1), 0):
                        hit = True
                        break
                if not hit:
                    for fp in board.GetFootprints():
                        for pad in fp.Pads():
                            if pad.GetNetCode() == t.GetNetCode() and pad.IsOnLayer(t.GetLayer()) and \
                                    pad.GetEffectiveShape(t.GetLayer()).Collide(pcbnew.SHAPE_CIRCLE(end, 1), 0):
                                hit = True
                                break
                        if hit:
                            break
                if not hit:
                    dead.append(t)
                    break
        if not dead:
            return removed
        for t in dead:
            board.Delete(t)
            removed += 1


def main():
    board = pcbnew.LoadBoard(PCB)
    print(f"stitch: {prune_dangling(board)} dangling segments removed")
    print(f"stitch: {spread_vias(board)} vias pushed apart")
    print(f"stitch: {stitch_grid(board)} ground vias on the grid")
    pcbnew.ZONE_FILLER(board).Fill(board.Zones())
    pcbnew.SaveBoard(PCB, board)
    board = pcbnew.LoadBoard(PCB)
    print(f"stitch: {stitch_islands(board)} vias into isolated pour pieces")
    pcbnew.ZONE_FILLER(board).Fill(board.Zones())
    pcbnew.SaveBoard(PCB, board)
    for attempt in range(3):
        items = floating_items()
        if not items:
            print("stitch: nothing left floating")
            board = pcbnew.LoadBoard(PCB)
            print(f"stitch: {prune_dangling(board)} dangling segments removed at the end")
            pcbnew.ZONE_FILLER(board).Fill(board.Zones())
            pcbnew.SaveBoard(PCB, board)
            return
        board = pcbnew.LoadBoard(PCB)
        for (desc, x, y), net in sorted(items.items()):
            r = stitch(board, x, y, net)
            print(f"stitch: {desc} at {x:.2f},{y:.2f} " + (f"via at {r:.1f} mm" if r else "NO ROOM"))
        pcbnew.ZONE_FILLER(board).Fill(board.Zones())
        pcbnew.SaveBoard(PCB, board)


if __name__ == "__main__":
    main()
