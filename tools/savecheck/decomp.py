"""Inflate a VotV 0.10.0 compressed save (CompressedSaveLoadLib) to raw bytes. Read-only."""
import struct, sys, zlib

TAG = 0x9E2A83C1


def decompress(data: bytes) -> bytes:
    out = bytearray()
    at = 0
    while at < len(data):
        tag, _pad, chunk = struct.unpack_from("<IIq", data, at)
        if tag != TAG:
            raise ValueError("bad chunk tag at %d" % at)
        _csum, total = struct.unpack_from("<qq", data, at + 16)
        at += 32
        n = (total + chunk - 1) // chunk
        sizes = []
        for _ in range(n):
            sizes.append(struct.unpack_from("<qq", data, at))
            at += 16
        for comp, raw in sizes:
            part = zlib.decompress(data[at:at + comp])
            if len(part) != raw:
                raise ValueError("chunk size mismatch")
            out += part
            at += comp
    return bytes(out)


if __name__ == "__main__":
    raw = decompress(open(sys.argv[1], "rb").read())
    open(sys.argv[2], "wb").write(raw)
    print(len(raw))
