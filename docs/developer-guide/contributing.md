# 贡献与约定

本仓库的规矩大多来自**已经付过代价的错误**。改代码前请先读这一页。

## 1. 不要声称没有依据的结论

- 任何「已验证 / 已测试 / 实测」的说法，必须能指向**一个文件路径或一条命令**；
- 做不到就不要这样写，改为说明它需要什么条件（实机、特定环境、尚未实现的部分）；
- 这条同样适用于文档与注释：**注释里不写「将来会做」**，要么现在做，要么明确写成未实现。

## 2. 公开 ABI 是 append-only

`include/cabbird/sdk/` 下的结构体是 ABI：

- 字段**不得重排、不得改类型、不得删除**；
- 新字段/新入口**追加在末尾**，并让 `struct_size` 覆盖它；
- 新服务用**新 id + 新 version**，不要改老表；
- 插件用 `struct_size` 判断字段是否存在（见 `examples/hello_ui/plugin.c` 的 `HAS_FIELD`），
  宿主也应当保持这个可判断性。

改完公开结构体后，`abi/cabbird-sdk-v1-windows-x64.json` 需要同步手工维护。

## 3. 注释写「为什么」，而且要写清代价

本仓库的注释风格是刻意的，请保持：

- 说明**为什么这样而不是那样**，尤其是被否决的替代方案；
- 记录**性能数字**（例如「`Transform::get_position` 约 2.5 ms/次」）；
- 记录**改动的原因**，包括曾经走错、后来纠正的那些决定；
- 不要留下指向已不存在文件的引用；引用路径前先确认它还在。

## 4. 线程与生命周期

| 规则 | 出处 |
|---|---|
| IL2CPP 只能在 Game 域 | `include/cabbird/sdk/plugin.h` |
| Render 域只画：不分配、不加锁、不做 IO、不加载 DLL | 同上 |
| 插件注册的资源必须在代次内可撤销 | `CabbirdGenerationHandleV1` / scope 账本 |
| hook 回调必须取租约 | `cabbird.interop.hook` 的 `begin_callback` / `end_callback` |

新代码如果引入新的跨域动作，请在文档里同步说明它属于哪个域。

## 5. 文档约定

- 三套文档：`docs/user-guide/`（使用者）、`docs/developer-guide/`（贡献者）、
  `docs/api-reference/`（插件作者）。见 [`docs/README.md`](../README.md)；
- 每一条事实性断言都要能落到**文件路径**（`include/...`、`src/...`、`CMakeLists.txt`）或
  **命令**上；
- 相对链接必须有效。改文件名时同步改引用；
- 使用 `> [!IMPORTANT]` / `> [!WARNING]` / `> [!CAUTION]` 标注会让人踩坑的地方；
- 中文为主，标识符、路径、代码保持原文。

## 6. 构建与提交

- 用 `build.cmd`，**不要**给 `cmake --build` 加 `--parallel`（见[构建](building.md)）；
- 配置类型只有 `Debug` / `RelWithDebInfo`；
- 提交前自检：
  1. `build.cmd` 通过；
  2. 文档里新增/改动的链接能打开；
  3. 新增的「已验证 / 已测试」说法有可指认的证据。

## 7. 相关页面

- [构建](building.md)
- [架构](architecture.md)
- [插件开发](plugin-development.md)
