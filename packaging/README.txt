gakumas-mod-runtime
===================

学园偶像大师 DMM Windows 版的 AssetBundle Mod 运行库，内含游戏内 Mod 管理界面。

安装
----

把本压缩包的内容解压到**游戏根目录**（`gakumas.exe` 所在的目录），解压后应该是：

    <游戏目录>\xinput1_3.dll
    <游戏目录>\gakumas-mod\config.json

项目与第三方许可说明位于压缩包根目录的 `LICENSE`、`third-party-notices.md` 和
`licenses\`。这些文件不影响运行，重新分发本包时必须一并保留。

已经装过的话直接覆盖 `xinput1_3.dll` 即可；`gakumas-mod\config.json` 若已存在
请勿覆盖，那是你自己的设置。

Mod 放在：

    <游戏目录>\gakumas-mod\mods\<mod-id>\
        mod.json
        your-mod.bundle

启动游戏后从主页菜单进入「MOD 管理」开关各个 Mod。

config.json
-----------

两个键，都可以省略：

    modManagerUi  游戏内管理界面开关，默认 true。写 false 只关界面，
                  Mod 替换照常工作。
    logLevel      "info" / "warn" / "error"，默认 "error"。
                  出问题时改成 "info" 再复现一次，然后看日志。

日志在 `<游戏目录>\gakumas-mod\` 下的 `mod-plugin.log` 和 `mod-manager.log`。
无论等级如何，每次启动都会写一行 `[BOOT]`，里面有版本号——报问题时请附上它。
如果连这一行都没有，说明 DLL 根本没被加载。

卸载
----

删掉 `xinput1_3.dll` 即可。`gakumas-mod\` 目录可以保留，里面是你的 Mod 和设置。
