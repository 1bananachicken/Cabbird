# 用户文档

这里写 Cabbird 的日常用法：怎么部署、怎么附加到游戏、装哪些插件、怎么改配置，以及出问题时先看什么。
如果你要从源码构建或自己写插件，请转到[开发者文档](../developer-guide/README.md)。

## 阅读顺序

1. [安装与部署](installation.md) — 运行包里有什么、放在哪、怎么启动。
2. [快速上手](quickstart.md) — 第一次附加、打开界面、启用第一个插件。
3. [内建插件](built-in-plugins.md) — 随包提供的 7 个插件分别做什么、依赖什么。
4. [配置参考](configuration.md) — 切换键、语言、插件目录、运行时开关、Profile。
5. [故障排查与 FAQ](troubleshooting.md) — 界面不出现、插件不加载、功能显示不可用等。

## 术语速查

| 术语 | 含义 |
|---|---|
| **Core / Runtime** | 载入游戏进程的 `Cabbird.Core.dll`，承载全部宿主能力。 |
| **启动器** | 包根目录的 `CabbirdLauncher.exe`（ImGui 界面，英文/中文），负责选游戏目录、列出进程、附加 Core。 |
| **注入器** | `cabbird_inject.exe`，命令行工具；**不随运行包安装**，只存在于构建树里。 |
| **插件** | 一个目录，含 `manifest.json` 与 `plugin.dll`；宿主扫描 `PluginDirectory` 下的每个目录。 |
| **服务** | 宿主通过 C ABI 发布的接口表，id 形如 `cabbird.ui`、`cabbird.unity.player`。 |
| **capability** | 插件在 manifest 里**请求被允许做的事**（如 `ui`、`memory-write`），与服务是两件事。 |
| **Profile** | 某个游戏 build 的符号/方法描述，见 [`profiles/unity-build-profiles.json`](../../profiles/unity-build-profiles.json)。 |
| **generation** | 一次加载/重载/换代。旧代次的 endpoint 在撤销后不可再使用。 |
| **线程域** | 宿主内部的四个执行域：`Lifecycle`、`Worker`、`Game`、`Render`（见 [`runtime_dispatchers.hpp`](../../include/cabbird/runtime_dispatchers.hpp)）。 |

> 使用前请阅读根目录 [README](../../README.md) 中的免责声明。
