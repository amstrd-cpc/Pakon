#!/usr/bin/env python3
"""Prototype: run our scan through the OEM Pakon look (stage 2).

Our .pakraw + .scan.json + .frames.json  ->  per frame the OEM's PakonIMAu
input (TLB's negative density x 1000, 1000x1500 u16 planar, plus a 250x375
Ansel thumbnail)  ->  an oem_host script that replays TLB's PakonIMAu
sequence (CN-Enhanced roll, AddScene per frame, AnalyzeRoll, scene balance,
scale to 3000x2000 rotated 270, ColorAdjust)  ->  8-bit planar  ->  PNG.

Evidence (docs/OEM_RE.md §13, VM proxy runs of 2026-10-14):
- TLB's input to PakonIMAu is density x 1000 relative to the open gate,
  R/B rows aligned +-4 lines (Base 4), fixed 1500-line frames; on river1
  vs the OEM's frame 1 it is a linear map of ours, fitted 3x4 below
  (R2 0.98, rms 0.02 D) -- TLB's exact formula is not recovered yet.
- The Ansel thumbnail is a 4x4 reduction rotated 180 degrees; dmin is the
  film base density per channel (TLB: 607/876/1111 on the river strip).
- Every PakonIMAu step replays byte for byte in tools/oem_host (script).

The descriptor templates (struct images captured from TLB's calls) are
read from --capture; the OEM DLLs from --oem. Neither is in this repo.

usage: pakon_look.py <prefix> --capture DIR --work DIR [--frames N] [--frames-json F]
then:  oem_host.exe script <oem dir> <capture>\\settings.bin <work>\\look.txt
       pakon_look.py <prefix> --capture DIR --work DIR --png
"""
import argparse
import array
import json
import math
import os
import struct
import zlib

# TLB density = M @ [D_R, D_G, D_B, 1], ours D = -1000 log10(t) (river1 fit).
M = [[0.9184, 0.0702, -0.0330, 207.2352],
     [0.0821, 0.9687, -0.0116, 452.7056],
     [-0.0249, -0.1871, 1.2039, 655.5540]]
W, H = 1000, 1500          # OEM frame, Base 4: CCD pixels x lines
TW, TH = 250, 375          # Ansel thumbnail
SHIFT = 4                  # tri-linear CCD rows, Base 4 lines
PIXEL_OFFSET = -1          # OEM column x = our film pixel x - 1


def load(prefix, frames_json):
    side = json.load(open(prefix + '.scan.json'))
    frames = json.load(open(frames_json or prefix + '.frames.json'))
    f = open(prefix + '.film.pakraw', 'rb')
    spr, rows = struct.unpack_from('<II', f.read(44), 0x0C)
    return side, frames, f, spr, rows


def density_tables(side):
    c = side['corrections']
    sh = side['geometry']['film']['start'] - side['geometry']['calibration']['start']
    dark = [[c['dark']['planes'][ch][p + sh] for p in range(W)] for ch in range(3)]
    span = [[max(1.0, c['white']['planes'][ch][p + sh] - dark[ch][p]) for p in range(W)] for ch in range(3)]
    return dark, span


def frame_density(f, spr, rows, first, dark, span):
    """OEM-scale density planes (3 x H x W, u16) for lines [first, first + H)."""
    out = [array.array('H', bytes(2 * W * H)) for _ in range(3)]
    cache = {}

    def row(r):
        r = min(max(r, 0), rows - 1)
        if r not in cache:
            f.seek(44 + r * spr * 2)
            a = array.array('H')
            a.frombytes(f.read(spr * 2))
            cache[r] = a
            if len(cache) > 64:
                cache.pop(min(cache))
        return cache[r]

    log10 = math.log10
    for y in range(H):
        line = first + y
        rr, rg, rb = row(line - SHIFT), row(line), row(line + SHIFT)
        for x in range(W):
            p = min(max(x + PIXEL_OFFSET, 0), W - 1)
            d = (-1000 * log10(max(1e-4, (rr[3 * p] - dark[0][p]) / span[0][p])),
                 -1000 * log10(max(1e-4, (rg[3 * p + 1] - dark[1][p]) / span[1][p])),
                 -1000 * log10(max(1e-4, (rb[3 * p + 2] - dark[2][p]) / span[2][p])))
            for ch in range(3):
                m = M[ch]
                v = m[0] * d[0] + m[1] * d[1] + m[2] * d[2] + m[3]
                out[ch][y * W + x] = 0 if v < 0 else 65535 if v > 65535 else int(v + 0.5)
    return out


def thumbnail(planes):
    """4x4 box mean, rotated 180 degrees (what Ansel gets from TLB)."""
    t = array.array('H', bytes(2 * 3 * TW * TH))
    for ch in range(3):
        P = planes[ch]
        for y in range(TH):
            for x in range(TW):
                s = 0
                for dy in range(4):
                    b = (4 * y + dy) * W + 4 * x
                    s += P[b] + P[b + 1] + P[b + 2] + P[b + 3]
                t[ch * TW * TH + (TH - 1 - y) * TW + (TW - 1 - x)] = (s + 8) // 16
    return t


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('prefix')
    ap.add_argument('--capture', required=True)
    ap.add_argument('--work', required=True)
    ap.add_argument('--frames', type=int, default=0)
    ap.add_argument('--frames-json', default='')
    ap.add_argument('--png', action='store_true')
    a = ap.parse_args()
    os.makedirs(a.work, exist_ok=True)
    side, frames, f, spr, rows = load(a.prefix, a.frames_json)
    pics = [fr for fr in frames['frames'] if fr.get('kind', 'picture') == 'picture']
    if a.frames:
        pics = pics[:a.frames]
    if a.png:
        for i in range(len(pics)):
            raw = os.path.join(a.work, f'look{i + 1:02}.raw')
            if os.path.exists(raw):
                write_png(raw, os.path.join(a.work, f'look{i + 1:02}.png'))
        return
    dark, span = density_tables(side)
    script = ['# generated by tools/pakon_look.py', f'buf roll blob {a.capture}\\roll.bin 4096',
              'call PIAnselStartNewRoll @roll 1']
    firsts = []
    for i, fr in enumerate(pics):
        mid = (fr['first_line'] + fr['end_line']) // 2
        first = min(max(mid - H // 2, 0), rows - H)
        firsts.append(first)
        planes = frame_density(f, spr, rows, first, dark, span)
        dens = os.path.join(a.work, f'dens{i + 1:02}.raw')
        with open(dens, 'wb') as o:
            for p in planes:
                o.write(p.tobytes())
        with open(os.path.join(a.work, f'thumb{i + 1:02}.raw'), 'wb') as o:
            o.write(thumbnail(planes).tobytes())
        print(f'frame {i + 1}: lines {first}..{first + H}', flush=True)
    # dmin = 1st percentile per channel over all thumbnails (the clearest
    # film = the base), a stand-in for TLB's own measurement.
    vals = [[], [], []]
    for i in range(len(pics)):
        t = array.array('H')
        t.frombytes(open(os.path.join(a.work, f'thumb{i + 1:02}.raw'), 'rb').read())
        for ch in range(3):
            vals[ch] += t[ch * TW * TH:(ch + 1) * TW * TH:7].tolist()
    dmin = [sorted(v)[len(v) // 100] for v in vals]
    print('dmin', dmin)
    w = a.work
    for i in range(len(pics)):
        n = i + 1
        script += [f'buf scene{n} blob {a.capture}\\scene_4.bin 4096',
                   f'buf img{n} blob {w}\\thumb{n:02}.raw',
                   f'buf desc{n} blob {a.capture}\\adddesc_4.bin',
                   f'pokeref desc{n} 0 img{n}',
                   f'poke desc{n} 0x54 {dmin[0]:x}', f'poke desc{n} 0x58 {dmin[1]:x}',
                   f'poke desc{n} 0x5c {dmin[2]:x}',
                   f'call PIAnselAddScene @roll @scene{n} @desc{n}']
    script += ['call PIAnselEndRoll @roll 1', 'call PIAnselAnalyzeRoll @roll']
    s43 = ['3e8', '5dc', 'bb8', '7d0', '3', '10e', '2']
    for i in range(len(pics)):
        n = i + 1
        script += [f'buf in{n} blob {w}\\dens{n:02}.raw',
                   f'buf big{n} zero 36000000',
                   f'buf bal{n} blob {a.capture}\\baldesc_42.bin',
                   f'pokeref bal{n} 0 in{n}', f'pokeref bal{n} 4 big{n}',
                   f'call PIAnselColorSceneBalancePlanar @roll @scene{n} @bal{n}',
                   f'call PIScaleAndRotatePlanar @big{n} 1 ' + ' '.join(s43) +
                   f' @s43a9 2 @s43a11 @scene{n} @scene{n} @s43a14 @s43a15',
                   f'buf out{n} zero 18000000',
                   f'buf ca{n} blob {a.capture}\\coloradjust44_a0.bin',
                   f'pokeref ca{n} 0 big{n}', f'pokeref ca{n} 4 out{n}',
                   f'call PIColorAdjustPlanar @ca{n}',
                   f'save out{n} 18000000 {w}\\look{n:02}.raw',
                   f'buf in{n} zero 4', f'buf big{n} zero 4', f'buf out{n} zero 4']
    pre = [f'buf s43a{k} blob {a.capture}\\s43\\a{k}.bin' for k in (9, 11, 14, 15)]
    with open(os.path.join(a.work, 'look.txt'), 'w') as o:
        o.write('\n'.join(script[:3] + pre + script[3:]) + '\n')
    print('wrote', os.path.join(a.work, 'look.txt'))


def write_png(raw, png):
    b = open(raw, 'rb').read()
    w, h = 3000, 2000
    n = w * h
    rows = []
    for y in range(h):
        line = bytearray([0])
        o = y * w
        r, g, bl = b[o:o + w], b[n + o:n + o + w], b[2 * n + o:2 * n + o + w]
        inter = bytearray(3 * w)
        inter[0::3], inter[1::3], inter[2::3] = r, g, bl
        rows.append(bytes(line + inter))

    def ck(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    with open(png, 'wb') as o:
        o.write(b'\x89PNG\r\n\x1a\n' + ck(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                ck(b'IDAT', zlib.compress(b''.join(rows), 6)) + ck(b'IEND', b''))
    print('wrote', png)


if __name__ == '__main__':
    main()
