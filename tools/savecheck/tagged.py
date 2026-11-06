"""Minimal UE tagged-property reader for VotV 0.10.0 saves. Read-only, offline."""
import struct

NATIVE_BINARY = {"Vector", "Rotator", "Quat", "Guid", "LinearColor", "Color", "DateTime", "Timespan",
                 "Vector2D", "IntPoint", "Vector4", "Plane", "Box"}


class R:
    def __init__(self, b, at=0):
        self.b, self.at = b, at

    def i32(self):
        v = struct.unpack_from("<i", self.b, self.at)[0]; self.at += 4; return v

    def i64(self):
        v = struct.unpack_from("<q", self.b, self.at)[0]; self.at += 8; return v

    def u8(self):
        v = self.b[self.at]; self.at += 1; return v

    def take(self, n):
        v = self.b[self.at:self.at + n]; self.at += n; return v

    def fstr(self):
        n = self.i32()
        if n == 0:
            return ""
        if n > 0:
            s = self.take(n)[:-1].decode("latin1")
        else:
            s = self.take(-n * 2)[:-2].decode("utf-16le")
        return s


def read_value(r, ptype, size, inner, sname):
    end = r.at + size
    if ptype == "ArrayProperty":
        return read_array(r, size, inner, end)
    if ptype == "StructProperty":
        if sname in NATIVE_BINARY or sname is None:
            return ("raw", sname, r.take(size))
        try:
            sub = R(r.b, r.at)
            props = read_props(sub, end)
            if sub.at == end:
                r.at = end
                return ("struct", sname, props)
        except Exception:
            pass
        return ("raw", sname, r.take(size))
    if ptype == "SoftObjectProperty":
        p = r.fstr(); sub = r.fstr(); r.at = end; return p + sub
    if ptype in ("StrProperty", "NameProperty"):
        v = r.fstr(); r.at = end; return v
    if ptype == "IntProperty":
        v = r.i32(); r.at = end; return v
    if ptype == "BoolProperty":
        return None
    return ("raw", ptype, r.take(size))


def read_array(r, size, inner, end):
    count = r.i32()
    if inner == "StructProperty":
        name = r.fstr(); t = r.fstr(); sz = r.i64(); sname = r.fstr(); r.take(16); r.u8()
        out = []
        for _ in range(count):
            sub = R(r.b, r.at)
            out.append(read_props(sub, end))
            r.at = sub.at
        r.at = end
        return ("structarray", sname, out)
    if inner == "ByteProperty":
        v = r.take(count); r.at = end; return ("bytes", v)
    if inner in ("NameProperty", "StrProperty"):
        out = [r.fstr() for _ in range(count)]; r.at = end; return out
    if inner == "ObjectProperty":
        out = [r.fstr() for _ in range(count)]; r.at = end; return out
    v = r.take(end - r.at); return ("raw", inner, v)


def read_props(r, limit):
    props = {}
    while r.at < limit:
        name = r.fstr()
        if name == "None":
            break
        ptype = r.fstr()
        size = r.i64()
        inner = sname = None
        if ptype == "StructProperty":
            sname = r.fstr(); r.take(16); r.u8()
        elif ptype == "ArrayProperty":
            inner = r.fstr(); r.u8()
        elif ptype in ("ByteProperty", "EnumProperty"):
            r.fstr(); r.u8()
        elif ptype == "BoolProperty":
            v = r.u8(); r.u8(); props[name] = bool(v); continue
        elif ptype == "SetProperty" or ptype == "MapProperty":
            r.fstr(); r.fstr(); r.u8()
        else:
            r.u8()
        props[name] = read_value(r, ptype, size, inner, sname)
    return props
