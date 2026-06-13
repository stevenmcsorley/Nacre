#!/usr/bin/env python3
"""Generates res/Sirocco.svg and src/layout_sirocco.hpp."""

W, H = 101.6, 128.5  # 20 HP

# ---------------------------------------------------------------- layout ----
C = [10.6, 30.7, 50.8, 70.9, 91.0]

YA = 14.0          # FX trimpot, mode buttons, SIZE trimpot
YA_LED = 8.8
YA_LABEL = 21.0
YB = 33.0          # GEN btn, SCATTER, MELODY, TAP btn
YB_BTN_LABEL = 39.4
YB_LABEL = 45.2
YC = 56.5          # WANDER, LOCK btn, RATE, FREEZE btn, SPIN
YC_BTN_LABEL = 62.5
YC_SMALL_LABEL = 67.6
YC_LABEL = 68.6
YD = 72.5          # MIX, POS, PITCH, SHAPE trimpots
YD_LABEL = 77.6

R1, R2, R3, R4 = 90.5, 100.8, 111.1, 121.4

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

svg.append(text_paths(*TITLE, "SIROCCO", 4.2, ACCENT, weight=0.85))
# wind swirl decorations
import math
for (cx0, cy0, n, sc, op) in [(16, 6.5, 26, 0.9, 0.55), (85, 6.5, 26, 0.9, 0.55)]:
    pts = []
    for i in range(n):
        a = 0.9 + i * 0.22
        r = sc * (0.4 + 0.16 * a)
        pts.append((cx0 + r * math.cos(a) * 2.2, cy0 + r * math.sin(a)))
    d = "M " + " L ".join(f"{px:.2f} {py:.2f}" for px, py in pts)
    svg.append(f'<path d="{d}" fill="none" stroke="{ACCENT}" stroke-width="0.5" opacity="{op}"/>')

# row A
svg.append(text_paths(C[0], YA_LABEL, "FX", 2.1, FG))
svg.append(text_paths(C[1], YA_LABEL, "GEN MODE", 1.6, DIM))
svg.append(text_paths(C[2], YA_LABEL, "CLK MODE", 1.6, DIM))
svg.append(text_paths(C[3], YA_LABEL, "SCALE", 1.6, DIM))
svg.append(text_paths(C[4], YA_LABEL, "SIZE", 2.1, FG))

# row B
svg.append(text_paths(C[0], YB_BTN_LABEL, "GEN", 1.8, DIM))
svg.append(text_paths(C[1], YB_LABEL, "SCATTER", 2.1, FG))
svg.append(text_paths(C[3], YB_LABEL, "MELODY", 2.1, FG))
svg.append(text_paths(C[4], YB_BTN_LABEL, "TAP", 1.8, DIM))

# row C
svg.append(text_paths(C[0], YC_SMALL_LABEL, "WANDER", 2.1, FG))
svg.append(text_paths(C[1], YC_BTN_LABEL, "LOCK", 1.8, DIM))
svg.append(text_paths(C[2], YC_LABEL, "RATE", 2.1, FG))
svg.append(text_paths(C[3], YC_BTN_LABEL, "FREEZE", 1.8, DIM))
svg.append(text_paths(C[4], YC_SMALL_LABEL, "SPIN", 2.1, FG))

# row D
for x, label in zip(C[:4], ["MIX", "POS", "PITCH", "SHAPE"]):
    svg.append(text_paths(x, YD_LABEL, label, 2.1, FG))

# jacks
JACKS = [
    (C[0], R1, "FX", 0),     (C[1], R1, "SCTR", 0),  (C[2], R1, "MLDY", 0),  (C[3], R1, "SIZE", 0),  (C[4], R1, "RATE", 0),
    (C[0], R2, "WNDR", 0),   (C[1], R2, "POS", 0),   (C[2], R2, "PITCH", 0), (C[3], R2, "SHAPE", 0), (C[4], R2, "SPIN", 0),
    (C[0], R3, "MIX", 0),    (C[1], R3, "GEN", 0),   (C[2], R3, "CLOCK", 0), (C[3], R3, "LOCK", 0),  (C[4], R3, "FRZ", 0),
    (C[0], R4, "IN L", 0),   (C[1], R4, "IN R", 0),  (C[2], R4, "CV", 1),    (C[3], R4, "OUT L", 1), (C[4], R4, "OUT R", 1),
]
for x, y, label, is_out in JACKS:
    svg.append(ring(x, y, 4.0, ACCENT if is_out else RING))
    svg.append(text_paths(x, y - 5.9, label, 1.4, FG if is_out else DIM))

svg.append('</svg>')

import os
base = os.path.dirname(os.path.abspath(__file__))
with open(os.path.join(base, "res", "Sirocco.svg"), "w") as f:
    f.write("\n".join(svg) + "\n")

hpp = f"""// Generated by gen_sirocco.py — do not edit by hand.
#pragma once

namespace wlayout {{
constexpr float C[5] = {{{C[0]}f, {C[1]}f, {C[2]}f, {C[3]}f, {C[4]}f}};
constexpr float YA = {YA}f, YA_LED = {YA_LED}f;
constexpr float YB = {YB}f;
constexpr float YC = {YC}f;
constexpr float YD = {YD}f;
constexpr float R1 = {R1}f, R2 = {R2}f, R3 = {R3}f, R4 = {R4}f;
}}
"""
with open(os.path.join(base, "src", "layout_sirocco.hpp"), "w") as f:
    f.write(hpp)

print("wrote res/Sirocco.svg and src/layout_sirocco.hpp")
