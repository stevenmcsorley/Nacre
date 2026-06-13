#!/usr/bin/env python3
"""Generates res/Haymaker.svg and src/layout_haymaker.hpp."""

W, H = 121.92, 128.5  # 24 HP

# ---------------------------------------------------------------- layout ----
# pad grid (3 cols x 4 rows), pad 1 top-left
PAD_X = [12.0, 30.0, 48.0]
PAD_Y = [30.0, 48.0, 66.0, 84.0]
PAD_LIGHT_DX, PAD_LIGHT_DY = 6.2, -6.2

# slot knobs
KN_Y = 27.0
KN_LABEL = 35.5
KN_X = [70.0, 84.0, 98.0, 112.0]   # pitch, level, pan, length

# fx buttons
FX_Y = 46.5
FX_LED_Y = 40.8
FX_LABEL = 53.0
FX_X = [68.0, 77.4, 86.8, 96.2, 105.6, 115.0]

# transport / clock knobs
TK_Y = 64.0
TK_LABEL = 72.0
BPM_X, SWING_X, STEPS_X = 70.0, 84.0, 98.0
FADER = (114.0, 72.0)

RUN_X, REC_X = 70.0, 84.0
TR_Y = 84.0
TR_LABEL = 90.5

# step buttons: 2 rows of 8
ST_X0, ST_DX = 11.0, 11.6
ST_Y1, ST_Y2 = 103.0, 112.0
ST_LIGHT_DY = -4.6

# jacks
JK_Y1, JK_Y2 = 103.5, 117.0
CLK_X, RST_X = 102.0, 113.5
OUTL_X, OUTR_X = 102.0, 113.5

TITLE = (W / 2, 7.0)
PADS_LABEL_Y = 19.5

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

svg.append(text_paths(*TITLE, "HAYMAKER", 4.2, ACCENT, weight=0.85))
# boxing-glove-ish starburst
for d in ["M 24 5 L 28 7", "M 24 9 L 28 7", "M 30 7 L 34 7", "M 88 7 L 92 5", "M 88 7 L 92 9", "M 84 7 L 88 7"]:
    svg.append(f'<path d="{d}" fill="none" stroke="{ACCENT}" stroke-width="0.55" opacity="0.6"/>')

svg.append(text_paths(30.0, PADS_LABEL_Y, "PADS", 2.1, FG))
# pad squares
for px in PAD_X:
    for py in PAD_Y:
        svg.append(f'<rect x="{px-6.4}" y="{py-6.4}" width="12.8" height="12.8" rx="1.6" fill="none" stroke="{RING}" stroke-width="0.5"/>')

for x, label in zip(KN_X, ["PITCH", "LEVEL", "PAN", "LEN"]):
    svg.append(text_paths(x, KN_LABEL, label, 1.7, FG))

for x, label in zip(FX_X, ["STUT", "TAPE", "LP", "HP", "CRSH", "REV"]):
    svg.append(text_paths(x, FX_LABEL, label, 1.4, DIM))

svg.append(text_paths(BPM_X, TK_LABEL, "BPM", 1.7, FG))
svg.append(text_paths(SWING_X, TK_LABEL, "SWING", 1.7, FG))
svg.append(text_paths(STEPS_X, TK_LABEL, "STEPS", 1.7, FG))
svg.append(text_paths(FADER[0], 92.0, "FADER", 1.7, FG))

svg.append(text_paths(RUN_X, TR_LABEL, "RUN", 1.6, DIM))
svg.append(text_paths(REC_X, TR_LABEL, "REC", 1.6, DIM))

svg.append(text_paths(ST_X0 + 3.5 * ST_DX + 5.8, ST_Y1 - 8.2, "STEPS", 1.7, FG))

# jacks
svg.append(ring(CLK_X, JK_Y1, 4.4))
svg.append(text_paths(CLK_X, JK_Y1 - 6.4, "CLOCK", 1.5, DIM))
svg.append(ring(RST_X, JK_Y1, 4.4))
svg.append(text_paths(RST_X, JK_Y1 - 6.4, "RESET", 1.5, DIM))
svg.append(ring(OUTL_X, JK_Y2, 4.4, ACCENT))
svg.append(text_paths(OUTL_X - 6.8, JK_Y2, "L", 1.6, FG))
svg.append(ring(OUTR_X, JK_Y2, 4.4, ACCENT))
svg.append(text_paths(OUTR_X + 6.8, JK_Y2, "R", 1.6, FG))

svg.append('</svg>')

import os
base = os.path.dirname(os.path.abspath(__file__))
with open(os.path.join(base, "res", "Haymaker.svg"), "w") as f:
    f.write("\n".join(svg) + "\n")

hpp = f"""// Generated by gen_haymaker.py — do not edit by hand.
#pragma once

namespace hlayout {{
constexpr float PAD_X[3] = {{{', '.join(f'{x}f' for x in PAD_X)}}};
constexpr float PAD_Y[4] = {{{', '.join(f'{y}f' for y in PAD_Y)}}};
constexpr float PAD_LIGHT_DX = {PAD_LIGHT_DX}f, PAD_LIGHT_DY = {PAD_LIGHT_DY}f;
constexpr float KN_Y = {KN_Y}f;
constexpr float KN_X[4] = {{{', '.join(f'{x}f' for x in KN_X)}}};
constexpr float FX_Y = {FX_Y}f, FX_LED_Y = {FX_LED_Y}f;
constexpr float FX_X[6] = {{{', '.join(f'{x}f' for x in FX_X)}}};
constexpr float TK_Y = {TK_Y}f;
constexpr float BPM_X = {BPM_X}f, SWING_X = {SWING_X}f, STEPS_X = {STEPS_X}f;
constexpr float FADER_X = {FADER[0]}f, FADER_Y = {FADER[1]}f;
constexpr float RUN_X = {RUN_X}f, REC_X = {REC_X}f, TR_Y = {TR_Y}f;
constexpr float ST_X0 = {ST_X0}f, ST_DX = {ST_DX}f;
constexpr float ST_Y1 = {ST_Y1}f, ST_Y2 = {ST_Y2}f, ST_LIGHT_DY = {ST_LIGHT_DY}f;
constexpr float JK_Y1 = {JK_Y1}f, JK_Y2 = {JK_Y2}f;
constexpr float CLK_X = {CLK_X}f, RST_X = {RST_X}f;
constexpr float OUTL_X = {OUTL_X}f, OUTR_X = {OUTR_X}f;
}}
"""
with open(os.path.join(base, "src", "layout_haymaker.hpp"), "w") as f:
    f.write(hpp)

print("wrote res/Haymaker.svg and src/layout_haymaker.hpp")
