## 改动

说明行为、归属边界与证据。

## 验证

列出实际跑过的命令：configure、build、test、sanitizer、打包，或外部验证。

> 提交前自检见 [贡献与约定](../docs/developer-guide/contributing.md#6-构建与提交)：
> `build.cmd` 通过、文档链接可打开、「已验证 / 已测试」都有可指认的证据。

## 公开接口审查

改动涉及公开 SDK 头文件、服务行为、Manifest / IPC / Repository schema、capability，
或已文档化的插件契约时，填写本节；否则写 `不适用`。

- 目标版本与 RFC：
- 公开 ABI 是否 append-only（字段未重排 / 未改类型 / 未删除）：
- `abi/cabbird-sdk-v1-windows-x64.json` 是否已同步：
- 新服务是否使用新 id + 新 version：
- `struct_size` / 字段可判断性（旧插件 + 新宿主、新插件 + 旧宿主）：
- 线程域归属（Game / Render / 其他）与阻塞、停止期限：
- 所有权、代次与句柄/表/缓冲区生命周期：
- 状态码、部分写入、重试与 fail-closed 行为：
- capability、schema 协商、Profile / Feature 可用性：
- API 参考、所有权文档、示例、限制与迁移说明：

无法解释的二进制或语义差异会阻塞合并与发布。

## 许可与依赖

- 是否新增第三方依赖？若是，注明版本、许可与 `CMakeLists.txt` 里的 pin；
- **不要把第三方源码放进仓库**：`third_party/` 只允许放许可证文本，
  组件一律由 `FetchContent` 按 tag 动态下载；
- 新增组件时同步更新 `NOTICE` 与 `third_party/licenses/`（两者会随每个发布组件安装）。
