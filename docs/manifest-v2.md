# Manifest v2

runtime 扫描：

```text
gakumas-local/local-files/mods/<mod-id>/mod.json
```

最小示例：

```json
{
  "schemaVersion": 2,
  "id": "example-body-mod",
  "name": "Example Body Mod",
  "version": "0.1.0",
  "author": "author",
  "priority": 0,
  "enabled": true,
  "replacements": [
    {
      "source": "mdl_chr_example-cstm-0000_body",
      "part": "body",
      "priority": 0,
      "bundle": "example-body.bundle",
      "asset": "Assets/Mods/example/mdl_chr_example-cstm-0000_body.prefab",
      "type": "GameObject",
      "renderers": [
        {
          "rendererId": "body",
          "targetRenderer": "Geo_Body",
          "modRenderer": "Geo_Body"
        }
      ],
      "replaceMaterials": false,
      "textures": [
        {
          "rendererName": "Geo_Body",
          "materialSlot": 0,
          "property": "_BaseMap",
          "asset": "Assets/Mods/example/body_t0.png",
          "type": "Texture2D"
        }
      ]
    }
  ]
}
```

## 字段

| 字段 | 说明 |
|---|---|
| `enabled` | 是否注册该 Mod |
| `priority` | 冲突优先级；高值生效，同值时后扫描规则覆盖并记录日志 |
| `source` | 游戏原始资源名；兼容旧别名 `from`、`target` |
| `part` | `face`、`hair` 或 `body` |
| `bundle` | 相对当前 Mod 目录的 AssetBundle 路径 |
| `asset` | bundle 内资源路径 |
| `type` | 当前主路径为 `GameObject` |
| `renderers` | 原 renderer 与 Mod renderer 的明确配对 |
| `replaceMaterials` | 是否直接替换材质；通常保持 `false` 并使用贴图规则 |
| `textures` | renderer、材质槽、shader property 与 Texture2D 资源的映射 |

## 部位约定

| part | renderer |
|---|---|
| `body` | 只允许单个 `Geo_Body` |
| `hair` | 单个 `Geo_Hair`，或 `Geo_Hair` + `Geo_HairProp` |
| `face` | 尚未固化公开 renderer 约定，必须按 source profile 单独验证 |

一个 bundle 可以包含多个部位，但每个 prefab/FBX 只对应一个部位；各部位使用独立 replacement。
runtime 可以重排已存在骨骼的 skinning 数据，但不会自动修权重、创建 Animator 驱动或转换材质。

> 管理器第一版的产品约束更窄：一个 Mod Manifest 只显示一个逻辑目标（一个 `body` 服装
> 或一个 `hair` 发型）。Runtime 仍按兼容性保留 `replacements[]` 数组并逐项解析；如果
> 管理器发现多条 replacement，必须将该 Mod 标记为配置异常，不在玩家界面展开成多件服装。
