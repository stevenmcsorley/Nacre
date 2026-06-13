#!/usr/bin/env python3
"""Generates res/Murmur.svg and src/layout_murmur.hpp."""

W, H = 71.12, 128.5  # 14 HP

# ---------------------------------------------------------------- layout ----
ELED_Y = 13.0
ELED_X0, ELED_DX = 14.0, 6.2   # 8 engine lights (bank shown by color)

MODEL_X, MODEL_Y = 12.0, 26.0
FREQ_X, FREQ_Y = 35.5, 27.0
DECAY_X, DECAY_Y = 59.0, 21.5
LPG_X, LPG_Y = 59.0, 34.0

KB_Y = 47.5
KB_LABEL = 56.4
HARM_X, TIMB_X, MORPH_X = 12.0, 35.5, 59.0

ATT_Y = 64.0
ATT_LABEL = 69.6
FMATT_X = 12.0

R1, R2 = 81.0, 97.0
J6 = [9.0, 19.5, 30.0, 40.5, 51.0, 61.5]

TITLE = (W / 2, 7.0)

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
    'L': [[(0,0),(0,6),(4,6)]],
    'M': [[(0,6),(0,0),(2,3.2),(4,0),(4,6)]],
    'N': [[(0,6),(0,0),(4,6),(4,0)]],
    'O': [[(1,0),(3,0),(4,1),(4,5),(3,6),(1,6),(0,5),(0,1),(1,0)]],
    'P': [[(0,6),(0,0),(3,0),(4,0.9),(4,2.3),(3,3.2),(0,3.2)]],
    'Q': [[(1,0),(3,0),(4,1),(4,5),(3,6),(1,6),(0,5),(0,1),(1,0)], [(2.6,4.4),(4.3,6.4)]],
    'R': [[(0,6),(0,0),(3,0),(4,0.9),(4,2.3),(3,3.2),(0,3.2)], [(2,3.2),(4,6)]],
    'S': [[(4,0.9),(3,0),(1,0),(0,0.9),(0,2.1),(1,3),(3,3),(4,3.9),(4,5.1),(3,6),(1,6),(0,5.1)]],
    'T': [[(0,0),(4,0)], [(2,0),(2,6)]],
    'U': [[(0,0),(0,5),(1,6),(3,6),(4,5),(4,0)]],
    'V': [[(0,0),(2,6),(4,0)]],
    'X': [[(0,0),(4,6)], [(4,0),(0,6)]],
    'Y': [[(0,0),(2,3),(4,0)], [(2,3),(2,6)]],
    '/': [[(0.4,6.4),(3.6,-0.4)]],
    '1': [[(1,1.2),(2.2,0),(2.2,6)], [(1,6),(3.4,6)]],
    '6': [[(3.8,0.8),(2.8,0),(1,0),(0,1),(0,5),(1,6),(3,6),(4,5),(4,3.8),(3,3),(1,3),(0,3.8)]],
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

svg.append(text_paths(*TITLE, "MURMUR", 4.2, ACCENT, weight=0.85))
# murmuration: scattered dots forming a swoosh
import math
for i in range(16):
    a = i / 15.0
    bx = 10 + a * 14
    by = 7.5 - math.sin(a * math.pi) * 3.2
    svg.append(f'<circle cx="{bx:.1f}" cy="{by:.1f}" r="0.45" fill="{ACCENT}" opacity="{0.3+0.4*a:.2f}"/>')
    bx2 = 47 + a * 14
    by2 = 7.5 - math.sin((1-a) * math.pi) * 3.2
    svg.append(f'<circle cx="{bx2:.1f}" cy="{by2:.1f}" r="0.45" fill="{ACCENT}" opacity="{0.7-0.4*a:.2f}"/>')

svg.append(text_paths(MODEL_X, MODEL_Y + 9.0, "MODEL", 1.9, FG))
svg.append(text_paths(FREQ_X, FREQ_Y + 11.2, "FREQ", 2.1, FG))
svg.append(text_paths(DECAY_X, DECAY_Y + 6.6, "DECAY", 1.5, DIM))
svg.append(text_paths(LPG_X, LPG_Y + 6.6, "LPG", 1.5, DIM))
svg.append(text_paths(HARM_X, KB_LABEL, "HARMONICS", 1.7, FG))
svg.append(text_paths(TIMB_X, KB_LABEL, "TIMBRE", 1.9, FG))
svg.append(text_paths(MORPH_X, KB_LABEL, "MORPH", 1.9, FG))
for x, label in zip([FMATT_X, TIMB_X, MORPH_X], ["FM", "TIMBRE", "MORPH"]):
    svg.append(text_paths(x, ATT_LABEL, label, 1.4, DIM))

for x, label in zip(J6, ["MODEL", "HARM", "TIMBRE", "MORPH", "FM", "LEVEL"]):
    svg.append(ring(x, R1, 4.4))
    svg.append(text_paths(x, R1 - 6.4, label, 1.35, DIM))
JR2 = [(J6[0], "V/OCT", 0), (J6[1], "TRIG", 0), (J6[4], "OUT", 1), (J6[5], "AUX", 1)]
for x, label, is_out in JR2:
    svg.append(ring(x, R2, 4.4, ACCENT if is_out else RING))
    svg.append(text_paths(x, R2 - 6.4, label, 1.35, FG if is_out else DIM))
svg.append(text_paths((J6[2]+J6[3])/2, R2, "POLY X16", 1.5, DIM))

svg.append('</svg>')

import os
base = os.path.dirname(os.path.abspath(__file__))
with open(os.path.join(base, "res", "Murmur.svg"), "w") as f:
    f.write("\n".join(svg) + "\n")

hpp = f"""// Generated by gen_murmur.py — do not edit by hand.
#pragma once

namespace ulayout {{
constexpr float ELED_Y = {ELED_Y}f, ELED_X0 = {ELED_X0}f, ELED_DX = {ELED_DX}f;
constexpr float MODEL_X = {MODEL_X}f, MODEL_Y = {MODEL_Y}f;
constexpr float FREQ_X = {FREQ_X}f, FREQ_Y = {FREQ_Y}f;
constexpr float DECAY_X = {DECAY_X}f, DECAY_Y = {DECAY_Y}f;
constexpr float LPG_X = {LPG_X}f, LPG_Y = {LPG_Y}f;
constexpr float KB_Y = {KB_Y}f;
constexpr float HARM_X = {HARM_X}f, TIMB_X = {TIMB_X}f, MORPH_X = {MORPH_X}f;
constexpr float ATT_Y = {ATT_Y}f, FMATT_X = {FMATT_X}f;
constexpr float R1 = {R1}f, R2 = {R2}f;
constexpr float J6[6] = {{{', '.join(f'{x}f' for x in J6)}}};
}}
"""
with open(os.path.join(base, "src", "layout_murmur.hpp"), "w") as f:
    f.write(hpp)

print("wrote res/Murmur.svg and src/layout_murmur.hpp")
