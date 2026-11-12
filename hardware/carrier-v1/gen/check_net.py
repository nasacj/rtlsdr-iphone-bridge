"""Compare KiCad's exported netlist with design.py, net by net."""
import os, re, sys
sys.path.insert(0, os.path.dirname(__file__))
import design
from collections import defaultdict
want = defaultdict(set)
for ref, lib, fp, val, lcsc, pm in design.parts():
    for p, n in pm.items():
        want[n].add(f"{ref}.{p}")
text = open(sys.argv[1]).read()
got = {}
for blk in re.split(r'\(net\s*\(code', text)[1:]:
    name = re.search(r'\(name "([^"]*)"\)', blk).group(1).lstrip("/")
    nodes = set(f"{a}.{b}" for a, b in re.findall(r'\(ref "([^"]+)"\)\s*\(pin "([^"]+)"\)', blk))
    nodes = {x for x in nodes if not x.startswith("#")}
    if nodes:
        got[name] = nodes
bad = 0
for n, nodes in want.items():
    g = got.get(n)
    if g != nodes:
        bad += 1; print("MISMATCH", n, "want", sorted(nodes), "got", sorted(g or []))
for n, nodes in got.items():
    if n not in want and len(nodes) > 1:
        bad += 1; print("EXTRA", n, sorted(nodes))
print(f"{len(want)} nets checked, {bad} differences")
