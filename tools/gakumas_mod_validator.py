#!/usr/bin/env python3
"""
Validate a Gakumas model replacement mod package before launching the game.

This first version intentionally checks deterministic package problems only:
manifest shape, bundle file presence, renderer names against a source profile,
material slots, and texture declarations. It does not inspect Unity AssetBundle
contents yet because that requires Unity/AssetBundle parsing support.
"""

from __future__ import annotations

import argparse
import html
import json
from dataclasses import dataclass
from pathlib import Path
from typing import Any

BODY_RENDERERS = {"Geo_Body"}
HAIR_RENDERERS_SINGLE = {"Geo_Hair"}
HAIR_RENDERERS_WITH_PROP = {"Geo_Hair", "Geo_HairProp"}
KNOWN_HAIR_RENDERERS = HAIR_RENDERERS_SINGLE | HAIR_RENDERERS_WITH_PROP


@dataclass
class Finding:
    level: str
    area: str
    message: str


class Validator:
    def __init__(self, mod_dir: Path, profile_path: Path | None) -> None:
        self.mod_dir = mod_dir
        self.profile_path = profile_path
        self.findings: list[Finding] = []
        self.manifest: dict[str, Any] = {}
        self.profile: dict[str, Any] = {}

    def add(self, level: str, area: str, message: str) -> None:
        self.findings.append(Finding(level, area, message))

    def load_json(self, path: Path, area: str) -> dict[str, Any] | None:
        try:
            with path.open("r", encoding="utf-8") as stream:
                data = json.load(stream)
        except FileNotFoundError:
            self.add("FAIL", area, f"Missing file: {path}")
            return None
        except json.JSONDecodeError as exc:
            self.add("FAIL", area, f"Invalid JSON: {path} ({exc})")
            return None
        if not isinstance(data, dict):
            self.add("FAIL", area, f"Root JSON must be an object: {path}")
            return None
        return data

    def validate(self) -> list[Finding]:
        if not self.mod_dir.exists():
            self.add("FAIL", "Package", f"Mod directory does not exist: {self.mod_dir}")
            return self.findings
        if not self.mod_dir.is_dir():
            self.add("FAIL", "Package", f"Mod path is not a directory: {self.mod_dir}")
            return self.findings

        manifest_path = self.mod_dir / "mod.json"
        manifest = self.load_json(manifest_path, "Manifest")
        if manifest is None:
            return self.findings
        self.manifest = manifest
        self.add("PASS", "Manifest", "mod.json loaded")

        if self.profile_path:
            profile = self.load_json(self.profile_path, "SourceProfile")
            if profile is not None:
                self.profile = profile
                self.add("PASS", "SourceProfile", f"Profile loaded: {self.profile_path}")

        self.validate_metadata()
        self.validate_replacements()
        self.validate_optional_files()
        return self.findings

    def validate_metadata(self) -> None:
        for key in ("id", "name", "version", "author"):
            if key in self.manifest:
                if not isinstance(self.manifest[key], str) or not self.manifest[key].strip():
                    self.add("FAIL", "Manifest", f"{key} must be a non-empty string")
            else:
                level = "WARN" if key in ("id", "version", "author") else "FAIL"
                self.add(level, "Manifest", f"Missing metadata field: {key}")

        if "schemaVersion" not in self.manifest:
            self.add("WARN", "Manifest", "schemaVersion is missing; treating package as manifest v1")
        elif not isinstance(self.manifest["schemaVersion"], int):
            self.add("FAIL", "Manifest", "schemaVersion must be an integer")

        if self.manifest.get("enabled") is not None and not isinstance(self.manifest["enabled"], bool):
            self.add("FAIL", "Manifest", "enabled must be a boolean")

        if self.manifest.get("priority") is not None and not isinstance(self.manifest["priority"], int):
            self.add("FAIL", "Manifest", "priority must be an integer")

    def validate_replacements(self) -> None:
        replacements = self.manifest.get("replacements")
        if not isinstance(replacements, list) or not replacements:
            self.add("FAIL", "Manifest", "replacements must be a non-empty array")
            return

        seen_sources: dict[str, str] = {}
        for index, replacement in enumerate(replacements):
            area = f"Replacement[{index}]"
            if not isinstance(replacement, dict):
                self.add("FAIL", area, "replacement must be an object")
                continue

            source = first_string(replacement, "from", "source", "target")
            bundle = first_string(replacement, "bundle", "assetBundle", "assetbundle")
            asset = first_string(replacement, "asset", "to", "name")
            if not source:
                self.add("FAIL", area, "Missing from/source/target")
            else:
                source_key = normalize_asset_name(source)
                if source_key in seen_sources:
                    self.add("WARN", area, f"Duplicate source in this manifest; later rule wins at runtime unless priority differs: {source} (first: {seen_sources[source_key]})")
                else:
                    seen_sources[source_key] = area
            if not bundle:
                self.add("FAIL", area, "Missing bundle/assetBundle")
            else:
                bundle_path = (self.mod_dir / bundle).resolve()
                if bundle_path.exists():
                    self.add("PASS", area, f"Bundle exists: {bundle}")
                else:
                    self.add("FAIL", area, f"Bundle file not found: {bundle}")
            if asset:
                self.add("PASS", area, f"Asset path declared: {asset}")
            else:
                self.add("WARN", area, "Asset path missing; plugin will fallback to source name")

            part = string_value(replacement, "part")
            if part:
                if part in {"face", "hair", "body"}:
                    self.add("PASS", area, f"part declared: {part}")
                    inferred_part = infer_part_from_asset_name(source or "")
                    if inferred_part and inferred_part != part:
                        self.add("WARN", area, f"part does not match source name suffix: declared={part}, inferred={inferred_part}")
                else:
                    self.add("FAIL", area, "part must be one of: face, hair, body")
            else:
                self.add("WARN", area, "part is missing; recommended values are face, hair, body")

            if replacement.get("priority") is not None and not isinstance(replacement["priority"], int):
                self.add("FAIL", area, "priority must be an integer")

            renderer_name = string_value(replacement, "rendererName")
            if renderer_name:
                self.validate_renderer_name(area, renderer_name)

            self.validate_textures(area, replacement, renderer_name)
            self.validate_manifest_v2_renderers(area, replacement)
            if part in {"face", "hair", "body"}:
                self.validate_part_renderer_convention(area, part, replacement)

    def validate_renderer_name(self, area: str, renderer_name: str) -> None:
        if not self.profile:
            self.add("WARN", area, f"rendererName declared but no source profile provided: {renderer_name}")
            return

        renderers = self.profile.get("renderers")
        if not isinstance(renderers, list):
            self.add("WARN", "SourceProfile", "Profile has no renderers array")
            return

        names = {r.get("name") for r in renderers if isinstance(r, dict)}
        if renderer_name in names:
            self.add("PASS", area, f"target renderer exists in profile: {renderer_name}")
        else:
            self.add("FAIL", area, f"target renderer not found in profile: {renderer_name}")

    def validate_textures(self, area: str, replacement: dict[str, Any], renderer_name: str | None) -> None:
        textures = replacement.get("textures")
        if textures is None:
            self.add("WARN", area, "No textures array; mesh-only replacement")
            return
        if not isinstance(textures, list):
            self.add("FAIL", area, "textures must be an array")
            return

        for texture_index, texture in enumerate(textures):
            texture_area = f"{area}.textures[{texture_index}]"
            if not isinstance(texture, dict):
                self.add("FAIL", texture_area, "texture rule must be an object")
                continue
            property_name = first_string(texture, "property", "shaderProperty", "name")
            asset_name = first_string(texture, "asset", "texture", "to")
            material_slot = texture.get("materialSlot", -1)
            texture_renderer = string_value(texture, "rendererName") or renderer_name

            if not property_name:
                self.add("FAIL", texture_area, "Missing property/shaderProperty")
            if not asset_name:
                self.add("FAIL", texture_area, "Missing asset/texture")
            if not isinstance(material_slot, int):
                self.add("FAIL", texture_area, "materialSlot must be an integer")
            elif material_slot < 0:
                self.add("WARN", texture_area, "materialSlot is not set; plugin may skip this texture")
            if asset_name:
                self.add("PASS", texture_area, f"Texture asset declared: {asset_name}")
            if texture_renderer:
                self.validate_material_slot(texture_area, texture_renderer, material_slot, property_name)

    def validate_material_slot(self, area: str, renderer_name: str, material_slot: int, property_name: str | None) -> None:
        if not self.profile or material_slot < 0:
            return
        renderer = find_profile_renderer(self.profile, renderer_name)
        if not renderer:
            return
        materials = renderer.get("materials")
        if not isinstance(materials, list):
            self.add("WARN", area, f"Profile renderer has no materials: {renderer_name}")
            return
        if material_slot >= len(materials):
            self.add("FAIL", area, f"materialSlot out of range for {renderer_name}: {material_slot}")
            return
        self.add("PASS", area, f"materialSlot exists: {renderer_name}[{material_slot}]")
        if property_name:
            props = materials[material_slot].get("properties")
            if isinstance(props, list) and props:
                if property_name in props:
                    self.add("PASS", area, f"shader property exists: {property_name}")
                else:
                    self.add("WARN", area, f"shader property not listed in profile: {property_name}")

    def validate_manifest_v2_renderers(self, area: str, replacement: dict[str, Any]) -> None:
        renderers = replacement.get("renderers")
        if renderers is None:
            return
        if not isinstance(renderers, list):
            self.add("FAIL", area, "renderers must be an array")
            return
        for renderer_index, renderer in enumerate(renderers):
            renderer_area = f"{area}.renderers[{renderer_index}]"
            if not isinstance(renderer, dict):
                self.add("FAIL", renderer_area, "renderer rule must be an object")
                continue
            for key in ("targetRenderer", "modRenderer"):
                value = string_value(renderer, key)
                if value:
                    if key == "targetRenderer":
                        self.validate_renderer_name(renderer_area, value)
                else:
                    self.add("FAIL", renderer_area, f"Missing {key}")
            renderer_id = string_value(renderer, "rendererId")
            if renderer_id:
                self.add("PASS", renderer_area, f"rendererId declared: {renderer_id}")
            else:
                self.add("WARN", renderer_area, "rendererId is recommended for manifest v2")

    def validate_part_renderer_convention(self, area: str, part: str, replacement: dict[str, Any]) -> None:
        target_renderers = collect_target_renderers(replacement)
        if not target_renderers:
            return

        target_set = set(target_renderers)
        if part == "body":
            if target_set != BODY_RENDERERS:
                self.add("FAIL", area, "body replacement must target exactly Geo_Body")
            else:
                self.add("PASS", area, "body renderer convention matched: Geo_Body")
            return

        if part == "hair":
            if target_set in (HAIR_RENDERERS_SINGLE, HAIR_RENDERERS_WITH_PROP):
                self.add("PASS", area, f"hair renderer convention matched: {', '.join(sorted(target_set))}")
            else:
                self.add("FAIL", area, "hair replacement must target Geo_Hair, or Geo_Hair + Geo_HairProp")
            return

        if part == "face":
            self.add("WARN", area, "face renderer convention is not finalized yet; inspect the source profile manually")

    def validate_optional_files(self) -> None:
        for name in ("README.md", "icon.png"):
            if (self.mod_dir / name).exists():
                self.add("PASS", "Package", f"{name} exists")
            else:
                self.add("WARN", "Package", f"{name} is recommended for public packages")

    def write_reports(self, output_dir: Path) -> None:
        output_dir.mkdir(parents=True, exist_ok=True)
        report_json = {
            "modDir": str(self.mod_dir),
            "profile": str(self.profile_path) if self.profile_path else None,
            "summary": self.summary(),
            "findings": [finding.__dict__ for finding in self.findings],
        }
        (output_dir / "report.json").write_text(json.dumps(report_json, ensure_ascii=False, indent=2), encoding="utf-8")
        (output_dir / "report.html").write_text(render_html(report_json), encoding="utf-8")

    def summary(self) -> dict[str, int]:
        result = {"PASS": 0, "WARN": 0, "FAIL": 0}
        for finding in self.findings:
            result[finding.level] = result.get(finding.level, 0) + 1
        return result


def string_value(data: dict[str, Any], key: str) -> str | None:
    value = data.get(key)
    return value if isinstance(value, str) and value.strip() else None


def first_string(data: dict[str, Any], *keys: str) -> str | None:
    for key in keys:
        value = string_value(data, key)
        if value:
            return value
    return None


def normalize_asset_name(value: str) -> str:
    return value.replace("\\", "/").lower()


def infer_part_from_asset_name(value: str) -> str | None:
    normalized = normalize_asset_name(value)
    if "_face" in normalized or normalized.endswith("-face"):
        return "face"
    if "_hair" in normalized or normalized.endswith("-hair"):
        return "hair"
    if "_body" in normalized or normalized.endswith("-body"):
        return "body"
    return None


def find_profile_renderer(profile: dict[str, Any], renderer_name: str) -> dict[str, Any] | None:
    renderers = profile.get("renderers")
    if not isinstance(renderers, list):
        return None
    for renderer in renderers:
        if isinstance(renderer, dict) and renderer.get("name") == renderer_name:
            return renderer
    return None


def collect_target_renderers(replacement: dict[str, Any]) -> list[str]:
    result: list[str] = []
    renderers = replacement.get("renderers")
    if isinstance(renderers, list):
        for renderer in renderers:
            if not isinstance(renderer, dict):
                continue
            target = string_value(renderer, "targetRenderer")
            if target:
                result.append(target)
    legacy = string_value(replacement, "rendererName")
    if legacy and not result:
        result.append(legacy)
    return result


def render_html(report: dict[str, Any]) -> str:
    rows = []
    for finding in report["findings"]:
        level = html.escape(finding["level"])
        area = html.escape(finding["area"])
        message = html.escape(finding["message"])
        rows.append(f"<tr class='{level}'><td>{level}</td><td>{area}</td><td>{message}</td></tr>")
    summary = report["summary"]
    return f"""<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <title>Gakumas Mod Validation Report</title>
  <style>
    body {{ font-family: Segoe UI, Microsoft YaHei, sans-serif; margin: 24px; }}
    table {{ border-collapse: collapse; width: 100%; }}
    th, td {{ border: 1px solid #ddd; padding: 8px; text-align: left; }}
    th {{ background: #f3f3f3; }}
    .PASS td:first-child {{ color: #187a35; font-weight: 700; }}
    .WARN td:first-child {{ color: #9a6200; font-weight: 700; }}
    .FAIL td:first-child {{ color: #b00020; font-weight: 700; }}
  </style>
</head>
<body>
  <h1>Gakumas Mod Validation Report</h1>
  <p>PASS: {summary.get("PASS", 0)} | WARN: {summary.get("WARN", 0)} | FAIL: {summary.get("FAIL", 0)}</p>
  <table>
    <thead><tr><th>Level</th><th>Area</th><th>Message</th></tr></thead>
    <tbody>{''.join(rows)}</tbody>
  </table>
</body>
</html>
"""


def main() -> int:
    parser = argparse.ArgumentParser(description="Validate a Gakumas mod package.")
    parser.add_argument("mod_dir", type=Path, help="Path to a mod package directory containing mod.json")
    parser.add_argument("--profile", type=Path, help="Source profile JSON dumped by the mod plugin")
    parser.add_argument("--out", type=Path, help="Report output directory")
    args = parser.parse_args()

    validator = Validator(args.mod_dir.resolve(), args.profile.resolve() if args.profile else None)
    findings = validator.validate()
    output_dir = args.out.resolve() if args.out else args.mod_dir.resolve() / "reports"
    validator.write_reports(output_dir)

    for finding in findings:
        print(f"{finding.level:<4} {finding.area}: {finding.message}")

    return 1 if any(f.level == "FAIL" for f in findings) else 0


if __name__ == "__main__":
    raise SystemExit(main())
