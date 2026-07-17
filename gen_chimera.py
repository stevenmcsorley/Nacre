#!/usr/bin/env python3
"""Generates res/Chimera.svg and src/layout_chimera.hpp."""

W, H = 111.76, 128.5  # 22 HP

# ---------------------------------------------------------------- layout ----
YA = 15.0                              # transport buttons
YA_LED_DY = -5.4
YA_LABEL = 21.6
BTN_X = [12.0, 26.0, 40.0, 54.0, 68.0] # rec, play, splice, sync, mode

YB = 32.5                              # sos + eq row
YB_LABEL = 40.8
SOS_X = 14.0
EQ_X = [56.0, 76.0, 96.0]              # low, mid, high
EQ_SEP_X = 42.0

YC = 53.0                              # main knobs
YC_LABEL = 64.0
SPEED_X, PITCH_X = 16.0, 42.0
GRAIN_X, SLICE_X, CHOP_X = 66.0, 86.0, 103.0

YD = 72.5                              # scatter + reel display
YD_LABEL = 80.6
SCATTER_X = 16.0
DISP_X, DISP_Y = 28.5, 66.5            # reel display (waveform / slices / playhead)
DISP_W, DISP_H = 78.0, 15.0
CLK_LIGHT = (106.6, 90.0)              # beside the clock jack

R1, R2, R3 = 90.0, 104.0, 118.0
J6 = [10.0, 28.0, 46.0, 64.0, 82.0, 100.0]

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
    'G': [[(4,1),(3,0),(1,0),(0,1),(0,5),(1,6),(3,6),(4,5),(4,3.2),(2.4,3.2)]],
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

svg.append(text_paths(*TITLE, "CHIMERA", 4.4, ACCENT, weight=0.85))

for x, label in zip(BTN_X, ["REC", "PLAY", "SPLICE", "SYNC", "MODE"]):
    svg.append(text_paths(x, YA_LABEL, label, 1.5, DIM))

svg.append(text_paths(SOS_X, YB_LABEL, "SOS", 1.8, FG))
svg.append(f'<path d="M {EQ_SEP_X} {YB-7.5} L {EQ_SEP_X} {YB+8.5}" stroke="{RING}" stroke-width="0.4"/>')
svg.append(text_paths(EQ_SEP_X + 3.6, YB - 6.0, "EQ", 1.6, DIM))
for x, label in zip(EQ_X, ["LOW", "MID", "HIGH"]):
    svg.append(text_paths(x, YB_LABEL, label, 1.7, FG))

svg.append(text_paths(SPEED_X, YC_LABEL, "SPEED", 2.3, FG))
svg.append(text_paths(PITCH_X, YC_LABEL, "PITCH", 2.3, FG))
svg.append(text_paths(GRAIN_X, YC_LABEL, "GRAIN", 2.1, FG))
svg.append(text_paths(SLICE_X, YC_LABEL, "SLICE", 2.1, FG))
svg.append(text_paths(CHOP_X, YC_LABEL, "CHOP", 1.6, FG))

svg.append(text_paths(SCATTER_X, YD_LABEL, "SCATTER", 2.1, FG))
# recessed frame around the reel display
svg.append(f'<rect x="{DISP_X-0.8}" y="{DISP_Y-0.8}" width="{DISP_W+1.6}" height="{DISP_H+1.6}" rx="1.2" fill="#14161a" stroke="{RING}" stroke-width="0.5"/>')

JACKS = [
    (J6[0], R1, "SPEED", 0), (J6[1], R1, "V/OCT", 0), (J6[2], R1, "GRAIN", 0),
    (J6[3], R1, "SLICE", 0), (J6[4], R1, "SCAT", 0), (J6[5], R1, "CLOCK", 0),
    (J6[0], R2, "REC", 0), (J6[1], R2, "SPLICE", 0), (J6[2], R2, "IN L", 0),
    (J6[3], R2, "IN R", 0), (J6[4], R2, "EOS", 1), (J6[5], R2, "ENV", 1),
    (J6[4], R3, "OUT L", 1), (J6[5], R3, "OUT R", 1),
]
for x, y, label, is_out in JACKS:
    svg.append(ring(x, y, 4.4, ACCENT if is_out else RING))
    svg.append(text_paths(x, y - 6.4, label, 1.5, FG if is_out else DIM))
svg.append('</svg>')

import os
base = os.path.dirname(os.path.abspath(__file__))
with open(os.path.join(base, "res", "Chimera.svg"), "w") as f:
    f.write("\n".join(svg) + "\n")

hpp = f"""// Generated by gen_chimera.py — do not edit by hand.
#pragma once

namespace clayout {{
constexpr float YA = {YA}f, YA_LED_DY = {YA_LED_DY}f;
constexpr float BTN_X[5] = {{{', '.join(f'{x}f' for x in BTN_X)}}};
constexpr float SOS_X = {SOS_X}f;
constexpr float EQ_X[3] = {{{', '.join(f'{x}f' for x in EQ_X)}}};
constexpr float YB = {YB}f;
constexpr float SPEED_X = {SPEED_X}f, PITCH_X = {PITCH_X}f;
constexpr float GRAIN_X = {GRAIN_X}f, SLICE_X = {SLICE_X}f, CHOP_X = {CHOP_X}f;
constexpr float YC = {YC}f;
constexpr float YD = {YD}f;
constexpr float SCATTER_X = {SCATTER_X}f;
constexpr float DISP_X = {DISP_X}f, DISP_Y = {DISP_Y}f, DISP_W = {DISP_W}f, DISP_H = {DISP_H}f;
constexpr float CLK_LIGHT_X = {CLK_LIGHT[0]}f, CLK_LIGHT_Y = {CLK_LIGHT[1]}f;
constexpr float R1 = {R1}f, R2 = {R2}f, R3 = {R3}f;
constexpr float J6[6] = {{{', '.join(f'{x}f' for x in J6)}}};
}}
"""
with open(os.path.join(base, "src", "layout_chimera.hpp"), "w") as f:
    f.write(hpp)

print("wrote res/Chimera.svg and src/layout_chimera.hpp")
