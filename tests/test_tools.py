from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from gakumas_mod_doctor import AuthorDoctor


class ToolSmokeTest(unittest.TestCase):
    def test_valid_body_package_has_no_failures(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            package = root / "mod"
            package.mkdir()
            (package / "example.bundle").write_bytes(b"fixture")
            (package / "README.md").write_text("# Example\n", encoding="utf-8")
            (package / "mod.json").write_text(
                json.dumps(
                    {
                        "schemaVersion": 2,
                        "id": "example",
                        "name": "Example",
                        "version": "0.1.0",
                        "author": "test",
                        "enabled": True,
                        "replacements": [
                            {
                                "source": "mdl_chr_example-cstm-0000_body",
                                "part": "body",
                                "bundle": "example.bundle",
                                "asset": "Assets/Mods/example/example_body.prefab",
                                "renderers": [
                                    {
                                        "rendererId": "body",
                                        "targetRenderer": "Geo_Body",
                                        "modRenderer": "Geo_Body",
                                    }
                                ],
                            }
                        ],
                    }
                ),
                encoding="utf-8",
            )
            profile = root / "profile.json"
            profile.write_text(
                json.dumps(
                    {
                        "source": "mdl_chr_example-cstm-0000_body",
                        "part": "body",
                        "renderers": [
                            {
                                "name": "Geo_Body",
                                "mesh": {"vertexCount": 3, "subMeshCount": 1},
                                "bones": ["Hips"],
                                "materials": [],
                            }
                        ],
                    }
                ),
                encoding="utf-8",
            )

            report = AuthorDoctor(package, profile).run()

            self.assertEqual(report["summary"]["FAIL"], 0)
            self.assertEqual(report["replacements"][0]["part"], "body")


if __name__ == "__main__":
    unittest.main()
