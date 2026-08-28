# Gakumas Mod Runtime

面向《学园偶像大师》DMM Windows 版的本地 AssetBundle Mod 插件。

插件通过 `xinput1_3.dll` 随游戏加载，负责读取本地 Mod，并替换游戏中的服装、发型、模型和贴图等资源。插件内置游戏内 Mod 管理界面，不依赖汉化插件。

## 功能

- 自动扫描并加载本地 AssetBundle Mod
- 支持服装、发型、Mesh、贴图和骨骼映射
- 在游戏内查看、启用和停用 Mod
- 自动保存 Mod 的启用状态
- 提供运行日志，方便排查安装或 Mod 配置问题

## 安装

1. 从 [GitHub Releases](https://github.com/chihya72/gakumas-mod-runtime/releases) 下载最新版本。
2. 关闭游戏，将压缩包内容解压到 `gakumas.exe` 所在目录。
3. 启动游戏。

安装后的主要文件如下：

```text
<游戏目录>\
├─ gakumas.exe
├─ xinput1_3.dll
└─ gakumas-mod\
   ├─ config.json
   ├─ gmi_shaders.bundle
   └─ mods\
```

更新插件时覆盖 `xinput1_3.dll` 和压缩包内的运行资源即可。如果已经修改过 `gakumas-mod\config.json`，请保留自己的配置文件。

## 安装 Mod

将完整的 Mod 文件夹放入：

```text
<游戏目录>\gakumas-mod\mods\<mod-id>\
├─ mod.json
└─ your-mod.bundle
```

启动游戏后，从主页菜单进入「MOD 管理」，即可启用或停用已经识别的 Mod。

## 配置

配置文件位于 `gakumas-mod\config.json`：

```json
{
  "modManagerUi": true,
  "logLevel": "error"
}
```

- `modManagerUi`：是否显示游戏内 Mod 管理界面，默认为 `true`。关闭界面不会停止 Mod 加载。
- `logLevel`：日志等级，可选 `info`、`warn` 或 `error`，默认为 `error`。

## 日志与排查

日志位于：

```text
<游戏目录>\gakumas-mod\mod-plugin.log
<游戏目录>\gakumas-mod\mod-manager.log
```

遇到问题时，可将 `logLevel` 改为 `info`，重新启动游戏并复现问题。正常加载时，日志中会出现带版本信息的 `[BOOT]` 记录；如果没有这条记录，通常表示插件没有被游戏加载。

## 卸载

关闭游戏并删除游戏目录下的 `xinput1_3.dll`。`gakumas-mod` 目录中保存的是 Mod 和个人配置，可以按需保留或删除。

## 从源码构建

需要 Visual Studio 2022，并安装“使用 C++ 的桌面开发”组件。在仓库根目录运行：

```powershell
.\tools\package.ps1 -Version dev
```

生成的安装包位于 `dist`，DLL 位于 `build\bin\x64\Release\xinput1_3.dll`。

## 许可证

项目许可证见 [LICENSE](LICENSE)，第三方组件及其许可证见 [third-party-notices.md](third-party-notices.md)。
