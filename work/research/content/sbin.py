"""SBIN v3 decoder (Firemonkeys IronMonkey .sb / .prefabs.sb / .scene.sb). Research tool, read-only.
Sections: ENUM STRU FIEL OHDR DATA CHDR CDAT (each: tag, u32 size, u32 fnv1a). OHDR[i] = (offset<<3)|kind,
kind 0 = typed struct (u16 struct idx + body), 1 = untyped map (u16 count, u16 size, members {u16 key,u16 type,u32 off}),
2 = array (u32 elemtype | structidx<<16, u32 count, elements). Types: 1 i8 2 u8 3 i16 4 u16 5 i32 6 u32 7 i64 8 u64
9 bool 10 f32 11 f64 12 enum? 13 string(u16) 15 objref(u32) 16 inline struct 17 array ref(u32)."""
import struct, sys, json
SIZES = {1:1, 2:1, 3:2, 4:2, 5:4, 6:4, 7:8, 8:8, 9:1, 10:4, 11:8, 12:4, 13:2, 14:4, 15:4, 17:4, 18:4, 20:2}
FMT = {1:'<b', 2:'<B', 3:'<h', 4:'<H', 5:'<i', 6:'<I', 7:'<q', 8:'<Q', 9:'<B', 10:'<f', 11:'<d'}
class SB:
    def __init__(self, data):
        assert data[:4] == b'SBIN', 'not SBIN'
        secs = {}; off = 8
        while off + 12 <= len(data):
            tag = data[off:off+4]
            if not tag.isalpha():
                off = (off + 3) & ~3; continue
            size, crc = struct.unpack_from('<II', data, off+4)
            secs[tag.decode()] = data[off+12:off+12+size]; off += 12 + size
        self.secs = secs
        ch = secs.get('CHDR', b''); cd = secs.get('CDAT', b'')
        self.strs = [cd[o:o+l].decode('utf-8', 'replace') for o, l in struct.iter_unpack('<II', ch)]
        st = secs.get('STRU', b'')
        self.structs = [struct.unpack_from('<HHH', st, i) for i in range(0, len(st) - len(st) % 6, 6)]
        fi = secs.get('FIEL', b'')
        self.fields = [struct.unpack_from('<HHHH', fi, i) for i in range(0, len(fi) - len(fi) % 8, 8)]
        oh = secs.get('OHDR', b'')
        self.ohdr = list(struct.unpack_from('<%dI' % (len(oh)//4), oh))
        self.D = secs.get('DATA', b'')
        self.enum_raw = secs.get('ENUM', b'')
        self.warn = []; self.cache = {}; self.ssize = {}
    def s(self, i):
        return self.strs[i] if 0 <= i < len(self.strs) else '<str#%d>' % i
    def struct_size(self, si):
        if si in self.ssize: return self.ssize[si]
        name, ff, fc = self.structs[si]
        end = 0; al = 1
        for fidx in range(ff, ff + fc):
            fn, ft, fo, fx = self.fields[fidx]
            sz = self.struct_size(fx) if ft == 16 else SIZES.get(ft, 4)
            a = 4 if ft == 16 else min(4, SIZES.get(ft, 4))
            al = max(al, a); end = max(end, fo + sz)
        end = (end + al - 1) // al * al
        self.ssize[si] = end
        return end
    def inline(self, si, pos):
        name, ff, fc = self.structs[si]
        res = {'__type': self.s(name)}
        for fidx in range(ff, ff + fc):
            fn, ft, fo, fx = self.fields[fidx]
            res[self.s(fn)] = self.prim(ft, pos + fo, fx)
        return res
    def prim(self, t, pos, extra=0):
        D = self.D
        if t in FMT:
            v = struct.unpack_from(FMT[t], D, pos)[0]
            if t == 9: v = bool(v)
            if t == 10: v = round(v, 6)
            return v
        if t in (13, 20):
            return self.s(struct.unpack_from('<H', D, pos)[0])
        if t == 18:
            return struct.unpack_from('<i', D, pos)[0]
        if t in (15, 17):
            r = struct.unpack_from('<I', D, pos)[0]
            return None if r == 0xFFFFFFFF else self.obj(r)
        if t == 16:
            return self.inline(extra, pos)
        if t == 12:
            return {'enum': struct.unpack_from('<I', D, pos)[0]}
        self.warn.append('unknown type %d at %d' % (t, pos))
        return {'unk_type': t, 'raw': D[pos:pos+4].hex()}
    def obj(self, idx):
        if idx in self.cache: return self.cache[idx]
        if idx >= len(self.ohdr): return {'bad_ref': idx}
        e = self.ohdr[idx]; off = e >> 3; kind = e & 7; D = self.D
        self.cache[idx] = {'__cycle': idx}
        if kind == 1:
            cnt, size = struct.unpack_from('<HH', D, off)
            pos = off + 4; res = {}
            for _ in range(cnt):
                k, t, vo = struct.unpack_from('<HHI', D, pos)
                res[self.s(k)] = self.prim(t, off + vo)
                pos = (off + vo + SIZES.get(t, 4) + 1) & ~1
        elif kind == 0:
            si = struct.unpack_from('<H', D, off)[0]
            res = self.inline(si, off + 2)
        elif kind == 2:
            et, n = struct.unpack_from('<II', D, off)
            t = et & 0xffff; si = et >> 16
            if t == 16:
                es = self.struct_size(si); base = (off + 8 + 3) & ~3
                res = [self.inline(si, base + i*es) for i in range(n)]
            else:
                es = SIZES.get(t, 4); base = off + 8
                if es >= 4: base = (base + 3) & ~3
                res = [self.prim(t, base + i*es) for i in range(n)]
        else:
            res = {'unk_kind': kind, 'off': off}; self.warn.append('unk kind %d' % kind)
        self.cache[idx] = res
        return res
    def root(self):
        return self.obj(0)
def load(path):
    return SB(open(path, 'rb').read())
if __name__ == '__main__':
    sb = load(sys.argv[1])
    print(json.dumps(sb.root(), indent=1, ensure_ascii=False, default=str))
    if sb.warn: print('WARN', sb.warn[:20], file=sys.stderr)
