#!/usr/bin/env python3
"""Generates res/Tektite.svg and src/layout_tektite.hpp."""

W, H = 91.44, 128.5  # 18 HP

# ---------------------------------------------------------------- layout ----
J8 = [7.5, 18.7, 29.9, 41.1, 52.3, 63.5, 74.7, 85.9]

YA = 14.0
YA_LED = 8.3
YA_LABEL = 21.2
FLUT_X = 7.5
FRZ_X, LOOP_X = 20.0, 32.0
VARI = (45.7, 19.0)
FX_X, UNDO_X = 59.4, 71.4
HISS_X = 85.9

YB = 35.0
YB_LABEL = 43.5
ERASE_X, START_X, INERTIA_X, SIZE_X, MIX_X = 7.5, 21.0, 45.7, 70.4, 85.9
INERTIA_Y = 36.5

YC = 52.0
YC_LABEL = 60.2
LVL_X, SKIP_X, SLICE_X = 7.5, 26.0, 65.4

YD = 66.5
YD_LABEL = 73.0
TR_X = [20.5, 37.0, 53.5, 70.0]   # record, play, reset, reverse

R1, R2, R3 = 84.0, 99.0, 114.0

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

svg.append(text_paths(*TITLE, "TEKTITE", 4.2, ACCENT, weight=0.85))
# little falling-star streaks
for (x1, y1, x2, y2, op) in [(12, 4.5, 17, 8.5, 0.6), (15.5, 3.8, 19, 6.6, 0.4),
                              (74.5, 8.5, 79.5, 4.5, 0.6), (72.5, 6.6, 76, 3.8, 0.4)]:
    svg.append(f'<path d="M {x1} {y1} L {x2} {y2}" stroke="{ACCENT}" stroke-width="0.5" opacity="{op}"/>')
    svg.append(f'<circle cx="{x2}" cy="{y2}" r="0.7" fill="{ACCENT}" opacity="{op}"/>')

# row A
svg.append(text_paths(FLUT_X, YA_LABEL, "FLUTTER", 1.6, FG))
svg.append(text_paths(FRZ_X, YA_LABEL, "FRZ", 1.6, DIM))
svg.append(text_paths(LOOP_X, YA_LABEL, "LOOP", 1.6, DIM))
svg.append(text_paths(VARI[0], 29.5, "VARISPEED", 2.1, FG))
svg.append(text_paths(FX_X, YA_LABEL, "FX", 1.6, DIM))
svg.append(text_paths(UNDO_X, YA_LABEL, "UNDO", 1.6, DIM))
svg.append(text_paths(HISS_X, YA_LABEL, "HISS", 1.6, FG))

# row B
svg.append(text_paths(ERASE_X, YB_LABEL, "ERASE", 1.6, DIM))
svg.append(text_paths(START_X, YB_LABEL, "START", 2.1, FG))
svg.append(text_paths(INERTIA_X, INERTIA_Y + 7.2, "INERTIA", 1.6, FG))
svg.append(text_paths(SIZE_X, YB_LABEL, "SIZE", 2.1, FG))
svg.append(text_paths(MIX_X, YB_LABEL, "MIX", 2.1, FG))

# row C
svg.append(text_paths(LVL_X, YC_LABEL, "LEVEL", 2.1, FG))
svg.append(text_paths(SKIP_X, YC_LABEL, "SKIP", 2.1, FG))
svg.append(text_paths(SLICE_X, YC_LABEL, "SLICE", 2.1, FG))

# transport
for x, label in zip(TR_X, ["REC", "PLAY", "RESET", "REV"]):
    svg.append(text_paths(x, YD_LABEL, label, 1.6, DIM))

# jacks
JACKS_R1 = ["LEVEL", "FLUT", "HISS", "MIX", "START", "SIZE", "SLICE", "SKIP"]
for x, label in zip(J8, JACKS_R1):
    svg.append(ring(x, R1, 4.4))
    svg.append(text_paths(x, R1 - 6.4, label, 1.5, DIM))
JACKS_R2 = [("REC", 0), ("PLAY", 0), ("RESET", 0), ("REV", 0), ("FRZ", 0), ("CLOCK", 0), ("V/OCT", 0), ("NOVA", 1)]
for x, (label, is_out) in zip(J8, JACKS_R2):
    svg.append(ring(x, R2, 4.4, ACCENT if is_out else RING))
    svg.append(text_paths(x, R2 - 6.4, label, 1.5, FG if is_out else DIM))
JACKS_R3 = [(J8[0], "IN L", 0), (J8[1], "IN R", 0), (J8[6], "OUT L", 1), (J8[7], "OUT R", 1)]
for x, label, is_out in JACKS_R3:
    svg.append(ring(x, R3, 4.4, ACCENT if is_out else RING))
    svg.append(text_paths(x, R3 - 6.4, label, 1.5, FG if is_out else DIM))
# center deco star
svg.append(f'<circle cx="{(J8[3]+J8[4])/2}" cy="{R3}" r="1.2" fill="{ACCENT}" opacity="0.5"/>')

svg.append('</svg>')

import os
base = os.path.dirname(os.path.abspath(__file__))
with open(os.path.join(base, "res", "Tektite.svg"), "w") as f:
    f.write("\n".join(svg) + "\n")

hpp = f"""// Generated by gen_tektite.py — do not edit by hand.
#pragma once

namespace tlayout {{
constexpr float J8[8] = {{{', '.join(f'{x}f' for x in J8)}}};
constexpr float YA = {YA}f, YA_LED = {YA_LED}f;
constexpr float FLUT_X = {FLUT_X}f, FRZ_X = {FRZ_X}f, LOOP_X = {LOOP_X}f;
constexpr float VARI_X = {VARI[0]}f, VARI_Y = {VARI[1]}f;
constexpr float FX_X = {FX_X}f, UNDO_X = {UNDO_X}f, HISS_X = {HISS_X}f;
constexpr float YB = {YB}f;
constexpr float ERASE_X = {ERASE_X}f, START_X = {START_X}f, INERTIA_X = {INERTIA_X}f;
constexpr float INERTIA_Y = {INERTIA_Y}f, SIZE_X = {SIZE_X}f, MIX_X = {MIX_X}f;
constexpr float YC = {YC}f;
constexpr float LVL_X = {LVL_X}f, SKIP_X = {SKIP_X}f, SLICE_X = {SLICE_X}f;
constexpr float YD = {YD}f;
constexpr float TR_X[4] = {{{', '.join(f'{x}f' for x in TR_X)}}};
constexpr float R1 = {R1}f, R2 = {R2}f, R3 = {R3}f;
}}
"""
with open(os.path.join(base, "src", "layout_tektite.hpp"), "w") as f:
    f.write(hpp)

print("wrote res/Tektite.svg and src/layout_tektite.hpp")
