#!/usr/bin/env python3
"""Generates res/Ammonite.svg and src/layout_ammonite.hpp."""

W, H = 81.28, 128.5  # 16 HP

# ---------------------------------------------------------------- layout ----
C = [11.0, 30.7, 50.5, 70.2]          # four main columns

BTN_Y = 16.0
BTN_LED_Y = 10.5
BTN_LABEL_Y = 22.5
# freeze, delay mode, feedback route, purge
BTNS = [(C[0], "FREEZE"), (C[1], "MODE"), (C[2], "ROUTE"), (C[3], "PURGE")]

K1_Y = 31.0
K1_LABEL_Y = 39.6
K2_Y = 50.5
K2_LABEL_Y = 59.1
KNOBS_R1 = ["TAPS", "DIV", "COLOR", "DEPTH"]
KNOBS_R2 = ["SPREAD", "REVERSE", "FDBK", "MIX"]
CHROMA_LED = (58.2, 24.6)

R3, R4 = 71.5, 83.5                    # CV jack rows (under matching knobs)

J5 = [9.0, 25.0, 41.0, 57.0, 73.0]
R5 = 97.5                              # clock, tap btn, freeze gate, purge gate, sonar
R6 = 113.5                             # in L, in R, clock led, out L, out R
CLK_LED = (41.0, 110.0)

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

svg.append(text_paths(*TITLE, "AMMONITE", 4.2, ACCENT, weight=0.85))
# spiral shell glyph (logarithmic-ish spiral from arcs)
import math
cx, cy = 7.5, 6.8
pts = []
for i in range(40):
    a = i * 0.35
    r = 0.28 * math.exp(0.135 * a)
    pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
d = "M " + " L ".join(f"{px:.2f} {py:.2f}" for px, py in pts)
svg.append(f'<path d="{d}" fill="none" stroke="{ACCENT}" stroke-width="0.55" opacity="0.7"/>')

for (x, label) in BTNS:
    svg.append(text_paths(x, BTN_LABEL_Y, label, 1.7, DIM))

for x, label in zip(C, KNOBS_R1):
    svg.append(text_paths(x, K1_LABEL_Y, label, 2.1, FG))
for x, label in zip(C, KNOBS_R2):
    svg.append(text_paths(x, K2_LABEL_Y, label, 2.1, FG))

# CV jacks under matching knobs, each labeled
for x, label in zip(C, ["TAPS", "DIV", "COLOR", "DEPTH"]):
    svg.append(ring(x, R3, 4.4))
    svg.append(text_paths(x, R3 - 6.4, label, 1.4, DIM))
for x, label in zip(C, ["SPRD", "REV", "FDBK", "MIX"]):
    svg.append(ring(x, R4, 4.4))
    svg.append(text_paths(x, R4 - 6.4, label, 1.4, DIM))

# row 5: clock, tap, freeze gate, purge gate, sonar out
for x, label, is_out in [(J5[0], "CLOCK", 0), (J5[2], "FRZ", 0), (J5[3], "PRG", 0), (J5[4], "SONAR", 1)]:
    svg.append(ring(x, R5, 4.4, ACCENT if is_out else RING))
    svg.append(text_paths(x, R5 - 6.4, label, 1.6, FG if is_out else DIM))
svg.append(text_paths(J5[1], R5 - 6.4, "TAP", 1.6, DIM))

# row 6: audio io
for x, label, is_out in [(J5[0], "IN L", 0), (J5[1], "IN R", 0), (J5[3], "OUT L", 1), (J5[4], "OUT R", 1)]:
    svg.append(ring(x, R6, 4.4, ACCENT if is_out else RING))
    svg.append(text_paths(x, R6 - 6.4, label, 1.6, FG if is_out else DIM))

svg.append('</svg>')

import os
base = os.path.dirname(os.path.abspath(__file__))
with open(os.path.join(base, "res", "Ammonite.svg"), "w") as f:
    f.write("\n".join(svg) + "\n")

hpp = f"""// Generated by gen_ammonite.py — do not edit by hand.
#pragma once

namespace mlayout {{
constexpr float C[4] = {{{C[0]}f, {C[1]}f, {C[2]}f, {C[3]}f}};
constexpr float BTN_Y = {BTN_Y}f;
constexpr float BTN_LED_Y = {BTN_LED_Y}f;
constexpr float K1_Y = {K1_Y}f;
constexpr float K2_Y = {K2_Y}f;
constexpr float CHROMA_LED_X = {CHROMA_LED[0]}f, CHROMA_LED_Y = {CHROMA_LED[1]}f;
constexpr float R3 = {R3}f, R4 = {R4}f;
constexpr float J5[5] = {{{J5[0]}f, {J5[1]}f, {J5[2]}f, {J5[3]}f, {J5[4]}f}};
constexpr float R5 = {R5}f, R6 = {R6}f;
constexpr float CLK_LED_X = {CLK_LED[0]}f, CLK_LED_Y = {CLK_LED[1]}f;
}}
"""
with open(os.path.join(base, "src", "layout_ammonite.hpp"), "w") as f:
    f.write(hpp)

print("wrote res/Ammonite.svg and src/layout_ammonite.hpp")
