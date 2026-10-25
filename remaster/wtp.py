"""WTA (texture table) + WTP (concatenated DDS) for NieR:Automata: unpack to DDS files and repack."""
import struct

WTP_ALIGN = 0x1000


def _align(n, a):
    return (n + a - 1) // a * a


class Wta:
    """WTB v3 table: per-texture offset into the WTP, size, flags, id, and 20-byte info record."""

    def __init__(self, buf):
        magic, self.version, count, o_off, o_size, o_flag, o_id, o_info = struct.unpack_from("<4s7I", buf, 0)
        if magic != b"WTB\0":
            raise ValueError("not a WTA table")
        self.offsets = list(struct.unpack_from(f"<{count}I", buf, o_off))
        self.sizes = list(struct.unpack_from(f"<{count}I", buf, o_size))
        self.flags = list(struct.unpack_from(f"<{count}I", buf, o_flag))
        self.ids = [f"{i:08x}" for i in struct.unpack_from(f"<{count}I", buf, o_id)]
        self.info = [buf[o_info + i * 20:o_info + (i + 1) * 20] for i in range(count)] if o_info else []
        self._raw = bytearray(buf)
        self._o_off, self._o_size = o_off, o_size

    def dxgi_format(self, i):
        if i >= len(self.info) or len(self.info[i]) < 4:
            return None
        return struct.unpack_from("<I", self.info[i], 0)[0]

    def to_bytes(self):
        out = bytearray(self._raw)
        struct.pack_into(f"<{len(self.offsets)}I", out, self._o_off, *self.offsets)
        struct.pack_into(f"<{len(self.sizes)}I", out, self._o_size, *self.sizes)
        return bytes(out)


def unpack(wta_bytes, wtp_bytes):
    """Return (Wta, {id: dds_bytes})."""
    wta = Wta(wta_bytes)
    return wta, {tid: wtp_bytes[o:o + s] for tid, o, s in zip(wta.ids, wta.offsets, wta.sizes)}


def repack(wta, textures):
    """Rebuild WTP from {id: dds_bytes} in table order; updates wta offsets/sizes. Returns (wta_bytes, wtp_bytes)."""
    wtp = bytearray()
    for i, tid in enumerate(wta.ids):
        dds = textures[tid]
        if dds[:4] != b"DDS ":
            raise ValueError(f"texture {tid} is not a DDS file")
        wtp += b"\0" * (_align(len(wtp), WTP_ALIGN) - len(wtp))
        wta.offsets[i] = len(wtp)
        wta.sizes[i] = len(dds)
        wtp += dds
    return wta.to_bytes(), bytes(wtp)


def dds_info(dds):
    """(width, height, mip_count, fourcc) from a DDS header."""
    h, w, _pitch, _depth, mips = struct.unpack_from("<5I", dds, 12)
    return w, h, max(mips, 1), dds[84:88].decode("ascii", "replace")
