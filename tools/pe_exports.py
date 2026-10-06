"""Minimal PE export-table dumper (no deps). Used to decide native-hook feasibility."""
import struct, sys, re

def parse(path):
    d = open(path, 'rb').read()
    e_lfanew = struct.unpack_from('<I', d, 0x3C)[0]
    assert d[e_lfanew:e_lfanew + 4] == b'PE\0\0', 'not PE'
    coff = e_lfanew + 4
    machine, nsec, _, _, _, optsize, _ = struct.unpack_from('<HHIIIHH', d, coff)
    opt = coff + 20
    is64 = struct.unpack_from('<H', d, opt)[0] == 0x20B
    dd = opt + (112 if is64 else 96)
    exp_rva, exp_size = struct.unpack_from('<II', d, dd)
    sec_off = opt + optsize
    secs = []
    for i in range(nsec):
        o = sec_off + i * 40
        nm = d[o:o + 8].rstrip(b'\0').decode('latin1')
        vsize, vaddr, rawsize, rawptr = struct.unpack_from('<IIII', d, o + 8)
        secs.append((nm, vaddr, vsize, rawptr, rawsize))
    def r2o(rva):
        for nm, va, vs, rp, rs in secs:
            if va <= rva < va + max(vs, rs):
                return rp + (rva - va)
        return None
    if not exp_rva:
        return [], 0, 0
    eo = r2o(exp_rva)
    nfun, nnam = struct.unpack_from('<II', d, eo + 20)
    an = struct.unpack_from('<I', d, eo + 32)[0]
    no = r2o(an)
    out = []
    for i in range(nnam):
        nr = struct.unpack_from('<I', d, no + i * 4)[0]
        o = r2o(nr)
        out.append(d[o:d.index(b'\0', o)].decode('latin1'))
    return out, nfun, nnam

if __name__ == '__main__':
    path = sys.argv[1]
    pat = re.compile(sys.argv[2], re.I) if len(sys.argv) > 2 else None
    names, nfun, nnam = parse(path)
    print(f'FILE={path}')
    print(f'exports: nfunc={nfun} nnames={nnam} parsed={len(names)}')
    if pat:
        hits = [n for n in names if pat.search(n)]
        print(f'matched({pat.pattern}): {len(hits)}')
        for h in hits[:80]:
            print('  ', h)
    else:
        for n in names[:200]:
            print('  ', n)
