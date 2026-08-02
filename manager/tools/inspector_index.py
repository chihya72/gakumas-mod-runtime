"""Full method signatures from an Il2CppInspector address-map export.

metadata_index.py reads global-metadata.dat and can only give names: parameter
and return *types* live in the binary, not the metadata file.  That gap is what
made the menu work proceed one wrong guess per game launch (`_button` turned out
to be CampusButton, not UnityEngine.UI.Button).

Il2CppInspector's JSON has the resolved C and .NET signatures for every method,
so this answers "what does this actually take and return" offline.

    python inspector_index.py CampusButtonBase          # methods of a class
    python inspector_index.py -c ErrorSheet             # match on class name only
    python inspector_index.py --raw MenuButtonViewBase  # also print the C signature

Source is the iOS export; it is a different build from the DMM PC client, so
treat *addresses* as meaningless here and signatures as strongly indicative but
still worth confirming against metadata_index.py, which reads the PC metadata.
Override the export path with GKMS_INSPECTOR.
"""
import json
import os
import re
import sys

PATH = os.environ.get(
    "GKMS_INSPECTOR",
    r"D:/GIT/gkms-localify-ios/workspace/3.2.0/inspector/il2cpp.json")


def load(path=None):
    with open(path or PATH, "rb") as handle:
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
