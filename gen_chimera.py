#!/usr/bin/env python3
"""Generate Chimera's production panel and shared widget layout.

The acacia-dusk artwork stays expressive, while the control overlay and
Half a Giraf marks provide the hierarchy needed at actual Rack scale.
"""

import os

W, H = 111.76, 128.5  # 22 HP

# ---------------------------------------------------------------- layout ----
YA = 19.0                              # transport light-buttons
YA_LABEL = 26.0
BTN_X = [14.0, 35.0, 56.0, 77.0, 98.0]  # rec, play, splice, sync, mode

YB = 35.0                              # sos + eq + chop row
YB_LABEL = 43.2
SOS_X = 13.0
EQ_X = [42.0, 63.0, 84.0]              # low, mid, high
CHOP_X = 102.0

YC = 55.0                              # main knobs
YC_LABEL = 64.2
SPEED_X, PITCH_X = 14.0, 38.0
GRAIN_X, SLICE_X, SCATTER_X = 62.0, 82.0, 101.0

DISP_X, DISP_Y = 6.0, 67.0             # reel display, full width
DISP_W, DISP_H = 99.76, 14.0

R1, R2, R3 = 90.0, 104.0, 118.0
J6 = [10.0, 28.0, 46.0, 64.0, 82.0, 100.0]

TITLE = (W / 2, 6.8)

BG = "#e9e7e1"
EDGE = "#c8c5bc"
FG = "#f2efe8"
DIM = "#d8d3c6"
BLUE = "#83a9d4"
ACCENT = "#df9a31"
RING = "#2a2a2a"
BRAND_DIM = "#c9d2dd"

F = {
    'A': [[(0,6),(2,0),(4,6)], [(0.9,3.9),(3.1,3.9)]],
    'B': [[(0,6),(0,0),(3,0),(4,0.8),(4,2.2),(3,3),(0,3)], [(3,3),(4,3.8),(4,5.2),(3,6),(0,6)]],
    'C': [[(4,1),(3,0),(1,0),(0,1),(0,5),(1,6),(3,6),(4,5)]],
    'D': [[(0,0),(0,6)], [(0,0),(2.8,0),(4,1.4),(4,4.6),(2.8,6),(0,6)]],
    'E': [[(4,0),(0,0),(0,6),(4,6)], [(0,3),(2.8,3)]],
    'F': [[(4,0),(0,0),(0,6)], [(0,3),(2.6,3)]],
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

def text_paths(x, y, s, h, color, weight=None, spacing=5.4):
    scale = h / 6.0
    adv = spacing * scale
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

def chip(x, y, s, h, spacing=5.4, pad=0.9):
    """dark plate behind a text_paths label so it reads over the artwork"""
    scale = h / 6.0
    adv = spacing * scale
    width = adv * len(s) - 1.4 * scale
    return (f'<rect x="{x - width/2 - pad:.2f}" y="{y - h/2 - 0.55:.2f}" '
            f'width="{width + 2*pad:.2f}" height="{h + 1.1:.2f}" rx="0.7" '
            f'fill="#14161a" opacity="0.90"/>')

def font_text_paths(x, y, s, size, color, letter_spacing=0.0):
    """Return real lowercase DejaVu outlines; NanoSVG cannot render <text>."""
    from fontTools.pens.svgPathPen import SVGPathPen
    from fontTools.ttLib import TTFont

    font_path = os.path.join(
        os.environ.get("ProgramFiles", r"C:\Program Files"),
        "VCV", "Rack2Pro", "res", "fonts", "DejaVuSans.ttf")
    font = TTFont(font_path)
    glyphs = font.getGlyphSet()
    cmap = font.getBestCmap()
    hmtx = font["hmtx"]
    units = font["head"].unitsPerEm
    scale = size / units
    spacing_units = letter_spacing / scale

    records = []
    cursor = 0.0
    for i, ch in enumerate(s):
        name = cmap[ord(ch)]
        pen = SVGPathPen(glyphs)
        glyphs[name].draw(pen)
        records.append((cursor, pen.getCommands()))
        cursor += hmtx[name][0]
        if i + 1 < len(s):
            cursor += spacing_units

    out = [f'<g transform="translate({x:.3f},{y:.3f}) scale({scale:.8f},-{scale:.8f})">']
    for offset, path in records:
        out.append(f'<path d="{path}" transform="translate({offset:.3f},0)" fill="{color}"/>')
    out.append('</g>')
    return "\n".join(out)

svg = []
svg.append(f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}mm" height="{H}mm" viewBox="0 0 {W} {H}">')
svg.append(f'<rect x="0" y="0" width="{W}" height="{H}" fill="{BG}"/>')
# acacia dusk artwork (ComfyUI render, traced) as the full-bleed backplate
import re as _re
_art = open(r"D:\comfy\panels\chimera_dusk.svg", encoding="utf-8").read()
_vb = _re.search(r'viewBox="([^"]+)"', _art).group(1).split()
_inner = _art[_art.index(">", _art.index("<svg")) + 1 : _art.rindex("</svg>")]
svg.append(f'<g transform="scale({W/float(_vb[2]):.6f} {H/float(_vb[3]):.6f})">{_inner}</g>')
# A restrained veil keeps the illustration behind the controls instead of
# competing with them, without losing the dusk palette.
svg.append(f'<rect x="0" y="0" width="{W}" height="{H}" fill="#11131a" opacity="0.13"/>')
svg.append(f'<rect x="0.35" y="0.35" width="{W-0.7}" height="{H-0.7}" fill="none" stroke="#14161a" stroke-width="0.7"/>')

# ---- header: bullets + letterspaced title + blue subtitle (Timiszoara style)
tw = 6.4 * (4.8 / 6.0) * 7
svg.append(f'<rect x="{W/2 - tw/2 - 7.2}" y="{TITLE[1] - 3.4}" width="{tw + 14.4}" height="6.8" rx="1.4" fill="#14161a"/>')
svg.append(text_paths(*TITLE, "CHIMERA", 4.8, FG, weight=0.95, spacing=6.4))
svg.append(f'<circle cx="{W/2 - tw/2 - 3.4}" cy="{TITLE[1]}" r="0.95" fill="{FG}"/>')
svg.append(f'<circle cx="{W/2 + tw/2 + 3.4}" cy="{TITLE[1]}" r="0.95" fill="{FG}"/>')
svg.append(chip(W / 2, 12.4, "STEREO SPLICE SAMPLER", 1.8))
svg.append(text_paths(W / 2, 12.4, "STEREO SPLICE SAMPLER", 1.8, BLUE, weight=0.42))

# ---- transport labels
for x, label in zip(BTN_X, ["REC", "PLAY", "SPLICE", "SYNC", "MODE"]):
    svg.append(chip(x, YA_LABEL, label, 1.6))
    svg.append(text_paths(x, YA_LABEL, label, 1.6, FG))

# ---- sos / eq / chop
svg.append(chip(SOS_X, YB_LABEL, "SOS", 1.8))
svg.append(text_paths(SOS_X, YB_LABEL, "SOS", 1.8, FG))
svg.append(f'<rect x="33.5" y="27.6" width="57.5" height="18.4" rx="1.4" fill="none" stroke="#f2efe8" stroke-width="0.32"/>')
svg.append(chip(37.6, 29.6, "EQ", 1.5))
svg.append(text_paths(37.6, 29.6, "EQ", 1.5, FG))
for x, label in zip(EQ_X, ["LOW", "MID", "HIGH"]):
    svg.append(chip(x, YB_LABEL, label, 1.6))
    svg.append(text_paths(x, YB_LABEL, label, 1.6, FG))
svg.append(chip(CHOP_X, YB_LABEL, "CHOP", 1.5))
svg.append(text_paths(CHOP_X, YB_LABEL, "CHOP", 1.5, FG))

# ---- main row
svg.append(chip(SPEED_X, YC_LABEL, "SPEED", 2.1))
svg.append(text_paths(SPEED_X, YC_LABEL, "SPEED", 2.1, FG))
svg.append(chip(PITCH_X, YC_LABEL, "PITCH", 2.1))
svg.append(text_paths(PITCH_X, YC_LABEL, "PITCH", 2.1, FG))
svg.append(chip(GRAIN_X, YC_LABEL, "GRAIN", 1.9))
svg.append(text_paths(GRAIN_X, YC_LABEL, "GRAIN", 1.9, FG))
svg.append(chip(SLICE_X, YC_LABEL, "SLICE", 1.9))
svg.append(text_paths(SLICE_X, YC_LABEL, "SLICE", 1.9, FG))
svg.append(chip(SCATTER_X, YC_LABEL, "SCAT", 1.9))
svg.append(text_paths(SCATTER_X, YC_LABEL, "SCAT", 1.9, FG))

# ---- reel display frame
svg.append(f'<rect x="{DISP_X-0.7}" y="{DISP_Y-0.7}" width="{DISP_W+1.4}" height="{DISP_H+1.4}" rx="1.4" fill="#0c0d10" stroke="{DIM}" stroke-width="0.4"/>')

# ---- output block (dark, like Timiszoara's output zone)
OB_X, OB_Y, OB_W, OB_H = 74.5, 96.6, 33.0, 28.6
svg.append(f'<rect x="{OB_X}" y="{OB_Y}" width="{OB_W}" height="{OB_H}" rx="1.8" fill="#1b1c20"/>')

JACKS = [
    (J6[0], R1, "SPEED", 0), (J6[1], R1, "V/OCT", 0), (J6[2], R1, "GRAIN", 0),
    (J6[3], R1, "SLICE", 0), (J6[4], R1, "SCAT", 0), (J6[5], R1, "CLOCK", 0),
    (J6[0], R2, "REC", 0), (J6[1], R2, "SPLICE", 0), (J6[2], R2, "IN L", 0),
    (J6[3], R2, "IN R", 0), (J6[4], R2, "EOS", 1), (J6[5], R2, "ENV", 1),
    (J6[4], R3, "OUT L", 1), (J6[5], R3, "OUT R", 1),
]
for x, y, label, is_out in JACKS:
    inside = x > OB_X and y > OB_Y
    rcol = ACCENT if is_out else "#f2efe8"
    svg.append(ring(x, y, 4.4, rcol))
    if not inside:
        svg.append(chip(x, y - 6.4, label, 1.5))
    svg.append(text_paths(x, y - 6.4, label, 1.5, "#f2efe8"))

# ---- Half a Giraf family marks: full Bad Sector-size giraffe and wordmark
LOGO_MAIN = "M 144.456 7.255 C 144.184 7.964, 144.082 18.884, 144.231 31.522 L 144.500 54.500 151.250 54.796 L 158 55.091 158 84.046 L 158 113 170.500 113 L 183 113 183 84.042 L 183 55.084 190.750 54.792 L 198.500 54.500 198.500 30.500 L 198.500 6.500 171.725 6.234 C 150.612 6.024, 144.845 6.240, 144.456 7.255 M 232 30.500 L 232 55 239 55 L 246 55 246 84 L 246 113 258.864 113 L 271.727 113 272.265 102.750 C 272.561 97.112, 272.669 86.425, 272.506 79 C 272.342 71.575, 272.438 63.138, 272.719 60.250 L 273.230 55 280.115 55 L 287 55 287 30.500 L 287 6 259.500 6 L 232 6 232 30.500 M 6.477 117.678 C 9.269 143.937, 26.269 168.765, 49.385 180.345 C 63.322 187.326, 68.877 188.197, 100.500 188.358 L 128.500 188.500 128.766 162.430 L 129.033 136.360 118.106 130.106 C 88.827 113.347, 82.068 111.722, 39.626 111.241 L 5.752 110.856 6.477 117.678 M 148.300 148 C 148.300 151.025, 148.487 152.262, 148.716 150.750 C 148.945 149.238, 148.945 146.762, 148.716 145.250 C 148.487 143.738, 148.300 144.975, 148.300 148 M 272.402 188 C 272.402 196.525, 272.556 200.012, 272.743 195.750 C 272.931 191.488, 272.931 184.512, 272.743 180.250 C 272.556 175.988, 272.402 179.475, 272.402 188 M 148.425 195 C 148.425 206.825, 148.569 211.662, 148.746 205.750 C 148.923 199.838, 148.923 190.162, 148.746 184.250 C 148.569 178.338, 148.425 183.175, 148.425 195 M 272.434 240.500 C 272.433 254.250, 272.574 260.014, 272.747 253.308 C 272.919 246.603, 272.920 235.353, 272.748 228.308 C 272.576 221.264, 272.434 226.750, 272.434 240.500 M 148.409 256.500 C 148.408 265.850, 148.558 269.810, 148.743 265.299 C 148.928 260.789, 148.929 253.139, 148.745 248.299 C 148.562 243.460, 148.410 247.150, 148.409 256.500 M 192.089 252.089 C 186.168 258.963, 182.104 264.490, 182.611 264.979 C 184.178 266.487, 232.948 285.893, 233.388 285.182 C 233.762 284.577, 205.861 243.461, 203.419 241.019 C 202.800 240.400, 198.744 244.363, 192.089 252.089 M 272.468 334 C 272.468 365.075, 272.594 377.788, 272.749 362.250 C 272.904 346.713, 272.904 321.288, 272.749 305.750 C 272.594 290.213, 272.468 302.925, 272.468 334 M 144.242 367.750 L 144.500 426.500 171.609 426.766 L 198.718 427.032 199.350 421.766 C 199.698 418.870, 199.987 408.593, 199.991 398.930 L 200 381.360 172.901 345.180 C 157.996 325.281, 145.392 309, 144.893 309 C 144.355 309, 144.089 332.998, 144.242 367.750 M 217 493.500 L 217 578 244.500 578 L 272 578 272 528.422 L 272 478.845 264.750 469.547 C 260.762 464.434, 248.769 448.719, 238.099 434.625 C 227.428 420.531, 218.316 409, 217.849 409 C 217.382 409, 217 447.025, 217 493.500 M 145 596.500 L 145 639.032 172.250 638.766 L 199.500 638.500 199.500 596.500 L 199.500 554.500 172.250 554.234 L 145 553.968 145 596.500 M 71 670 L 71 703 100 703 L 129 703 129 670 L 129 637 100 637 L 71 637 71 670 M 246.486 693.981 C 198.343 704.867, 159.739 751.267, 158.695 799.500 L 158.500 808.500 215.500 808.500 L 272.500 808.500 272.813 750.250 L 273.125 692 263.813 692.084 C 258.691 692.131, 250.894 692.984, 246.486 693.981 M 71 764.500 L 71 809.031 99.750 808.765 L 128.500 808.500 128.500 764.500 L 128.500 720.500 99.750 720.235 L 71 719.969 71 764.500"
LOGO_GOLD = "M 149 208.710 L 149 285.134 167.805 310.317 C 178.148 324.168, 191.063 341.575, 196.505 349 C 210.688 368.351, 226.481 389.720, 250.500 422.060 L 271.500 450.336 271.753 292.005 C 271.892 204.922, 271.742 133.409, 271.420 133.086 C 271.097 132.764, 243.421 132.452, 209.917 132.393 L 149 132.286 149 208.710 M 192.089 252.089 C 186.168 258.963, 182.104 264.490, 182.611 264.979 C 184.178 266.487, 232.948 285.893, 233.388 285.182 C 233.762 284.577, 205.861 243.461, 203.419 241.019 C 202.800 240.400, 198.744 244.363, 192.089 252.089 M 145 490.525 L 145 536.049 172 535.967 L 199 535.885 199 490.443 L 199 445 172 445 L 145 445 145 490.525 M 217 635 L 217 674 244.500 674 L 272 674 272 635 L 272 596 244.500 596 L 217 596 217 635 M 145 682.500 L 145 709 165.757 709 L 186.514 709 192 704.500 C 195.017 702.025, 197.827 700, 198.243 700 C 198.659 700, 199 690.100, 199 678 L 199 656 172 656 L 145 656 145 682.500"
ls = 14.5 / 815.0
logo_x = W - 0.6 - 293.0 * ls
svg.append(f'<g transform="translate({logo_x:.3f}, 8.6) scale({ls:.6f})">')
svg.append(f'<path d="{LOGO_MAIN}" fill="{FG}"/>')
svg.append(f'<path d="{LOGO_GOLD}" fill="{ACCENT}"/>')
svg.append('</g>')
svg.append('<g transform="translate(20.5,123.0)">')
svg.append(f'<path d="M-3.2,1.6 A3.4,3.4 0 0 1 3.2,1.6" fill="none" stroke="{BRAND_DIM}" stroke-width="0.55" opacity="0.9"/>')
svg.append(f'<path d="M-1.7,1.6 A1.8,1.8 0 0 1 1.7,1.6" fill="none" stroke="{BRAND_DIM}" stroke-width="0.55" opacity="0.55"/>')
svg.append(f'<circle cx="0" cy="1.6" r="0.42" fill="{BRAND_DIM}"/>')
svg.append('</g>')
svg.append(font_text_paths(25.5, 125.4, "halfagiraf", 2.2, BRAND_DIM, letter_spacing=1.1))

svg.append('</svg>')

base = os.path.dirname(os.path.abspath(__file__))
with open(os.path.join(base, "res", "Chimera.svg"), "w") as f:
    f.write("\n".join(svg) + "\n")

hpp = f"""// Generated by gen_chimera.py - do not edit by hand.
#pragma once

namespace clayout {{
constexpr float YA = {YA}f;
constexpr float BTN_X[5] = {{{', '.join(f'{x}f' for x in BTN_X)}}};
constexpr float SOS_X = {SOS_X}f;
constexpr float EQ_X[3] = {{{', '.join(f'{x}f' for x in EQ_X)}}};
constexpr float CHOP_X = {CHOP_X}f;
constexpr float YB = {YB}f;
constexpr float SPEED_X = {SPEED_X}f, PITCH_X = {PITCH_X}f;
constexpr float GRAIN_X = {GRAIN_X}f, SLICE_X = {SLICE_X}f, SCATTER_X = {SCATTER_X}f;
constexpr float YC = {YC}f;
constexpr float DISP_X = {DISP_X}f, DISP_Y = {DISP_Y}f, DISP_W = {DISP_W}f, DISP_H = {DISP_H}f;
constexpr float R1 = {R1}f, R2 = {R2}f, R3 = {R3}f;
constexpr float J6[6] = {{{', '.join(f'{x}f' for x in J6)}}};
}}
"""
with open(os.path.join(base, "src", "layout_chimera.hpp"), "w") as f:
    f.write(hpp)

print("wrote res/Chimera.svg and src/layout_chimera.hpp")
