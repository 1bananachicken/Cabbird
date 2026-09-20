# 插件间 IPC

头文件：[`services/ipc.h`](../../include/cabbird/sdk/services/ipc.h)
（宿主侧实现与登记表见 `include/cabbird/ipc_registry.hpp`、`src/plugin/ipc_registry.cpp`）

`cabbird.ipc` 让插件互相调用与订阅，而不必知道对方是谁、也不必共享头文件。
它是**插件之间**的通道，不是插件与外部进程之间的通道（对外诊断走的是命名管道，
见[故障排查](../user-guide/troubleshooting.md#6-命令行诊断)）。

## 1. 模式与亲和性

```c
typedef enum CabbirdIpcModeV1 {
    CABBIRD_IPC_MODE_V1_SYNC_REQUEST  = 1u << 0u,
    CABBIRD_IPC_MODE_V1_ASYNC_REQUEST = 1u << 1u,
    CABBIRD_IPC_MODE_V1_EVENT         = 1u << 2u
} CabbirdIpcModeV1;

typedef enum CabbirdIpcAffinityV1 {
    CABBIRD_IPC_AFFINITY_V1_CALLER = 0,
    CABBIRD_IPC_AFFINITY_V1_WORKER = 1,
    CABBIRD_IPC_AFFINITY_V1_LIFECYCLE = 2,
    CABBIRD_IPC_AFFINITY_V1_GAME = 3,
    CABBIRD_IPC_AFFINITY_V1_RENDER = 4
} CabbirdIpcAffinityV1;
```

`affinity` 决定 handler 在哪个域被调用。**`GAME` 亲和性意味着 handler 会跑在游戏帧里**：
不要阻塞、不要做重活。

`reentrancy` 是 `REJECT`（默认）或 `ALLOW`。默认拒绝重入，
否则一次 A→B→A 的调用链会变成死锁或无限递归，只能靠 `REENTRANT_CYCLE` 错误收场。

## 2. 端点描述符与选择器

注册时用 `CabbirdIpcEndpointDescriptorV1`，调用时用 `CabbirdIpcEndpointSelectorV1`。
两者都带 schema 哈希（`CABBIRD_IPC_SCHEMA_HASH_V1_SIZE` = 32 字节），
所以「版本号相同但结构变了」不会被当成兼容。

描述符里的限额字段：`timeout_milliseconds`、`maximum_request_bytes`、
`maximum_response_bytes`、`maximum_event_bytes`、`maximum_queue_depth`。

## 3. 服务入口

| 入口 | 说明 |
|---|---|
| `register_endpoint(user, descriptor, handler, callback_user, *endpoint)` | 发布一个端点；句柄 scope 拥有 |
| `unregister_endpoint(user, endpoint)` | 撤销 |
| `invoke(user, selector, request, response, *response_size)` | 同步请求 |
| `invoke_async(user, selector, request, completion, completion_user, *pending_call)` | 异步请求，完成回调带 `CabbirdGenerationHandleV1 pending_call` 与响应视图 |
| `cancel(user, pending_call)` | 取消在飞请求 |
| `subscribe(user, selector, callback, callback_user, *subscription)` | 订阅事件 |
| `unsubscribe(user, subscription)` | 取消订阅 |
| `publish(user, endpoint, event)` | 向自己的端点发布事件 |

请求上下文 `CabbirdIpcRequestContextV1` 带 `request_id` 与 `caller_plugin_id`，
所以 handler 能知道「谁在问我」。

## 4. 错误码

`CabbirdIpcErrorV1`：

| 值 | 名字 | 含义 |
|---|---|---|
| 0 | `NONE` | |
| 1 | `PROVIDER_MISSING` | 没有这个端点 |
| 2 | `VERSION_MISMATCH` | 主版本不兼容 |
| 3 | `SCHEMA_MISMATCH` | schema 哈希不匹配 |
| 4 | `MODE_UNAVAILABLE` | 对方不支持你要的模式 |
| 5 | `TIMEOUT` | 超时 |
| 6 | `REENTRANT_CYCLE` | 重入被拒 |
| 7 | `QUEUE_FULL` | 队列满 |
| 8 | `STALE_GENERATION` | 句柄属于旧代次 |
| 9 | `DEPENDENCY_REQUIRED` | 需要先声明依赖 |

## 5. 使用要点

- 端点 id 是**全局命名空间**，用反向域名式前缀（如 `cabbird.example.foo`）避免撞名；
- 需要对方存在时，在 manifest 的 `dependencies` 里声明，而不是在运行时才发现
  `PROVIDER_MISSING`；
- 事件订阅与异步请求都返回句柄，**在 `on_stop` 之前撤销**；
- 句柄跨代次一律失效（`STALE_GENERATION`），热重载后要重新注册；
- capability `ipc` 是必需的，否则 `cabbird.ipc` 服务不会被授权。
