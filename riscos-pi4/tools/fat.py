#!/usr/bin/env python3
"""Read and write files on a card image's FAT16 boot partition.

The RISC OS Pi card boots from the first partition, a FAT16 filesystem
SDFS presents as SDFS::SD_FSDisc.$; !Boot/Choices/Boot/PreDesk is where
the HostFS module soft-loads from.  This tool edits the partition in a
raw image file (convert qcow2 overlays to raw and back — qemu-img drops
internal snapshots on conversion, so bake before overlaying).

Names are 8.3, uppercase; RISC OS filetype extensions map as usual
(ffa = relocatable module, ff8 = Obey).  Long names are not written:
the 8.3 short name is what SDFS shows.

    fat.py image.img ls [PATH]
    fat.py image.img write LOCALFILE /Guest/Path/Name,ffa
    fat.py image.img read /Guest/Path /Local/Out
"""

import struct
import sys

SECTOR = 512


class Fat16:
    def __init__(self, path):
        self.f = open(path, "r+b")
        self._partition()
        self._bpb()

    def _partition(self):
        self.f.seek(0x1BE)
        for _ in range(4):
            e = self.f.read(16)
            if e[4] in (0x01, 0x04, 0x06, 0x0B, 0x0C, 0x0E):
                self.part_lba, = struct.unpack("<I", e[8:12])
                return
        raise SystemExit("no FAT partition in the MBR")

    def _bpb(self):
        self.f.seek(self.part_lba * SECTOR)
        bpb = self.f.read(SECTOR)
        self.bps, = struct.unpack("<H", bpb[11:13])
        self.spc = bpb[13]
        reserved, = struct.unpack("<H", bpb[14:16])
        self.nfats = bpb[16]
        rootents, = struct.unpack("<H", bpb[17:19])
        self.spf, = struct.unpack("<H", bpb[22:24])
        if self.spf == 0:
            raise SystemExit("FAT32 not supported; this tool is FAT16")
        base = self.part_lba * SECTOR
        self.fat_off = base + reserved * self.bps
        self.root_off = self.fat_off + self.nfats * self.spf * self.bps
        self.root_bytes = rootents * 32
        self.data_off = self.root_off + self.root_bytes
        self.cluster = self.spc * self.bps
        self.fat_bytes = self.spf * self.bps
        self.f.fat = None

    # ---- FAT table ----

    def fat_load(self):
        self.f.seek(self.fat_off)
        self.fat = bytearray(self.f.read(self.fat_bytes))

    def fat_flush(self):
        for i in range(self.nfats):
            self.f.seek(self.fat_off + i * self.fat_bytes)
            self.f.write(self.fat)

    def fat_get(self, n):
        return struct.unpack("<H", self.fat[2 * n:2 * n + 2])[0]

    def fat_set(self, n, v):
        struct.pack_into("<H", self.fat, 2 * n, v)

    def chain(self, first):
        out = []
        c = first
        seen = set()
        while 2 <= c < 0xFFF8 and c not in seen and c != 0:
            seen.add(c)
            out.append(c)
            c = self.fat_get(c)
        return out

    def free_cluster(self):
        n = self.data_off // self.cluster + 2 - 2  # first data cluster no.
        # cluster numbers start at 2 for the first data sector
        first = 2
        total = self.fat_bytes // 2
        for c in range(first, total):
            if self.fat_get(c) == 0:
                return c
        raise SystemExit("disc full")

    def alloc_chain(self, nbytes):
        """Return (first, clusters) for a fresh chain holding nbytes."""
        need = max(1, (nbytes + self.cluster - 1) // self.cluster)
        clusters = []
        for _ in range(need):
            c = self.free_cluster()
            self.fat_set(c, 0xFFFF)
            clusters.append(c)
        for a, b in zip(clusters, clusters[1:]):
            self.fat_set(a, b)
        self.fat_flush()
        return clusters[0], clusters

    def cluster_off(self, c):
        return self.data_off + (c - 2) * self.cluster

    # ---- directories ----

    def read_dir(self, clusters):
        """clusters: list for a dir chain, or None for the fixed root."""
        ents = []
        if clusters is None:
            self.f.seek(self.root_off)
            raw = self.f.read(self.root_bytes)
            for i in range(0, len(raw), 32):
                e = raw[i:i + 32]
                if e[0] and e[0] != 0xE5 and e[11] != 0x0F:
                    ents.append((e, None, i))
        else:
            raw = b""
            for c in clusters:
                self.f.seek(self.cluster_off(c))
                raw += self.f.read(self.cluster)
            for ci, c in enumerate(clusters):
                for k in range(self.cluster // 32):
                    off = ci * self.cluster + k * 32
                    e = raw[off:off + 32]
                    if e[0] and e[0] != 0xE5 and e[11] != 0x0F:
                        ents.append((e, c, k * 32))
        return ents

    @staticmethod
    def name_of(e):
        base = e[:8].decode("ascii", "replace").rstrip()
        ext = e[8:11].decode("ascii", "replace").rstrip()
        return base + ("." + ext if ext else "")

    def find(self, path):
        """-> (dirent, dir_clusters, slot) or None."""
        parts = [p for p in path.strip("/").split("/") if p]
        clusters = None                      # root
        for i, part in enumerate(parts):
            hit = None
            for e, c, slot in self.read_dir(clusters):
                if self.name_of(e).upper() == part.upper():
                    hit = (e, c, slot)
                    break
            if not hit:
                return None
            e, c, slot = hit
            if i == len(parts) - 1:
                return hit
            if not (e[11] & 0x10):
                raise SystemExit(f"{part}: not a directory")
            clusters = self.chain(
                struct.unpack("<H", e[26:28])[0])
        return None

    def dir_clusters_of(self, dirpath):
        if not dirpath or dirpath == "/":
            return None
        hit = self.find(dirpath)
        if not hit:
            raise SystemExit(f"{dirpath}: no such directory")
        e = hit[0]
        if not (e[11] & 0x10):
            raise SystemExit(f"{dirpath}: not a directory")
        return self.chain(struct.unpack("<H", e[26:28])[0])

    # ---- files ----

    def read_file(self, path):
        self.fat_load()
        hit = self.find(path)
        if not hit:
            raise SystemExit(f"{path}: not found")
        e = hit[0]
        size, = struct.unpack("<I", e[28:32])
        first, = struct.unpack("<H", e[26:28])
        out = b""
        for c in self.chain(first):
            self.f.seek(self.cluster_off(c))
            out += self.f.read(self.cluster)
        return out[:size]

    @staticmethod
    def shortname(name):
        """RISC OS 'HostFS,ffa' -> 8.3 'HOSTFS  .FFA'-style bytes."""
        base, _, ext = name.replace(",", ".").partition(".")
        base = base.upper()[:8].ljust(8)
        ext = ext.upper()[:3].ljust(3)
        return base.encode("ascii") + ext.encode("ascii")

    def write_file(self, path, data):
        self.fat_load()
        parent, _, leaf = path.strip("/").rpartition("/")
        clusters = self.dir_clusters_of("/" + parent if parent else "/")
        if self.find(path):
            raise SystemExit(f"{path}: already exists")

        # The first free dirent: byte 0 (never used) or 0xE5 (deleted).
        # If the directory has no free slot and is a cluster chain, it
        # grows by one cluster.
        def scan(root, size):
            self.f.seek(root)
            raw = self.f.read(size)
            for off in range(0, len(raw), 32):
                if raw[off] in (0x00, 0xE5):
                    return root + off
            return None

        slot_at = None
        if clusters is None:
            slot_at = scan(self.root_off, self.root_bytes)
        else:
            for c in clusters:
                slot_at = scan(self.cluster_off(c), self.cluster)
                if slot_at is not None:
                    break
        if slot_at is None:
            if clusters is None:
                raise SystemExit("root directory full")
            new = self.free_cluster()
            self.fat_set(clusters[-1], new)
            self.fat_set(new, 0xFFFF)
            self.fat_flush()
            clusters.append(new)
            slot_at = self.cluster_off(new)

        first, chain = self.alloc_chain(len(data))
        for i, c in enumerate(chain):
            self.f.seek(self.cluster_off(c))
            chunk = data[i * self.cluster:(i + 1) * self.cluster]
            self.f.write(chunk + bytes(self.cluster - len(chunk)))

        ent = bytearray(32)
        ent[0:11] = self.shortname(leaf)
        ent[11] = 0x20                          # archive
        ent[26:28] = struct.pack("<H", first)
        ent[28:32] = struct.pack("<I", len(data))
        self.f.seek(slot_at)
        self.f.write(bytes(ent))
        self.f.flush()
        print(f"wrote {path} ({len(data)} bytes, cluster {first})")


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    img, op = sys.argv[1], sys.argv[2]
    fs = Fat16(img)
    if op == "ls":
        path = sys.argv[3] if len(sys.argv) > 3 else "/"
        fs.fat_load()
        clusters = fs.dir_clusters_of(path)
        for e, c, slot in fs.read_dir(clusters):
            size, = struct.unpack("<I", e[28:32])
            kind = "dir" if e[11] & 0x10 else f"{size:>8}"
            print(f"{kind}  {fs.name_of(e)}")
    elif op == "read":
        open(sys.argv[4], "wb").write(fs.read_file(sys.argv[3]))
        print(f"read {sys.argv[3]} -> {sys.argv[4]}")
    elif op == "write":
        data = open(sys.argv[3], "rb").read()
        fs.write_file(sys.argv[4], data)
    else:
        print(__doc__)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
