#!/usr/bin/env python3
"""Convert the mod's DX10-header DDS dumps to PNG (tonemapped for float formats)."""
import sys, struct, os
import numpy as np
from PIL import Image

FMT = {  # dxgi -> (dtype, channels, kind)
    2: ('f4', 4, 'f'), 10: ('f2', 4, 'f'), 11: ('u2', 4, 'unorm16'), 16: ('f4', 2, 'f'),
    24: ('u4', 1, 'r10g10b10a2'), 26: ('u4', 1, 'r11g11b10'),
    28: ('u1', 4, 'u8'), 29: ('u1', 4, 'u8'), 87: ('u1', 4, 'bgra'), 88: ('u1', 4, 'bgrx'), 91: ('u1', 4, 'bgra'),
    34: ('f2', 2, 'f'), 35: ('u2', 2, 'unorm16'), 41: ('f4', 1, 'f'), 54: ('f2', 1, 'f'), 56: ('u2', 1, 'unorm16'),
    61: ('u1', 1, 'u8'), 65: ('u1', 1, 'u8'), 49: ('u1', 2, 'u8'), 46: ('u4', 1, 'r24'),
}

def f11(v, bits):
    m = v & ((1 << (bits - 5)) - 1); e = (v >> (bits - 5)) & 31
    out = np.where(e == 0, m / (1 << (bits - 5)) * 2.0 ** -14, (1 + m / (1 << (bits - 5))) * 2.0 ** (e.astype(np.float64) - 15))
    return out.astype(np.float32)

def load(path):
    d = open(path, 'rb').read()
    h, w = struct.unpack_from('<II', d, 12)
    fmt = struct.unpack_from('<I', d, 128)[0]
    raw = d[148:]
    dt, ch, kind = FMT.get(fmt, (None, 0, None))
    if dt is None:
        raise ValueError(f'unsupported dxgi format {fmt}')
    a = np.frombuffer(raw, dtype=np.dtype(dt).newbyteorder('<'))[: w * h * ch].reshape(h, w, ch)
    if kind == 'r11g11b10':
        v = a[..., 0]
        img = np.stack([f11(v & 0x7FF, 11), f11((v >> 11) & 0x7FF, 11), f11((v >> 22) & 0x3FF, 10)], -1)
        return img, True
    if kind == 'r10g10b10a2':
        v = a[..., 0]
        img = np.stack([(v & 1023), (v >> 10) & 1023, (v >> 20) & 1023], -1) / 1023.0
        return img.astype(np.float32), False
    if kind == 'r24':
        return ((a[..., 0] & 0xFFFFFF) / float(0xFFFFFF)).astype(np.float32)[..., None], False
    if kind in ('bgra', 'bgrx'):
        a = a[..., [2, 1, 0, 3]]
    if kind == 'f':
        return a.astype(np.float32), True
    if kind == 'unorm16':
        return (a / 65535.0).astype(np.float32), False
    return (a / 255.0).astype(np.float32), False

def to_png(path, out=None):
    img, hdr = load(path)
    if img.shape[2] >= 3:
        rgb = img[..., :3]
    elif img.shape[2] == 2:
        rgb = np.concatenate([img, np.zeros_like(img[..., :1])], -1)
    else:
        v = img[..., 0]
        lo, hi = np.percentile(v, 1), np.percentile(v, 99)
        rgb = np.repeat(((v - lo) / max(hi - lo, 1e-6))[..., None], 3, -1)
        hdr = False
    if hdr:
        rgb = np.nan_to_num(rgb, nan=0, posinf=0, neginf=0)
        rgb = rgb / (1.0 + rgb)  # reinhard
        rgb = np.power(np.clip(rgb, 0, 1), 1 / 2.2)
    im = Image.fromarray((np.clip(rgb, 0, 1) * 255).astype(np.uint8))
    out = out or os.path.splitext(path)[0] + '.png'
    im.save(out)
    return out

if __name__ == '__main__':
    for p in sys.argv[1:]:
        try:
            print(to_png(p))
        except Exception as e:
            print(p, 'ERR', e)
