"""Just enough S-expression handling to lift symbols out of KiCad libraries."""

import re


def block_at(text, start):
    """The balanced (...) block that opens at text[start]."""
    depth, i, n = 0, start, len(text)
    in_str = False
    while i < n:
        ch = text[i]
        if in_str:
            if ch == "\\":
                i += 1
            elif ch == '"':
                in_str = False
        elif ch == '"':
            in_str = True
        elif ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
            if depth == 0:
                return text[start:i + 1]
        i += 1
    raise ValueError("unbalanced block")


def top_symbols(text):
    """name -> block for every top-level symbol in a .kicad_sym file."""
    out = {}
    lib = block_at(text, text.index("(kicad_symbol_lib"))
    i = 1
    while True:
        m = re.compile(r'\(symbol\s+"([^"]+)"').search(lib, i)
        if not m:
            break
        blk = block_at(lib, m.start())
        out[m.group(1)] = blk
        i = m.start() + len(blk)
    return out


PIN = re.compile(r'\(pin\s+\w+\s+\w+\s*\(at\s+([-\d.]+)\s+([-\d.]+)\s*([-\d.]*)\)')


def pins(block):
    """[(number, name, x, y, angle)] for every pin in a symbol block."""
    out = []
    for m in PIN.finditer(block):
        pin = block_at(block, m.start())
        num = re.search(r'\(number\s+"([^"]*)"', pin).group(1)
        name = re.search(r'\(name\s+"([^"]*)"', pin).group(1)
        out.append((num, name, float(m.group(1)), float(m.group(2)), int(float(m.group(3) or 0))))
    return out
