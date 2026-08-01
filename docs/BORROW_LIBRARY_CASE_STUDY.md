# 借卡库案例与 Campus API 研究

> 更新：2026-08-01
>
> **范围说明：本文是早期背景研究，不是当前游戏内 Mod 管理器的产品需求、里程碑或
> 下一步任务。当前项目不实现借卡、好友、名片或 Campus 网络 API。除非用户明确改变
> 产品范围，否则接手者不得把本文候选接口加入管理器代码。**
>
> 本文把第三方借卡库截图、Campus 仓库资料和本机 metadata 分开记录。截图能证明的
> 才写成观察结果；没有第三方源码或实机日志的内容标为推测。

## 1. 用户实际流程

当前借卡网站展示支援卡和拥有者的公开用户 ID。玩家在游戏内搜索该 ID，打开用户资料，
点击添加好友后即可借到支援卡。

目标流程是：

```text
游戏内借卡入口
  → 支援卡列表
  → 选择支援卡
  → 网站/API 返回拥有者 publicUserId
  → 打开游戏原生用户资料页
  → 玩家手动添加好友
```

插件不应自动发送好友请求。用户确认和官方名片页的关注按钮仍由玩家操作。

## 2. 第三方雏形能证明什么

截图可以观察到：

- 游戏菜单中出现了“借卡库”入口；
- 页面显示支援卡、回忆、用户列表、等级和排序控件；
- 页面有分页和支援卡筛选；
- 点击用户后可以继续使用游戏内的资料/关注流程；
- 页面提示已经加载若干条借卡记录。

这些现象证明“网站数据进入游戏内页面”的方案可行，但不能仅凭截图确定：

- 它是否复用了 Friend 页面，还是自建了相同外观的 Screen；
- 数据是在 gRPC 响应层、Presenter 层还是 ItemModel 层注入；
- 它是否真的调用了 `FriendListPresenter`；
- 分页数字是否由原生列表重新计算；
- 它使用的是哪一版客户端和哪一组方法签名。

## 3. Campus 仓库的作用

参考仓库：<https://github.com/vertesan/campus>

相关资料包括：

- `proto/papi.proto`：客户端/服务端消息定义；
- `network/rpc/campusClient.go`：已封装的 gRPC client；
- 服务端下发的 API host 和 RPC 路由。

合理的研究方向是优先复用游戏自带的 network client 和认证会话，再调用游戏已有的
好友/资料请求，而不是自己拼 HTTP 或重新实现登录协议。

账号 refresh token 的导出、复制或重放不属于本项目实现范围，也不应写入插件或文档。

## 4. 候选原生页面和数据模型

以下内容来自 metadata/截图对照，目前是“候选签名”，不是已完成的调用：

| 目标 | 候选类型/方法 | 当前状态 |
|---|---|---|
| 好友页 | `Campus.OutGame.FriendTopScreenPresenter` | 候选，未实机调用 |
| 用户资料跳转 | `MoveToProfileOthersScreen` | 候选，参数和调用时机未确认 |
| 支援卡详情 | `MoveToSupportCardDetailScreen` | 候选，未确认 |
| 回忆详情 | `MoveToMemoryDetailScreen` | 候选，未确认 |
| 关注操作 | `FriendFollowAsync` / `FriendUnfollowAsync` | 只保留为官方页面行为参考，不自动调用 |
| 好友列表数据 | `FriendListPresenter.SetItemModels` | 候选注入点，未确认 |
| 行数据 | `FriendListItemModel` | 候选模型，字段映射未确认 |

网站数据可能需要映射到 `FriendInfo`、`SupportCard` 等内部对象，但不能在没有当前版本
字段类型和构造方法的情况下直接写入。

## 5. 推荐的数据链路

```text
Campus 游戏网络客户端
  → 取得支援卡/用户记录
  → 转换成当前版本的 FriendListItemModel 或原生响应对象
  → 交给 FriendListPresenter / 原生 Cell
  → 点击用户沿官方路径打开名片
```

如果网络层注入需要构造大量 protobuf 字段，才考虑在列表 Presenter 的
`SetItemModels` 附近注入。优先复用游戏的 Cell、头像、卡图、等级和点击行为。

## 6. 菜单入口的已知事实

当前版本已确认或曾经实机观察到：

- `MenuView` 有主格子和副格子容器；
- `MenuButtonViewBase.SetCustomText` 可以修改格子文字；
- `OnSelected` 实际为零参数，点击项通过 `SelectedButtonType` 属性传递；
- `MenuButtonType` 是整数枚举；
- `MenuButtonSerializeType` 是对象类型，用于部分序列化字段和字典键；两者不能混用；
- 12:23 当前 Mod 管理器构建已验证入口克隆、点击和自建文本面板可见；这只证明通用 UI
  调用层可行，不代表本文的借卡网络、列表或名片功能已经实现。

第三方插件具体使用了哪种入口注入方式仍是推测，不能从截图直接反推出内部代码。

## 7. 后续验证顺序

1. 在不修改好友状态的前提下，验证游戏自带 network client 的 API host、请求入口和返回模型；
2. 从当前 PC metadata 确认好友页、资料跳转和列表模型的实际程序集、参数和返回值；
3. 用固定的、只读的公开用户 ID 验证资料页跳转；
4. 再用一条本地静态记录验证列表 Cell 显示；
5. 最后才接入网站/API 数据和筛选分页；
6. 添加好友继续由玩家手动完成。

在上述调用未通过实机验证前，不能把“协议存在”写成“借卡功能已经实现”。
