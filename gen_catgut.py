#!/usr/bin/env python3
"""Generates res/Catgut.svg and src/layout_catgut.hpp."""

W, H = 81.28, 128.5  # 16 HP

# ---------------------------------------------------------------- layout ----
SOLO_X, MID_X, CHORD_X = 13.0, 40.64, 68.3
ROW1, ROW2, ROW3 = 19.5, 38.5, 57.5
LABEL_DY = 9.6
TIMBRE_SX, TIMBRE_CX, TIMBRE_Y = 31.0, 50.3, 57.5
TIMBRE_LED_DY = -6.2

HDR_Y = 71.2   # SOLO VOICE / CHORD VOICE headers

J6 = [9.0, 21.8, 34.6, 47.4, 60.2, 73.0]
R1, R2, R3 = 80.0, 94.0, 108.0

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
    'R': [[(0,6),(0,0),(3,0),(4,0.9),(4,2.3),(3,3.2),(0,3.2)], [(2,3.2),(4,6)]],
    'S': [[(4,0.9),(3,0),(1,0),(0,0.9),(0,2.1),(1,3),(3,3),(4,3.9),(4,5.1),(3,6),(1,6),(0,5.1)]],
    'T': [[(0,0),(4,0)], [(2,0),(2,6)]],
    'U': [[(0,0),(0,5),(1,6),(3,6),(4,5),(4,0)]],
    'V': [[(0,0),(2,6),(4,0)]],
    'Y': [[(0,0),(2,3),(4,0)], [(2,3),(2,6)]],
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

svg.append(text_paths(*TITLE, "CATGUT", 4.2, ACCENT, weight=0.85))
# string glyphs: three strings with a pluck
for i, yy in enumerate([5.2, 6.8, 8.4]):
    svg.append(f'<path d="M 10 {yy} L 26 {yy}" stroke="{ACCENT}" stroke-width="0.4" opacity="0.6"/>')
    svg.append(f'<path d="M 55 {yy} L 71 {yy}" stroke="{ACCENT}" stroke-width="0.4" opacity="0.6"/>')
svg.append(f'<path d="M 18 4.4 Q 19.5 6.8 18 9.2" fill="none" stroke="{ACCENT}" stroke-width="0.5" opacity="0.8"/>')

# knobs + labels
svg.append(text_paths(SOLO_X, ROW1 + LABEL_DY, "DAMP", 2.1, FG))
svg.append(text_paths(MID_X, ROW1 + LABEL_DY, "TUNE", 2.1, FG))
svg.append(text_paths(CHORD_X, ROW1 + LABEL_DY, "DAMP", 2.1, FG))
svg.append(text_paths(SOLO_X, ROW2 + LABEL_DY, "DECAY", 2.1, FG))
svg.append(text_paths(MID_X, ROW2 + LABEL_DY, "HARMONY", 2.1, FG))
svg.append(text_paths(CHORD_X, ROW2 + LABEL_DY, "DECAY", 2.1, FG))
svg.append(text_paths(SOLO_X, ROW3 + LABEL_DY, "ATTACK", 2.1, FG))
svg.append(text_paths(CHORD_X, ROW3 + LABEL_DY, "ATTACK", 2.1, FG))
svg.append(text_paths(TIMBRE_SX, TIMBRE_Y + 7.4, "TIMBRE", 1.5, DIM))
svg.append(text_paths(TIMBRE_CX, TIMBRE_Y + 7.4, "TIMBRE", 1.5, DIM))

# section headers with rules
svg.append(text_paths(20.5, HDR_Y, "SOLO VOICE", 1.9, ACCENT))
svg.append(text_paths(61.0, HDR_Y, "CHORD VOICE", 1.9, ACCENT))
svg.append(f'<path d="M 40.64 {HDR_Y+2.4} L 40.64 {R3+5}" stroke="{RING}" stroke-width="0.4"/>')

JACKS = [
    (J6[0], R1, "TRIG", 0), (J6[1], R1, "ATTACK", 0), (J6[2], R1, "V/OCT", 0),
    (J6[3], R1, "V/OCT", 0), (J6[4], R1, "ATTACK", 0), (J6[5], R1, "TRIG", 0),
    (J6[0], R2, "DAMP", 0), (J6[1], R2, "DETUNE", 0), (J6[2], R2, "DECAY", 0),
    (J6[3], R2, "DECAY", 0), (J6[4], R2, "HARMNY", 0), (J6[5], R2, "DAMP", 0),
    (J6[0], R3, "PITCH", 0), (J6[1], R3, "IN", 0),
    (J6[3], R3, "IN", 0), (J6[4], R3, "OUT L", 1), (J6[5], R3, "OUT R", 1),
]
for x, y, label, is_out in JACKS:
    svg.append(ring(x, y, 4.4, ACCENT if is_out else RING))
    svg.append(text_paths(x, y - 6.4, label, 1.4, FG if is_out else DIM))

svg.append('</svg>')

import os
base = os.path.dirname(os.path.abspath(__file__))
with open(os.path.join(base, "res", "Catgut.svg"), "w") as f:
    f.write("\n".join(svg) + "\n")

hpp = f"""// Generated by gen_catgut.py — do not edit by hand.
#pragma once

namespace glayout {{
constexpr float SOLO_X = {SOLO_X}f, MID_X = {MID_X}f, CHORD_X = {CHORD_X}f;
constexpr float ROW1 = {ROW1}f, ROW2 = {ROW2}f, ROW3 = {ROW3}f;
constexpr float TIMBRE_SX = {TIMBRE_SX}f, TIMBRE_CX = {TIMBRE_CX}f, TIMBRE_Y = {TIMBRE_Y}f;
constexpr float TIMBRE_LED_DY = {TIMBRE_LED_DY}f;
constexpr float J6[6] = {{{', '.join(f'{x}f' for x in J6)}}};
constexpr float R1 = {R1}f, R2 = {R2}f, R3 = {R3}f;
}}
"""
with open(os.path.join(base, "src", "layout_catgut.hpp"), "w") as f:
    f.write(hpp)

print("wrote res/Catgut.svg and src/layout_catgut.hpp")
