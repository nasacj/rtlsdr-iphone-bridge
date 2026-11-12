"""Route carrier-v1.kicad_pcb with Freerouting and refill the planes.

Run with KiCad's Python, after gen_pcb.py. The board is exported as a Specctra DSN,
routed headless, and the session file imported back, so the routing step is repeatable
from a freshly placed board.
"""

import os
import subprocess
import sys

import pcbnew

HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.dirname(HERE)
TOOLS = os.path.join(os.path.dirname(PROJ), "tools")
JAVA = os.path.join(TOOLS, "jre", "Contents", "Home", "bin", "java")
JAR = os.path.join(TOOLS, "freerouting.jar")
PCB = os.path.join(PROJ, "carrier-v1.kicad_pcb")
OUT = os.path.join(PROJ, "out")


def route_once(dsn, ses, passes):
    if os.path.exists(ses):
        os.remove(ses)
    subprocess.run([JAVA, "-jar", JAR, "-de", dsn, "-do", ses, "-mp", passes,
                    "--gui.enabled=false"], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return os.path.exists(ses)


def unrouted(board):
    pcbnew.ZONE_FILLER(board).Fill(board.Zones())
    conn = board.GetConnectivity()
    conn.RecalculateRatsnest()
    return conn.GetUnconnectedCount(False)


def main():
    """Freerouting is not deterministic, so route a few times from the same placement
    and keep the run that leaves the fewest connections open."""
    passes = sys.argv[1] if len(sys.argv) > 1 else "40"
    tries = int(sys.argv[2]) if len(sys.argv) > 2 else 2
    board = pcbnew.LoadBoard(PCB)
    pcbnew.ZONE_FILLER(board).Fill(board.Zones())
    dsn = os.path.join(OUT, "carrier-v1.dsn")
    if not pcbnew.ExportSpecctraDSN(board, dsn):
        sys.exit("DSN export failed")
    best = None
    for i in range(tries):
        ses = os.path.join(OUT, f"carrier-v1-{i}.ses")
        if not route_once(dsn, ses, passes):
            print(f"try {i}: no session file")
            continue
        b = pcbnew.LoadBoard(PCB)
        if not pcbnew.ImportSpecctraSES(b, ses):
            print(f"try {i}: SES import failed")
            continue
        n = unrouted(b)
        print(f"try {i}: {n} connections open")
        if best is None or n < best[0]:
            best = (n, ses)
        if n == 0:
            break
    if best is None:
        sys.exit("no routing run succeeded")
    board = pcbnew.LoadBoard(PCB)
    pcbnew.ImportSpecctraSES(board, best[1])
    sys.path.insert(0, HERE)
    import gen_pcb
    for layer, net in gen_pcb.POURS:
        gen_pcb.zone(board, layer, board.FindNet(net), priority=0)
    pcbnew.ZONE_FILLER(board).Fill(board.Zones())
    pcbnew.SaveBoard(PCB, board)
    print(f"routed {PCB}: kept the run with {best[0]} open, {len(board.GetTracks())} track segments and vias")


if __name__ == "__main__":
    main()
