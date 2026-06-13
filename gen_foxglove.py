#!/usr/bin/env python3
"""Generates res/Foxglove.svg and src/layout_foxglove.hpp."""

W, H = 50.8, 128.5  # 10 HP

# ---------------------------------------------------------------- layout ----
SW_Y = 13.0
SW_LABEL_Y = 19.6
SW_X = [7.0, 19.2, 31.6, 44.0]   # TYPE, BOOST, AUX, MONO

KX1, KX2 = 14.0, 36.8
KY1, KY2 = 33.5, 55.5
KLABEL1, KLABEL2 = 25.4, 47.8

CLIP_LED = (25.4, 35.0)
OS_TRIM = (25.4, 55.5)

TRIM_Y = 79.0
TRIM_LABEL_Y = 84.5
TRIM_X = [8.5, 19.8, 31.0, 42.3]

R1, R2 = 95.5, 114.5
J_X = [8.5, 19.8, 31.0, 42.3]

TITLE = (W / 2, 5.6)

BG = "#23262c"
FG = "#e8e4da"
DIM = "#8b8f99"
RING = "#3c414b"
ACCENT = "#d9a441"

F = {
    'A': [[(0,6),(2,0),(4,6)], [(0.9,3.9),(3.1,3.9)]],
    'B': [[(0,0),(0,6)], [(0,0),(3,0),(4,0.9),(4,2.2),(3,3),(0,3)], [(3,3),(4,3.9),(4,5.1),(3,6),(0,6)]],
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
    'Q': [[(1,0),(3,0),(4,1),(4,5),(3,6),(1,6),(0,5),(0,1),(1,0)], [(2.6,4.4),(4.3,6.5)]],
    'R': [[(0,6),(0,0),(3,0),(4,0.9),(4,2.3),(3,3.2),(0,3.2)], [(2,3.2),(4,6)]],
    'S': [[(4,0.9),(3,0),(1,0),(0,0.9),(0,2.1),(1,3),(3,3),(4,3.9),(4,5.1),(3,6),(1,6),(0,5.1)]],
    'T': [[(0,0),(4,0)], [(2,0),(2,6)]],
    'U': [[(0,0),(0,5),(1,6),(3,6),(4,5),(4,0)]],
    'V': [[(0,0),(2,6),(4,0)]],
    'X': [[(0,0),(4,6)], [(4,0),(0,6)]],
    'Y': [[(0,0),(2,3),(4,0)], [(2,3),(2,6)]],
    '/': [[(0.4,6.4),(3.6,-0.4)]],
    '<': [[(3.4,0.6),(0.6,3),(3.4,5.4)]],
    '>': [[(0.6,0.6),(3.4,3),(0.6,5.4)]],
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

svg.append(text_paths(*TITLE, "FOXGLOVE", 3.8, ACCENT, weight=0.8))
# foxglove bell glyph
svg.append(f'<path d="M 5.5 5 C 4.6 6.8 4.8 8.4 6 9.2 C 7.2 8.4 7.4 6.8 6.5 5 Z" fill="none" stroke="{ACCENT}" stroke-width="0.5" opacity="0.7"/>')

# switch labels + position legends (bottom-up)
svg.append(text_paths(SW_X[0], SW_LABEL_Y, "TYPE", 1.6, DIM))
svg.append(text_paths(SW_X[1], SW_LABEL_Y, "BOOST", 1.6, DIM))
svg.append(text_paths(SW_X[2], SW_LABEL_Y, "AUX", 1.6, DIM))
svg.append(text_paths(SW_X[3], SW_LABEL_Y, "MONO", 1.6, DIM))
for dy, label in [(3.2, "LD"), (0.0, "SK"), (-3.2, "SV")]:
    svg.append(text_paths(SW_X[0] + 4.9, SW_Y + dy, label, 1.25, DIM))
for dy, label in [(3.2, "MIX"), (0.0, "Q"), (-3.2, "CO")]:
    svg.append(text_paths(SW_X[2] + 5.3, SW_Y + dy, label, 1.25, DIM))

# big knob labels
svg.append(text_paths(KX1, KLABEL1, "CUTOFF", 2.1, FG))
svg.append(text_paths(KX2, KLABEL1, "RES", 2.1, FG))
svg.append(text_paths(KX1, KLABEL2, "BP<LP>HP", 1.7, FG))
svg.append(text_paths(KX2, KLABEL2, "OFFSET", 2.1, FG))

svg.append(text_paths(CLIP_LED[0], CLIP_LED[1] - 4.2, "CLIP", 1.5, DIM))
svg.append(text_paths(OS_TRIM[0], OS_TRIM[1] + 6.2, "OS", 1.5, DIM))

for x, label in zip(TRIM_X, ["L", "R", "CO", "AUX"]):
    svg.append(text_paths(x, TRIM_LABEL_Y, label, 1.5, DIM))
svg.append(text_paths(W / 2, TRIM_Y - 5.6, "GAIN", 1.5, DIM))

for x, label in zip(J_X, ["IN L", "IN R", "CO CV", "AUX"]):
    svg.append(ring(x, R1, 4.4))
    svg.append(text_paths(x, R1 - 6.4, label, 1.5, DIM))
for x, label, is_out in [(J_X[0], "V/OCT", 0), (J_X[1], "OS CV", 0), (J_X[2], "OUT L", 1), (J_X[3], "OUT R", 1)]:
    svg.append(ring(x, R2, 4.4, ACCENT if is_out else RING))
    svg.append(text_paths(x, R2 - 6.4, label, 1.5, FG if is_out else DIM))

svg.append('</svg>')

import os
base = os.path.dirname(os.path.abspath(__file__))
with open(os.path.join(base, "res", "Foxglove.svg"), "w") as f:
    f.write("\n".join(svg) + "\n")

hpp = f"""// Generated by gen_foxglove.py — do not edit by hand.
#pragma once

namespace fglayout {{
constexpr float SW_Y = {SW_Y}f;
constexpr float SW_X[4] = {{{', '.join(f'{x}f' for x in SW_X)}}};
constexpr float KX1 = {KX1}f, KX2 = {KX2}f, KY1 = {KY1}f, KY2 = {KY2}f;
constexpr float CLIP_LED_X = {CLIP_LED[0]}f, CLIP_LED_Y = {CLIP_LED[1]}f;
constexpr float OS_TRIM_X = {OS_TRIM[0]}f, OS_TRIM_Y = {OS_TRIM[1]}f;
constexpr float TRIM_Y = {TRIM_Y}f;
constexpr float TRIM_X[4] = {{{', '.join(f'{x}f' for x in TRIM_X)}}};
constexpr float R1 = {R1}f, R2 = {R2}f;
constexpr float J_X[4] = {{{', '.join(f'{x}f' for x in J_X)}}};
}}
"""
with open(os.path.join(base, "src", "layout_foxglove.hpp"), "w") as f:
    f.write(hpp)

print("wrote res/Foxglove.svg and src/layout_foxglove.hpp")
