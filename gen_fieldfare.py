#!/usr/bin/env python3
"""Generates res/Fieldfare.svg and src/layout_fieldfare.hpp."""

W, H = 121.92, 128.5  # 24 HP

# ---------------------------------------------------------------- layout ----
YA = 16.0
YA_LABEL = 22.6
BTN_X = [13.0, 42.0, 71.0, 100.0]

YB = 38.0
YB_LABEL = 48.6
KNOB_X = [16.0, 46.0, 76.0, 106.0]

YC = 62.0
YC_LABEL = 70.6
SPEED_X = 16.0
TR_X = [46.0, 66.0, 86.0, 106.0]

R1, R2 = 92.0, 113.0
J6 = [11.0, 31.0, 51.0, 71.0, 91.0, 111.0]

TITLE = (W / 2, 7.0)

BG = "#23262c"
FG = "#e8e4da"
DIM = "#8b8f99"
RING = "#3c414b"
ACCENT = "#d9a441"

F = {
    'A': [[(0,6),(2,0),(4,6)], [(0.9,3.9),(3.1,3.9)]],
    'C': [[(4,1),(3,0),(1,0),(0,1),(0,5),(1,6),(3,6),(4,5)]],
    'D': [[(0,0),(0,6)], [(0,0),(2.8,0),(4,1.4),(4,4.6),(2.8,6),(0,6)]],
    'E': [[(4,0),(0,0),(0,6),(4,6)], [(0,3),(2.8,3)]],
    'F': [[(4,0),(0,0),(0,6)], [(0,3),(2.8,3)]],
    'G': [[(4,1),(3,0),(1,0),(0,1),(0,5),(1,6),(3,6),(4,5),(4,3.2),(2.4,3.2)]],
    'I': [[(2,0),(2,6)]],
    'K': [[(0,0),(0,6)], [(4,0),(0,3.2),(4,6)]],
    'L': [[(0,0),(0,6),(4,6)]],
    'M': [[(0,6),(0,0),(2,3.2),(4,0),(4,6)]],
    'N': [[(0,6),(0,0),(4,6),(4,0)]],
    'O': [[(1,0),(3,0),(4,1),(4,5),(3,6),(1,6),(0,5),(0,1),(1,0)]],
    'P': [[(0,6),(0,0),(3,0),(4,0.9),(4,2.3),(3,3.2),(0,3.2)]],
    'Q': [[(1,0),(3,0),(4,1),(4,5),(3,6),(1,6),(0,5),(0,1),(1,0)], [(2.6,4.4),(4.3,6.5)]],
    'R': [[(0,6),(0,0),(3,0),(4,0.9),(4,2.3),(3,3.2),(0,3.2)], [(2,3.2),(4,6)]],
    'S': [[(4,0.9),(3,0),(1,0),(0,0.9),(0,2.1),(1,3),(3,3),(4,3.9),(4,5.1),(3,6),(1,6),(0,5.1)]],
    'T': [[(0,0),(4,0)], [(2,0),(2,6)]],
    'U': [[(0,0),(0,5),(1,6),(3,6),(4,5),(4,0)]],
    'V': [[(0,0),(2,6),(4,0)]],
    'X': [[(0,0),(4,6)], [(4,0),(0,6)]],
    'Y': [[(0,0),(2,3),(4,0)], [(2,3),(2,6)]],
    '/': [[(0.4,6.4),(3.6,-0.4)]],
    '1': [[(1,1.2),(2.2,0),(2.2,6)], [(1,6),(3.4,6)]],
    '2': [[(0,1),(1,0),(3,0),(4,1),(4,2),(0,6),(4,6)]],
    '3': [[(0,0.8),(1,0),(3,0),(4,1),(4,2),(3,3),(1.6,3)], [(3,3),(4,4),(4,5),(3,6),(1,6),(0,5.2)]],
    '4': [[(3,6),(3,0),(0,4),(4,4)]],
    ' ': [],
}

def text_paths(x, y, s, h, color, weight=None):
    scale = h / 6.0
    adv = 5.4 * scale
    width = adv * len(s) - 1.4 * scale
    x0 = x - width / 2.0
    sw = weight if weight else max(0.28, h * 0.16)
    out = []
    for i, ch in enumerate(s.upper()):
        gx = x0 + i * adv
        for line in F.get(ch, []):
            pts = " ".join(
                f"{'M' if j == 0 else 'L'} {gx + px * scale:.3f} {y - h / 2.0 + py * scale:.3f}"
                for j, (px, py) in enumerate(line)
            )
            out.append(
                f'<path d="{pts}" fill="none" stroke="{color}" '
                f'stroke-width="{sw:.3f}" stroke-linecap="round" stroke-linejoin="round"/>'
            )
    return "\n".join(out)

def ring(x, y, r, color=RING, sw=0.5):
    return f'<circle cx="{x}" cy="{y}" r="{r}" fill="none" stroke="{color}" stroke-width="{sw}"/>'

svg = []
svg.append(f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}mm" height="{H}mm" viewBox="0 0 {W} {H}">')
svg.append(f'<rect x="0" y="0" width="{W}" height="{H}" fill="{BG}"/>')
svg.append(f'<rect x="0.4" y="0.4" width="{W-0.8}" height="{H-0.8}" fill="none" stroke="#171a1f" stroke-width="0.8"/>')

svg.append(text_paths(*TITLE, "FIELDFARE", 4.2, ACCENT, weight=0.85))
# little bird glyph
for d in ["M 12 8.5 C 14 5.5 17 6 18 8 C 19.5 6 22 6.5 22.5 8.5",
          "M 17.4 8.2 L 17.4 10.2 L 19 11.4"]:
    svg.append(f'<path d="{d}" fill="none" stroke="{ACCENT}" stroke-width="0.55" opacity="0.7"/>')

for x, label in zip(BTN_X, ["ENGINE", "PAGE", "SEQ", "TRACK"]):
    svg.append(text_paths(x, YA_LABEL, label, 1.7, DIM))

# colored knob page legend
for x, label in zip(KNOB_X, ["1", "2", "3", "4"]):
    svg.append(text_paths(x, YB_LABEL, label, 1.7, DIM))
svg.append(text_paths(W / 2, 29.5, "SOUND / ENV / FX / MIX", 1.6, DIM))

svg.append(text_paths(SPEED_X, YC_LABEL, "SPEED", 2.1, FG))
for x, label in zip(TR_X, ["REC", "PLAY", "REV", "LOOP"]):
    svg.append(text_paths(x, YC_LABEL, label, 1.7, DIM))

# tape strip decoration
svg.append(f'<path d="M 28 58 L 36 58" fill="none" stroke="{RING}" stroke-width="2.2"/>')
svg.append(f'<circle cx="28" cy="58" r="3.2" fill="none" stroke="{RING}" stroke-width="1.1"/>')
svg.append(f'<circle cx="36" cy="58" r="3.2" fill="none" stroke="{RING}" stroke-width="1.1"/>')

for x, label, is_out in [(J6[0], "V/OCT", 0), (J6[1], "GATE", 0), (J6[2], "CLOCK", 0),
                         (J6[3], "SPEED", 0), (J6[4], "REC", 0), (J6[5], "PLAY", 0)]:
    svg.append(ring(x, R1, 4.4))
    svg.append(text_paths(x, R1 - 6.4, label, 1.5, DIM))
for x, label, is_out in [(J6[0], "IN L", 0), (J6[1], "IN R", 0), (J6[4], "OUT L", 1), (J6[5], "OUT R", 1)]:
    svg.append(ring(x, R2, 4.4, ACCENT if is_out else RING))
    svg.append(text_paths(x, R2 - 6.4, label, 1.5, FG if is_out else DIM))

svg.append('</svg>')

import os
base = os.path.dirname(os.path.abspath(__file__))
with open(os.path.join(base, "res", "Fieldfare.svg"), "w") as f:
    f.write("\n".join(svg) + "\n")

hpp = f"""// Generated by gen_fieldfare.py — do not edit by hand.
#pragma once

namespace fflayout {{
constexpr float YA = {YA}f;
constexpr float BTN_X[4] = {{{', '.join(f'{x}f' for x in BTN_X)}}};
constexpr float YB = {YB}f;
constexpr float KNOB_X[4] = {{{', '.join(f'{x}f' for x in KNOB_X)}}};
constexpr float YC = {YC}f;
constexpr float SPEED_X = {SPEED_X}f;
constexpr float TR_X[4] = {{{', '.join(f'{x}f' for x in TR_X)}}};
constexpr float R1 = {R1}f, R2 = {R2}f;
constexpr float J6[6] = {{{', '.join(f'{x}f' for x in J6)}}};
}}
"""
with open(os.path.join(base, "src", "layout_fieldfare.hpp"), "w") as f:
    f.write(hpp)

print("wrote res/Fieldfare.svg and src/layout_fieldfare.hpp")
