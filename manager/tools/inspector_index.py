r"""Full method signatures from an Il2CppInspector address-map export.

metadata_index.py reads global-metadata.dat and can only give names: parameter
and return *types* live in the binary, not the metadata file.  That gap is what
made the menu work proceed one wrong guess per game launch (`_button` turned out
to be CampusButton, not UnityEngine.UI.Button).

Il2CppInspector's JSON has the resolved C and .NET signatures for every method,
so this answers "what does this actually take and return" offline.

    python inspector_index.py CampusButtonBase          # methods of a class
    python inspector_index.py -c ErrorSheet             # match on class name only
    python inspector_index.py --raw MenuButtonViewBase  # also print the C signature

Source is an Il2CppInspector export of an iOS build; it is a different build
from the DMM PC client, so treat *addresses* as meaningless here and signatures
as strongly indicative but still worth confirming against metadata_index.py,
which reads the PC metadata.

Point GKMS_INSPECTOR at that export's il2cpp.json, e.g.

    $env:GKMS_INSPECTOR = "<export>\il2cpp.json"

The matching dump.cs sits beside it and is read by hand, not by this tool: it
carries inheritance chains, SerializeField markers and enum values that neither
index exposes.
"""
import json
import os
import re
import sys

ENV = "GKMS_INSPECTOR"
PATH = os.environ.get(ENV)


def resolve_input(path=None):
    """The input lives outside the repo, so it is named by GKMS_INSPECTOR, never hardcoded."""
    target = path or PATH
    if not target:
        raise SystemExit(
            f"{ENV} is not set. Point it at an Il2CppInspector export's "
            f"il2cpp.json, for example:\n"
            f'    $env:{ENV} = "<export-dir>\\il2cpp.json"')
    if not os.path.exists(target):
        raise SystemExit(f"{ENV} points at a missing file: {target}")
    return target


def load(path=None):
    with open(resolve_input(path), "rb") as handle:
        return json.load(handle)["addressMap"]["methodDefinitions"]


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("-")]
    flags = {a for a in sys.argv[1:] if a.startswith("-")}
    if not args:
        print(__doc__)
        return
    needle = args[0].lower()
    class_only = "-c" in flags

    methods = load()
    by_group = {}
    for method in methods:
        group = method.get("group", "")
        # group is Assembly/Namespace/Path/Class -- the class is the last segment
        klass = group.rsplit("/", 1)[-1]
        haystack = klass if class_only else group
        if needle not in haystack.lower():
            continue
        by_group.setdefault(group, []).append(method)

    if not by_group:
        print(f"no class matching {args[0]!r}")
        return
    for group, entries in sorted(by_group.items()):
        print(f"\n== {group.replace('/', '.')}")
        for method in entries:
            print(f"   {method.get('dotNetSignature', '?')}")
            if "--raw" in flags:
                print(f"      {method.get('signature', '?')}")


if __name__ == "__main__":
    main()
