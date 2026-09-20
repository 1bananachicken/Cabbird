# 开发者文档

面向**改这个仓库**的人。只想用 Cabbird 请去[使用者文档](../user-guide/README.md)；
只想写插件请先看[插件开发](plugin-development.md)，API 细节在
[API 参考](../api-reference/README.md)。

## 阅读顺序

1. [架构](architecture.md) —— 进程形态、四个线程域、插件生命周期与资源账本、游戏适配层。
2. [构建](building.md) —— `build.cmd`、presets、目标与产物名、包布局、构建期坑。
3. [插件开发](plugin-development.md) —— 从零写一个插件：manifest、CMake 助手、
   入口、生命周期、窗口、持久化、热重载、调试。
4. [贡献与约定](contributing.md) —— append-only ABI、注释风格、文档约定。

## 最短路径

```cmd
build.cmd
.build\windows-vs2022\RelWithDebInfo\cabbird-platform-preview.exe --frames=120
```

第二条命令会在**普通窗口**里跑起宿主完整界面，**不接触游戏进程**，
是改 UI / 改插件界面的首选夹具（`apps/render_fixture/platform_preview.cpp`）。

## 三条最容易踩的规则

1. **不要给 `cmake --build` 加 `--parallel`** —— 在这个 MSBuild 下会在打印任何有用信息前 exit 1。
2. **公开结构体 append-only** —— 字段顺序就是 ABI。
3. **不要声称没验证过的结论** —— 没有路径或命令可指，就如实标注。
