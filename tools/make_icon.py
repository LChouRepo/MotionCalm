"""make_icon.py — 仅用 Python 标准库生成 src/res/icon.ico。

图案与产品一致：深色圆形暗角（中心偏亮的灰蓝、向边缘渐暗到近黑，外缘一圈淡色描边方便在深色任务栏上辨认）
+ 白色中心参照点（深色描边）；48 px 及以上再画 8 个淡色边缘参照点。

条目：16 / 32 / 48 px 为 32-bit BGRA DIB（带 AND 掩码，兼容所有工具链），256 px 为 PNG（Vista+ 标准做法）。
输出是确定性的（同样的脚本 → 同样的字节），可以直接提交。

用法：python tools/make_icon.py [--out PATH] [--preview PNG]
"""

import argparse
import math
import os
import struct
import sys
import zlib

SIZES = (16, 32, 48, 256)


def smoothstep(e0, e1, x):
    if e1 == e0:
        return 1.0 if x >= e1 else 0.0
    t = (x - e0) / (e1 - e0)
    t = 0.0 if t < 0.0 else (1.0 if t > 1.0 else t)
    return t * t * (3.0 - 2.0 * t)


def lerp(a, b, t):
    return a + (b - a) * t


def mix(c0, c1, t):
    return tuple(lerp(a, b, t) for a, b in zip(c0, c1))


# 颜色（线性 0..255）
CENTER_COL = (78, 112, 138)   # 清晰区：柔和的灰蓝
EDGE_COL = (8, 10, 14)        # 暗角最深处
RIM_COL = (120, 132, 146)     # 外缘描边
DOT_COL = (255, 255, 255)
DOT_OUTLINE = (16, 18, 22)
EDGE_DOT_COL = (200, 200, 200)


def sample(size, px, py):
    """返回 (r, g, b, a)，a ∈ [0,1]；px, py 为像素坐标（可为子像素）。"""
    c = size / 2.0
    dx, dy = px - c, py - c
    dist = math.hypot(dx, dy)
    R = size * 0.47                      # 圆盘半径
    if dist > R + 0.5:
        return (0.0, 0.0, 0.0, 0.0)
    r = dist / R                         # 0 = 中心，1 = 圆盘边缘

    # 暗角：中心灰蓝 → 边缘近黑（椭圆清晰区的意思）
    col = mix(CENTER_COL, EDGE_COL, smoothstep(0.18, 0.92, r))

    # 外缘细描边
    rim_w = max(0.9, size * 0.022) / R   # 归一化宽度
    col = mix(col, RIM_COL, 0.75 * smoothstep(1.0 - rim_w * 1.6, 1.0 - rim_w * 0.4, r))

    # 8 个边缘参照点（48 px 及以上）
    if size >= 48:
        ring = 0.74
        er = max(1.2, size * 0.022)
        for k in range(8):
            ang = k * math.pi / 4.0
            ex = c + math.cos(ang) * R * ring
            ey = c + math.sin(ang) * R * ring
            d = math.hypot(px - ex, py - ey)
            if d < er:
                col = mix(col, EDGE_DOT_COL, 0.55)

    # 中心白点 + 深色描边
    dot_r = max(1.6, size * 0.075)
    outline = max(0.9, size * 0.03)
    if dist <= dot_r:
        col = DOT_COL
    elif dist <= dot_r + outline:
        col = mix(col, DOT_OUTLINE, 0.85)

    alpha = 1.0 if dist <= R - 0.5 else max(0.0, min(1.0, R + 0.5 - dist))
    return (col[0], col[1], col[2], alpha)


def render(size):
    """超采样渲染；返回自上而下的行，每行是 [(r,g,b,a), ...]，各分量 0..255 整数。"""
    ss = 4 if size <= 48 else 3
    rows = []
    inv = 1.0 / (ss * ss)
    for y in range(size):
        row = []
        for x in range(size):
            pr = pg = pb = pa = 0.0
            for sy in range(ss):
                for sx in range(ss):
                    r, g, b, a = sample(size, x + (sx + 0.5) / ss, y + (sy + 0.5) / ss)
                    pr += r * a
                    pg += g * a
                    pb += b * a
                    pa += a
            if pa <= 0.0:
                row.append((0, 0, 0, 0))
                continue
            a = pa * inv
            row.append((
                int(round(pr / pa)),
                int(round(pg / pa)),
                int(round(pb / pa)),
                int(round(a * 255.0)),
            ))
        rows.append(row)
    return rows


def png_bytes(rows):
    h = len(rows)
    w = len(rows[0])
    raw = bytearray()
    for row in rows:
        raw.append(0)  # filter: none
        for (r, g, b, a) in row:
            raw += bytes((r, g, b, a))

    def chunk(tag, data):
        body = tag + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    ihdr = struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)   # 8-bit RGBA
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr)
            + chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b""))


def dib_bytes(rows):
    """ICO 用的 32-bit DIB：BITMAPINFOHEADER(高度 ×2) + 自下而上 BGRA + 1-bit AND 掩码。"""
    h = len(rows)
    w = len(rows[0])
    xor = bytearray()
    for row in reversed(rows):
        for (r, g, b, a) in row:
            xor += bytes((b, g, r, a))
    mask_stride = ((w + 31) // 32) * 4
    andm = bytearray()
    for row in reversed(rows):
        line = bytearray(mask_stride)
        for x, (_, _, _, a) in enumerate(row):
            if a == 0:
                line[x // 8] |= 0x80 >> (x % 8)
        andm += line
    header = struct.pack("<IiiHHIIiiII", 40, w, h * 2, 1, 32, 0, len(xor) + len(andm), 0, 0, 0, 0)
    return header + bytes(xor) + bytes(andm)


def ico_bytes(images):
    """images: [(size, data_bytes)]"""
    out = bytearray(struct.pack("<HHH", 0, 1, len(images)))
    offset = 6 + 16 * len(images)
    blobs = []
    for size, data in images:
        dim = 0 if size >= 256 else size
        out += struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
        blobs.append(data)
    for data in blobs:
        out += data
    return bytes(out)


def main(argv):
    repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap = argparse.ArgumentParser(description="Generate src/res/icon.ico (standard library only).")
    ap.add_argument("--out", default=os.path.join(repo, "src", "res", "icon.ico"))
    ap.add_argument("--preview", help="also write the 256 px image to this PNG path")
    args = ap.parse_args(argv)

    images = []
    preview = None
    for size in SIZES:
        rows = render(size)
        if size >= 256:
            data = png_bytes(rows)
            preview = data
        else:
            data = dib_bytes(rows)
        images.append((size, data))

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    blob = ico_bytes(images)
    with open(args.out, "wb") as f:
        f.write(blob)
    print("wrote %s (%d bytes; %s)" % (args.out, len(blob), ", ".join("%dpx" % s for s in SIZES)))

    if args.preview and preview is not None:
        with open(args.preview, "wb") as f:
            f.write(preview)
        print("wrote preview %s" % args.preview)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
