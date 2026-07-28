# Gakumas Mod Runtime

这是学园偶像大师本地 AssetBundle Mod 的独立游戏运行时。它通过 `xinput1_3.dll` 代理入口加载，
只负责扫描本地 Mod、拦截游戏资源加载并替换 Mesh、骨架映射和贴图。

本仓库不保存游戏提取资产、成品 Mod、旧测试 AB 包或生成报告。

## 当前能力

- 扫描 `gakumas-local/local-files/mods/<mod-id>/mod.json`；
- 支持 manifest v2 的 `priority`、`part`、`renderers` 和贴图规则；
- 支持 `body=Geo_Body`；
- 支持 `hair=Geo_Hair` 或 `Geo_Hair+Geo_HairProp`；
- 从本地 AssetBundle 懒加载替换资源；
- 在原始 `SkinnedMeshRenderer` 上替换克隆后的 Mesh；
- 按骨骼名重排 skinning 数据，失败时保留原始 Mesh；
- 按 renderer、材质槽和 shader property 替换贴图；
- 写入 `gakumas-local/mod-plugin.log`；
- 输出 source profile，并提供离线 Validator 与 Author Doctor。

manifest 格式见 [docs/manifest-v2.md](docs/manifest-v2.md)，当前限制和后续任务见
[docs/roadmap.md](docs/roadmap.md)。

## 构建

要求 Visual Studio 2022 与 C++ 桌面开发组件。

```bat
generate.bat
msbuild build\gakumas_mod_runtime.sln /p:Configuration=Release /p:Platform=x64
```

产物：

```text
build/bin/x64/Release/xinput1_3.dll
```

`generate.bat` 使用仓库内固定的 Premake 5.0.0-beta1。第三方组件和许可证见
[third-party-notices.md](third-party-notices.md)。

## 安装

把 `xinput1_3.dll` 放入游戏目录。每个 Mod 放在：

```text
gakumas-local/local-files/mods/<mod-id>/
  mod.json
  your-mod.bundle
```

## 作者诊断

```bat
python tools\gakumas_mod_validator.py <mod-dir> --profile <source-profile.json>
python tools\gakumas_mod_doctor.py <mod-dir> --profile <source-profile.json>
```

默认报告会写入 `<mod-dir>\reports`。工具只检查 manifest、文件、renderer、材质槽和贴图声明，
目前不离线解析 AssetBundle 内部对象。

## 验证

```bat
python -m unittest discover -s tests -v
```

完整验收还应重新生成 Release DLL，并核对 `xinput1_3.def` 的 8 个导出。
