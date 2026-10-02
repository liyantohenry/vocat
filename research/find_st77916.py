import struct, sys
BK = "./echoear_cyberai_backup_20260914_1015.bin"
d = open(BK, "rb").read()
# otadata: two 32-byte entries at 0xd000 / 0xe000 (seq number first)
for o in (0xd000, 0xe000):
    print("otadata@%#x seq=%#x" % (o, struct.unpack_from("<I", d, o)[0]))
def segments(base):
    magic, nseg = d[base], d[base+1]
    if magic != 0xE9: return []
    off = base + 24; segs = []
    if nseg > 16: return []
    for _ in range(nseg):
        if off + 8 > len(d): return []
        addr, ln = struct.unpack_from("<II", d, off)
        if ln > 0x600000 or off + 8 + ln > len(d): return segs
        segs.append((addr, off + 8, ln)); off += 8 + ln
    return segs
def v2f(segs, va):
    for addr, fo, ln in segs:
        if addr <= va < addr + ln: return fo + (va - addr)
    return None
for slot, base in (("ota_0", 0x20000), ("ota_1", 0x520000)):
    segs = segments(base)
    print("\n==", slot, "segments:", [(hex(a), hex(l)) for a, _, l in segs])
    drom = [s for s in segs if 0x3C000000 <= s[0] < 0x3E000000]
    for addr, fo, ln in drom:
        i = fo
        while i < fo + ln - 16 * 40:
            run = []; j = i
            while j < fo + ln - 16:
                cmd, ptr, n, dly = struct.unpack_from("<IIII", d, j)
                ok = cmd <= 0xFF and n <= 32 and dly <= 1000 and ((n == 0) or (0x3C000000 <= ptr < 0x3E000000))
                if not ok: break
                run.append((cmd, ptr, n, dly)); j += 16
            if len(run) >= 40 and run[0][0] in (0xF0, 0x11, 0x28, 0x01) :
                print("  table @file %#x (%d entries), first cmds: %s" % (i, len(run), " ".join("%02X" % r[0] for r in run[:12])))
                rows = []
                for cmd, ptr, n, dly in run:
                    fo2 = v2f(segs, ptr) if n else None
                    data = d[fo2:fo2+n] if fo2 is not None else b""
                    rows.append("{0x%02X, {%s}, %d, %d}" % (cmd, ", ".join("0x%02X" % b for b in data), n, dly))
                open("./scratch/cyberai_st77916_%s_%x.txt" % (slot, i), "w").write("\n".join(rows))
                i = j; continue
            i += 4
