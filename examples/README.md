# Cabbird SDK 示例

| 示例 | 语言 | 演示内容 |
| --- | --- | --- |
| `hello_ui` | C11 | ABI v1 生命周期、作用域 Window/Font/Texture/Input 查询、generation handle，以及一个宿主自有窗口 |
| `tick_counter` | C++20 | 游戏更新回调与轻量 C++ SDK wrapper |
| `reliable_config` | C++20 | 通过 Config ABI 的宿主自有 JSON 设置；UI 改动把状态标记为 dirty，`on_stop` 时提交 |
| `unity_entity_inspector` | C++20 | 真实的 Unity/IL2CPP 插件：读取宿主的实体源（`cabbird.unity.entities`）与本地玩家快照，并自报两个可选 Unity 服务的可用性 |

上游的两个引擎交互示例（Session 事件浏览、战斗演示）**未移植**：它们依赖上游的
`anomaly.nte.*` 服务与 NTE 快照类型，属于 NTE 的具体功能，与本作的 Unity 目标无关。
替代物是 `unity_entity_inspector`，它只使用宿主实际发布的 `cabbird.unity.*` 服务。

> 这个示例曾经叫 `unity_world_inspector`，查询 `cabbird.unity.build` / `.objects` / `.world`，
> 并在自己的注释里写着「只使用宿主实际发布的服务」——**那句话是假的**：这三个服务是从上游
> UE5（`anomaly.ue5.*`）改名搬来的声明，本仓库没有任何代码发布过它们，所以窗口在每一个
> build 上都把三项显示为 unavailable。服务声明已删除，示例改为读取真实存在的实体源。

本目录是一个独立的 CMake 工程，消费已安装的 `CabbirdSDK` 包。配置时让 CMake 指向 SDK 包目录：

```powershell
cmake -S . -B build `
  -DCabbirdSDK_DIR=C:/path/to/sdk/lib/cmake/CabbirdSDK
cmake --build build --config RelWithDebInfo
```

四个可加载包写入 `build/packages` 下。每个包包含 `plugin.dll` 与对应的 `manifest.json`。

`unity_entity_inspector` 只声明宿主真实发布的服务：必需的 `cabbird.ui`，以及两个
`optional: true` 的 `cabbird.unity.entities` / `cabbird.unity.player`。可选服务缺失时窗口
照常打开并把该项显示为 unavailable，这样插件作者能一眼看出运行时构建上到底发布了哪些服务。
它的 manifest 声明 `games: ["ap"]`（本作 game id），而不是上游的 `nte`。

`hello_ui` 使用 Manifest schema v2，并为每项资源 capability 显式声明。它的 font 请求命名为 `assets/hello-ui.ttf`；派生包应在该包内相对路径放置一个带合适再分发许可的字体。缺少该文件时 font 进入 `FAILED`，示例只是跳过 `font.push`；它只会 push 状态带 `READY` 标志的字体。示例的 1x1 RGBA 纹理来自调用方所有的字节，因此不需要私有 renderer 或图像解码依赖。
