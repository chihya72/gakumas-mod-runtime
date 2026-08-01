# 调查数据源与证据等级

> 更新：2026-08-01

接入任何游戏方法前，先离线查完整类和签名，再用一次受控实机验证。以下路径中的
外部文件属于本机调查输入，不随仓库发布。

## 1. 证据等级

| 等级 | 含义 |
|---|---|
| A | 当前 DMM PC 版本日志或截图直接证明调用结果 |
| B | 当前 PC metadata 存在，且签名/字段已由工具解析；尚未验证调用 |
| C | iOS 导出、第三方截图或 Campus 源码的跨版本推断 |
| D | 仅名称、截图外观或经验推测 |

文档和代码必须标出等级。C/D 级内容不能直接作为 Release 调用依据。

## 2. PC `global-metadata.dat`

默认输入：

```text
D:/Games/gakumas/gakumas_Data/il2cpp_data/Metadata/global-metadata.dat
```

工具：

```text
python tools/metadata_index.py MenuView
python tools/metadata_index.py -m SetCustomText
python tools/metadata_index.py --selfcheck
```

它可以确认当前 PC 版本的类名、命名空间、字段名、方法名、参数个数、参数名和 static
属性，但不能独立提供完整参数类型或函数地址。游戏更新后必须重新运行 `--selfcheck`。

## 3. Il2CppInspector iOS 导出

默认输入由 `GKMS_INSPECTOR` 指定，当前调查机路径为：

```text
D:/GIT/gkms-localify-ios/workspace/3.2.0/inspector/il2cpp.json
```

工具：

```text
python tools/inspector_index.py -c CampusButtonBase --raw
```

它提供返回类型、参数类型、C 层签名和程序集信息，但 iOS 3.2.0 与 DMM PC 不是同一
二进制：地址、偏移和运行时布局不能搬到 PC。它只能作为 C 级签名的 C 级参考（等级 C），
仍需 PC metadata 和实机验证。

`dump.cs`、`il2cpp.json` 和 IDA 数据库是外部调查产物，不在本仓库内；文档不得暗示
其他机器可以直接复现这些绝对路径。

## 4. Campus 仓库与借卡 API

参考仓库：<https://github.com/vertesan/campus>

相关内容：

- `proto/`：protobuf 消息和字段定义；
- `network/rpc/campusClient.go`：游戏网络客户端封装参考；
- `master/`、`octo/`：Master 和资源索引参考；
- `analyser/`：从 dump 生成协议分析的工具。

对 Mod 管理器本身，Campus 协议不是 Mod JSON API 的来源；对借卡库，它是网络和消息
语义的重要参考。两条线必须分开记录。

借卡研究中的 `publicUserId`、好友列表、资料页跳转和支援卡字段目前是候选接口，除非
有当前版本的 PC 实机证据，否则不能标为已完成。账号 token 的导出或重放不属于本项目。

## 5. 方法体和运行时调查

方法内部行为需要使用对应版本的反编译/IDA 数据和日志确认。iOS 方法体只能提供控制
流程参考，地址和偏移不可用于 DMM PC。

运行时日志用于确认：

- 对象是否为空；
- 字段偏移是否正确；
- Hook 是否进入；
- 调用顺序；
- 调用后 UI 是否真的可见、可关闭。

“某函数返回”不等于“页面显示成功”；必须有可见结果或下一步生命周期日志。

## 6. 当前管理器 UI 证据

当前证据和时间线见 `UI_FLOW.md`：

- 11:23 旧构建曾记录菜单入口克隆和 `OpenAsync returned`；
- 11:37 当前构建记录字段解析为 0，未再次注入入口；
- 因此入口和页面均不能标为稳定完成。

## 7. 调查原则

- 能离线确认的签名不靠多次实机试错；
- 一次实机验证一组明确假设，并记录版本、时间和结果；
- 不把 iOS 地址、截图推断或名称猜测写成 PC 已验证事实；
- UI 失败只关闭管理器功能，不影响独立 Runtime 的资源替换；
- 不提交游戏二进制、提取资产、账号 token 或绝对路径依赖。
