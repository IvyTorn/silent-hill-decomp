"""PSX TIM image parser + per-CLUT-row RGBA export.

Map TIMs are 4bpp with a 16xN CLUT (one row per palette variant); a map prim's
`clut` field selects the row (row = clut // 64). Raw texel value 0x0000 is fully
transparent (PSX rule); the STP bit (bit 15) marks semi-transparent texels, which
map terrain renders additively — kept opaque here, flagged for the caller.
"""

import struct
from dataclasses import dataclass, field


@dataclass
class Tim:
    pmode: int              # 0=4bpp 1=8bpp 2=16bpp 3=24bpp
    clut_x: int
    clut_y: int
    clut_w: int             # entries per row (16 for map TIMs)
    clut_h: int             # number of rows
    clut: bytes             # raw u16 entries, row-major (may be empty)
    pix_x: int
    pix_y: int
    stored_w: int           # halfwords per row
    height: int
    pixels: bytes           # raw pixel data

    @property
    def width(self):
        return self.stored_w * (4, 2, 1, 1)[self.pmode]

    @classmethod
    def parse(cls, data):
        magic, flags = struct.unpack_from("<II", data, 0)
        if magic != 0x10:
            raise ValueError(f"bad TIM magic 0x{magic:x}")
        pmode = flags & 7
        off = 8
        clut_x = clut_y = clut_w = clut_h = 0
        clut = b""
        if flags & 8:
            size, clut_x, clut_y, clut_w, clut_h = struct.unpack_from("<IHHHH", data, off)
            clut = data[off + 12 : off + 12 + clut_w * clut_h * 2]
            off += size
        size, pix_x, pix_y, stored_w, height = struct.unpack_from("<IHHHH", data, off)
        pixels = data[off + 12 : off + 12 + stored_w * height * 2]
        return cls(pmode, clut_x, clut_y, clut_w, clut_h, clut,
                   pix_x, pix_y, stored_w, height, pixels)

    def clut_entry(self, row, idx):
        return struct.unpack_from("<H", self.clut, (row * self.clut_w + idx) * 2)[0]

    @property
    def bpp(self):
        return (4, 8, 16, 24)[self.pmode]

    def palette(self, row):
        """CLUT row as a list of u16 PSX-1555 entries, or None if out of range."""
        if row < 0 or row >= self.clut_h:
            return None
        n = self.clut_w
        return list(struct.unpack_from("<%dH" % n, self.clut, row * n * 2))

    def pixels_rect(self, u0, v0, w, h):
        """Indexed pixels of a texel rect, row-major, PSX packing (4bpp: low
        nibble = left texel, w texels -> w/2 bytes; 8bpp: 1 byte per texel).
        Out-of-range texels read as 0. u0 and w must be even for 4bpp."""
        out = bytearray()
        if self.pmode == 0:
            assert (u0 & 1) == 0 and (w & 1) == 0, (u0, w)
            row_bytes = self.stored_w * 2
            for y in range(v0, v0 + h):
                if 0 <= y < self.height:
                    base = y * row_bytes
                    row = self.pixels[base + (u0 >> 1) : base + ((u0 + w) >> 1)]
                    row = row + b"\x00" * ((w >> 1) - len(row))
                else:
                    row = b"\x00" * (w >> 1)
                out += row
        elif self.pmode == 1:
            row_bytes = self.stored_w * 2
            for y in range(v0, v0 + h):
                if 0 <= y < self.height:
                    base = y * row_bytes
                    row = self.pixels[base + u0 : base + u0 + w]
                    row = row + b"\x00" * (w - len(row))
                else:
                    row = b"\x00" * w
                out += row
        else:
            raise ValueError("pixels_rect: only 4/8bpp")
        return bytes(out)

    def rgba_for_clut_row(self, row):
        """Full image as flat RGBA bytes using CLUT row `row` (4/8bpp) or raw (16bpp).

        Returns (rgba_bytes, has_stp) where has_stp reports whether any visible
        texel had the STP semi-transparency bit set.
        """
        w, h = self.width, self.height
        out = bytearray(w * h * 4)
        has_stp = False

        def emit(i, c16):
            nonlocal has_stp
            if c16 == 0:
                return  # already 0,0,0,0
            r = (c16 & 0x1F) << 3
            g = ((c16 >> 5) & 0x1F) << 3
            b = ((c16 >> 10) & 0x1F) << 3
            if c16 & 0x8000:
                has_stp = True
            out[i * 4 + 0] = r | (r >> 5)
            out[i * 4 + 1] = g | (g >> 5)
            out[i * 4 + 2] = b | (b >> 5)
            out[i * 4 + 3] = 255

        if self.pmode == 0:
            for y in range(h):
                base = y * self.stored_w * 2
                for x in range(w):
                    byte = self.pixels[base + (x >> 1)]
                    idx = (byte >> ((x & 1) * 4)) & 0xF  # low nibble = left pixel
                    emit(y * w + x, self.clut_entry(row, idx))
        elif self.pmode == 1:
            for y in range(h):
                base = y * self.stored_w * 2
                for x in range(w):
                    emit(y * w + x, self.clut_entry(row, self.pixels[base + x]))
        elif self.pmode == 2:
            for i in range(w * h):
                emit(i, struct.unpack_from("<H", self.pixels, i * 2)[0])
        else:
            raise ValueError("24bpp TIM not supported")
        return bytes(out), has_stp

    def save_png(self, path, row=0):
        from PIL import Image
        rgba, has_stp = self.rgba_for_clut_row(row)
        img = Image.frombytes("RGBA", (self.width, self.height), rgba)
        img.save(path)
        return has_stp
