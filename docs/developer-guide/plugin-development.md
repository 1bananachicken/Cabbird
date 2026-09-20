# 插件开发

从零写一个 Cabbird 插件。**先读**
[API 参考：约定与总览](../api-reference/README.md) 与
[生命周期与 Core 服务](../api-reference/lifecycle-and-core.md)，本文只讲怎么把它组装起来。

## 1. 最小包结构

```
MyPlugin\
  plugin.dll
  manifest.json
  locales\zh-CN.json      (可选)
  assets\...              (可选，字体/纹理)
```

宿主按 `<插件根>\<任意目录名>\manifest.json` 扫描，**id 以 manifest 为准**。

## 2. manifest

```json
{
  "schemaVersion": 2,
  "id": "cabbird.example.my-plugin",
  "name": "My Plugin",
  "description": "Shows something.",
  "author": "You",
  "license": "AGPL-3.0-only",
  "version": "1.0.0",
  "entry": "plugin.dll",
  "api": { "major": 1, "minMinor": 0, "maxMinor": 0 },
  "games": ["ap"],
  "builds": ["ap-*"],
  "loadPhase": "game-ready",
  "services": [
    { "id": "cabbird.ui", "minVersion": 1 },
    { "id": "cabbird.config", "minVersion": 1, "optional": true }
  ],
  "capabilities": ["ui", "configuration"]
}
```

规则与诊断码见 [Manifest 与 capability](../api-reference/manifest-and-capabilities.md)。
要点：

- `games: ["ap"]`、`builds: ["ap-*"]`（本作 game id 是 `ap`）；
- 必需服务必须有对应 capability，否则授权会被标记不可执行；
- **能选 `optional: true` 就选**——目标 build 上服务缺失是常态；
- 只读插件**不要**申请写权限（`unity-player-teleport`、`memory-write`、`interop-hook`……）。

## 3. 用 SDK 的 CMake 助手

SDK 安装后提供 `CabbirdPlugin.cmake`（`cmake/sdk/CabbirdPlugin.cmake`）：

```cmake
cmake_minimum_required(VERSION 3.22)
project(MyPlugin LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
find_package(CabbirdSDK CONFIG REQUIRED)

cabbird_add_plugin(cabbird_example_my_plugin
    SOURCES my_plugin/plugin.cpp
    MANIFEST my_plugin/manifest.json
    PACKAGE_NAME MyPlugin
    OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/packages/MyPlugin")
```

配置时指向 SDK 包：

```powershell
cmake -S . -B build -DCabbirdSDK_DIR=C:/path/to/sdk/lib/cmake/CabbirdSDK
cmake --build build --config RelWithDebInfo
```

`cabbird_add_plugin` 做的事（同文件，共 49 行）：

| 行为 | 说明 |
|---|---|
| 建 `SHARED` 库并链接 `Cabbird::sdk` | 纯接口目标，只带头文件 |
| `PREFIX ""`、`OUTPUT_NAME "plugin"` | 产物就是 `plugin.dll`（不带 `lib` 前缀） |
| 输出到 `OUTPUT_DIRECTORY` | 默认 `<build>/package/<PACKAGE_NAME>` |
| POST_BUILD 复制 manifest | 保证 `plugin.dll` 与 `manifest.json` 同目录 |
| MSVC `/W4 /permissive-` | `C_ONLY` 时不加 `/EHsc`（C 插件不引 C++ 异常运行时） |
| 登记 `CABBIRD_RELEASE_PLUGIN_TARGETS` | 供发布打包使用；`NO_RELEASE` 可退出 |

**注意**：助手**不会**替你复制 `locales\` 与 `assets\`。需要就自己加
`add_custom_command(... POST_BUILD ...)`，否则界面会静默回退英文
（这正是 `CMakeLists.txt` 里 DamageReplay 那段安装块存在的原因）。

## 4. 入口与生命周期

```c
#include "cabbird/sdk/cabbird_sdk.h"
#include <stddef.h>
#include <string.h>

/* 借用视图：SDK 不做 NUL 结尾假设，所以要自己带长度 */
static CabbirdStringViewV1 view(const char* text) {
    CabbirdStringViewV1 result = {text, strlen(text)};
    return result;
}

static CabbirdStatusV1 status(unsigned int code) {
    CabbirdStatusV1 result = {code, 0, {0, 0}};
    return result;
}

/* 按 struct_size 判断某个入口在当前宿主里到底存不存在 */
#define HAS_FIELD(table, type, field) \
    ((table) != NULL && (table)->struct_size >= \
        offsetof(type, field) + sizeof((table)->field))

static CabbirdStatusV1 query(const CabbirdHostApiV1* host, const char* id,
                             unsigned int version, const void** table) {
    if (table != NULL) *table = NULL;
    if (host == NULL || table == NULL || host->query_service == NULL) {
        return status(CABBIRD_STATUS_V1_INVALID_ARGUMENT);
    }
    return host->query_service(host->host_context, view(id), version, table);
}

CABBIRD_SDK_EXPORT CabbirdStatusV1 CABBIRD_CALL CabbirdPluginEntryV1(
    CabbirdPluginDescriptorV1* descriptor) {
    if (descriptor == NULL || descriptor->struct_size < sizeof(*descriptor)) {
        return status(CABBIRD_STATUS_V1_INVALID_ARGUMENT);
    }
    *descriptor = (CabbirdPluginDescriptorV1){
        sizeof(*descriptor), CABBIRD_PLUGIN_API_V1_MAJOR, CABBIRD_PLUGIN_API_V1_MINOR,
        view("cabbird.example.my-plugin"), view("My Plugin"), view("You"), view("1.0.0"),
        load, start, stop, unload, update, draw};
    return status(CABBIRD_STATUS_V1_OK);
}
```

> [!NOTE]
> 字段顺序就是 ABI：`struct_size, api_major, api_minor, id, name, author, version,`
> `on_load, on_start, on_stop, on_unload, on_update, on_draw`。
> 不需要的回调填 `NULL`（示例里 `on_update` 就是 `NULL`）。
> 上面这段与 [`examples/hello_ui/plugin.c`](../../examples/hello_ui/plugin.c) 的写法一致。

`on_load` 的签名是 `(const CabbirdHostApiV1* host, void** plugin_context)`——
**`plugin_context` 是插件自己给的 out 参数**，不是描述符字段：

```c
static CabbirdStatusV1 CABBIRD_CALL load(const CabbirdHostApiV1* host, void** context) {
    const void* table = NULL;

    CabbirdStatusV1 result = query(host, CABBIRD_UI_SERVICE_V1_ID,
                                  CABBIRD_UI_SERVICE_V1_VERSION, &table);
    if (result.code != CABBIRD_STATUS_V1_OK) return result;
    g_ui = (const CabbirdUiServiceV1*)table;

    /* optional 服务：拿不到就降级，绝不因此失败 */
    table = NULL;
    result = query(host, CABBIRD_CONFIG_SERVICE_V1_ID,
                   CABBIRD_CONFIG_SERVICE_V1_VERSION, &table);
    if (result.code == CABBIRD_STATUS_V1_OK) g_config = (const CabbirdConfigServiceV1*)table;

    *context = &g_context;          /* 每个回调都会原样收到这个指针 */
    return status(CABBIRD_STATUS_V1_OK);
}
```

`query_service` 失败时 `*service` 为 null，状态码是 `UNAVAILABLE`（这个 build 没有）或
`NOT_FOUND`（从来没有过这个 id）。**`UNAVAILABLE` 是正常结果，不是崩溃条件。**

从 `on_load` 返回非 `OK` 会**中止加载**，而且 `on_unload` 不会被调用。
所以只有真正「没有它我就没意义」的**必需**服务才该这么做
（`hello_ui` 对 `cabbird.ui` 就是这么做的）；可选服务缺失请降级。

### 域纪律

| 回调 | 域 | 你能做 | 你不能做 |
|---|---|---|---|
| `on_load` / `on_start` / `on_stop` / `on_unload` | Lifecycle | 查服务、注册/撤销资源 | 调 IL2CPP |
| `on_update(ctx, dt)` | **Game** | IL2CPP、玩家/实体读取、排队写 | 建线程、阻塞、做重活 |
| `on_draw(ctx, ui)` | **Render** | 只用 `ui` 画 | 分配、加锁、IO、IL2CPP、加载 DLL |

## 5. 画一个窗口

推荐用 `cabbird.window` 注册窗口（宿主会持久化位置与尺寸），在 `on_draw` 里只负责画内容：

```c
/* on_start：注册窗口（句柄 scope 拥有） */
CabbirdWindowSpecV1 spec = {sizeof(spec)};
spec.id    = view("my-plugin.window");
spec.title = view("My Plugin");
spec.initial_width  = 320.0f;  spec.initial_height = 200.0f;
spec.default_open   = 1;
g_window->register_window(g_window->user, &spec, &context->window);

/* on_draw：只在窗口打开时画 */
static void CABBIRD_CALL draw(void* plugin_context, const CabbirdUiServiceV1* ui) {
    MyContext* context = (MyContext*)plugin_context;
    CabbirdWindowStateV1 state = {sizeof(state)};
    int visible = 0;

    if (ui == NULL || g_window == NULL || context->window.id == 0) return;
    if (g_window->state(g_window->user, context->window, &state).code
            != CABBIRD_STATUS_V1_OK || state.open == 0) {
        return;
    }
    if (g_window->begin(g_window->user, context->window, 0, &visible).code
            != CABBIRD_STATUS_V1_OK) {
        return;
    }
    if (visible != 0) {
        ui->text(ui->user, view("hello"));
        if (ui->button(ui->user, view("Do it"), 0.0f, 0.0f) != 0) {
            /* 按钮被按下：把工作排给调度器，不要在渲染域里做事 */
        }
    }
    (void)g_window->end(g_window->user, context->window);
}
```

也可以直接 `ui->begin_window(ui->user, view("Title"), &open, 0)` / `ui->end_window(ui->user)`
（签名见 `ui.h`），但那样位置与尺寸要自己管。

- 控件全部由**宿主**绘制，插件改不了样式（结构上就没有这种入口）；
- 绘制回调**只在当前 `on_draw` 内**有效；
- 需要世界空间绘制用 `draw_entity_bbox` / `draw_entity_box3d` / `draw_entity_label`，
  相机是 Unity 约定（左手系、+Z 前、正 x 俯角向下）——见
  [UI 服务](../api-reference/ui-services.md#4-世界空间绘制与坐标约定)；
- 按钮里**不要**直接做重活：渲染域里不能做 IO、加锁、加载 DLL。

## 6. 持久化设置

用 `cabbird.config`，**不要**自己写文件：

```
register_schema(schema_id, version, schema_json, &handle)   /* 每一代都要重新注册 */
read(...)          /* NOT_FOUND = 还没有保存过，用默认值 */
write_atomic(...)  /* 校验后原子替换 */
```

`examples/reliable_config` 演示了完整模式：UI 改动只把状态标脏，`on_stop` 时提交一次。
把写盘放在绘制回调里是**错误**的（Render 域禁止 IO）。

## 7. 热重载

宿主支持插件热重载，这是插件开发效率的核心：**换一个 DLL 很便宜，重启游戏很贵。**

热重载的正确写法：

- 所有资源登记在代次下（`cabbird.window` / `cabbird.font` / `cabbird.texture` / hook / 订阅 / 任务），
  宿主会替你撤销；
- 不要跨代缓存句柄：换代后句柄一律 `STALE_GENERATION`；
- 不要起自己的线程并让它活过 `on_unload`；要周期工作就用 `cabbird.scheduler`；
- 静态状态会随 DLL 卸载消失，需要跨代保留就写进 `cabbird.config` / `cabbird.storage`。

## 8. 调试

| 手段 | 做法 |
|---|---|
| 不碰游戏看界面 | `cabbird-platform-preview.exe --frames=120`（`apps/render_fixture/`） |
| 日志 | `cabbird.core` 的 `log`；宿主日志见[故障排查](../user-guide/troubleshooting.md#1-先看日志) |
| 插件没被列出 | 看 manifest 诊断码（`plugin_manifest.hpp`）与目录结构 |
| 服务拿不到 | 打印 `query_service` 的状态码；`UNAVAILABLE` 通常意味着 Profile 没绑上。宿主侧对照 `诊断 → 开发者 → Unity 兼容性`（级别 + 每个方法的绑定结果） |
| 崩溃 | 先确认没有在 Render / Worker 域碰 IL2CPP；再确认 hook 里有 `begin_callback` / `end_callback` 租约 |

> [!WARNING]
> 仓库内没有离线自检可执行文件；插件的验证方式是 `cabbird-platform-preview.exe`
> 加上在游戏里的实际运行。

## 9. 参考示例

| 示例 | 语言 | 演示 |
|---|---|---|
| [`hello_ui`](../../examples/hello_ui/plugin.c) | C11 | 生命周期、窗口/字体/纹理/输入、generation handle |
| [`tick_counter`](../../examples/tick_counter/plugin.cpp) | C++20 | 游戏更新回调 + 轻量 C++ wrapper |
| [`reliable_config`](../../examples/reliable_config/plugin.cpp) | C++20 | Config ABI + 脏标记 + `on_stop` 提交 |
| [`unity_entity_inspector`](../../examples/unity_entity_inspector/plugin.cpp) | C++20 | 实体源（`cabbird.unity.entities`）+ 本地玩家快照、自报两个可选服务可用性 |

随包内建插件的源码同样可读：`plugins/player_coords/`（最简）、`plugins/entity_overlay/`（最全）、
`plugins/damage_replay/`（hook + 本地化 + 存储）。
