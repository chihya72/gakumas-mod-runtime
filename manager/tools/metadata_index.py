r"""Type/method/field index straight out of il2cpp global-metadata.dat.

Il2CppDumper needs GameAssembly.dll only to resolve addresses; every name and
every class->member relation already lives in the metadata file.  On this game
the dumper's binary search fails and its PE fallback cannot LoadLibrary
GameAssembly.dll, so this reads the metadata directly instead.

Section layout is probed rather than hardcoded, so a metadata version bump does
not silently produce garbage -- run --selfcheck after a game update.

    python metadata_index.py MenuView                # members of matching types
    python metadata_index.py -m SetCustomText        # which type owns a member
    python metadata_index.py --selfcheck             # verify the probe still works

Point GKMS_METADATA at the game's global-metadata.dat, e.g.

    $env:GKMS_METADATA = "<game>\gakumas_Data\il2cpp_data\Metadata\global-metadata.dat"
"""
import os
import string
import struct
import sys

ENV = "GKMS_METADATA"
PATH = os.environ.get(ENV)

TYPE_STRIDE = 88  # Il2CppTypeDefinition, metadata v29..v31
FIELD_STRIDE = 12  # Il2CppFieldDefinition
METHOD_STRIDES = (0x20, 0x24, 0x28, 0x2C)  # varies with metadata version
PRINTABLE = set((string.ascii_letters + string.digits
                 + "_.<>`|=+-[],() /:$@!?&*%;'\"#\\~^{}").encode())


def probe(data):
    """Locate the string / type / method / field sections by shape and content."""
    sanity, _version = struct.unpack_from("<II", data, 0)
    assert sanity == 0xFAB11BAF, f"not global-metadata.dat (magic {sanity:#x})"

    raw = struct.unpack_from("<240i", data, 8)
    pairs = [(raw[i], raw[i + 1]) for i in range(0, 240, 2)]
    pairs = [(o, s) for o, s in pairs if 0 < o < len(data) and 0 < s and o + s <= len(data)]

    def printable_ratio(o, s):
        blob = data[o:o + min(s, 4096)]
        return sum(1 for c in blob if c in PRINTABLE or c == 0) / len(blob) if blob else 0

    strings = max((p for p in pairs if printable_ratio(*p) > 0.99), key=lambda p: p[1])

    def name_at(index):
        if not 0 <= index < strings[1]:
            return None
        end = data.index(b"\0", strings[0] + index)
        return data[strings[0] + index:end].decode("utf-8", "replace")

    def looks_like_names(o, s, stride, count=32):
        if s % stride or s // stride < count:
            return False
        for i in range(count):
            name = name_at(struct.unpack_from("<I", data, o + i * stride)[0])
            if not name or not (name[0].isalpha() or name[0] in "_<"):
                return False
        return True

    types = next(p for p in pairs if looks_like_names(*p, TYPE_STRIDE))
    type_count = min(types[1] // TYPE_STRIDE, 400)

    def resolve(off_field, count_field, stride, matches):
        """Pick the section whose members, indexed via the type table, look right.

        Methods, fields, parameters and properties all begin with a name index,
        so shape alone is ambiguous -- only cross-referencing from the type table
        tells them apart.
        """
        def score(o, s):
            hits = 0
            for i in range(type_count):
                base = types[0] + i * TYPE_STRIDE
                start = struct.unpack_from("<i", data, base + off_field)[0]
                count = struct.unpack_from("<H", data, base + count_field)[0]
                if start < 0 or (start + count) * stride > s:
                    continue
                for j in range(count):
                    if matches(name_at(struct.unpack_from("<I", data, o + (start + j) * stride)[0])):
                        hits += 1
            return hits

        best = max((p for p in pairs if p != types and p[1] % stride == 0), key=lambda p: score(*p))
        return best, score(*best)

    # ".ctor" is near-universal; backing fields exist only in the field table.
    # Both anchors must match from the *start* of the string: a stray index that
    # lands one byte into "<X>k__BackingField" still ends with the suffix.
    methods, mstride = max(
        ((resolve(36, 64, st, lambda n: n == ".ctor"), st) for st in METHOD_STRIDES),
        key=lambda candidate: candidate[0][1])
    fields, _ = resolve(32, 68, FIELD_STRIDE,
                        lambda n: bool(n) and n.startswith("<") and n.endswith(">k__BackingField"))

    # Parameters share the 12-byte shape with fields, so pick the best remaining
    # candidate by cross-referencing from the method table instead.
    def param_score(o, s):
        hits = 0
        for i in range(min(methods[0][1] // mstride, 4000)):
            base = methods[0][0] + i * mstride
            start = struct.unpack_from("<i", data, base + 16)[0]
            count = struct.unpack_from("<H", data, base + 34)[0]
            if start < 0 or not count or (start + count) * 12 > s:
                continue
            for j in range(count):
                name = name_at(struct.unpack_from("<I", data, o + (start + j) * 12)[0])
                if name and (name[0].islower() or name[0] == "_"):
                    hits += 1
        return hits

    params = max((p for p in pairs if p[1] % 12 == 0 and p not in (types, fields)),
                 key=lambda p: param_score(*p))
    return name_at, types, (methods[0], mstride), fields, params


def resolve_input(path=None):
    """The input lives outside the repo, so it is named by GKMS_METADATA, never hardcoded."""
    target = path or PATH
    if not target:
        raise SystemExit(
            f"{ENV} is not set. Point it at the game's global-metadata.dat, "
            f"for example:\n"
            f'    $env:{ENV} = "<game-dir>\\gakumas_Data\\il2cpp_data'
            f'\\Metadata\\global-metadata.dat"')
    if not os.path.exists(target):
        raise SystemExit(f"{ENV} points at a missing file: {target}")
    return target


def load(path=None):
    data = open(resolve_input(path), "rb").read()
    name_at, (toff, tsize), ((moff, msize), mstride), (foff, fsize), (poff, psize) = probe(data)

    def members(start, count, base, size, stride):
        if start < 0 or (start + count) * stride > size:
            return []
        return [name_at(struct.unpack_from("<I", data, base + (start + i) * stride)[0])
                for i in range(count)]

    def signature(index):
        """Name, parameter names and static-ness -- the three things a wrong
        RuntimeInvoke gets wrong silently."""
        base = moff + index * mstride
        args = members(struct.unpack_from("<i", data, base + 16)[0],
                       struct.unpack_from("<H", data, base + 34)[0], poff, psize, 12)
        static = struct.unpack_from("<H", data, base + 28)[0] & 0x0010  # METHOD_ATTRIBUTE_STATIC
        return (f"{'static ' if static else ''}"
                f"{name_at(struct.unpack_from('<I', data, base)[0])}"
                f"({', '.join(a or '?' for a in args)})")

    out = []
    for i in range(tsize // TYPE_STRIDE):
        b = toff + i * TYPE_STRIDE
        name, namespace = (name_at(x) for x in struct.unpack_from("<II", data, b))
        mstart = struct.unpack_from("<i", data, b + 36)[0]
        mcount = struct.unpack_from("<H", data, b + 64)[0]
        methods = ([signature(mstart + j) for j in range(mcount)]
                   if 0 <= mstart and (mstart + mcount) * mstride <= msize else [])
        fields = members(struct.unpack_from("<i", data, b + 32)[0],
                         struct.unpack_from("<H", data, b + 68)[0], foff, fsize, FIELD_STRIDE)
        out.append((f"{namespace}.{name}" if namespace else name, methods, fields))
    return out


def selfcheck(types):
    """Fails loudly if the section probe drifted -- run this after a game update."""
    index = {full: (ms, fs) for full, ms, fs in types}
    assert len(types) > 10000, f"only {len(types)} types decoded"
    for full, method, field in (
            ("Campus.Common.MenuView", "GetSubButton", "_subButtons"),
            ("Campus.Common.MenuPresenter", "OnAfterInitialize", "_commonView"),
            ("Campus.Common.MenuButtonViewBase", "SetCustomText", "_buttonType")):
        methods, fields = index[full]
        assert any(m.split()[-1].startswith(method + "(") for m in methods), f"{full}.{method} missing"
        assert field in fields, f"{full}.{field} missing"
    print(f"ok: {len(types)} types, anchors resolved")


if __name__ == "__main__":
    parsed = load()
    args = sys.argv[1:]
    if args and args[0] == "--selfcheck":
        selfcheck(parsed)
    elif args and args[0] == "-m":  # reverse lookup: which type owns this member
        needles = [a.lower() for a in args[1:]]
        for full, methods, fields in parsed:
            for member in methods + fields:
                if member and all(n in member.lower() for n in needles):
                    print(f"{full} :: {member}")
    else:
        needles = [a.lower() for a in args]
        for full, methods, fields in parsed:
            if all(n in full.lower() for n in needles):
                print(f"\n== {full}")
                for f in fields:
                    print(f"   field {f}")
                for m in methods:
                    print(f"   {m}")
