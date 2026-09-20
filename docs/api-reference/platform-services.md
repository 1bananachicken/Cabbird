# 平台作用域服务

头文件：[`services/platform.h`](../../include/cabbird/sdk/services/platform.h)、
[`services/json.h`](../../include/cabbird/sdk/services/json.h)、
[`services/localization.h`](../../include/cabbird/sdk/services/localization.h)

这一族服务与游戏无关：配置、存储、调度、本地化、JSON、命令、通知、运行时信息、自检。

## 1. `cabbird.config`（v1）

用 JSON Schema 约束插件自己的持久化文档。**必须在每一代加载时先注册 schema**，
之后才能 `read` / `write_atomic` / `migrate`。

| 入口 | 说明 |
|---|---|
| `register_schema(user, schema_id, schema_version, schema_json, *handle)` | 注册一份插件本地文档的 schema；返回的句柄是 scope 拥有的 |
| `unregister_schema(user, handle)` | 撤销 |
| `read(user, schema_id, *schema_version, destination, *inout_size)` | 按两次调用协议读取。**`NOT_FOUND` 表示这个插件还没有保存过文档**，不是错误；`*schema_version` 回传存储时的版本 |
| `write_atomic(user, schema_id, schema_version, document)` | 按已注册 schema 校验后**原子替换**插件本地文档。持久位置由宿主拥有 |
| `migrate(user, schema_id, CabbirdConfigMigrationV1 migration, migration_user)` | 老版本文档的迁移回调，签名见下 |

```c
typedef CabbirdStatusV1 (CABBIRD_CALL *CabbirdConfigMigrationV1)(
    void* user, uint32_t source_schema_version,
    CabbirdByteSpanV1 source,
    CabbirdMutableByteSpanV1 destination, size_t* inout_size);
```

## 2. `cabbird.storage`（v1）

给插件的**相对路径**键值/文件存储，三条入口：`read` / `write_atomic` / `remove`。
`read` 同样用两次调用协议。路径是相对的，宿主决定真实根目录
（`cabbird.plugin-state` 的 `directory` 可以拿到它）。

## 3. `cabbird.runtime-info`（v1）

| 入口 | 说明 |
|---|---|
| `snapshot(user, CabbirdRuntimeInfoV1* snapshot)` | 一次取全：运行时版本三段、`process_id`、`thread_id`、`uptime_milliseconds`、`plugin_generation` |
| `runtime_version_utf8(user, dest, *inout_size)` | 版本字符串 |

`plugin_generation` 是判断「我是不是换代了」最直接的字段。

## 4. `cabbird.diagnostics`（v1）

| 入口 | 说明 |
|---|---|
| `register_self_test(user, id, callback, callback_user, *handle)` | 注册一个自检；宿主可以主动跑它 |
| `unregister_self_test(user, handle)` | 撤销 |
| `run_self_test(user, id, destination, *inout_size)` | 跑指定自检并拿结果 |
| `snapshot_json(user, destination, *inout_size)` | 整份诊断快照（JSON） |

`CabbirdDiagnosticSelfTestV1` 的签名是
`(void* user, CabbirdMutableByteSpanV1 destination, size_t* inout_size)`。

## 5. `cabbird.scheduler`（v1）

| 入口 | 说明 |
|---|---|
| `schedule(user, delay_milliseconds, CabbirdTaskCallbackV1 callback, callback_user, *handle)` | 延时任务；句柄 scope 拥有 |
| `cancel(user, handle)` | 取消 |

回调签名 `void (CABBIRD_CALL *CabbirdTaskCallbackV1)(void* user, CabbirdGenerationHandleV1 task)`。
调度跑在 `Worker` 域，**不要**在回调里碰 IL2CPP。

## 6. `cabbird.commands`（v1）

| 入口 | 说明 |
|---|---|
| `register_command(user, name, description, callback, callback_user, *handle)` | 注册一个命令 |
| `unregister_command(user, handle)` | 撤销 |
| `invoke(user, name, arguments, destination, *inout_size)` | 调用（宿主也可以调你的） |

命令回调签名：`(void* user, CabbirdStringViewV1 arguments, CabbirdMutableByteSpanV1 destination, size_t* inout_size)`。
参数是一整行文本，返回值写进 `destination`。

## 7. `cabbird.notifications`（v1）

| 入口 | 说明 |
|---|---|
| `post(user, severity, title, body, timeout_milliseconds, *handle)` | 发一条通知；`severity` 见 `CabbirdNotificationSeverityV1`：`INFO`/`WARNING`/`ERROR` |
| `dismiss(user, handle)` | 撤掉 |

`timeout_milliseconds` 为 0 时由宿主决定停留时长。

## 8. `cabbird.json`（v1）

对 nlohmann/json 的**插件作用域 DOM**。`parse` / `array_item` / `object_find` 返回的句柄
都由插件 scope 拥有，**必须在 DLL 卸载前释放**；句柄在所属插件代次停止时失效。

| 入口 | 说明 |
|---|---|
| `parse(user, document, *handle)` | 解析一份 JSON 文档 |
| `release(user, handle)` | 释放 |
| `kind(user, handle, *kind)` | `CabbirdJsonKindV1`：`NULL`/`BOOLEAN`/`NUMBER`/`STRING`/`ARRAY`/`OBJECT` |
| `boolean_value` / `number_value` / `string_value` | 取值 |
| `array_size` / `array_item` | 数组遍历 |
| `object_size` / `object_key_at` / `object_find` | 对象遍历与查找 |
| `serialize(user, handle, dest, *inout_size)` | 序列化回文本 |

## 9. `cabbird.localization`（v1）

| 入口 | 说明 |
|---|---|
| `locale(user, dest, *inout_size)` | 当前 locale（如 `zh-CN`） |
| `translate(user, key, english_fallback, arguments, argument_count, dest, *inout_size)` | 按 key 取文案；缺失时用英文回退串 |

插件自己的文案放在**插件目录**的 `locales/<locale>.json`，key 形如
`cabbird.damage-replay.replay-count`；宿主在找不到 `<包>/locales/<locale>.json` 时
**静默回退英文**，所以拷漏目录会得到「看起来完整其实不是」的包
（`CMakeLists.txt` 的 DamageReplay 安装块就是为这件事写的）。

## 10. `cabbird.plugin-state`

见[生命周期与 Core 服务](lifecycle-and-core.md#5-cabbirdplugin-state)。

## 11. 可用性

| 服务 | 备注 |
|---|---|
| `cabbird.config` / `cabbird.storage` / `cabbird.runtime-info` / `cabbird.diagnostics` / `cabbird.scheduler` / `cabbird.commands` / `cabbird.notifications` / `cabbird.json` / `cabbird.localization` | 由宿主平台层发布 |
| `cabbird.websocket` | **没有服务头文件发布它**；`cabbird_sdk.h` 也不包含 `services/websocket.h`。按 `UNAVAILABLE` 处理 |

声明为 `optional` 的服务缺失时插件必须降级而不是失败——这是本套 ABI 的核心约定，
见[约定与总览](README.md#可用性与代次)。
