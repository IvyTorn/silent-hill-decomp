"""IPD collision: structured parse and re-serialize.

The block at file offset 0x54 is a 2D spatial index over two kinds of element:

  WALLS   ("subcells") - a line segment between two split vertices. The stored
          (dirX, dirZ) is the UNIT direction in Q12 from splitVertices[idx1] to
          splitVertices[idx0] (the runtime swaps them, see func_8006B318), and
          `length` is that segment's XZ length in Q8. Verified against retail:
          |(dirX,dirZ)| is 4096 in every sampled subcell and `length` equals the
          segment length exactly.
          surfaceIdx0/1 name the floor on each side (0xFF = none). idA/idB pick
          which event-flag bit gates the wall: the runtime tests
          `flags >> (idA*4 | idB)`, so 0/0 means "gated by bit 0" = always on.

  FLOORS  ("surfaces") - a tilted plane, not a bounded polygon. Ground height at
          (x,z) is `base + tiltX*(x-relX) + tiltZ*(z-relZ)` in collision-local
          coordinates (func_8006CA18). Extent comes purely from which grid cells
          list it. -Y is up, so the SMALLEST height wins.

  GRID    `ranges[z*countX + x]` holds prefix offsets into the wall list (ptr28)
          and the floor list (ptr2c); a cell's elements are
          [ranges[i].wall, ranges[i+1].wall). Hence countX*countZ + 1 entries.
          A wall index >= subcellCount addresses ptr18 instead.

Everything is kept as per-cell membership lists so re-serializing untouched data
reproduces the original bytes exactly, and adding an element only appends to the
cells it actually touches.
"""

import struct

COLL_BASE = 0x54


def _s14(w):
    v = w & 0x3FFF
    return v - 0x4000 if v >= 0x2000 else v


def _u14(v):
    return v & 0x3FFF


class Wall:
    """A collision line segment (an IPD 'subcell')."""
    __slots__ = ("dir_x", "dir_z", "length", "id_a", "id_b",
                 "vert0", "vert1", "surface0", "surface1")

    def __init__(self, dir_x, dir_z, length, id_a, id_b, vert0, vert1,
                 surface0, surface1):
        self.dir_x, self.dir_z, self.length = dir_x, dir_z, length
        self.id_a, self.id_b = id_a, id_b
        self.vert0, self.vert1 = vert0, vert1      # indices into split_verts
        self.surface0, self.surface1 = surface0, surface1

    def pack(self):
        w0 = _u14(self.dir_x) | (self.id_a << 14)
        w1 = _u14(self.dir_z) | (self.id_b << 14)
        return struct.pack("<HHh4B", w0, w1, self.length, self.vert0,
                           self.vert1, self.surface0, self.surface1)

    @staticmethod
    def unpack(b):
        w0, w1, length, v0, v1, s0, s1 = struct.unpack("<HHh4B", b)
        return Wall(_s14(w0), _s14(w1), length, w0 >> 14, w1 >> 14,
                    v0, v1, s0, s1)


class Floor:
    """A collision ground plane (an IPD 'surface')."""
    __slots__ = ("rel_x", "base_height", "rel_z", "ground_type",
                 "disable_height", "field_6_8", "field_6_11", "tilt_x", "tilt_z")

    def __init__(self, rel_x, base_height, rel_z, ground_type, disable_height,
                 field_6_8, field_6_11, tilt_x, tilt_z):
        self.rel_x, self.base_height, self.rel_z = rel_x, base_height, rel_z
        self.ground_type = ground_type
        self.disable_height = disable_height
        self.field_6_8, self.field_6_11 = field_6_8, field_6_11
        self.tilt_x, self.tilt_z = tilt_x, tilt_z

    def pack(self):
        bits = ((self.ground_type & 0x1F)
                | ((self.disable_height & 7) << 5)
                | ((self.field_6_8 & 7) << 8)
                | ((self.field_6_11 & 0xF) << 11))
        return struct.pack("<hhhHhh", self.rel_x, self.base_height, self.rel_z,
                           bits, self.tilt_x, self.tilt_z)

    @staticmethod
    def unpack(b):
        rx, base, rz, bits, tx, tz = struct.unpack("<hhhHhh", b)
        return Floor(rx, base, rz, bits & 0x1F, (bits >> 5) & 7,
                     (bits >> 8) & 7, (bits >> 11) & 0xF, tx, tz)


class Collision:
    """Structured collision block. `cell_walls` / `cell_floors` are the grid's
    per-cell membership lists, indexed z*count_x + x."""

    def __init__(self):
        self.position_x = 0
        self.position_z = 0
        self.split_verts = []      # [(x, y, z)] Q8
        self.walls = []            # [Wall]
        self.floors = []           # [Floor]
        self.obstacles = b""       # ptr18 records, kept opaque (10 B each)
        self.subcell_size = 512
        self.count_x = 0
        self.count_z = 0
        self.cell_walls = []       # [[wall index or obstacle index]]
        self.cell_floors = []      # [[floor index]]
        self.present = False       # retail files with no collision store 0s

    @property
    def cell_count(self):
        return self.count_x * self.count_z

    # ---------- parse ----------

    @classmethod
    def parse(cls, data):
        c = cls()
        C = COLL_BASE
        c.position_x, c.position_z, bits = struct.unpack_from("<iiI", data, C)
        n_sv = bits & 0xFF
        n_sf = (bits >> 8) & 0xFF
        n_sc = (bits >> 16) & 0xFF
        n_18 = (bits >> 24) & 0xFF
        o_sv, o_sf, o_sc, o_18 = struct.unpack_from("<IIII", data, C + 0xC)
        c.subcell_size, c.count_x, c.count_z = struct.unpack_from("<hBB", data, C + 0x1C)
        o_rng = struct.unpack_from("<I", data, C + 0x20)[0]
        n_28, n_2c = struct.unpack_from("<HH", data, C + 0x24)
        o_28, o_2c = struct.unpack_from("<II", data, C + 0x28)

        if not (o_sv or o_sf or o_sc or o_18 or o_rng or o_28 or o_2c):
            return c   # no collision in this chunk
        c.present = True

        c.split_verts = [struct.unpack_from("<3h", data, C + o_sv + i * 6)
                         for i in range(n_sv)]
        c.floors = [Floor.unpack(data[C + o_sf + i * 12: C + o_sf + i * 12 + 12])
                    for i in range(n_sf)]
        c.walls = [Wall.unpack(data[C + o_sc + i * 10: C + o_sc + i * 10 + 10])
                   for i in range(n_sc)]
        c.obstacles = bytes(data[C + o_18: C + o_18 + n_18 * 10])

        cells = c.cell_count
        ranges = [struct.unpack_from("<hh", data, C + o_rng + i * 4)
                  for i in range(cells + 1)] if cells else []
        p28 = data[C + o_28: C + o_28 + n_28]
        p2c = data[C + o_2c: C + o_2c + n_2c]

        c.cell_walls = [list(p28[ranges[i][0]:ranges[i + 1][0]]) for i in range(cells)]
        c.cell_floors = [list(p2c[ranges[i][1]:ranges[i + 1][1]]) for i in range(cells)]
        return c

    # ---------- serialize ----------

    def flat_lists(self):
        p28 = bytearray()
        p2c = bytearray()
        ranges = []
        for i in range(self.cell_count):
            ranges.append((len(p28), len(p2c)))
            p28.extend(self.cell_walls[i])
            p2c.extend(self.cell_floors[i])
        ranges.append((len(p28), len(p2c)))
        return bytes(p28), bytes(p2c), ranges

    def blobs(self):
        """The seven subarrays, in the canonical on-disc order."""
        if not self.present:
            return dict.fromkeys(
                ("split_verts", "surfaces", "subcells", "ptr18", "ranges",
                 "ptr28", "ptr2c"), b"")
        p28, p2c, ranges = self.flat_lists()
        return {
            "split_verts": b"".join(struct.pack("<3h", *v) for v in self.split_verts),
            "surfaces": b"".join(f.pack() for f in self.floors),
            "subcells": b"".join(w.pack() for w in self.walls),
            "ptr18": self.obstacles,
            "ranges": b"".join(struct.pack("<hh", a, b) for a, b in ranges),
            "ptr28": p28,
            "ptr2c": p2c,
        }

    def header(self, offsets):
        """The fixed 0x134-byte block, given each subarray's offset (relative to
        file+0x54, 0 when there is no collision)."""
        bits = (len(self.split_verts) | (len(self.floors) << 8)
                | (len(self.walls) << 16) | ((len(self.obstacles) // 10) << 24))
        p28, p2c, _ = self.flat_lists() if self.present else (b"", b"", [])
        out = struct.pack("<iiI", self.position_x, self.position_z, bits)
        out += struct.pack("<IIII", offsets["split_verts"], offsets["surfaces"],
                           offsets["subcells"], offsets["ptr18"])
        out += struct.pack("<hBB", self.subcell_size, self.count_x, self.count_z)
        out += struct.pack("<IHH", offsets["ranges"], len(p28), len(p2c))
        out += struct.pack("<II", offsets["ptr28"], offsets["ptr2c"])
        out += bytes(0x134 - 0x30)   # subcellCheckCount + pad + scratch, all zero
        return out

    # ---------- authoring ----------

    def grid_cell(self, x_q8, z_q8):
        """Grid cell containing a collision-local XZ position, or None."""
        if not self.count_x or self.subcell_size <= 0:
            return None
        gx = int(x_q8) // self.subcell_size
        gz = int(z_q8) // self.subcell_size
        if 0 <= gx < self.count_x and 0 <= gz < self.count_z:
            return gz * self.count_x + gx
        return None

    def cells_touched_by_segment(self, p0, p1):
        """Every grid cell the XZ segment p0->p1 passes through. Sampled at half
        a cell so no crossed cell is missed; walls must be listed in each cell
        they cross or the player walks through them from that cell."""
        if not self.count_x or self.subcell_size <= 0:
            return []
        dx, dz = p1[0] - p0[0], p1[1] - p0[1]
        dist = max(abs(dx), abs(dz))
        steps = max(2, int(dist * 2 // max(self.subcell_size, 1)) + 2)
        out = []
        for s in range(steps + 1):
            t = s / steps
            cell = self.grid_cell(p0[0] + dx * t, p0[1] + dz * t)
            if cell is not None and cell not in out:
                out.append(cell)
        return out

    def cells_touched_by_rect(self, min_x, min_z, max_x, max_z):
        if not self.count_x or self.subcell_size <= 0:
            return []
        gx0 = max(0, int(min_x) // self.subcell_size)
        gx1 = min(self.count_x - 1, int(max_x) // self.subcell_size)
        gz0 = max(0, int(min_z) // self.subcell_size)
        gz1 = min(self.count_z - 1, int(max_z) // self.subcell_size)
        return [gz * self.count_x + gx
                for gz in range(gz0, gz1 + 1) for gx in range(gx0, gx1 + 1)]

    def add_split_vertex(self, v):
        """Reuse an identical vertex when possible - the index is a u8, so the
        table cannot hold more than 256."""
        for i, existing in enumerate(self.split_verts):
            if existing == v:
                return i
        if len(self.split_verts) >= 256:
            raise ValueError("split vertex table full (256)")
        self.split_verts.append(v)
        return len(self.split_verts) - 1

    def add_wall(self, p0, p1, surface0=0xFF, surface1=0xFF, y=0,
                 cells=None):
        """Add a wall segment between two collision-local XZ points."""
        import math
        if len(self.walls) >= 256:
            raise ValueError("wall table full (256)")
        i0 = self.add_split_vertex((int(round(p0[0])), int(y), int(round(p0[1]))))
        i1 = self.add_split_vertex((int(round(p1[0])), int(y), int(round(p1[1]))))
        # Runtime direction runs splitVertices[vert1] -> splitVertices[vert0].
        dx = self.split_verts[i0][0] - self.split_verts[i1][0]
        dz = self.split_verts[i0][2] - self.split_verts[i1][2]
        length = math.hypot(dx, dz)
        if length < 1.0:
            return None
        w = Wall(int(round(dx / length * 4096)), int(round(dz / length * 4096)),
                 int(round(length)), 0, 0, i0, i1, surface0, surface1)
        self.walls.append(w)
        idx = len(self.walls) - 1
        # `cells` preserves an existing wall's original grid membership, which
        # is wider than a strict crossing test and is what stops the player
        # clipping it from a neighbouring cell.
        for cell in (cells if cells is not None
                     else self.cells_touched_by_segment(p0, p1)):
            self.cell_walls[cell].append(idx)
        return idx

    def add_floor(self, min_x, min_z, max_x, max_z, height, ground_type=0):
        """Add a flat ground plane covering an XZ rectangle (collision-local)."""
        if len(self.floors) >= 256:
            raise ValueError("floor table full (256)")
        self.floors.append(Floor(int(round(min_x)), int(round(height)),
                                 int(round(min_z)), ground_type, 0, 0, 0, 0, 0))
        idx = len(self.floors) - 1
        for cell in self.cells_touched_by_rect(min_x, min_z, max_x, max_z):
            self.cell_floors[cell].append(idx)
        return idx
