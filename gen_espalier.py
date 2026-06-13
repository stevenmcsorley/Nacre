#!/usr/bin/env python3
"""Generates res/Espalier.svg and src/layout_espalier.hpp."""

W, H = 91.44, 128.5  # 18 HP

# ---------------------------------------------------------------- layout ----
J8 = [7.5, 18.7, 29.9, 41.1, 52.3, 63.5, 74.7, 85.9]

YA = 15.0
YA_LABEL = 21.8

YB = 31.5
YB_LABEL = 40.2
GROW_X, ROUTE_X, EVOLVE_X = 27.0, 46.7, 66.4

YM = 47.5            # knob-mode btn (left), tune-mode btn (right), page lights
YM_LABEL = 53.4
PAGE_LIGHT_X0 = 26.0
PAGE_LIGHT_DX = 5.7
PAGE_LIGHT_Y = 47.5

SC = [14.5, 35.3, 56.1, 76.9]   # step knob columns
SY1, SY2 = 64.0, 81.5
SHIFT_LABEL_DY = -8.6
STEP_LIGHT_DX, STEP_LIGHT_DY = -7.8, -4.5
STEP_BTN_DX, STEP_BTN_DY = 7.8, 4.8

R1, R2 = 99.0, 115.5

TITLE = (W / 2, 6.5)

BG = "#23262c"
FG = "#e8e4da"
DIM = "#8b8f99"
RING = "#3c414b"
ACCENT = "#d9a441"

F = {
    'A': [[(0,6),(2,0),(4,6)], [(0.9,3.9),(3.1,3.9)]],
    'B': [[(0,6),(0,0),(3,0),(4,0.8),(4,2.2),(3,3),(0,3)], [(3,3),(4,3.8),(4,5.2),(3,6),(0,6)]],
    'C': [[(4,1),(3,0),(1,0),(0,1),(0,5),(1,6),(3,6),(4,5)]],
    'D': [[(0,0),(0,6)], [(0,0),(2.8,0),(4,1.4),(4,4.6),(2.8,6),(0,6)]],
    'E': [[(4,0),(0,0),(0,6),(4,6)], [(0,3),(2.8,3)]],
    'F': [[(4,0),(0,0),(0,6)], [(0,3),(2.8,3)]],
    'G': [[(4,1),(3,0),(1,0),(0,1),(0,5),(1,6),(3,6),(4,5),(4,3.2),(2.4,3.2)]],
    'H': [[(0,0),(0,6)], [(4,0),(4,6)], [(0,3),(4,3)]],
    'I': [[(2,0),(2,6)]],
    'K': [[(0,0),(0,6)], [(4,0),(0,3.2),(4,6)]],
    'L': [[(0,0),(0,6),(4,6)]],
    'M': [[(0,6),(0,0),(2,3.2),(4,0),(4,6)]],
    'N': [[(0,6),(0,0),(4,6),(4,0)]],
    'O': [[(1,0),(3,0),(4,1),(4,5),(3,6),(1,6),(0,5),(0,1),(1,0)]],
    'P': [[(0,6),(0,0),(3,0),(4,0.9),(4,2.3),(3,3.2),(0,3.2)]],
    'R': [[(0,6),(0,0),(3,0),(4,0.9),(4,2.3),(3,3.2),(0,3.2)], [(2,3.2),(4,6)]],
    'S': [[(4,0.9),(3,0),(1,0),(0,0.9),(0,2.1),(1,3),(3,3),(4,3.9),(4,5.1),(3,6),(1,6),(0,5.1)]],
    'T': [[(0,0),(4,0)], [(2,0),(2,6)]],
    'U': [[(0,0),(0,5),(1,6),(3,6),(4,5),(4,0)]],
    'V': [[(0,0),(2,6),(4,0)]],
    'W': [[(0,0),(0.9,6),(2,2.4),(3.1,6),(4,0)]],
    'X': [[(0,0),(4,6)], [(4,0),(0,6)]],
    'Y': [[(0,0),(2,3),(4,0)], [(2,3),(2,6)]],
    'Z': [[(0,0),(4,0),(0,6),(4,6)]],
    '1': [[(1,1.2),(2.2,0),(2.2,6)], [(1,6),(3.4,6)]],
    '2': [[(0,1),(1,0),(3,0),(4,1),(4,2),(0,6),(4,6)]],
    '3': [[(0,0.8),(1,0),(3,0),(4,1),(4,2),(3,3),(1.6,3)], [(3,3),(4,4),(4,5),(3,6),(1,6),(0,5.2)]],
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

svg.append(text_paths(*TITLE, "ESPALIER", 4.2, ACCENT, weight=0.85))
# trained-tree glyph: trunk with right-angle branches
for d in [
    "M 12 9.5 L 22 9.5", "M 14.5 9.5 L 14.5 5.5 L 18 5.5", "M 19 9.5 L 19 4.5 L 21.5 4.5",
    "M 69.5 9.5 L 79.5 9.5", "M 77 9.5 L 77 5.5 L 73.5 5.5", "M 72.5 9.5 L 72.5 4.5 L 70 4.5",
]:
    svg.append(f'<path d="{d}" fill="none" stroke="{ACCENT}" stroke-width="0.55" opacity="0.65"/>')

# row A
svg.append(text_paths(13.1, YA_LABEL, "ROOT", 1.8, FG))
svg.append(text_paths(35.5, YA_LABEL, "RESEED", 1.8, DIM))
svg.append(text_paths(57.9, YA_LABEL, "RESET", 1.8, DIM))
svg.append(text_paths(80.3, YA_LABEL, "RATE", 1.8, FG))

# row B
svg.append(text_paths(J8[0], YB_LABEL, "CHAN", 1.6, DIM))
svg.append(text_paths(GROW_X, YB_LABEL, "GROW", 2.1, FG))
svg.append(text_paths(ROUTE_X, YB_LABEL, "ROUTE", 2.1, FG))
svg.append(text_paths(EVOLVE_X, YB_LABEL, "EVOLVE", 2.1, FG))
svg.append(text_paths(J8[7], YB_LABEL, "PAGE", 1.6, DIM))

# mode row
svg.append(text_paths(J8[0], YM_LABEL, "KNOB", 1.5, DIM))
svg.append(text_paths(J8[7], YM_LABEL, "TUNE", 1.5, DIM))

# step grid shift labels
for x, label in zip(SC, ["LEN", "SCALE", "ORDER", "PATT"]):
    svg.append(text_paths(x, SY1 + SHIFT_LABEL_DY, label, 1.5, DIM))
for x, label in zip(SC, ["RATIO", "SIZE", "ROT", "TRANS"]):
    svg.append(text_paths(x, SY2 + SHIFT_LABEL_DY, label, 1.5, DIM))
# step numbers
for i, x in enumerate(SC):
    svg.append(text_paths(x + STEP_BTN_DX, SY1 + STEP_BTN_DY + 4.6, f"{i+1}", 1.5, DIM))
    svg.append(text_paths(x + STEP_BTN_DX, SY2 + STEP_BTN_DY + 4.6, f"{i+5}", 1.5, DIM))

# jacks
JACKS = [
    (J8[0], R1, "CLOCK", 0), (J8[1], R1, "GROW", 0), (J8[2], R1, "ROUTE", 0), (J8[3], R1, "EVOLVE", 0),
    (J8[4], R1, "GATE 1", 1), (J8[5], R1, "GATE 2", 1), (J8[6], R1, "GATE 3", 1), (J8[7], R1, "CLK", 1),
    (J8[1], R2, "MOD 1", 1), (J8[2], R2, "MOD 2", 1), (J8[3], R2, "MOD 3", 1),
    (J8[4], R2, "NOTE 1", 1), (J8[5], R2, "NOTE 2", 1), (J8[6], R2, "NOTE 3", 1),
]
for x, y, label, is_out in JACKS:
    svg.append(ring(x, y, 4.4, ACCENT if is_out else RING))
    svg.append(text_paths(x, y - 6.4, label, 1.5, FG if is_out else DIM))
svg.append(text_paths(J8[0], R2 - 6.4, "SHIFT", 1.5, DIM))
# reseed/reset/root/rate jacks in row A get rings too
for x in [18.7, 41.1, 52.3, 74.7]:
    svg.append(ring(x, YA, 4.4))

svg.append('</svg>')

import os
base = os.path.dirname(os.path.abspath(__file__))
with open(os.path.join(base, "res", "Espalier.svg"), "w") as f:
    f.write("\n".join(svg) + "\n")

hpp = f"""// Generated by gen_espalier.py — do not edit by hand.
#pragma once

namespace elayout {{
constexpr float J8[8] = {{{', '.join(f'{x}f' for x in J8)}}};
constexpr float YA = {YA}f;
constexpr float YB = {YB}f;
constexpr float GROW_X = {GROW_X}f, ROUTE_X = {ROUTE_X}f, EVOLVE_X = {EVOLVE_X}f;
constexpr float YM = {YM}f;
constexpr float PAGE_LIGHT_X0 = {PAGE_LIGHT_X0}f, PAGE_LIGHT_DX = {PAGE_LIGHT_DX}f, PAGE_LIGHT_Y = {PAGE_LIGHT_Y}f;
constexpr float SC[4] = {{{', '.join(f'{x}f' for x in SC)}}};
constexpr float SY1 = {SY1}f, SY2 = {SY2}f;
constexpr float STEP_LIGHT_DX = {STEP_LIGHT_DX}f, STEP_LIGHT_DY = {STEP_LIGHT_DY}f;
constexpr float STEP_BTN_DX = {STEP_BTN_DX}f, STEP_BTN_DY = {STEP_BTN_DY}f;
constexpr float R1 = {R1}f, R2 = {R2}f;
}}
"""
with open(os.path.join(base, "src", "layout_espalier.hpp"), "w") as f:
    f.write(hpp)

print("wrote res/Espalier.svg and src/layout_espalier.hpp")
