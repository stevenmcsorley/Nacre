#!/usr/bin/env python3
"""Generates res/Capo.svg and src/layout_capo.hpp."""

W, H = 71.12, 128.5  # 14 HP

# ---------------------------------------------------------------- layout ----
QLED_Y = 13.0
QLED_X0, QLED_DX = 11.0, 8.2   # 7 quality LEDs
QLED_LABEL_Y = 17.6

BANK_X, BANK_Y = 12.0, 27.0
WAVE_X, WAVE_Y = 12.0, 43.0
FREQ_X, FREQ_Y = 36.5, 28.0
FINE_X, FINE_Y = 36.5, 44.5
QUAL_X, QUAL_Y = 59.0, 45.0

BTN_X, BTN_LED_DX = 63.0, -6.8
MODE_Y, HARM_Y, TRIAD_Y = 21.5, 29.0, 36.5

FM_JACK_X, FM_ROW_Y = 9.0, 64.0
FM_ATT_X = 20.5
VOICING_X, VOICING_Y = 40.0, 58.5

CV_Y = 78.0
CV_X = [9.0, 19.6, 30.2, 40.8, 51.4, 62.0]   # bank wave voicing quality lead v/oct

OUT_Y = 96.0
OUT_X = [11.0, 23.2, 35.4, 47.6, 59.8]       # root third fifth seventh mix

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
    'J': [[(3,0),(3,5),(2,6),(1,6),(0,5)]],
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
    '5': [[(4,0),(0,0),(0,2.6),(2.8,2.6),(4,3.6),(4,5),(3,6),(1,6),(0,5.2)]],
    '7': [[(0,0),(4,0),(1.6,6)]],
    '3': [[(0,0.8),(1,0),(3,0),(4,1),(4,2),(3,3),(1.6,3)], [(3,3),(4,4),(4,5),(3,6),(1,6),(0,5.2)]],
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

svg.append(text_paths(*TITLE, "CAPO", 4.2, ACCENT, weight=0.85))
# fretboard glyph with capo bar
for i in range(4):
    svg.append(f'<path d="M {14+i*4} 4.5 L {14+i*4} 9.5" stroke="{ACCENT}" stroke-width="0.4" opacity="0.55"/>')
    svg.append(f'<path d="M {45+i*4} 4.5 L {45+i*4} 9.5" stroke="{ACCENT}" stroke-width="0.4" opacity="0.55"/>')
svg.append(f'<rect x="20.5" y="4" width="1.6" height="6" rx="0.8" fill="{ACCENT}" opacity="0.85"/>')
svg.append(f'<rect x="51.5" y="4" width="1.6" height="6" rx="0.8" fill="{ACCENT}" opacity="0.85"/>')

# quality LED labels
for i, label in enumerate(["MAJ", "MIN", "DOM", "DIM", "SUS", "AUG", "X"]):
    svg.append(text_paths(QLED_X0 + i * QLED_DX, QLED_LABEL_Y, label, 1.3, DIM))

svg.append(text_paths(BANK_X, BANK_Y + 9.0, "BANK", 1.9, FG))
svg.append(text_paths(WAVE_X, WAVE_Y + 9.0, "WAVE", 1.9, FG))
svg.append(text_paths(FREQ_X, FREQ_Y + 11.2, "FREQ", 2.1, FG))
svg.append(text_paths(FINE_X, FINE_Y + 6.8, "FINE", 1.5, DIM))
svg.append(text_paths(QUAL_X, QUAL_Y + 9.0, "QUALITY", 1.9, FG))
svg.append(text_paths(BTN_X - 11.5, MODE_Y, "MODE", 1.5, DIM))
svg.append(text_paths(BTN_X - 11.5, HARM_Y, "HARM", 1.5, DIM))
svg.append(text_paths(BTN_X - 11.8, TRIAD_Y, "TRIAD", 1.5, DIM))

svg.append(ring(FM_JACK_X, FM_ROW_Y, 4.4))
svg.append(text_paths(FM_JACK_X, FM_ROW_Y - 6.4, "FM", 1.5, DIM))
svg.append(text_paths(VOICING_X, VOICING_Y + 9.0, "VOICING", 1.9, FG))

for x, label in zip(CV_X, ["BANK", "WAVE", "VOICE", "QUAL", "LEAD", "V/OCT"]):
    svg.append(ring(x, CV_Y, 4.4))
    svg.append(text_paths(x, CV_Y - 6.4, label, 1.35, DIM))

for x, label in zip(OUT_X, ["ROOT", "THIRD", "FIFTH", "SVNTH", "MIX"]):
    svg.append(ring(x, OUT_Y, 4.4, ACCENT))
    svg.append(text_paths(x, OUT_Y - 6.4, label, 1.35, FG))

svg.append('</svg>')

import os
base = os.path.dirname(os.path.abspath(__file__))
with open(os.path.join(base, "res", "Capo.svg"), "w") as f:
    f.write("\n".join(svg) + "\n")

hpp = f"""// Generated by gen_capo.py — do not edit by hand.
#pragma once

namespace pl {{
constexpr float QLED_Y = {QLED_Y}f, QLED_X0 = {QLED_X0}f, QLED_DX = {QLED_DX}f;
constexpr float BANK_X = {BANK_X}f, BANK_Y = {BANK_Y}f;
constexpr float WAVE_X = {WAVE_X}f, WAVE_Y = {WAVE_Y}f;
constexpr float FREQ_X = {FREQ_X}f, FREQ_Y = {FREQ_Y}f;
constexpr float FINE_X = {FINE_X}f, FINE_Y = {FINE_Y}f;
constexpr float QUAL_X = {QUAL_X}f, QUAL_Y = {QUAL_Y}f;
constexpr float BTN_X = {BTN_X}f, BTN_LED_DX = {BTN_LED_DX}f;
constexpr float MODE_Y = {MODE_Y}f, HARM_Y = {HARM_Y}f, TRIAD_Y = {TRIAD_Y}f;
constexpr float FM_JACK_X = {FM_JACK_X}f, FM_ROW_Y = {FM_ROW_Y}f, FM_ATT_X = {FM_ATT_X}f;
constexpr float VOICING_X = {VOICING_X}f, VOICING_Y = {VOICING_Y}f;
constexpr float CV_Y = {CV_Y}f;
constexpr float CV_X[6] = {{{', '.join(f'{x}f' for x in CV_X)}}};
constexpr float OUT_Y = {OUT_Y}f;
constexpr float OUT_X[5] = {{{', '.join(f'{x}f' for x in OUT_X)}}};
}}
"""
with open(os.path.join(base, "src", "layout_capo.hpp"), "w") as f:
    f.write(hpp)

print("wrote res/Capo.svg and src/layout_capo.hpp")
