# 生命周期与 Core 服务

头文件：[`include/cabbird/sdk/plugin.h`](../../include/cabbird/sdk/plugin.h)、
[`services/core.h`](../../include/cabbird/sdk/services/core.h)、
[`services/plugin_state.h`](../../include/cabbird/sdk/services/plugin_state.h)、
[`base.h`](../../include/cabbird/sdk/base.h)

## 1. 握手

宿主做的事（`plugin.h` 开头的注释就是这段）：

```
LoadLibrary(plugin.dll)
fn = GetProcAddress(handle, "CabbirdPluginEntryV1")
descriptor.struct_size = sizeof(descriptor)
descriptor.api_major   = CABBIRD_PLUGIN_API_V1_MAJOR
descriptor.api_minor   = CABBIRD_PLUGIN_API_V1_MINOR
status = fn(&descriptor)          /* 插件填写身份与回调 */
校验身份与 API 兼容性
on_load(host_api, &plugin_context)
```

插件只导出**一个**符号，名字由宏给出，不要手写字符串：

```c
CABBIRD_SDK_EXPORT CabbirdStatusV1 CABBIRD_CALL
CabbirdPluginEntryV1(CabbirdPluginDescriptorV1* descriptor);
```

`CABBIRD_PLUGIN_V1_ENTRY_NAME` == `"CabbirdPluginEntryV1"`。
名字被修饰或调用约定不对的 DLL 会被报成「不是 Cabbird 插件」，而不是让加载器崩。

## 2. `CabbirdPluginDescriptorV1`

| 字段 | 谁填 | 说明 |
|---|---|---|
| `struct_size` / `api_major` / `api_minor` | 宿主 | 调用前写入 |
| `id` | 插件 | 稳定的反向域名式标识，如 `cabbird.hello_ui` |
| `name` | 插件 | 面向人的显示名；UI 与本地化目录以它为准 |
| `author` / `version` | 插件 | 借用视图，生命周期同 DLL |
| `on_load` / `on_start` / `on_stop` / `on_unload` / `on_update` / `on_draw` | 插件 | 见下 |

描述符**由宿主分配、宿主拥有**：插件只往里写，不得保留指针。
服务表也由宿主拥有，比插件活得久。

**`plugin_context` 不是描述符字段**，而是 `on_load` 的 out 参数：

```c
CabbirdStatusV1 (CABBIRD_CALL *on_load)(const CabbirdHostApiV1* host, void** plugin_context);
```

插件在里面写入自己的上下文指针，宿主之后在**每个回调里原样传回**。

`CabbirdHostApiV1` 提供 `host_context`、`allocator` 与 `query_service`：

```c
CabbirdStatusV1 (CABBIRD_CALL *query_service)(
    void* host_context, CabbirdStringViewV1 service_id,
    uint32_t minimum_version, const void** service);
```

成功时 `*service` 非空且**在整个进程生命周期内有效**；失败时状态码是 `UNAVAILABLE`
（这个 build 没有）或 `NOT_FOUND`（从来没有过），`*service` 为 null。

## 3. 回调顺序

```
on_load -> on_start -> [on_update / on_draw]* -> on_stop -> on_unload -> FreeLibrary
```

| 回调 | 域 | 要点 |
|---|---|---|
| `on_load(const CabbirdHostApiV1* host, void** plugin_context)` | Lifecycle | 查服务、写自己的上下文指针。**返回非 OK 会中止加载，此时 `on_unload` 不会被调用。** |
| `on_start(void* plugin_context)` | Lifecycle | 资源就绪后开始工作 |
| `on_update(void* plugin_context, double delta_seconds)` | **Game** | 跑在游戏帧里。不建线程、不阻塞。IL2CPP 只在这里用 |
| `on_draw(void* plugin_context, const CabbirdUiServiceV1* ui)` | **Render** | 在 `Present` 里。只画。见 README 的禁止清单 |
| `on_stop(void* plugin_context, uint32_t deadline_milliseconds)` | Lifecycle | 有**软预算**；到点没停，宿主照样硬撤销资源 |
| `on_unload(void* plugin_context)` | Lifecycle | 之后宿主撤销全部资源、等回调排空，然后才 `FreeLibrary` |

> [!IMPORTANT]
> `on_stop` 之后宿主会**撤销插件注册的每一项资源并等待在飞回调排空**，然后才 `FreeLibrary`。
> 插件自己起线程、并让它在 `on_unload` 之后回调 = 未定义行为。
> 这正是 scope 账本要替规范插件排除、替不规范插件**可检测**的那件事。

`on_draw` 拿到的 `CabbirdUiServiceV1*` 只在当前回调内有效（见 [UI 服务](ui-services.md)）。

## 4. `cabbird.core`（永远可解析）

`CABBIRD_CORE_SERVICE_V1_ID` = `"cabbird.core"`，`VERSION` = 1。

`CabbirdCoreServiceV1`：

| 入口 | 签名要点 | 说明 |
|---|---|---|
| `log` | `(user, level, message)` | level 见 `CabbirdCoreLogLevelV1`：`TRACE`/`INFO`/`WARNING`/`ERROR` |
| `read_memory` | `(user, address, CabbirdMutableByteSpanV1 destination)` | 范围未提交或不可读时返回 `UNAVAILABLE`，**不会**触发访问违例 |
| `write_memory` | `(user, address, CabbirdByteSpanV1 source)` | 仅当范围内每一页**已经可写**才写；要改代码页请用 `patch_memory` |
| `patch_memory` | 同上 | 需要时改保护、写、再恢复。**hook/patch 路径**，可被 capability 策略单独门禁 |
| `plugin_directory` | `(user, char* dest, size_t* inout_size)` | 写入插件加载目录（NUL 结尾），`*inout_size` 为所需大小（含终止符）；缓冲不足返回 `BUFFER_TOO_SMALL` 且不写 |
| `module_base` | `(user, module_name) -> uintptr_t` | 按名取已加载模块基址，未加载返回 0。Unity 运行时与游戏程序集是不同模块，插件常需两者 |
| `last_status` | `(user) -> CabbirdStatusV1` | 取本线程最近一次状态；两个字段都是借用视图 |

`write_memory` 与 `patch_memory` 分开是刻意的：只想改一个数据值的插件，
不应该能悄悄改写代码页。

## 5. `cabbird.plugin-state`

`CABBIRD_PLUGIN_STATE_SERVICE_V1_ID` = `"cabbird.plugin-state"`，`VERSION` = 1。

| 入口 | 说明 |
|---|---|
| `directory(user, dest, inout_size)` | 写这个插件的**状态目录**（两次调用协议，同 `plugin_directory`） |

这是唯一一个入口的服务表，存在的理由是「插件要一个自己的目录，而不是把文件丢进宿主根目录」。

## 6. 卸载安全：scope 账本

框架的核心保证。插件通过 ABI 拿到的每一项资源都是**代次句柄**，撤销顺序是固定的：

```
冻结回调源（不再接受新回调）
排空已在栈上的回调
逆序撤销每一项资源
-> 到这一步 FreeLibrary 才安全
```

对插件作者的可操作结论：

- 所有资源在 `on_stop` 之前撤销（或者让宿主在 `on_stop` 之后替你撤销）；
- 回调里要进入可能被撤销的代码时，先取回调租约（例如 `cabbird.interop.hook` 的
  `begin_callback` / `end_callback`）——否则宿主的 drain 会在你的代码还在栈上时报成功，
  紧接着的 `FreeLibrary` 就是崩溃；
- 线程局部状态不要用 `thread_local` 以外的假设：以内存映像载入的模块**没有静态 TLS**，
  宿主为此提供了 FLS 方案（`include/cabbird/thread_local_value.hpp`）。
