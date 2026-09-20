# Interop 与内存

头文件：[`services/interop.h`](../../include/cabbird/sdk/services/interop.h)、
[`services/il2cpp.h`](../../include/cabbird/sdk/services/il2cpp.h)、
[`services/core.h`](../../include/cabbird/sdk/services/core.h)

这一族服务会**改变进程状态**，因此是 capability 策略的重点。

| 服务 | capability |
|---|---|
| `cabbird.interop.hook` | `interop-hook` |
| `cabbird.interop.patch` | `interop-patch` |
| `cabbird.interop.signature` | `interop-signature` |
| `cabbird.unity.il2cpp` | `unity-il2cpp` |
| `cabbird.core` 的 `read_memory` / `write_memory` / `patch_memory` | `memory-read` / `memory-write` |

## 1. `cabbird.interop.hook`（v1）

```c
typedef enum CabbirdHookKindV1 {
    CABBIRD_HOOK_V1_FUNCTION = 1,
    CABBIRD_HOOK_V1_IAT      = 2,
    CABBIRD_HOOK_V1_EXPORT   = 3,
    CABBIRD_HOOK_V1_VTABLE   = 4
} CabbirdHookKindV1;

typedef struct CabbirdHookRequestV1 {
    uint32_t struct_size;
    uint32_t kind;
    uintptr_t target;
    void* detour;
    CabbirdStringViewV1 label;
} CabbirdHookRequestV1;
```

| 入口 | 说明 |
|---|---|
| `create(user, request, *original, *handle)` | 安装 hook；`*original` 回传可调用的原函数指针，`*handle` 是 scope 拥有的句柄 |
| `release(user, handle)` | 卸载 |
| `begin_callback(user, hook, *callback_lease)` | **进入你的 detour 之前必须调用**，拿一张回调租约 |
| `end_callback(user, lease)` | 离开 detour 时归还 |

> [!IMPORTANT]
> `begin_callback` / `end_callback` 不是可选的礼节。没有租约时，宿主在卸载流程里
> **排空在飞回调**这一步无法知道你的代码还在栈上，于是它会在你还在执行时报告「已排空」
> 并卸载 DLL。`plugins/damage_replay` 的 detour 就是按这个模式写的。

## 2. `cabbird.interop.patch`（v1）

| 入口 | 说明 |
|---|---|
| `apply(user, address, replacement, label, *handle)` | 改页保护后写入字节，句柄 scope 拥有 |
| `release(user, handle)` | 还原 |

与 `cabbird.core` 的 `patch_memory` 的区别：这一条**登记资源**（可被账本撤销、可在界面里看到），
`patch_memory` 是一次性写入。要做可撤销的补丁，用这个。

## 3. `cabbird.interop.signature`（v1）

| 入口 | 说明 |
|---|---|
| `resolve(user, module_name, section_name, pattern, *address)` | 在模块的某个节区里按字节模式定位地址 |

签名解析的语义（匹配多处、匹配不到）由宿主决定并如实报错——`plugins/damage_replay` 的文档里
写明「匹配到多处或匹配不到都会明确报错，不会取第一个」，这是使用这一条时的预期行为。

## 4. 内存：`cabbird.core`

| 入口 | 语义 |
|---|---|
| `read_memory` | 范围未提交/不可读 → `UNAVAILABLE`，**不触发访问违例** |
| `write_memory` | 仅当范围内**每一页已经可写**才写 |
| `patch_memory` | 需要时改保护、写、恢复 |

三者的划分是刻意的：只改一个数据值的插件，不应该能悄悄改写代码页。

## 5. `cabbird.unity.il2cpp`（v1）

这是**唯一**能让插件读活体 IL2CPP 类型系统的服务，也是唯一能让宿主遍历整个运行时的入口，
所以它有独立的 capability `unity-il2cpp`。

```c
typedef struct CabbirdIl2CppServiceV1 {
    uint32_t struct_size;
    uint32_t service_version;
    void* user;
    CabbirdStatusV1 (CABBIRD_CALL *with_runtime)(
        void* user, CabbirdIl2CppCallbackV1 callback, void* callback_user);
    CabbirdStatusV1 (CABBIRD_CALL *release_handles)(
        void* user, const CabbirdIl2CppGCHandleV1* handles, size_t count);
} CabbirdIl2CppServiceV1;
```

- `with_runtime` **同步**执行回调，且**只能在 `on_update` 的游戏线程上**调用。
  宿主复用 `cabbird::il2cpp` 的初始化与 `ThreadScope`，不投递、不等待。
- 回调里的 `CabbirdIl2CppApiV1*` 只在本次回调内有效；**不得保存表指针，也不得在
  Render / Lifecycle 线程调用它的任何入口**。
- 对象引用若要在回调之后继续持有，必须取 GC 句柄（`il2cpp_gchandle_new`）。
- `release_handles` 只做 GC 清理，可在回调排空后的 `on_stop` 使用；它**不调用任何 Unity 方法**。

### 暴露的函数（`CabbirdIl2CppApiV1`，按头文件顺序）

域与程序集：`il2cpp_domain_get`、`il2cpp_domain_get_assemblies`、`il2cpp_assembly_get_image`、
`il2cpp_image_get_name`

类型与类：`il2cpp_class_from_name`、`il2cpp_class_get_parent`、`il2cpp_class_get_type`、
`il2cpp_class_get_field_from_name`、`il2cpp_class_get_method_from_name`、
`il2cpp_class_is_assignable_from`、`il2cpp_class_get_methods`

字段与方法：`il2cpp_field_get_type`、`il2cpp_field_get_value`、`il2cpp_method_get_param`、
`il2cpp_method_get_name`、`il2cpp_method_get_param_count`、`il2cpp_type_get_name`

对象与调用：`il2cpp_type_get_object`、`il2cpp_object_get_class`、`il2cpp_runtime_invoke`、
`il2cpp_object_unbox`

字符串与数组：`il2cpp_string_new`、`il2cpp_string_length`、`il2cpp_string_chars`、
`il2cpp_array_length`、`il2cpp_array_object_header_size`

GC 与内存：`il2cpp_gchandle_new`、`il2cpp_gchandle_get_target`、`il2cpp_gchandle_free`、
`il2cpp_free`

> [!CAUTION]
> `il2cpp_class_get_methods` / `get_properties` / `get_interfaces` 这类遍历会**强制懒元数据初始化**。
> 在一个运行时并不认识的线程上调用它们，曾经真的杀掉过宿主进程（`0xC0000005`，
> 发生在 `GameAssembly.dll` 内）。这就是「IL2CPP 只能在游戏域」这条规则的来源。
> 需要遍历时用 `cabbird.unity.dump` 服务——它会在自己的 worker 上先
> `il2cpp_thread_attach`，并把可能出错的调用放在 SEH 守卫里。

## 6. C++ 帮助类

C++ 插件可以用 [`include/cabbird/sdk/il2cpp.hpp`](../../include/cabbird/sdk/il2cpp.hpp) 的
`cabbird::sdk::il2cpp::Context` 等封装，省掉手工取句柄与释放的样板。
它只是封装，**不改变上面的线程与生命周期规则**。

## 7. 实现位置

| 项 | 位置 |
|---|---|
| `cabbird::mem` 原始内存层 | `src/mem/` |
| `cabbird.unity.il2cpp` 的宿主实现 | `src/mem/il2cpp*.cpp` |
| hook / patch / signature 的宿主实现 | `src/hook/`、`src/plugin/`；`plugins/damage_replay` 是唯一使用者 |

> 判断可用性请用运行时事实：`query_service` 返回 `UNAVAILABLE` 就是没有；不要根据本文档假设。
