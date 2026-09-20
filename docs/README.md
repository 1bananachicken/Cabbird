# Cabbird 文档

Cabbird 的文档分三套，按你的角色选择入口。

## 📘 [用户文档](user-guide/README.md)

面向**使用者**：把 Cabbird 部署到游戏目录、用启动器附加、启用内建插件、改配置、排错。

- [安装与部署](user-guide/installation.md)
- [快速上手](user-guide/quickstart.md)
- [内建插件](user-guide/built-in-plugins.md)
- [配置参考](user-guide/configuration.md)
- [故障排查与 FAQ](user-guide/troubleshooting.md)

## 🛠️ [开发者文档](developer-guide/README.md)

面向**贡献者与插件作者**：从源码构建、理解架构、开发插件、提交改动。

- [从源码构建](developer-guide/building.md)
- [架构概览](developer-guide/architecture.md)
- [插件开发](developer-guide/plugin-development.md)
- [贡献指南](developer-guide/contributing.md)

## 📑 [API 参考](api-reference/README.md)

面向**插件作者**：纯 C ABI v1 的完整契约。

- [约定与总览](api-reference/README.md)
- [生命周期与 Core 服务](api-reference/lifecycle-and-core.md)
- [平台作用域服务](api-reference/platform-services.md)
- [Interop 与内存](api-reference/interop-and-memory.md)
- [插件间 IPC](api-reference/ipc.md)
- [UI 服务](api-reference/ui-services.md)
- [Unity 服务](api-reference/unity-services.md)
- [Manifest 与 capability](api-reference/manifest-and-capabilities.md)

## 权威来源

文档描述稳定的接口与流程。下列易变事实以仓库内的源为准：

| 主题 | 权威来源 |
|---|---|
| 公开 ABI | `include/cabbird/sdk/` |
| 宿主内部框架头 | `include/cabbird/*.hpp` |
| Schema | `schemas/` |
| ABI 快照基线 | `abi/cabbird-sdk-v1-windows-x64.json` |
| 构建入口与预设 | [`build.cmd`](../build.cmd)、[`CMakePresets.json`](../CMakePresets.json)、[`CMakeLists.txt`](../CMakeLists.txt) |
| 宿主配置 | [`config/cabbird.ini`](../config/cabbird.ini) |
| 运行时开关 | [`assets/cabbird_overlay.ini`](../assets/cabbird_overlay.ini) |
| 目标游戏 build 描述 | [`profiles/unity-build-profiles.json`](../profiles/unity-build-profiles.json) |
| 界面文案 | `locales/host/`、各插件目录下的 `locales/` |

## 维护规则

- 改代码 → 同一提交里同步改动受影响的页面；新增跨模块 include → 同时更新[架构概览](developer-guide/architecture.md)。
- 新增插件或服务 → 更新[内建插件](user-guide/built-in-plugins.md)或 [API 参考](api-reference/README.md)。
- 相对链接必须有效；改文件名时同步改引用。
- 每条事实性说明都应能落到一个**文件路径**或一条**命令**上。

> 装机布局、安装清单这类会经常变化的信息，以当前 `CMakeLists.txt` 的 install 规则和构建输出为准。
