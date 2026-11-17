"""Check hole, rounded corner and opaque BlurGlass overlay in the rendered PNG."""
import struct
import sys
import zlib

def rgba_png(path):
    data = open(path, "rb").read()
    assert data[:8] == b"\x89PNG\r\n\x1a\n"
    pos, chunks = 8, []
    while pos < len(data):
        count = struct.unpack_from(">I", data, pos)[0]
        kind, payload = data[pos + 4:pos + 8], data[pos + 8:pos + 8 + count]
        if kind == b"IHDR":
            w, h, depth, color, _, _, interlace = struct.unpack(">IIBBBBB", payload)
            assert (depth, color, interlace) == (8, 6, 0)
        if kind == b"IDAT":
            chunks.append(payload)
        pos += count + 12
    raw, rows, stride = zlib.decompress(b"".join(chunks)), [], w * 4
    for y in range(h):
        offset = y * (stride + 1)
        mode, row = raw[offset], bytearray(raw[offset + 1:offset + 1 + stride])
        prior = rows[-1] if rows else bytes(stride)
        for x in range(stride):
            a, b, c = row[x - 4] if x >= 4 else 0, prior[x], prior[x - 4] if x >= 4 else 0
            if mode == 1:
                predictor = a
            elif mode == 2:
                predictor = b
            elif mode == 3:
                predictor = (a + b) // 2
            elif mode == 4:
                p = a + b - c
                distances = (abs(p - a), abs(p - b), abs(p - c))
                predictor = (a, b, c)[distances.index(min(distances))]
            else:
                assert mode == 0
                predictor = 0
            row[x] = (row[x] + predictor) & 255
        rows.append(row)
    return lambda x, y: tuple(rows[y][x * 4:x * 4 + 4])

pixel = rgba_png(sys.argv[1])
assert pixel(120, 100)[3] == 0, "portal center must punch alpha"
assert pixel(1, 1)[3] == 255, "rounded corner must preserve destination"
assert pixel(500, 400)[3] == 255, "outside portal must stay opaque"
r, g, b, a = pixel(60, 60)
assert a == 255 and r > 150 and r > g * 2, "floating BlurGlass overlay must paint above hole"
print("Portal alpha/rounded corner/BlurGlass overlay pixels PASS")
