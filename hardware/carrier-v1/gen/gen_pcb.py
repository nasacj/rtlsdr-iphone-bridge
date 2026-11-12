"""Build carrier-v1.kicad_pcb: outline, stackup, placement, nets and planes.

Run with KiCad's own Python, which carries the pcbnew module:
    /Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/Current/bin/python3 gen/gen_pcb.py

Nets come from KiCad's exported netlist (out/carrier-v1.net), not from design.py, so the
board can only contain what the schematic contains. Routing is a separate step
(route.sh: Specctra DSN -> Freerouting -> SES), and the planes are refilled after it.
"""

import os
import re
import sys

import pcbnew

HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.dirname(HERE)
SS = "/Applications/KiCad/KiCad.app/Contents/SharedSupport/footprints/"
LIBDIRS = {"carrier": os.path.join(PROJ, "lib", "carrier.pretty")}
for lib in ("Resistor_SMD", "Capacitor_SMD", "LED_SMD", "TestPoint", "MountingHole"):
    LIBDIRS[lib] = SS + lib + ".pretty"

W, H = 62.6, 34.6          # board outline, mm. The module's antenna hangs past x = W.

# ref: (x, y, rotation, side). y grows downwards; rotation is counter-clockwise on screen.
PLACE = {
    # RTL-SDR side, left edge: USB-A, its ESD, the switch right behind it
    "J2": (11.0, 16.0, 270), "D2": (16.5, 16.0, 90), "U4": (22.0, 16.4, 90), "C8": (22.0, 18.8, 0),
    "U5": (17.5, 8.5, 90), "R9": (19.8, 8.5, 90), "R10": (21.4, 8.5, 90), "C12": (17.5, 11.2, 0),
    "C13": (15.2, 5.4, 0), "C14": (18.6, 5.4, 0),
    # mode switch and its indicator, top edge
    "SW1": (26.0, 2.6, 180), "R3": (22.4, 5.8, 0), "R4": (32.0, 4.4, 0), "LED4": (32.0, 2.2, 180),
    # 3.3 V buck, top middle, tucked above the module's left-hand pins
    "U6": (34.2, 7.0, 0), "L1": (39.8, 5.8, 0), "C15": (30.8, 7.6, 90), "C16": (39.6, 11.4, 0),
    "C17": (36.2, 11.4, 0), "R11": (33.0, 10.6, 0), "R12": (33.0, 12.2, 0),
    # hub turned so its upstream pins face the USB-C below and port 2 faces the switch
    "U2": (30.0, 20.0, 180), "Y1": (30.0, 15.6, 0), "C2": (33.4, 21.2, 90), "C3": (33.4, 18.4, 90),
    "C4": (34.8, 18.4, 90),
    "J1": (30.0, 30.1, 0), "R1": (26.8, 25.0, 90), "R2": (33.2, 25.0, 90), "C1": (23.6, 31.4, 90),
    # USB to UART, facing the hub's port 1
    "U3": (22.8, 23.6, 0), "C5": (25.4, 21.0, 90), "C6": (20.0, 23.6, 90), "C7": (22.8, 26.6, 0),
    "TP1": (18.6, 29.0, 0), "TP2": (21.0, 30.8, 0),
    # module, antenna over the right edge, LEDs along its top
    "U1": (52.9, 16.0, 270), "C9": (57.6, 4.4, 90), "C10": (60.4, 4.4, 90),
    "R5": (60.4, 1.6, 0), "C11": (57.6, 1.6, 0),
    "LED1": (49.4, 2.4, 270), "R14": (44.6, 2.4, 90), "R8": (45.6, 2.4, 90), "R13": (46.6, 2.4, 90),
    # one row between the module's bottom pins and the buttons: BOOT resistors, C and W lamps
    "R7": (44.6, 26.6, 0), "R6": (46.5, 26.6, 0),
    "R16": (49.0, 26.6, 0), "LED6": (51.5, 26.6, 180),
    "R15": (54.0, 26.6, 0), "LED5": (56.5, 26.6, 180),
    "SW2": (49.5, 31.0, 0), "SW3": (57.5, 31.0, 0),
}
HOLES = [(3.0, 3.0), (3.0, 31.6), (42.0, 32.0)]

# name: (track mm, clearance mm). USB is 90 ohm differential on JLC04161H-7628 by a
# coupled-microstrip estimate (Z0 63, Zdiff 94); check it in JLC's impedance calculator.
# 0.22 mm is also what fits between the 0.5 mm-pitch pads of the switch and the QFNs.
CLASSES = {"Default": (0.2, 0.2), "USB": (0.22, 0.127), "Supply": (0.3, 0.2), "Power": (0.4, 0.2)}


def netclass_of(name):
    if name.endswith("_DP") or name.endswith("_DN"):
        return "USB"
    if name in ("VBUS_SDR", "BUCK_SW"):
        return "Power"
    if name in ("VBUS", "+3V3", "GND"):
        return "Supply"
    return "Default"


EP_VIAS = [("U2", "17"), ("U3", "17"), ("U1", "41")]

# copper roles of the four layers
PLANES = [("In1.Cu", "GND"), ("In2.Cu", "+3V3")]
POURS = [("F.Cu", "GND"), ("B.Cu", "GND")]


def mm(v):
    return pcbnew.FromMM(v)


def pt(x, y):
    return pcbnew.VECTOR2I(mm(x), mm(y))


def read_netlist(path):
    text = open(path, encoding="utf-8").read()
    comps = {}
    for blk in re.split(r'\(comp\s*\(ref', text)[1:]:
        ref = re.match(r'\s*"([^"]+)"', blk).group(1)
        fp = re.search(r'\(footprint "([^"]+)"\)', blk).group(1)
        val = re.search(r'\(value "([^"]*)"\)', blk).group(1)
        lcsc = re.search(r'\(field\s*\(name "LCSC"\)\s*"([^"]*)"\)', blk)
        comps[ref] = (fp, val, lcsc.group(1) if lcsc else "")
    nets = {}
    for blk in re.split(r'\(net\s*\(code', text)[1:]:
        name = re.search(r'\(name "([^"]*)"\)', blk).group(1).lstrip("/")
        for ref, pin in re.findall(r'\(ref "([^"]+)"\)\s*\(pin "([^"]+)"\)', blk):
            nets[(ref, pin)] = name
    return comps, nets


def rect(board, layer, x0, y0, x1, y1, width=0.1):
    for a, b in (((x0, y0), (x1, y0)), ((x1, y0), (x1, y1)), ((x1, y1), (x0, y1)), ((x0, y1), (x0, y0))):
        s = pcbnew.PCB_SHAPE(board)
        s.SetShape(pcbnew.SHAPE_T_SEGMENT)
        s.SetStart(pt(*a))
        s.SetEnd(pt(*b))
        s.SetLayer(layer)
        s.SetWidth(mm(width))
        board.Add(s)


def zone(board, layer_name, net, priority=0, inset=0.3):
    z = pcbnew.ZONE(board)
    z.SetLayer(board.GetLayerID(layer_name))
    z.SetNet(net)
    z.SetAssignedPriority(priority)
    z.SetLocalClearance(mm(0.25))
    z.SetMinThickness(mm(0.2))
    z.SetPadConnection(pcbnew.ZONE_CONNECTION_THERMAL)
    ol = z.Outline()
    ol.NewOutline()
    for x, y in ((inset, inset), (W - inset, inset), (W - inset, H - inset), (inset, H - inset)):
        ol.Append(mm(x), mm(y))
    board.Add(z)
    return z


def antenna_keepout(board):
    """No copper on any layer under the module's antenna end."""
    z = pcbnew.ZONE(board)
    z.SetIsRuleArea(True)
    z.SetDoNotAllowTracks(True)
    z.SetDoNotAllowVias(True)
    z.SetDoNotAllowZoneFills(True)
    z.SetDoNotAllowPads(False)
    z.SetDoNotAllowFootprints(False)
    ls = pcbnew.LSET()
    for name in ("F.Cu", "In1.Cu", "In2.Cu", "B.Cu"):
        ls.AddLayer(board.GetLayerID(name))
    z.SetLayerSet(ls)
    ol = z.Outline()
    ol.NewOutline()
    for x, y in ((W - 0.3, 0), (W + 8, 0), (W + 8, H), (W - 0.3, H)):
        ol.Append(mm(x), mm(y))
    board.Add(z)


def track(board, net, pts, width, layer=pcbnew.F_Cu):
    for a, b in zip(pts, pts[1:]):
        t = pcbnew.PCB_TRACK(board)
        t.SetStart(pt(*a))
        t.SetEnd(pt(*b))
        t.SetWidth(mm(width))
        t.SetLayer(layer)
        t.SetNet(net)
        t.SetLocked(True)   # exported as protected wiring: the router keeps it
        board.Add(t)


def preroute_usb_c(board, netobj):
    """Join the receptacle's two D+ pins and two D- pins, which it interleaves as
    D-, D+, D-, D+ (B7 A6 A7 B6). D+ loops over the top of A7; D- loops under the
    connector body from A7 to B7. Left to the router this was solved differently every
    run, and often not at all."""
    j1 = board.FindFootprintByReference("J1")
    pos = {n: j1.FindPadByNumber(n).GetPosition() for n in ("A6", "B6", "A7", "B7")}
    xy = {n: (pcbnew.ToMM(p.x), pcbnew.ToMM(p.y)) for n, p in pos.items()}
    w = CLASSES["USB"][0]
    y = xy["A6"][1]
    top, under = y - 0.92, y + 0.92   # pads are 1.1 mm tall: 0.2 mm clear of A7 either way
    track(board, netobj["USBC_DP"], [xy["A6"], (xy["A6"][0], top), (xy["B6"][0], top), xy["B6"]], w)
    track(board, netobj["USBC_DN"], [xy["A7"], (xy["A7"][0], under), (xy["B7"][0], under), xy["B7"]], w)


def via(board, net, x, y):
    v = pcbnew.PCB_VIA(board)
    v.SetPosition(pt(x, y))
    v.SetWidth(mm(0.6))
    v.SetDrill(mm(0.3))
    v.SetNet(net)
    v.SetLocked(True)
    board.Add(v)


def pad_xy(board, ref, num):
    p = board.FindFootprintByReference(ref).FindPadByNumber(num).GetPosition()
    return pcbnew.ToMM(p.x), pcbnew.ToMM(p.y)


def preroute_tight_spots(board, netobj):
    """The places every routing run failed in, drawn by hand and locked.

    D2 is a flow-through ESD array: the SDR pair passes over its outer pins, which leaves
    its middle pins (GND, VBUS_SDR) boxed in between the two lines, so each gets a via in
    the gap. The pair then closes from D2's 1.9 mm pitch to U4's 0.5 mm at 45 degrees.
    The receptacle's VBUS pins, U3's ground pin and C6's ground pad get a via or a link
    to the exposed pad, because the router kept running out of room around them.
    """
    usb = CLASSES["USB"][0]
    # J2 -> D2, straight
    track(board, netobj["SDR_DN"], [pad_xy(board, "J2", "2"), pad_xy(board, "D2", "4")], usb)
    track(board, netobj["SDR_DP"], [pad_xy(board, "J2", "3"), pad_xy(board, "D2", "6")], usb)
    # D2's boxed-in middle pins
    gx, gy = pad_xy(board, "D2", "2")
    track(board, netobj["GND"], [(gx, gy), (gx + 1.15, gy)], 0.3)
    via(board, netobj["GND"], gx + 1.15, gy)
    vx, vy = pad_xy(board, "D2", "5")
    track(board, netobj["VBUS_SDR"], [(vx, vy), (vx - 0.75, vy)], 0.3)
    via(board, netobj["VBUS_SDR"], vx - 0.75, vy)
    # D2 -> U4: close the pair at 45 degrees onto U4's left-hand pins
    dn0, dp0 = pad_xy(board, "D2", "3"), pad_xy(board, "D2", "1")
    dn1, dp1 = pad_xy(board, "U4", "7"), pad_xy(board, "U4", "8")
    xa = dn1[0] - 0.92
    track(board, netobj["SDR_DN"], [dn0, (xa - (dn1[1] - dn0[1]), dn0[1]), (xa, dn1[1]), dn1], usb)
    xb = xa - 0.8
    track(board, netobj["SDR_DP"], [dp0, (xb, dp0[1]), (xb + (dp0[1] - dp1[1]), dp1[1]), dp1], usb)
    # the receptacle's two VBUS pins
    for num in ("A4B9", "B4A9"):
        x, y = pad_xy(board, "J1", num)
        track(board, netobj["VBUS"], [(x, y), (x, y - 1.17)], 0.3)
        via(board, netobj["VBUS"], x, y - 1.17)
    # U3 pin 2 is ground, next to its exposed pad
    x, y = pad_xy(board, "U3", "2")
    track(board, netobj["GND"], [(x, y), (x, y - 0.7)], 0.2)
    # U3's right-hand side is HUB1_DN, HUB1_DP, V3 (+3V3), BL_TX in a row, so V3 is boxed
    # in unless the other three fan away from it: the pair up and right, TX down and right,
    # and V3 straight out to a via into the In2 plane.
    x, y = pad_xy(board, "U3", "8")
    track(board, netobj["HUB1_DN"], [(x, y), (x + 0.4, y), (x + 1.0, y - 0.6)], usb)
    x, y = pad_xy(board, "U3", "7")
    track(board, netobj["HUB1_DP"], [(x, y), (x + 0.7, y), (x + 1.3, y - 0.6)], usb)
    x, y = pad_xy(board, "U3", "6")
    track(board, netobj["+3V3"], [(x, y), (x + 1.4, y)], 0.25)
    via(board, netobj["+3V3"], x + 1.4, y)
    x, y = pad_xy(board, "U3", "5")
    track(board, netobj["BL_TX"], [(x, y), (x + 0.55, y), (x + 1.15, y + 0.6)], 0.2)
    # C6's ground pad
    x, y = pad_xy(board, "C6", "2")
    track(board, netobj["GND"], [(x, y), (x, y - 0.95)], 0.3)
    via(board, netobj["GND"], x, y - 0.95)


def main():
    comps, pinnets = read_netlist(os.path.join(PROJ, "out", "carrier-v1.net"))
    board = pcbnew.BOARD()
    board.SetCopperLayerCount(4)
    # In1 is the solid ground every USB pair refers to: marked power, so the DSN export
    # forbids signals on it. In2 is a +3V3 plane that signals may still cross; the fill
    # flows round whatever the router puts there.
    board.SetLayerType(pcbnew.In1_Cu, pcbnew.LT_POWER)
    ds = board.GetDesignSettings()
    ds.SetBoardThickness(mm(1.6))
    ds.m_TrackMinWidth = mm(0.127)
    ds.m_MinClearance = mm(0.127)
    ds.m_ViasMinSize = mm(0.45)
    ds.m_MinThroughDrill = mm(0.3)
    ds.m_CopperEdgeClearance = mm(0.4)

    netobj = {}
    for name in sorted(set(pinnets.values())):
        n = pcbnew.NETINFO_ITEM(board, name)
        board.Add(n)
        netobj[name] = n

    # net classes: what Freerouting routes with and what DRC checks against.
    # The same table is written into carrier-v1.kicad_pro by write_project().
    ns = ds.m_NetSettings
    for cname, (track, clear) in CLASSES.items():
        nc = ns.GetDefaultNetclass() if cname == "Default" else pcbnew.NETCLASS(cname)
        nc.SetTrackWidth(mm(track))
        nc.SetClearance(mm(clear))
        nc.SetViaDiameter(mm(0.6))
        nc.SetViaDrill(mm(0.3))
        if cname == "USB":
            nc.SetDiffPairWidth(mm(track))
            nc.SetDiffPairGap(mm(clear))
        if cname != "Default":
            ns.SetNetclass(cname, nc)
    for name, n in netobj.items():
        cname = netclass_of(name)
        if cname != "Default":
            n.SetNetClass(ns.GetNetClassByName(cname))

    missing = [r for r in comps if r not in PLACE]
    if missing:
        sys.exit(f"no placement for {missing}")
    for ref, (fpid, val, lcsc) in sorted(comps.items()):
        lib, name = fpid.split(":", 1)
        fp = pcbnew.FootprintLoad(LIBDIRS[lib], name)
        if fp is None:
            sys.exit(f"{ref}: footprint {fpid} not found")
        fp.SetFPID(pcbnew.LIB_ID(lib, name))
        fp.SetReference(ref)
        fp.SetValue(val)
        if lcsc:
            f = pcbnew.PCB_FIELD(fp, fp.GetNextFieldOrdinal(), "LCSC")
            f.SetText(lcsc)
            f.SetVisible(False)
            f.SetLayer(pcbnew.F_Fab)
            fp.Add(f)
        x, y, rot = PLACE[ref]
        board.Add(fp)
        fp.SetPosition(pt(x, y))
        fp.SetOrientationDegrees(rot)
        fp.Reference().SetTextSize(pcbnew.VECTOR2I(mm(0.8), mm(0.8)))
        fp.Reference().SetTextThickness(mm(0.12))
        fp.Value().SetVisible(False)
        # reference just above the part, wherever the library put it
        bb = fp.GetBoundingBox(False)
        fp.Reference().SetTextAngleDegrees(0)
        fp.Reference().SetPosition(pcbnew.VECTOR2I(bb.GetCenter().x, bb.GetTop() - mm(0.6)))
        for pad in fp.Pads():
            num = pad.GetNumber()
            if not num and pad.HasHole():
                # a locating peg: the converter writes it as a plated hole with no ring
                pad.SetAttribute(pcbnew.PAD_ATTRIB_NPTH)
                pad.SetLayerSet(pad.UnplatedHoleMask())
                continue
            net = pinnets.get((ref, num))
            if net:
                pad.SetNet(netobj[net])
            elif num and pad.GetAttribute() != pcbnew.PAD_ATTRIB_NPTH:
                print(f"note: {ref} pad {num} unconnected")

    # a ground via in the middle of every exposed pad, so each QFN and the module reach
    # the In1 plane directly rather than through whatever the router manages around them
    for ref, num in EP_VIAS:
        pad = board.FindFootprintByReference(ref).FindPadByNumber(num)
        v = pcbnew.PCB_VIA(board)
        v.SetPosition(pad.GetPosition())
        v.SetWidth(mm(0.6))
        v.SetDrill(mm(0.3))
        v.SetNet(netobj["GND"])
        board.Add(v)

    preroute_usb_c(board, netobj)
    preroute_tight_spots(board, netobj)

    for i, (x, y) in enumerate(HOLES):
        fp = pcbnew.FootprintLoad(LIBDIRS["MountingHole"], "MountingHole_2.2mm_M2")
        fp.SetReference(f"H{i + 1}")
        fp.Reference().SetVisible(False)
        fp.Value().SetVisible(False)
        board.Add(fp)
        fp.SetPosition(pt(x, y))

    rect(board, pcbnew.Edge_Cuts, 0, 0, W, H)
    for i, (layer, net) in enumerate(PLANES):
        zone(board, layer, netobj[net], priority=0)
    # the outer GND pours are added by route.py after routing: present during routing,
    # they make Freerouting count every top-layer GND pad as connected and skip its via.
    antenna_keepout(board)

    out = os.path.join(PROJ, "carrier-v1.kicad_pcb")
    pcbnew.SaveBoard(out, board)
    print(f"wrote {out}: {len(comps)} footprints, {len(netobj)} nets")


if __name__ == "__main__":
    main()
