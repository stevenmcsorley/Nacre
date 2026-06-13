#!/usr/bin/env python3
"""Generates res/Abacus.svg and src/layout_abacus.hpp — Maths-style layout."""

W, H = 101.6, 128.5  # 20 HP
MIR = lambda x: W - x

# ---------------------------------------------------------------- layout ----
TOP_Y = 13.0
IN1_X, TRIG1_X = 9.0, 21.5
IN2_X, IN3_X = 38.0, 63.6
TRIG4_X, IN4_X = MIR(21.5), MIR(9.0)

CYC_BTN_X, CYC_BTN_Y = 11.5, 26.5
CYC_LED_X, CYC_LED_Y = 18.5, 22.0

RISE_X, RISE_Y = 31.5, 30.0
FALL_X, FALL_Y = 31.5, 52.0
RESP_X, RESP_Y = 29.0, 72.0

ATT_X = 50.8
ATT_Y = [21.0, 37.0, 53.0, 69.0]
CH2LED = (43.6, 37.0)
CH3LED = (43.6, 53.0)

EDGE_X = 9.0
EDGE_Y = [37.0, 48.0, 59.0, 70.5]   # rise cv, both cv, fall cv, cycle gate

VAR_Y = 90.0
VAR_X = [33.5, 45.0, 56.6, 68.1]
BUS_Y = 104.0
BUS_X = [39.3, 50.8, 62.3]          # OR SUM INV
ORLED = (31.0, 104.0)
INVLED = (70.6, 104.0)

CORNER_Y = 117.0
EOR_X, U1_X = 9.0, 21.5
U4_X, EOC_X = MIR(21.5), MIR(9.0)
SBOX1 = (15.2, 105.5)
SBOX4 = (MIR(15.2), 105.5)
CH1LED = (9.0, 101.0)
CH4LED = (MIR(9.0), 101.0)
EORLED = (21.5, 101.0)
EOCLED = (MIR(21.5), 101.0)

TITLE = (W / 2, 5.2)

BG = "#23262c"
FG = "#e8e4da"
DIM = "#8b8f99"
RING = "#3c414b"
ACCENT = "#d9a441"

F = {
    'A': [[(0,6),(2,0),(4,6)], [(0.9,3.9),(3.1,3.9)]],
    'B': [[(0,6),(0,0),(3,0),(4,0.8),(4,2.2),(3,3),(0,3)], [(3,3),(4,3.8),(4,5.2),(3,6),(0,6)]],
    'C': [[(4,1),(3,0),(1,0),(0,1),(0,5),(1,6),(3,6),(4,5)]],
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
    'R': [[(0,6),(0,0),(3,0),(4,0.9),(4,2.3),(3,3.2),(0,3.2)], [(2,3.2),(4,6)]],
    'S': [[(4,0.9),(3,0),(1,0),(0,0.9),(0,2.1),(1,3),(3,3),(4,3.9),(4,5.1),(3,6),(1,6),(0,5.1)]],
    'T': [[(0,0),(4,0)], [(2,0),(2,6)]],
    'U': [[(0,0),(0,5),(1,6),(3,6),(4,5),(4,0)]],
    'V': [[(0,0),(2,6),(4,0)]],
    'X': [[(0,0),(4,6)], [(4,0),(0,6)]],
    'Y': [[(0,0),(2,3),(4,0)], [(2,3),(2,6)]],
    '/': [[(0.4,6.4),(3.6,-0.4)]],
    '1': [[(1,1.2),(2.2,0),(2.2,6)], [(1,6),(3.4,6)]],
    '2': [[(0,1),(1,0),(3,0),(4,1),(4,2),(0,6),(4,6)]],
    '3': [[(0,0.8),(1,0),(3,0),(4,1),(4,2),(3,3),(1.6,3)], [(3,3),(4,4),(4,5),(3,6),(1,6),(0,5.2)]],
    '4': [[(3,6),(3,0),(0,4),(4,4)]],
    ' ': [],
}

def text_paths(x, y, s, h, color, weight=None, rotate=None):
    scale = h / 6.0
    adv = 5.4 * scale
    width = adv * len(s) - 1.4 * scale
    x0 = x - width / 2.0
    sw = weight if weight else max(0.28, h * 0.16)
    out = []
    if rotate is not None:
        out.append(f'<g transform="rotate({rotate} {x} {y})">')
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
    if rotate is not None:
        out.append('</g>')
    return "\n".join(out)

def ring(x, y, r, color=RING, sw=0.5):
    return f'<circle cx="{x}" cy="{y}" r="{r}" fill="none" stroke="{color}" stroke-width="{sw}"/>'

def arrow(x1, y1, x2, y2, color, sw=0.7):
    import math
    a = math.atan2(y2 - y1, x2 - x1)
    h = 2.6
    p1 = (x2 - h * math.cos(a - 0.42), y2 - h * math.sin(a - 0.42))
    p2 = (x2 - h * math.cos(a + 0.42), y2 - h * math.sin(a + 0.42))
    return (f'<path d="M {x1:.2f} {y1:.2f} L {x2:.2f} {y2:.2f}" fill="none" stroke="{color}" stroke-width="{sw}"/>'
            f'<path d="M {p1[0]:.2f} {p1[1]:.2f} L {x2:.2f} {y2:.2f} L {p2[0]:.2f} {p2[1]:.2f}" '
            f'fill="none" stroke="{color}" stroke-width="{sw}" stroke-linejoin="round"/>')

svg = []
svg.append(f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}mm" height="{H}mm" viewBox="0 0 {W} {H}">')
svg.append(f'<rect x="0" y="0" width="{W}" height="{H}" fill="{BG}"/>')
svg.append(f'<rect x="0.4" y="0.4" width="{W-0.8}" height="{H-0.8}" fill="none" stroke="#171a1f" stroke-width="0.8"/>')

svg.append(text_paths(*TITLE, "ORRERY", 4.4, ACCENT, weight=0.9))

# ------- channel wings: rise / fall arrows with rotated labels -------
for side in (0, 1):
    mx = (lambda x: x) if side == 0 else MIR
    rot = -42 if side == 0 else 42
    # rising arrow up toward the rise knob
    svg.append(arrow(mx(17.0), 46.0, mx(41.5), 20.5, FG))
    svg.append(text_paths(mx(20.5), 34.5, "RISE", 2.2, ACCENT, rotate=rot))
    # falling arrow down from the fall knob
    svg.append(arrow(mx(41.5), 42.0, mx(17.0), 67.5, FG))
    svg.append(text_paths(mx(20.5), 60.5, "FALL", 2.2, ACCENT, rotate=rot))
    # log/exp arc under the response knob
    rx = mx(RESP_X)
    svg.append(f'<path d="M {rx-7.5:.2f} {RESP_Y+6.5:.2f} A 10 10 0 0 0 {rx+7.5:.2f} {RESP_Y+6.5:.2f}" '
               f'fill="none" stroke="{DIM}" stroke-width="0.4"/>')
    svg.append(text_paths(mx(19.5), RESP_Y + 8.6, "LOG", 1.4, DIM))
    svg.append(text_paths(mx(38.5), RESP_Y + 8.6, "EXP", 1.4, DIM))
    # rotated CYCLE label along the edge
    svg.append(text_paths(mx(3.6), CYC_BTN_Y, "CYCLE", 1.4, DIM, rotate=(-90 if side == 0 else 90)))
    # corner in-arrow
    svg.append(arrow(mx(3.0), 3.5, mx(7.0), 7.8, FG, 0.55))

# ------- top row -------
svg.append(text_paths(TRIG1_X, 6.9, "TRIG", 1.4, DIM))
svg.append(text_paths(TRIG4_X, 6.9, "TRIG", 1.4, DIM))
for x in [IN1_X, TRIG1_X, IN2_X, IN3_X, TRIG4_X, IN4_X]:
    svg.append(ring(x, TOP_Y, 4.4))
# short drop lines from the center inputs (subtle, no crossings)
svg.append(f'<path d="M {IN2_X} {TOP_Y+4.6} L {IN2_X} {TOP_Y+7.6}" fill="none" stroke="{FG}" stroke-width="0.35" opacity="0.45"/>')
svg.append(f'<path d="M {IN3_X} {TOP_Y+4.6} L {IN3_X} {TOP_Y+7.6}" fill="none" stroke="{FG}" stroke-width="0.35" opacity="0.45"/>')

# ------- attenuverter column -------
for i, y in enumerate(ATT_Y):
    g = ATT_X
    svg.append(f'<circle cx="{g-4.4}" cy="{y-6.6}" r="1.15" fill="none" stroke="{DIM}" stroke-width="0.35"/>')
    svg.append(f'<path d="M {g-5.0} {y-6.6} L {g-3.8} {y-6.6}" stroke="{DIM}" stroke-width="0.35"/>')
    svg.append(f'<path d="M {g} {y-7.6} L {g} {y-5.6}" stroke="{DIM}" stroke-width="0.35"/>')
    svg.append(f'<circle cx="{g+4.4}" cy="{y-6.6}" r="1.15" fill="none" stroke="{DIM}" stroke-width="0.35"/>')
    svg.append(f'<path d="M {g+3.8} {y-6.6} L {g+5.0} {y-6.6}" stroke="{DIM}" stroke-width="0.35"/>')
    svg.append(f'<path d="M {g+4.4} {y-7.2} L {g+4.4} {y-6.0}" stroke="{DIM}" stroke-width="0.35"/>')
    svg.append(text_paths(g + 7.6, y, str(i + 1), 1.7, DIM))

# ------- edge CV columns -------
for side in (0, 1):
    mx = (lambda x: x) if side == 0 else MIR
    for y, label in zip(EDGE_Y, ["RISE", "BOTH", "FALL", "CYCLE"]):
        svg.append(ring(mx(EDGE_X), y, 4.4))
        svg.append(text_paths(mx(EDGE_X), y - 5.9, label, 1.15, ACCENT if label == "BOTH" else DIM))

# ------- bottom: numbered outs, bus, corners -------
for x, n in zip(VAR_X, "1234"):
    svg.append(ring(x, VAR_Y, 4.4, ACCENT))
    svg.append(text_paths(x, VAR_Y - 6.4, n, 1.7, FG))
for x, label in zip(BUS_X, ["OR", "SUM", "INV"]):
    svg.append(ring(x, BUS_Y, 4.4, ACCENT))
    svg.append(text_paths(x, BUS_Y - 6.4, label, 1.6, FG))
# connector underline like the hardware
svg.append(f'<path d="M {ORLED[0]} {BUS_Y+6.8} L {INVLED[0]} {BUS_Y+6.8}" fill="none" stroke="{FG}" stroke-width="0.35" opacity="0.5"/>')

for cx, cy in (SBOX1, SBOX4):
    svg.append(f'<rect x="{cx-5}" y="{cy-3.4}" width="10" height="6.8" fill="none" stroke="{ACCENT}" stroke-width="0.5"/>')
    svg.append(f'<path d="M {cx-3.2} {cy+2.2} C {cx-0.5} {cy+2.2} {cx+0.5} {cy-2.2} {cx+3.2} {cy-2.2}" '
               f'fill="none" stroke="{ACCENT}" stroke-width="0.6"/>')

for x, label in [(EOR_X, "EOR"), (U1_X, "OUT 1"), (U4_X, "OUT 4"), (EOC_X, "EOC")]:
    svg.append(ring(x, CORNER_Y, 4.4, ACCENT))
    svg.append(text_paths(x, CORNER_Y - 5.6, label, 1.4, FG))

svg.append('</svg>')

import os
base = os.path.dirname(os.path.abspath(__file__))
with open(os.path.join(base, "res", "Abacus.svg"), "w") as f:
    f.write("\n".join(svg) + "\n")

def fmt(v):
    return f"{v}f"

hpp = f"""// Generated by gen_abacus.py — do not edit by hand.
#pragma once

namespace alayout {{
constexpr float TOP_Y = {TOP_Y}f;
constexpr float IN1_X = {IN1_X}f, TRIG1_X = {TRIG1_X}f, IN2_X = {IN2_X}f, IN3_X = {IN3_X}f;
constexpr float TRIG4_X = {TRIG4_X}f, IN4_X = {IN4_X}f;
constexpr float CYC_BTN_X = {CYC_BTN_X}f, CYC_BTN_Y = {CYC_BTN_Y}f;
constexpr float CYC_LED_X = {CYC_LED_X}f, CYC_LED_Y = {CYC_LED_Y}f;
constexpr float RISE_X = {RISE_X}f, RISE_Y = {RISE_Y}f;
constexpr float FALL_X = {FALL_X}f, FALL_Y = {FALL_Y}f;
constexpr float RESP_X = {RESP_X}f, RESP_Y = {RESP_Y}f;
constexpr float ATT_X = {ATT_X}f;
constexpr float ATT_Y[4] = {{{ATT_Y[0]}f, {ATT_Y[1]}f, {ATT_Y[2]}f, {ATT_Y[3]}f}};
constexpr float CH2LED_X = {CH2LED[0]}f, CH2LED_Y = {CH2LED[1]}f;
constexpr float CH3LED_X = {CH3LED[0]}f, CH3LED_Y = {CH3LED[1]}f;
constexpr float EDGE_X = {EDGE_X}f;
constexpr float EDGE_Y[4] = {{{EDGE_Y[0]}f, {EDGE_Y[1]}f, {EDGE_Y[2]}f, {EDGE_Y[3]}f}};
constexpr float VAR_Y = {VAR_Y}f;
constexpr float VAR_X[4] = {{{VAR_X[0]}f, {VAR_X[1]}f, {VAR_X[2]}f, {VAR_X[3]}f}};
constexpr float BUS_Y = {BUS_Y}f;
constexpr float BUS_X[3] = {{{BUS_X[0]}f, {BUS_X[1]}f, {BUS_X[2]}f}};
constexpr float ORLED_X = {ORLED[0]}f, ORLED_Y = {ORLED[1]}f;
constexpr float INVLED_X = {INVLED[0]}f, INVLED_Y = {INVLED[1]}f;
constexpr float CORNER_Y = {CORNER_Y}f;
constexpr float EOR_X = {EOR_X}f, U1_X = {U1_X}f, U4_X = {U4_X}f, EOC_X = {EOC_X}f;
constexpr float CH1LED_X = {CH1LED[0]}f, CH1LED_Y = {CH1LED[1]}f;
constexpr float CH4LED_X = {CH4LED[0]}f, CH4LED_Y = {CH4LED[1]}f;
constexpr float EORLED_X = {EORLED[0]}f, EORLED_Y = {EORLED[1]}f;
constexpr float EOCLED_X = {EOCLED[0]}f, EOCLED_Y = {EOCLED[1]}f;
constexpr float W_MM = {W}f;
}}
"""
with open(os.path.join(base, "src", "layout_abacus.hpp"), "w") as f:
    f.write(hpp)

print("wrote res/Abacus.svg and src/layout_abacus.hpp")
