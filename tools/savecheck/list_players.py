"""List player-ish records in a VotV 0.10.0 world save: keys, classes, per-array inventory counts.

Read-only and offline: run it on a COPY of a save, never the live SaveGames folder.
  python list_players.py <main.sav> [more.sav ...]
"""
import sys

from decomp import decompress
from tagged import R, read_props

ARRAYS = ("canvasInventory", "slotArray_A", "slotArray_B", "slotArray_C", "slotArray_D")
HINTS = ("mainPlayer", "Mannequin", "mannequin", "puppet", "Puppet", "preview", "Preview")


def field(rec, prefix):
    for k, v in rec.items():
        if k.startswith(prefix):
            return v
    return None


def array_counts(blob):
    """Item count of each inventory array inside a save_inventoryData blob."""
    r = R(blob, 0)
    r.fstr()
    props = read_props(r, len(blob))
    out = {}
    for name in ARRAYS:
        v = props.get(name)
        out[name] = len(v[2]) if isinstance(v, tuple) and v[0] == "structarray" else None
    return out


def main(paths):
    for path in paths:
        raw = decompress(open(path, "rb").read())
        r = R(raw, 0)
        r.i32()
        r.fstr()
        slot = read_props(r, len(raw))
        recs = slot["betterObjectsData"][2]
        print("== %s: %d records" % (path, len(recs)))
        keys = {}
        for rec in recs:
            cls, key = field(rec, "class"), field(rec, "key")
            keys.setdefault(key, []).append(cls)
        dup = {k: v for k, v in keys.items() if len(v) > 1 and k not in (None, "None")}
        print("  duplicate keys: %d" % len(dup))
        for k, v in list(dup.items())[:10]:
            print("    %s x%d %s" % (k, len(v), v[0]))
        for rec in recs:
            cls = field(rec, "class") or ""
            if not any(h in cls for h in HINTS):
                continue
            key = field(rec, "key")
            tr = field(rec, "transform")
            loc = tr[2].get("Translation") if tr else None
            line = "  %s key=%r" % (cls.rsplit("/", 1)[-1], key)
            if "mainPlayer" in cls:
                ad = field(rec, "actorData")
                blob = ad[1] if ad else b""
                rr = R(blob, 0)
                rr.fstr()
                p = read_props(rr, len(blob))
                inv = p.get("save_inventoryData")
                line += " actorData=%dB" % len(blob)
                if inv:
                    line += " inventory=%s" % array_counts(inv[1])
                else:
                    line += " inventory=<none>"
            print(line)


if __name__ == "__main__":
    main(sys.argv[1:])
