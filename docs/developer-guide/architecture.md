# 架构

本文描述**当前源码树**的分层与不变量。每条都指向一个文件，便于核对。

## 1. 进程形态

```
CabbirdLauncher.exe            启动器：选择目标、自提权、载入宿主、写运行时设置
  └─ Cabbird.Core.dll          载入游戏进程的宿主（实现见 src/manual_map/）
       ├─ 平台层               配置、存储、调度、日志、本地化、IPC
       ├─ UI 层                基于 ImGui 的宿主界面（D3D11/DXGI Present 钩子）
       ├─ 游戏层                IL2CPP / Unity 适配、Profile 绑定、实体集、玩家桥
       └─ 插件管理器           目录扫描、manifest 校验、capability 授权、代次与资源账本
            └─ plugins\*\plugin.dll   各插件（进程内 DLL）
```

- 载入方式：内存映像映射，映射器实现位于 `src/manual_map/`。启动器**不依赖代理 DLL**。
- 启动器只有**一种模式**：`LauncherMode::Attach`。界面上的「live-attach」按钮就是它。
- 启动器自提权：`ShellExecuteExW(..., L"runas", ...)`；`cabbird_inject` 则**故意不提权**
  （`CMakeLists.txt` 约 1247 行的链接选项只对 `cabbird_launcher` 生效）。

## 2. 线程域（最重要的不变量）

`include/cabbird/runtime_dispatchers.hpp` 定义 `ExecutionDomain { Lifecycle, Worker, Game, Render }`。

| 域 | 拥有者 | 谁在上面跑 | 允许什么 |
|---|---|---|---|
| `Lifecycle` | 宿主 | 加载/启动/停止/卸载 | 资源注册与撤销 |
| `Worker` | 宿主 | 调度任务、仓储 IO | 阻塞式 IO；**不碰 IL2CPP** |
| `Game` | **引擎**（被 pump） | `on_update` | 唯一能调 IL2CPP 的域；不建线程、不阻塞 |
| `Render` | **presenter**（被 pump） | `on_draw` | 只画；不分配、不加锁、不做 IO |

游戏与渲染两个域**不是宿主自己起线程跑的**，而是挂在引擎的调用点上（游戏帧、`Present`）。
这决定了「插件不能自己在 worker 上碰 IL2CPP」这条硬规则。

> [!CAUTION]
> 宿主曾经因为在一个运行时不知道的线程上做 IL2CPP 懒初始化而崩过一次
> （`0xC0000005`，`GameAssembly.dll` 内）。插件的工作线程必须把请求投递到 `Game` 域。

## 3. 插件生命周期与资源账本

```
扫描 plugins\*        -> manifest.json 校验 -> capability 授权
LoadLibrary            -> GetProcAddress("CabbirdPluginEntryV1") -> 握手
on_load -> on_start -> [on_update / on_draw]* -> on_stop -> on_unload -> FreeLibrary
```

**scope 账本**是安全卸载的核心：插件通过 ABI 取得的每一项资源（窗口、字体、纹理、hook、
订阅、端点、任务）都登记在所属代次下。卸载时：

```
冻结回调源 -> 排空在飞回调 -> 逆序撤销全部资源 -> FreeLibrary
```

`CabbirdGenerationHandleV1{id, generation}` 是这套机制的载体：热重载后旧代次句柄一律失效
（`STALE_GENERATION`），而不是被误当成有效资源。

实现分布：`src/plugin/plugin_manager.cpp`（扫描、加载、代次、界面）、
`src/plugin/plugin_manifest.cpp`（manifest 解析与诊断）、
`src/plugin/plugin_capability_policy.cpp`（capability 授权）、
`src/plugin/plugin_catalog.cpp`、`src/plugin/ipc_registry.cpp`。

## 4. 宿主 / 插件边界

- 边界是**纯 C ABI v1**（`include/cabbird/sdk/`）。规则见
  [API 参考：约定与总览](../api-reference/README.md)。
- 结构体版本化（`V1` + `struct_size`），服务表**只增不改**（`services/unity.h` 开头写明）。
- 分配不跨边界：`CabbirdAllocatorV1` 由宿主提供。
- 插件拿不到宿主内部指针，只能拿服务表；`cabbird.core` 提供 `read_memory` /
  `write_memory` / `patch_memory` 三条受控入口。

## 5. 游戏适配层

```
src/game/unity/           Unity / IL2CPP 适配（unity_adapter.cpp 等）
src/mem/                  内存读写、模式扫描、IL2CPP 绑定
src/hook/                 hook / patch / signature
profiles/unity-build-profiles.json   Profile：把「签名 + 偏移」绑到具体 build
```

Profile 是**宿主拥有的游戏知识**：类名、字段偏移、方法 RVA、签名。插件保持引擎无关，
只消费「世界空间位置 + 包围盒」这类结果。当前树里只有一个 Profile：

```
id            ap-0.6.2.2
game          AzurPromilia
module        GameAssembly.dll
method        unity.playerloop.dispatch
  symbol      Runtime.Extension.UPlayerLoop::Dispatch
  token       0x6000F59
  rva         0x5D53E00
  prologue    48895C2408574881EC900000000F29B4
```

Profile 没绑上或校验不过时，实体/玩家/变换服务返回 `UNAVAILABLE`——**这是常态而不是错误**，
插件应当降级显示。

## 6. 渲染与界面

- `src/render/dx11/`：`Present` 钩子、交换链管理、嵌入宿主界面。
- `src/ui/`：`cabbird.ui` 服务的宿主实现（窗口、字体、纹理、输入注册表）。
- 界面在**游戏窗口之上**绘制（`Embedded=1`、`AttachToProcessWindow=1`），
  `Insert`（`ToggleKey=45`）是默认开关。
- `apps/render_fixture/`：`cabbird-platform-preview.exe` 把同一套界面放进一个普通窗口，
  **不需要接触游戏进程**，是开发界面的首选夹具。

## 7. 配置与状态

| 路径 | 内容 |
|---|---|
| `<game>\Cabbird\cabbird.ini` | 运行时设置（分析器、平台、性能、Profile） |
| `<game>\Cabbird\plugin-repositories.json` | 第三方插件源（默认 `enabled: false`） |
| `<game>\Cabbird\config\plugin-enablement.json` | 启用状态，**带文件监听** |
| `<game>\Cabbird\plugins\<Name>\` | 插件包 |

`RuntimeSettingsRoot()`（`apps/launcher/main.cpp:450`）的规则：若 `<游戏目录>\Cabbird\` 下
存在 `Cabbird.Core.dll`，就用它；否则用启动器自己所在目录。启动器保存 `Insert` 键时
（`SaveToggleKeyImpl`）写入 `RuntimeSettingsRoot()/cabbird.ini` 的 `[Platform] ToggleKey`。

## 8. 相关页面

- [构建](building.md)
- [插件开发](plugin-development.md)
- [贡献与约定](contributing.md)
- [Unity 服务](../api-reference/unity-services.md)
