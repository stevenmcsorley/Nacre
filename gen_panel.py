#!/usr/bin/env python3
"""Generates res/Nacre.svg and src/layout.hpp from a single layout definition.

Rack's nanosvg renderer does not support <text>, so labels are drawn with a
simple single-stroke vector font defined below (original glyph data).
"""

W, H = 91.44, 128.5  # 18 HP

# ---------------------------------------------------------------- layout ----
C = [14.5, 35.3, 56.1, 76.9]          # four main columns
BIG_Y = 27.0                          # TIME PITCH SIZE SHAPE
LABEL_BIG_Y = 18.0
ATTN_Y = 41.5                         # attenurandomizers
CV_Y = 53.5                           # grain CV jacks
VOCT_LABEL_Y = 58.9

DENS = (14.5, 74.5)
DENS_LABEL = (14.5, 61.5)
SEED_BTN = (35.3, 74.5)
SEED_LED = (35.3, 68.5)
SEED_LABEL = (35.3, 63.8)

MIX_X = [56.1, 69.0, 81.9]            # FB, D/W, REV
MIX_KNOB_Y = 69.0
MIX_LED_Y = 63.4
MIX_LABEL_Y = 59.8

ROW2_Y = 87.5                         # density CV, seed trig, assign btn, mix CV
ROW2_LABEL_Y = 94.0

IO_Y = 113.5
IO_LABEL_Y = 106.8
IN_L, IN_R = (11.5, IO_Y), (25.0, IO_Y)
OUT_L, OUT_R = (67.5, IO_Y), (81.0, IO_Y)
PEAK_LED = (33.8, 121.0)

QUAL_BTN = (9.0, 10.5)
QUAL_LED = (15.5, 10.5)
QUAL_LABEL = (9.0, 5.0)
FRZ_LED = (75.9, 10.5)
FRZ_BTN = (82.4, 10.5)
FRZ_LABEL = (82.4, 5.0)

TITLE = (W / 2, 9.0)

BG = "#23262c"
FG = "#e8e4da"
DIM = "#8b8f99"
RING = "#3c414b"
ACCENT = "#d9a441"

# ----------------------------------------------------------- stroke font ----
# Each glyph: list of polylines, coords in a 4 (w) x 6 (h) box, y down.
F = {
    'A': [[(0,6),(2,0),(4,6)], [(0.9,3.9),(3.1,3.9)]],
    'B': [[(0,6),(0,0),(3,0),(4,0.8),(4,2.2),(3,3),(0,3)], [(3,3),(4,3.8),(4,5.2),(3,6),(0,6)]],
    'C': [[(4,1),(3,0),(1,0),(0,1),(0,5),(1,6),(3,6),(4,5)]],
    'D': [[(0,0),(0,6)], [(0,0),(2.8,0),(4,1.4),(4,4.6),(2.8,6),(0,6)]],
    'E': [[(4,0),(0,0),(0,6),(4,6)], [(0,3),(2.8,3)]],
    'F': [[(4,0),(0,0),(0,6)], [(0,3),(2.8,3)]],
    'G': [[(4,1),(3,0),(1,0),(0,1),(0,5),(1,6),(3,6),(4,5),(4,3.3),(2.4,3.3)]],
    'H': [[(0,0),(0,6)], [(4,0),(4,6)], [(0,3),(4,3)]],
    'I': [[(2,0),(2,6)]],
    'K': [[(0,0),(0,6)], [(4,0),(0,3.2),(4,6)]],
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
    'W': [[(0,0),(0.9,6),(2,2.4),(3.1,6),(4,0)]],
    'X': [[(0,0),(4,6)], [(4,0),(0,6)]],
    'Y': [[(0,0),(2,3),(4,0)], [(2,3),(2,6)]],
    'Z': [[(0,0),(4,0),(0,6),(4,6)]],
    '/': [[(0.4,6.4),(3.6,-0.4)]],
    ' ': [],
}

def text_paths(x, y, s, h, color, weight=None):
    """Centered single-stroke text. h = cap height in mm."""
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

# ------------------------------------------------------------------ svg -----
svg = []
svg.append(f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}mm" height="{H}mm" viewBox="0 0 {W} {H}">')
svg.append(f'<rect x="0" y="0" width="{W}" height="{H}" fill="{BG}"/>')
svg.append(f'<rect x="0.4" y="0.4" width="{W-0.8}" height="{H-0.8}" fill="none" stroke="#171a1f" stroke-width="0.8"/>')

# section separators
svg.append(f'<path d="M 5 {CV_Y+8.3} L {W-5} {CV_Y+8.3}" stroke="{RING}" stroke-width="0.35"/>')
svg.append(f'<path d="M 5 {ROW2_Y+11.0} L {W-5} {ROW2_Y+11.0}" stroke="{RING}" stroke-width="0.35"/>')

# title
svg.append(text_paths(*TITLE, "NACRE", 5.2, ACCENT, weight=0.95))

# big knob labels
for cx, name in zip(C, ["TIME", "PITCH", "SIZE", "SHAPE"]):
    svg.append(text_paths(cx, LABEL_BIG_Y, name, 2.3, FG))
    svg.append(ring(cx, CV_Y, 4.4))
svg.append(text_paths(C[1], VOCT_LABEL_Y, "V/OCT", 1.5, DIM))

# density / seed
svg.append(text_paths(*DENS_LABEL, "DENSITY", 2.3, FG))
svg.append(text_paths(*SEED_LABEL, "SEED", 2.3, FG))

# mix knobs
for x, name in zip(MIX_X, ["FB", "D/W", "REV"]):
    svg.append(text_paths(x, MIX_LABEL_Y, name, 2.0, FG))

# row 2 jacks
for (x, name) in [(C[0], "CV"), (C[1], "TRIG"), (MIX_X[2], "MIX")]:
    svg.append(ring(x, ROW2_Y, 4.4))
    svg.append(text_paths(x, ROW2_LABEL_Y, name, 1.7, DIM))
svg.append(text_paths(MIX_X[0], ROW2_LABEL_Y, "CV SEL", 1.7, DIM))

# top buttons
svg.append(text_paths(*QUAL_LABEL, "QUALITY", 1.6, DIM))
svg.append(text_paths(*FRZ_LABEL, "FREEZE", 1.6, DIM))

# io
svg.append(text_paths(33.8, 117.0, "LVL", 1.3, DIM))
svg.append(text_paths(18.2, IO_LABEL_Y, "IN", 2.3, FG))
svg.append(text_paths(74.2, IO_LABEL_Y, "OUT", 2.3, FG))
for (x, y), l in [(IN_L, "L"), (IN_R, "R"), (OUT_L, "L"), (OUT_R, "R")]:
    svg.append(ring(x, y, 4.4, ACCENT if x > 45 else RING))
    svg.append(text_paths(x, y + 7.2, l, 1.7, DIM))

svg.append('</svg>')

import os
base = os.path.dirname(os.path.abspath(__file__))
with open(os.path.join(base, "res", "Nacre.svg"), "w") as f:
    f.write("\n".join(svg) + "\n")

# ------------------------------------------------------------ layout.hpp ----
hpp = f"""// Generated by gen_panel.py — do not edit by hand.
#pragma once

namespace layout {{
constexpr float C[4] = {{{C[0]}f, {C[1]}f, {C[2]}f, {C[3]}f}};
constexpr float BIG_Y = {BIG_Y}f;
constexpr float ATTN_Y = {ATTN_Y}f;
constexpr float CV_Y = {CV_Y}f;
constexpr float DENS_X = {DENS[0]}f, DENS_Y = {DENS[1]}f;
constexpr float SEED_BTN_X = {SEED_BTN[0]}f, SEED_BTN_Y = {SEED_BTN[1]}f;
constexpr float SEED_LED_X = {SEED_LED[0]}f, SEED_LED_Y = {SEED_LED[1]}f;
constexpr float MIX_X[3] = {{{MIX_X[0]}f, {MIX_X[1]}f, {MIX_X[2]}f}};
constexpr float MIX_KNOB_Y = {MIX_KNOB_Y}f;
constexpr float MIX_LED_Y = {MIX_LED_Y}f;
constexpr float ROW2_Y = {ROW2_Y}f;
constexpr float IO_Y = {IO_Y}f;
constexpr float IN_L_X = {IN_L[0]}f, IN_R_X = {IN_R[0]}f;
constexpr float OUT_L_X = {OUT_L[0]}f, OUT_R_X = {OUT_R[0]}f;
constexpr float PEAK_X = {PEAK_LED[0]}f, PEAK_Y = {PEAK_LED[1]}f;
constexpr float QUAL_BTN_X = {QUAL_BTN[0]}f, QUAL_BTN_Y = {QUAL_BTN[1]}f;
constexpr float QUAL_LED_X = {QUAL_LED[0]}f, QUAL_LED_Y = {QUAL_LED[1]}f;
constexpr float FRZ_BTN_X = {FRZ_BTN[0]}f, FRZ_BTN_Y = {FRZ_BTN[1]}f;
constexpr float FRZ_LED_X = {FRZ_LED[0]}f, FRZ_LED_Y = {FRZ_LED[1]}f;
}}
"""
with open(os.path.join(base, "src", "layout.hpp"), "w") as f:
    f.write(hpp)

print("wrote res/Nacre.svg and src/layout.hpp")
