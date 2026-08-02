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


class RuntimeSourceContractTest(unittest.TestCase):
    def test_persistent_texture_override_keeps_unity6_abi_and_slot_precedence(self) -> None:
        source = (ROOT / "src" / "runtime" / "ModRuntime.cpp").read_text(encoding="utf-8")

        self.assertIn(
            "using RendererSetPropertyBlockFn = void (*)(void*, void*, void*);",
            source,
        )
        self.assertIn(
            "using RendererSetPropertyBlockMaterialIndexFn = void (*)(void*, void*, int, void*);",
            source,
        )
        self.assertIn("GetRendererPropertyBlock(renderer, block, slotOverrides.materialIndex);", source)
        self.assertIn("copyRendererBlock = IsMaterialPropertyBlockEmpty(block);", source)
        self.assertIn("GetRendererPropertyBlock(renderer, block);", source)
        self.assertIn("SetMaterialPropertyBlockTextures(block, slotOverrides.textures);", source)
        self.assertIn("Material_SetTexture_Hook", source)

        replacement = source[source.index("bool ApplySkinnedMeshReplacement") :]
        self.assertLess(
            replacement.index("EnsureRendererPrivateMaterials"),
            replacement.index("ApplyMaterialTextureReplacements"),
        )
        self.assertIn(
            "ApplyPersistentTextureOverrides(pair.originalRenderer);",
            replacement,
        )
        self.assertLess(
            replacement.index("ApplyMaterialTextureReplacements"),
            replacement.index("ApplyPersistentTextureOverrides(pair.originalRenderer);"),
        )

    def test_no_object_clone_or_instantiate_hooks(self) -> None:
        # Unity's Instantiate copies components but keeps Mesh/Material/Texture
        # references shared. Hot restoration therefore scans the shared patched
        # resources explicitly; it does not need global Instantiate hooks on the
        # hot path. See AB_DARK_RENDERING_INVESTIGATION.md.
        source = (ROOT / "src" / "runtime" / "ModRuntime.cpp").read_text(encoding="utf-8")
        for symbol in (
            "Object_InternalCloneSingle_Hook",
            "Object_InternalCloneSingleManaged_Hook",
            "Object_InternalCloneSingleWithParentManaged_Hook",
            "Object_InternalInstantiateSingleManaged_Hook",
            "Object_InternalInstantiateSingleWithParentManaged_Hook",
            "InheritPersistentTextureOverridesForClone",
            "RestorePersistentTextureOverridesOnRenderer",
            "RestorePersistentTextureOverridesOnGameObject",
            "RegisterRendererTextureOverridesForMesh",
            "g_meshTextureOverrides",
        ):
            self.assertNotIn(symbol, source, f"{symbol} was reverted; do not reintroduce")

    def test_session_toggle_has_reversible_live_renderer_path(self) -> None:
        source = (ROOT / "src" / "runtime" / "ModRuntime.cpp").read_text(encoding="utf-8")

        for symbol in (
            "struct ReversibleRendererPatch",
            "RegisterReversibleRendererPatch",
            "RestoreLiveModInstances",
            "CollectLiveReapplyTargets",
            "ReapplyLiveModInstances",
            "SourceRendererIdentity",
            "RefreshAnimationRigsAfterHotReapply",
            "RefreshSkinnedMeshRendererState",
            "ReactivateGameObject",
            "RendererPropertyBlockSnapshot",
            "CaptureRendererPropertyBlockSnapshot",
            "RestoreRendererPropertyBlockSnapshots",
            "GetComponentDepthFromRoot",
            "RememberLoadedSourceGameObject(sourceName, originalResult);",
        ):
            self.assertIn(symbol, source)

        toggle = source[source.index("GmrResult SetSessionModEnabled") : source.index("bool Initialize()")]
        self.assertIn("stateChanged && requestedEnabled", toggle)
        self.assertIn("ReapplyLiveModInstances(*replacement)", toggle)
        self.assertIn("RestoreLiveModInstances(modIdUtf8)", toggle)
        self.assertNotIn("existing instances refresh after asset reload", toggle)

        collect = source[
            source.index("std::vector<void*> CollectLiveReapplyTargets") :
            source.index("size_t ReapplyLiveModInstances")
        ]
        self.assertIn(
            "GetSourceRootGameObject(renderer, identity->depthFromSourceRoot)",
            collect,
        )
        self.assertNotIn(
            "AddUniqueLiveObject(targets, GetHierarchyRootGameObject(renderer))",
            collect,
        )

        reapply = source[
            source.index("size_t RefreshAnimationRigsAfterHotReapply") :
            source.index("std::vector<void*> CollectLiveReapplyTargets")
        ]
        self.assertIn("ReactivateGameObject(target)", reapply)
        self.assertIn("Hot-refreshed active character target", reapply)

        clear_blocks = source[
            source.index("void ClearRuntimePropertyBlocks") :
            source.index("size_t RestoreLiveModInstances")
        ]
        self.assertIn(
            "RestoreRendererPropertyBlockSnapshots(renderer, materialCount);",
            clear_blocks,
        )
        self.assertNotIn("Renderer_SetPropertyBlockMaterialIndex_Orig(", clear_blocks)


if __name__ == "__main__":
    unittest.main()
