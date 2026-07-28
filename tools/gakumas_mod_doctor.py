#!/usr/bin/env python3
"""
Author-facing diagnostics for Gakumas mod packages.

This tool wraps the deterministic validator and adds an author-oriented report:
which part a replacement targets, which renderers/materials are involved, what
the source profile says, and what the author should fix next.
"""

from __future__ import annotations

import argparse
import html
import json
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any

from gakumas_mod_validator import (
    BODY_RENDERERS,
    HAIR_RENDERERS_SINGLE,
    HAIR_RENDERERS_WITH_PROP,
    Finding,
    Validator,
    find_profile_renderer,
    first_string,
    infer_part_from_asset_name,
    normalize_asset_name,
    string_value,
)


EXPECTED_TEXTURE_PROPERTIES = {
    "body": {"_BaseMap", "_DefMap", "_ShadeMap", "_RampAddMap", "_RampMap"},
    "hair": {"_BaseMap", "_DefMap", "_ShadeMap", "_RampAddMap", "_RampMap"},
    "face": {"_BaseMap", "_DefMap", "_ShadeMap", "_RampAddMap", "_RampMap"},
}


@dataclass
class ReplacementSummary:
    index: int
    source: str
    part: str
    priority: int
    bundle: str
    asset: str
    renderers: list[dict[str, str]]
    textureCount: int


class AuthorDoctor:
    def __init__(self, mod_dir: Path, profile_path: Path | None) -> None:
        self.mod_dir = mod_dir
        self.profile_path = profile_path
        self.validator = Validator(mod_dir, profile_path)
        self.findings: list[Finding] = []
        self.replacements: list[ReplacementSummary] = []
        self.profile_summary: dict[str, Any] = {}
        self.next_actions: list[str] = []

    def add(self, level: str, area: str, message: str) -> None:
        self.findings.append(Finding(level, area, message))

    def run(self) -> dict[str, Any]:
        self.validator.validate()
        self.findings.extend(self.validator.findings)

        if self.validator.manifest:
            self.analyze_manifest()
            self.analyze_replacements()
        if self.validator.profile:
            self.analyze_profile()
        else:
            self.add("WARN", "SourceProfile", "No source profile was provided; renderer/material/bone checks are incomplete")

        self.build_next_actions()
        return self.report()

    def analyze_manifest(self) -> None:
        manifest = self.validator.manifest
        schema = manifest.get("schemaVersion")
        if schema == 2:
            self.add("PASS", "AuthorDoctor", "Manifest v2 package")
        else:
            self.add("WARN", "AuthorDoctor", "Manifest v2 is recommended for author packages")

        replacements = manifest.get("replacements")
        if isinstance(replacements, list):
            parts = sorted({
                string_value(r, "part") or infer_part_from_asset_name(first_string(r, "source", "from", "target") or "") or "unknown"
                for r in replacements
                if isinstance(r, dict)
            })
            self.add("INFO", "AuthorDoctor", f"Replacement parts in this package: {', '.join(parts) if parts else 'none'}")

    def analyze_replacements(self) -> None:
        manifest = self.validator.manifest
        replacements = manifest.get("replacements")
        if not isinstance(replacements, list):
            return

        manifest_priority = manifest.get("priority") if isinstance(manifest.get("priority"), int) else 0
        source_to_area: dict[str, str] = {}
        for index, replacement in enumerate(replacements):
            if not isinstance(replacement, dict):
                continue

            area = f"Doctor.Replacement[{index}]"
            source = first_string(replacement, "source", "from", "target") or ""
            part = string_value(replacement, "part") or infer_part_from_asset_name(source) or "unknown"
            priority = replacement.get("priority") if isinstance(replacement.get("priority"), int) else manifest_priority
            bundle = first_string(replacement, "bundle", "assetBundle", "assetbundle") or ""
            asset = first_string(replacement, "asset", "to", "name") or source
            renderer_rules = self.renderer_rules(replacement)
            textures = replacement.get("textures")
            texture_count = len(textures) if isinstance(textures, list) else 0
            self.replacements.append(ReplacementSummary(index, source, part, priority, bundle, asset, renderer_rules, texture_count))

            source_key = normalize_asset_name(source)
            if source_key:
                if source_key in source_to_area:
                    self.add("WARN", area, f"Duplicate source also appears at {source_to_area[source_key]}; prefer one replacement per source per mod")
                else:
                    source_to_area[source_key] = area

            if part == "unknown":
                self.add("WARN", area, "Unable to infer part; set part to face, hair, or body")
            elif not asset_name_matches_part(asset, part):
                self.add("WARN", area, f"Asset path does not clearly match part={part}: {asset}")

            if not renderer_rules:
                if string_value(replacement, "rendererName"):
                    self.add("WARN", area, "Using legacy rendererName shorthand; manifest v2 renderers[] is recommended")
                else:
                    self.add("WARN", area, "No deterministic renderer mapping; add renderers[].targetRenderer/modRenderer")
            else:
                self.analyze_renderer_rules(area, renderer_rules)
                self.analyze_part_renderer_expectations(area, part, renderer_rules)

            self.analyze_textures(area, part, replacement, renderer_rules)

    def renderer_rules(self, replacement: dict[str, Any]) -> list[dict[str, str]]:
        renderers = replacement.get("renderers")
        rules: list[dict[str, str]] = []
        if isinstance(renderers, list):
            for index, renderer in enumerate(renderers):
                if not isinstance(renderer, dict):
                    continue
                target = string_value(renderer, "targetRenderer")
                mod = string_value(renderer, "modRenderer")
                if not target or not mod:
                    continue
                rules.append({
                    "rendererId": string_value(renderer, "rendererId") or f"renderer-{index}",
                    "targetRenderer": target,
                    "modRenderer": mod,
                })
        elif legacy := string_value(replacement, "rendererName"):
            rules.append({
                "rendererId": "legacy",
                "targetRenderer": legacy,
                "modRenderer": legacy,
            })
        return rules

    def analyze_renderer_rules(self, area: str, renderer_rules: list[dict[str, str]]) -> None:
        seen_ids: set[str] = set()
        seen_targets: set[str] = set()
        for rule in renderer_rules:
            renderer_id = rule["rendererId"]
            target = rule["targetRenderer"]
            if renderer_id in seen_ids:
                self.add("WARN", area, f"Duplicate rendererId: {renderer_id}")
            seen_ids.add(renderer_id)
            if target in seen_targets:
                self.add("WARN", area, f"Duplicate targetRenderer mapping: {target}")
            seen_targets.add(target)

            profile_renderer = find_profile_renderer(self.validator.profile, target) if self.validator.profile else None
            if profile_renderer:
                mesh = profile_renderer.get("mesh") if isinstance(profile_renderer.get("mesh"), dict) else {}
                bones = profile_renderer.get("bones")
                bone_count = len(bones) if isinstance(bones, list) else 0
                self.add("INFO", area, f"Target renderer {target}: vertices={mesh.get('vertexCount', 'unknown')} bones={bone_count}")
            elif self.validator.profile:
                self.add("FAIL", area, f"targetRenderer not found in source profile: {target}")

    def analyze_part_renderer_expectations(self, area: str, part: str, renderer_rules: list[dict[str, str]]) -> None:
        mapped_targets = {rule["targetRenderer"] for rule in renderer_rules}

        if part == "body":
            if mapped_targets == BODY_RENDERERS:
                self.add("PASS", area, "Body convention: target renderer is Geo_Body")
            else:
                self.add("FAIL", area, "Body mods should map exactly one renderer: Geo_Body")
            return

        if part == "face":
            self.add("WARN", area, "Face resources are special and do not have a finalized public renderer convention yet")
            return

        if part != "hair":
            return

        if mapped_targets == HAIR_RENDERERS_SINGLE:
            self.add("PASS", area, "Hair convention: single Geo_Hair renderer")
        elif mapped_targets == HAIR_RENDERERS_WITH_PROP:
            self.add("PASS", area, "Hair convention: Geo_Hair + Geo_HairProp renderers")
        else:
            self.add("FAIL", area, "Hair mods should map Geo_Hair, or Geo_Hair + Geo_HairProp")

        if not self.validator.profile:
            return

        profile_renderer_names = set(profile_renderer_names(self.validator.profile))
        if "Geo_HairProp" in profile_renderer_names and "Geo_HairProp" not in mapped_targets:
            self.add("WARN", area, "Source hair profile has Geo_HairProp but manifest does not map it")
        if "Geo_Hair" in profile_renderer_names and "Geo_Hair" not in mapped_targets:
            self.add("WARN", area, "Source hair profile has Geo_Hair but manifest does not map it")

    def analyze_textures(self, area: str, part: str, replacement: dict[str, Any], renderer_rules: list[dict[str, str]]) -> None:
        textures = replacement.get("textures")
        if not isinstance(textures, list):
            self.add("WARN", area, "No texture rules declared")
            return

        declared_properties = {
            first_string(texture, "property", "shaderProperty", "name")
            for texture in textures
            if isinstance(texture, dict)
        }
        declared_properties.discard(None)
        expected = EXPECTED_TEXTURE_PROPERTIES.get(part, set())
        missing = sorted(expected - declared_properties)
        if missing:
            self.add("WARN", area, f"Common {part} texture properties are not declared: {', '.join(missing)}")

        default_renderer = renderer_rules[0]["targetRenderer"] if len(renderer_rules) == 1 else None
        for texture_index, texture in enumerate(textures):
            if not isinstance(texture, dict):
                continue
            texture_area = f"{area}.textures[{texture_index}]"
            renderer_name = string_value(texture, "rendererName") or default_renderer
            material_slot = texture.get("materialSlot", -1)
            property_name = first_string(texture, "property", "shaderProperty", "name")
            if renderer_name and isinstance(material_slot, int) and material_slot >= 0 and property_name:
                profile_renderer = find_profile_renderer(self.validator.profile, renderer_name) if self.validator.profile else None
                if not profile_renderer:
                    continue
                materials = profile_renderer.get("materials")
                if isinstance(materials, list) and material_slot < len(materials):
                    props = materials[material_slot].get("properties")
                    if isinstance(props, list) and property_name not in props:
                        self.add("WARN", texture_area, f"Property {property_name} is not listed on {renderer_name}[{material_slot}]")

    def analyze_profile(self) -> None:
        profile = self.validator.profile
        renderers = profile.get("renderers")
        renderer_summaries: list[dict[str, Any]] = []
        if isinstance(renderers, list):
            for renderer in renderers:
                if not isinstance(renderer, dict):
                    continue
                materials = renderer.get("materials")
                bones = renderer.get("bones")
                mesh = renderer.get("mesh") if isinstance(renderer.get("mesh"), dict) else {}
                renderer_summaries.append({
                    "name": renderer.get("name"),
                    "type": renderer.get("type"),
                    "rootBone": renderer.get("rootBone"),
                    "vertexCount": mesh.get("vertexCount"),
                    "subMeshCount": mesh.get("subMeshCount"),
                    "boneCount": len(bones) if isinstance(bones, list) else 0,
                    "materialCount": len(materials) if isinstance(materials, list) else 0,
                })
        self.profile_summary = {
            "source": profile.get("source"),
            "part": profile.get("part") or infer_part_from_asset_name(str(profile.get("source", ""))),
            "rendererCount": len(renderer_summaries),
            "renderers": renderer_summaries,
        }
        self.add("INFO", "SourceProfile", f"Profile renderers: {len(renderer_summaries)}")

    def build_next_actions(self) -> None:
        if any(f.level == "FAIL" for f in self.findings):
            self.next_actions.append("Fix all FAIL items before testing in game.")
        if any("Bundle file not found" in f.message for f in self.findings):
            self.next_actions.append("Build the AssetBundle and place it at the bundle path declared in mod.json.")
        if any("targetRenderer not found" in f.message for f in self.findings):
            self.next_actions.append("Open the source profile and copy the exact renderer name into targetRenderer.")
        if any("Common" in f.message and "texture properties" in f.message for f in self.findings):
            self.next_actions.append("Decide whether the missing texture properties are intentionally omitted or should be exported from the authoring model.")
        if not self.next_actions:
            self.next_actions.append("The package has no blocking diagnostic issues. Test it in game and compare mod-plugin.log with this report.")

    def report(self) -> dict[str, Any]:
        return {
            "modDir": str(self.mod_dir),
            "profile": str(self.profile_path) if self.profile_path else None,
            "summary": self.summary(),
            "profileSummary": self.profile_summary,
            "replacements": [asdict(item) for item in self.replacements],
            "findings": [asdict(item) for item in self.findings],
            "nextActions": self.next_actions,
        }

    def summary(self) -> dict[str, int]:
        result = {"PASS": 0, "INFO": 0, "WARN": 0, "FAIL": 0}
        for finding in self.findings:
            result[finding.level] = result.get(finding.level, 0) + 1
        return result


def asset_name_matches_part(asset: str, part: str) -> bool:
    if not asset or not part or part == "unknown":
        return True
    normalized = normalize_asset_name(asset)
    return f"_{part}" in normalized or f"-{part}" in normalized or f"/{part}" in normalized


def profile_renderer_names(profile: dict[str, Any]) -> list[str]:
    renderers = profile.get("renderers")
    if not isinstance(renderers, list):
        return []
    return [
        str(renderer.get("name"))
        for renderer in renderers
        if isinstance(renderer, dict) and isinstance(renderer.get("name"), str)
    ]


def render_html(report: dict[str, Any]) -> str:
    summary = report["summary"]
    replacement_rows = []
    for replacement in report["replacements"]:
        renderers = "<br>".join(
            f"{html.escape(r['rendererId'])}: {html.escape(r['targetRenderer'])} -> {html.escape(r['modRenderer'])}"
            for r in replacement["renderers"]
        ) or "none"
        replacement_rows.append(
            "<tr>"
            f"<td>{replacement['index']}</td>"
            f"<td>{html.escape(replacement['source'])}</td>"
            f"<td>{html.escape(replacement['part'])}</td>"
            f"<td>{replacement['priority']}</td>"
            f"<td>{html.escape(replacement['bundle'])}</td>"
            f"<td>{renderers}</td>"
            f"<td>{replacement['textureCount']}</td>"
            "</tr>"
        )

    finding_rows = []
    for finding in report["findings"]:
        level = html.escape(finding["level"])
        finding_rows.append(
            f"<tr class='{level}'><td>{level}</td><td>{html.escape(finding['area'])}</td><td>{html.escape(finding['message'])}</td></tr>"
        )

    actions = "".join(f"<li>{html.escape(action)}</li>" for action in report["nextActions"])
    return f"""<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <title>Gakumas Mod Author Diagnostics</title>
  <style>
    body {{ font-family: Segoe UI, Microsoft YaHei, sans-serif; margin: 24px; color: #202124; }}
    h1, h2 {{ margin-bottom: 8px; }}
    .summary span {{ display: inline-block; margin-right: 16px; font-weight: 700; }}
    table {{ border-collapse: collapse; width: 100%; margin: 12px 0 24px; }}
    th, td {{ border: 1px solid #ddd; padding: 8px; text-align: left; vertical-align: top; }}
    th {{ background: #f3f3f3; }}
    .PASS td:first-child {{ color: #187a35; font-weight: 700; }}
    .INFO td:first-child {{ color: #2459a6; font-weight: 700; }}
    .WARN td:first-child {{ color: #9a6200; font-weight: 700; }}
    .FAIL td:first-child {{ color: #b00020; font-weight: 700; }}
  </style>
</head>
<body>
  <h1>Gakumas Mod Author Diagnostics</h1>
  <p>Mod: {html.escape(report["modDir"])}</p>
  <p>Profile: {html.escape(str(report["profile"]))}</p>
  <p class="summary">
    <span>PASS: {summary.get("PASS", 0)}</span>
    <span>INFO: {summary.get("INFO", 0)}</span>
    <span>WARN: {summary.get("WARN", 0)}</span>
    <span>FAIL: {summary.get("FAIL", 0)}</span>
  </p>
  <h2>Next Actions</h2>
  <ul>{actions}</ul>
  <h2>Replacements</h2>
  <table>
    <thead><tr><th>#</th><th>Source</th><th>Part</th><th>Priority</th><th>Bundle</th><th>Renderers</th><th>Textures</th></tr></thead>
    <tbody>{''.join(replacement_rows)}</tbody>
  </table>
  <h2>Findings</h2>
  <table>
    <thead><tr><th>Level</th><th>Area</th><th>Message</th></tr></thead>
    <tbody>{''.join(finding_rows)}</tbody>
  </table>
</body>
</html>
"""


def write_report(report: dict[str, Any], output_dir: Path) -> None:
    output_dir.mkdir(parents=True, exist_ok=True)
    (output_dir / "author_diagnostics.json").write_text(
        json.dumps(report, ensure_ascii=False, indent=2),
        encoding="utf-8",
    )
    (output_dir / "author_diagnostics.html").write_text(render_html(report), encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate author diagnostics for a Gakumas mod package.")
    parser.add_argument("mod_dir", type=Path, help="Path to a mod package directory containing mod.json")
    parser.add_argument("--profile", type=Path, help="Source profile JSON dumped by the mod plugin")
    parser.add_argument("--out", type=Path, help="Report output directory")
    args = parser.parse_args()

    doctor = AuthorDoctor(args.mod_dir.resolve(), args.profile.resolve() if args.profile else None)
    report = doctor.run()
    output_dir = args.out.resolve() if args.out else args.mod_dir.resolve() / "reports"
    write_report(report, output_dir)

    for finding in doctor.findings:
        print(f"{finding.level:<4} {finding.area}: {finding.message}")
    print("Next actions:")
    for action in doctor.next_actions:
        print(f"- {action}")

    return 1 if any(f.level == "FAIL" for f in doctor.findings) else 0


if __name__ == "__main__":
    raise SystemExit(main())
