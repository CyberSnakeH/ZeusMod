"""Generate a simple ZeusMod icon (lightning bolt on dark background)."""
import struct


def create_ico(path):
    # Generate a single 256x256 icon so NSIS/electron-builder accepts it.
    size = 256
    pixels = []

    # Lightning bolt shape authored on a 32x32 grid, then scaled up.
    bolt = set()
    for y, xs in [
        (4, [14,15,16]),
        (5, [13,14,15]),
        (6, [12,13,14]),
        (7, [11,12,13]),
        (8, [10,11,12]),
        (9, [9,10,11]),
        (10, [8,9,10,11,12,13,14,15,16,17,18,19]),
        (11, [9,10,11,12,13,14,15,16,17,18]),
        (12, [14,15,16,17]),
        (13, [13,14,15,16]),
        (14, [12,13,14,15]),
        (15, [11,12,13,14]),
        (16, [10,11,12,13]),
        (17, [9,10,11,12,13,14,15,16,17,18,19,20]),
        (18, [10,11,12,13,14,15,16,17,18,19]),
        (19, [16,17,18]),
        (20, [15,16,17]),
        (21, [14,15,16]),
        (22, [13,14,15]),
        (23, [12,13,14]),
        (24, [11,12,13]),
        (25, [10,11,12]),
        (26, [9,10,11]),
        (27, [8,9,10]),
    ]:
        for x in xs:
            bolt.add((x, y))

    scale = size // 32
    scaled_bolt = set()
    for x, y in bolt:
        for sy in range(scale):
            for sx in range(scale):
                scaled_bolt.add((x * scale + sx, y * scale + sy))

    # Generate BGRA pixel data (bottom-up for ICO)
    for y in range(size-1, -1, -1):
        for x in range(size):
            if (x, y) in scaled_bolt:
                # Neon cyan: RGB(0, 200, 255)
                pixels.extend([255, 200, 0, 255])  # BGRA
            else:
                # Dark background: RGB(8, 8, 16)
                pixels.extend([16, 8, 8, 255])  # BGRA

    pixel_data = bytes(pixels)

    # AND mask (all zeros = fully opaque)
    and_mask = bytes(size * (size // 8))

    # BMP info header
    bmp_header = struct.pack('<IiiHHIIiiII',
        40,          # header size
        size,        # width
        size * 2,    # height (doubled for ICO)
        1,           # planes
        32,          # bits per pixel
        0,           # compression
        len(pixel_data) + len(and_mask),  # image size
        0, 0,        # pixels per meter
        0, 0         # colors
    )

    image_data = bmp_header + pixel_data + and_mask

    # ICO header
    ico_header = struct.pack('<HHH', 0, 1, 1)  # reserved, type=ICO, count=1

    # ICO directory entry
    ico_dir = struct.pack('<BBBBHHII',
        0,           # width (0 means 256)
        0,           # height (0 means 256)
        0,           # colors
        0,           # reserved
        1,           # planes
        32,          # bits per pixel
        len(image_data),  # size
        6 + 16       # offset (header + dir entry)
    )

    with open(path, 'wb') as f:
        f.write(ico_header + ico_dir + image_data)

    print(f"Icon created: {path}")


create_ico(r"C:\Users\anisl\OneDrive\Documents\IcarusMod\IcarusInjector\zeusmod.ico")
