#!/usr/bin/env python3
"""Побитовая сверка двух PNG на равенство пикселей.

Зачем не PIL: PIL не применяет tRNS-ключ для серых изображений и режет 16 бит
до 8, из-за чего корректные файлы выглядят как несовпадающие. Здесь IDAT
распаковывается вручную, а канонизация повторяет ту, что делает кодер:
  - PLTE            -> RGB (плюс альфа из tRNS, если он есть);
  - серый < 8 бит   -> 8 бит с масштабированием, как png_set_expand_gray_1_2_4_to_8;
  - tRNS-ключ       -> реальный альфа-канал;
  - 16 бит          -> остаются 16 битами.
Код возврата 0 при равенстве, 1 при расхождении.
"""
import struct
import sys
import zlib

import numpy as np


def read_png(path):
    d = open(path, 'rb').read()
    if d[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError(f'{path}: не PNG')
    pos, idat, ihdr, plte, trns = 8, [], None, None, None
    while pos + 8 <= len(d):
        ln, = struct.unpack('>I', d[pos:pos + 4])
        typ = d[pos + 4:pos + 8]
        body = d[pos + 8:pos + 8 + ln]
        if typ == b'IHDR':
            ihdr = struct.unpack('>IIBBBBB', body)
        elif typ == b'PLTE':
            plte = body
        elif typ == b'tRNS':
            trns = body
        elif typ == b'IDAT':
            idat.append(body)
        elif typ == b'IEND':
            break
        pos += 12 + ln
    w, h, depth, ctype, _, _, interlace = ihdr
    nch = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]
    raw = zlib.decompress(b''.join(idat))
    samples = (deinterlace(raw, w, h, depth, nch) if interlace
               else unpack_samples(unfilter(raw, 0, w, h, depth, nch), w, depth, nch))
    return w, h, depth, ctype, nch, plte, trns, samples


# Adam7: начальные смещения и шаги для каждого из семи проходов.
ADAM7 = ((0, 0, 8, 8), (4, 0, 8, 8), (0, 4, 4, 8), (2, 0, 4, 4),
         (0, 2, 2, 4), (1, 0, 2, 2), (0, 1, 1, 2))


def unfilter(u, off, w, h, depth, nch):
    """Снять фильтры строк, вернуть (h, stride) байтов. Начинает с u[off]."""
    bpp = max(1, nch * depth // 8)
    stride = (w * nch * depth + 7) // 8
    out = np.empty((h, stride), np.uint8)
    prev = bytearray(stride)
    o = off
    for y in range(h):
        ft = u[o]
        o += 1
        line = bytearray(u[o:o + stride])
        o += stride
        if ft == 1:
            for i in range(bpp, stride):
                line[i] = (line[i] + line[i - bpp]) & 255
        elif ft == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 255
        elif ft == 3:
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + (a + prev[i]) // 2) & 255
        elif ft == 4:
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                b = prev[i]
                c = prev[i - bpp] if i >= bpp else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 255
        elif ft:
            raise ValueError(f'неизвестный тип фильтра строки: {ft}')
        out[y] = np.frombuffer(bytes(line), np.uint8)
        prev = line
    return out, o


def deinterlace(u, w, h, depth, nch):
    """Собрать семь проходов Adam7 в единый массив (h, w, nch)."""
    full = np.zeros((h, w, nch), np.uint16)
    o = 0
    for x0, y0, dx, dy in ADAM7:
        pw = (w - x0 + dx - 1) // dx
        ph = (h - y0 + dy - 1) // dy
        if pw <= 0 or ph <= 0:
            continue
        rows, o = unfilter(u, o, pw, ph, depth, nch)
        full[y0::dy, x0::dx] = unpack_samples(rows, pw, depth, nch)
    return full


def unpack_samples(rows, w, depth, nch):
    """Строки байтов -> массив (h, w, nch) с сэмплами в native-порядке."""
    if isinstance(rows, tuple):
        rows = rows[0]
    h = rows.shape[0]
    if depth == 16:
        a = np.ascontiguousarray(rows).view('>u2').reshape(h, -1)[:, :w * nch]
        return a.astype(np.uint16).reshape(h, w, nch)
    if depth == 8:
        return rows[:, :w * nch].reshape(h, w, nch).astype(np.uint16)
    per = 8 // depth
    mask = (1 << depth) - 1
    flat = np.empty((h, rows.shape[1] * per), np.uint16)
    for k in range(per):
        shift = 8 - depth * (k + 1)
        flat[:, k::per] = (rows >> shift) & mask
    return flat[:, :w * nch].reshape(h, w, nch)


def canonical(path):
    w, h, depth, ctype, nch, plte, trns, s = read_png(path)
    maxv = (1 << depth) - 1

    if ctype == 3:  # палитра -> RGB(A)
        if not plte:
            raise ValueError(f'{path}: PLTE отсутствует')

        pal = np.frombuffer(plte, np.uint8).reshape(-1, 3).astype(np.uint16)
        idx = s[:, :, 0]
        px = pal[idx]
        if trns:
            al = np.full(len(pal), 255, np.uint16)
            t = np.frombuffer(trns, np.uint8).astype(np.uint16)
            al[:len(t)] = t
            px = np.concatenate([px, al[idx][:, :, None]], axis=2)
        return 8, px

    if depth < 8:  # серый < 8 бит -> 8 бит, как это делает libpng
        key = trns and struct.unpack('>H', trns)[0]
        scale = 255 // maxv
        out = s * scale
        if trns is not None:
            al = np.where(s[:, :, 0] == key, 0, 255).astype(np.uint16)
            out = np.concatenate([out, al[:, :, None]], axis=2)
        return 8, out

    if trns is not None and ctype in (0, 2):  # tRNS-ключ -> альфа-канал
        if ctype == 0:
            key = struct.unpack('>H', trns)[0]
            hit = s[:, :, 0] == key
        else:
            key = np.array(struct.unpack('>HHH', trns), np.uint16)
            hit = (s == key).all(axis=2)
        al = np.where(hit, 0, maxv).astype(np.uint16)
        s = np.concatenate([s, al[:, :, None]], axis=2)
    return depth, s


def main():
    if len(sys.argv) != 3:
        print('usage: pngcmp.py a.png b.png', file=sys.stderr)
        return 2
    try:
        da, a = canonical(sys.argv[1])
        db, b = canonical(sys.argv[2])
    except Exception as exc:  # разбор не удался — считаем сверку провалившейся
        print(f'pngcmp: {exc}', file=sys.stderr)
        return 1
    if da != db or a.shape != b.shape:
        print(f'pngcmp: форма/глубина разошлись {da}{a.shape} vs {db}{b.shape}',
              file=sys.stderr)
        return 1
    if not np.array_equal(a, b):
        diff = np.abs(a.astype(int) - b.astype(int))
        print(f'pngcmp: пиксели разошлись, max по каналам '
              f'{diff.reshape(-1, a.shape[2]).max(0).tolist()}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
