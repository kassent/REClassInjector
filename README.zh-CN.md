# ClassInjector 开发者指南

[English](README.md) | [简体中文](README.zh-CN.md) | [Provider SDK](include/ci/Provider.hpp)
[Build from source / 从源码构建](BUILDING.md)

**更早接入游戏初始化，让 Mod 拥有自己的组件、数据和原生行为。**

ClassInjector（下文简称 CI）是面向 RE Engine Mod 开发者的**早期插件 loader 与运行时类型扩展基础设施**。它提供两项互相配合的能力：一是将插件的接入时机提前到引擎 TDB 和原生方法入口就绪后、后续关键初始化尚未执行的阶段；二是允许开发者定义自己的托管 class、组件和 UserData，让它们参与引擎原生的创建、调用、资源加载和生命周期。

开发者通过 TOML 描述类型、字段和方法，通过 C++ 提供方法实现。CI 在游戏启动过程中完成注册，并通过早期就绪回调把控制权交给插件。这样，Mod 不必总是等游戏完成初始化后再修改结果，也不必总是依靠 hook 将新需求塞进已有逻辑；可以在适当的初始化阶段接入，并围绕自己的类型组织功能。

适用环境：**Monster Hunter Wilds、Windows x64、TDB81**。

## 目录

- [1. 它解决什么问题](#1-它解决什么问题)
- [2. 能力与职责边界](#2-能力与职责边界)
- [3. 安装与文件组织](#3-安装与文件组织)
- [4. 快速上手：一个计数器类型](#4-快速上手一个计数器类型)
- [5. TOML 声明参考](#5-toml-声明参考)
- [6. 继承、接口与组件](#6-继承接口与组件)
- [7. 字段、引用与资源](#7-字段引用与资源)
- [8. 扩展已有类型与枚举](#8-扩展已有类型与枚举)
- [9. 泛型与数组](#9-泛型与数组)
- [10. 初始化与 Provider SDK API](#10-初始化与-provider-sdk-api)
- [11. 兼容性、诊断与交付](#11-兼容性诊断与交付)
- [12. 常见问题](#12-常见问题)

## 1. 它解决什么问题

### 1.1 第一个问题：普通插件的业务接入时机偏晚

对于需要依赖 TDB 查询类型、字段和方法的插件，“DLL 已被加载”和“已经能基于类型系统执行业务”是两件事。常规 REFramework 插件需要等待 REFramework 成功取得并准备好 TDB，才能依赖其 SDK 完成这些操作。

但游戏不会以 Mod 的初始化需求为界划分所有工作。在普通插件能够使用 TDB 开始接入时，一些游戏逻辑、静态数据或初始化结果可能已经生成。例如：

- 某个类型列表已经被读取，并据此创建了后续使用的缓存。
- 一张业务表已经根据初始条件完成构造，之后只读缓存，不再查询原始来源。
- 一个系统已经完成首次初始化，决定了本次进程要创建哪些对象、注册哪些条目。

此时才安装 hook，可能只能影响**之后的调用**，无法改变已经发生的首次初始化。作者往往还需要回填旧数据、重建缓存，或寻找更多下游入口补救。问题不一定是 hook 能力不足，而是接入点已经错过。

### 1.2 CI 也是一个更早介入的插件 loader

CI 不只是“添加 class 的工具”。它还负责提前加载参与 CI 流程的 provider，并提供一个**在类型系统可用后立即接入后续初始化**的通知入口：`ci_on_initialize`。

该通知发生于 **TDB 类型及原生方法入口就绪之后、全局静态构造执行之前**。插件因此可以先查询所需类型和方法、准备自己的入口、安装需要赶在初始化之前生效的 hook，再让引擎继续正常初始化。

与插件接入相关的启动阶段如下：

```text
引擎启动
  │
  ├─ CI 提前加载参与其流程的 provider，收集函数登记和类型声明
  │
  ├─ 引擎建立 TDB，完成类型与原生方法入口准备
  │    │
  │    ├─ CI：全部 provider 的 ci_on_initialize(context)
  │    │    查询入口、准备接入；向 UDE 提交依赖 TDB 的声明
  │    └─ CI：全部 provider 的 ci_on_post_initialize(context)
  │         UDE 关闭登记、校验声明并安装解码 hook
  │
  ├─ 继续全局静态构造等初始化
  │    类型列表、静态表和缓存等在各自的初始化流程中建立
  │
  └─ 常规 REFramework 插件进入可依赖其 SDK/TDB 的业务阶段
       此时再接入，部分首次初始化结果可能已经存在
```

**示例：首次生成的类型缓存。** 假设一个系统只在启动时根据类型列表建立一次缓存。普通插件在缓存生成后改变列表来源，旧缓存不会因此自动重建；CI 插件可以在早期回调中接入列表来源，让该系统首次构造缓存时就读取扩展后的输入。这样改变的是“初始化如何发生”，而不只是“初始化结束后如何修补”。列表与缓存的具体扩展由插件实现。

这项能力对**只需要提前介入初始化、不需要新增类型**的插件也有价值。CI 可以单独作为早期 loader 使用，具体接入方式见第 10 节。

需要区分的是：CI 提前的是**参与 CI 流程的插件加载和回调时机**，并不是让 REFramework SDK 自身提前初始化。早期阶段使用 CI 提供的 ProviderContext；需要 REFramework SDK 的逻辑，仍放在该 SDK 真正可用之后。

### 1.3 第二个问题：新增功能不应永远只是对原版逻辑打补丁

一般的脚本或 hook 可以修改字段、拦截方法，也可以在脚本表或 C++ 容器中保存额外状态。但这些额外数据并不因此成为引擎认识的类型：原生代码无法仅凭一个类型名称将其当作组件创建，资源文件也不会自动知道如何实例化它。

ClassInjector 提供另一种扩展方式：

```text
TOML：声明类型、字段、继承关系和方法签名
                     +
C++ DLL：登记对应的方法实现
                     ↓
游戏启动时注册到引擎类型系统
                     ↓
原生代码 / REFramework / 配套资源按类型契约使用
```

例如，你可以定义自己的配置对象、行为参数或组件，而不是借用一个原版类型的名称、复用无关字段，或始终在外部维护“对象地址 → 额外状态”的映射。

### 1.4 接近 Unity 组件化开发的原生游戏创作体验

熟悉 Unity 等引擎的开发者，通常会把一个功能拆成“组件负责行为、数据对象负责参数、场景或 Prefab 负责组合”，再由引擎驱动它们的生命周期。CI 希望为 RE Engine Mod 创作者提供接近这种方式的开发体验：**不只是修改游戏已有的代码路径，而是定义自己的内容，让引擎以原生机制使用它。**

对应到 RE Engine，这种组件化、数据驱动的创作方式可以这样组织：

| 创作职责 | 使用 CI 后的组织方式 | 引擎负责的部分 |
|---|---|---|
| 定义行为 | 创建自己的 `app.AppBehavior` 派生组件 | 挂载、启停，以及匹配的更新和销毁生命周期 |
| 定义参数 | 创建自己的 `via.UserData` 派生类型 | 按类型声明读取配套资源中的参数 |
| 组合功能 | 将组件与数据引用放入匹配的 PFB/对象关系中 | 按正确资源与组件结构实例化和关联对象 |
| 管理运行态 | 将计时、状态和对象引用放在自己的实例字段中 | 识别对象布局与托管引用关系 |
| 扩展调用契约 | 实现已有接口或覆盖允许扩展的虚方法 | 沿原生分派路径调用相应实现 |

例如，为场景物件添加一种自定义周期行为时，可以定义：

```text
Example.PulseSettings : via.UserData
  ├─ IntervalSeconds   周期间隔，来自资源
  └─ EnabledAtStart    初始开关，来自资源

Example.PulseBehavior : app.AppBehavior
  ├─ Settings          对配置对象的引用
  ├─ Elapsed           当前实例的运行时计时
  ├─ doStart           读取已就绪配置，建立初始状态
  ├─ update            推进该实例自己的行为
  └─ doOnDestroy       清理该行为自行申请的外部资源或订阅

配套 PFB / 正规组件创建路径
  └─ 将组件挂到实际 GameObject，由引擎驱动生命周期
```

实现这组类型时，为组件提供行为代码，为配置提供匹配的资源，并通过组件创建路径完成挂载；配置引用与外部资源在相应生命周期中持有和释放。

这种方式与“hook 某个原版更新函数，遍历外部对象表，再按对象地址查状态”有明显不同：**功能属于自己的组件，参数属于自己的数据，运行态属于自己的对象，调度尽量归引擎。** 同一组件可以被多个对象复用，每个实例拥有独立状态；参数变化可以通过数据资源表达，不必都变成硬编码分支。

因此，CI 的目标不是简单增加更多修改点，而是让 Mod 逐步具备“用引擎创作新功能”的表达能力。hook 仍可用于连接尚未开放的业务入口或修改既有规则，但不必成为每一项新增功能的主体。

### 1.5 两项能力如何配合

早期 loader 解决“**何时接入**”，类型扩展解决“**以什么形式接入**”。两者可以独立使用，也可以组合：

- 只需要影响首次初始化：使用早期回调，不必为此定义新 class。
- 只需要独立组件或数据对象：定义类型，通过匹配的原生创建和生命周期使用。
- 需要让新类型参加启动注册或首次缓存构造：先注册类型，再在早期回调中接入相应业务入口，让后续初始化看到新增内容。

CI 提供时机与类型能力；具体业务系统如何接收新增内容，由扩展作者按该系统的契约接通。

### 1.6 对开发者的意义

- **更早的业务接入点**：在关键初始化之前准备逻辑，减少事后修补初始化结果的需要。
- **清晰的类型身份**：自定义对象拥有独立名称、父类关系、字段和方法声明。
- **对象与状态结合**：额外状态可以成为对象字段，其托管引用关系由引擎识别。
- **复用原生扩展点**：通过继承、虚方法覆盖和已有接口，将实现接入匹配的原生调用路径。
- **支持数据驱动设计**：新类型可声明资源序列化字段，让参数保存在 USER/PFB 等配套资源中，行为留在代码里。
- **便于跨插件协作**：其他扩展可以通过约定的类型名称和签名访问对象，不必依赖实现 DLL 的私有 C++ 类。

这些能力同时改善接入时机和功能组织方式。扩展作者可以优先沿原生机制设计对象创建、引用持有和业务调用，将组件调度交给引擎。

## 2. 能力与职责边界

| 能力 | 可以实现的效果 | 使用条件或边界 |
|---|---|---|
| 早期插件加载与通知 | 在 TDB/原生入口就绪后、后续关键初始化之前接入逻辑 | 使用 CI 配对加载与 `ci_on_initialize`；不等于 RF SDK 已就绪 |
| 新增托管 class | 独立数据对象、参数类型、行为实现 | 基类与实际调用路径须兼容 |
| 字段 | 标量、托管引用、已有 ValueType 内嵌字段 | 不等于可以新增任意 struct 定义 |
| 方法与构造 | 实例方法、构造重载、虚方法、覆盖和抽象方法 | 方法实现使用 C++ binding；签名须满足引擎调用约定 |
| 已有接口 | 让新类型参与匹配的接口消费路径 | 不包含新增接口定义或默认接口方法 |
| 自定义组件 | 继承 `app.AppBehavior`，接入原生组件生命周期 | 由 GameObject/组件系统创建和销毁 |
| RSZ 字段声明 | 让新类型的指定字段从资源中读取 | 资源须另行制作并匹配类型声明 |
| 已有 class 字段扩展 | 给符合条件的原版类型追加运行时状态 | 限纯托管具体 class，详见第 8 节 |
| 已有 Int32 枚举扩展 | 改成员名、追加成员 | 不改变已有成员数值，不新建枚举类型 |
| 显式闭合泛型 | 在启动时注册需要的具体泛型类型 | 不是完整 CLR 泛型系统 |
| 单维 class 引用数组 | 在字段和签名中使用新增类型的数组 | 新增数组形状有限，详见第 9 节 |
| ProviderContext 基础服务 | 查询类型/方法/字段，访问对象及请求资源 | 各入口有独立的生命周期和所有权要求 |

**开发职责分工：** CI 负责插件加载、类型注册及公开基础服务；扩展作者负责行为代码、配套资源和业务接入。例如，自定义 Action 需要相应的资源引用及创建入口，自定义 UserData 需要匹配的 USER 文件，供其他系统使用的类型需要接入对应工厂或业务注册表。

类型声明和 C++ 实现随游戏启动加载；更新或移除扩展时完整重启游戏。Lua 可访问注册后的类型，方法实现使用 C++ provider。

## 3. 安装与文件组织

### 3.1 运行时与扩展 DLL

ClassInjector 运行时为游戏根目录的 **`hid.dll`**。开发者自己的实现 DLL 在本文中称为 **provider**；它可以提供新类型，也可以仅使用早期加载与就绪回调。

示例目录结构：

```text
游戏根目录/
├─ MonsterHunterWilds.exe
├─ hid.dll                         # 集成 ClassInjector 的运行时
└─ reframework/
   └─ plugins/
      ├─ CounterPlugin.dll        # provider 实现
      ├─ CounterPlugin.toml       # 与 DLL 同目录、同主文件名
      ├─ BehaviorPlugin.dll       # 也可兼任 REFramework 插件
      └─ BehaviorPlugin.toml
```

使用发布包时保留其目录结构。若游戏根目录已经有其他来源的 `hid.dll`，应先确认两者的加载方案兼容，不应直接混装为同一个文件。

### 3.2 TOML 与 DLL 的配对规则

- provider DLL 与 TOML 统一安装在游戏根目录的 `reframework/plugins/` 顶层。
- **TOML 与 DLL 必须同目录、同主文件名**，例如 `CounterPlugin.toml` 对应 `CounterPlugin.dll`。
- 缺少同名 DLL 的 TOML 不参与加载；只有 DLL 而没有同名 TOML，也不会作为 CI provider 被扫描。
- 一份 provider 可以声明多个类型，每个 provider 安装一份 DLL 与同名 TOML。
- 需要 REFramework SDK 的 provider 应安排好自身的标准插件初始化；ClassInjector 不代为初始化 SDK。

### 3.3 修改后的生效方式

声明在游戏启动时读取。更新 TOML、类型布局或 provider DLL 后，**完整退出游戏再启动**；重载 Lua 脚本不会重新注册类型。

## 4. 快速上手：一个计数器类型

本例定义 `Example.Counter`，包含一个 Int32 字段 `Value` 和一个 `Add` 方法，并通过 REFramework Lua 创建实例、调用方法和读取结果。

### 4.1 声明 `CounterPlugin.toml`

```toml
schema_version = 1

[[types]]
name = "Example.Counter"
base = "System.Object"
fields = [{ name = "Value", type = "System.Int32" }]

[[types.methods]]
name = "Add"
kind = "instance"
return_type = "System.Int32"
parameters = [{ name = "delta", type = "System.Int32" }]
binding = { function = "Example.Counter.Add(System.Int32)", version = 1 }
```

这里有两种名称：

- `name = "Add"` 是引擎方法名，供方法查询、反射及调用使用。
- `binding.function` 是本 provider 内的方法实现登记键，须与 C++ 登记完全一致。建议使用“完整类型名 + 方法签名”，便于检查和维护。

### 4.2 编写 provider 方法

使用随开发接口提供的 `ci/Provider.hpp`。本例仅使用 ProviderContext，不依赖 REFramework SDK：

```cpp
#include <ci/Provider.hpp>
#include <ci/API.hpp>
#include <cstdint>
#include <cstdio>

namespace Example::Counter {
    static const ci::ProviderContext* g_context{}; // 借用；有效期与运行时相同。

    // self 是 Example.Counter 托管对象，不是本机 C++ Counter 实例。
    static int32_t Add(void* vmctx, void* self, int32_t delta) noexcept {
        (void)vmctx;
        auto* value = g_context
            ? ci::API::ManagedObject{g_context->sdk, self}.get_field<int32_t>("Value")
            : nullptr;
        if (!value) {
            std::fputs("CounterPlugin: Value field unavailable\n", stderr);
            return 0;
        }
        *value += delta;
        return *value;
    }
}

extern "C" __declspec(dllexport)
void ci_on_initialize(const ci::ProviderContext* context) noexcept {
    if (!context || !context->sdk || !context->sdk->managed_object || !context->sdk->field) {
        std::fputs("CounterPlugin: required ProviderContext service unavailable\n", stderr);
        return;
    }
    Example::Counter::g_context = context;
}

// 每个实现 DLL 只在一个 .cpp 中使用一次。
CI_REGISTER_PROVIDER(registration) {
    registration.register_func(
        "Example.Counter.Add(System.Int32)", &Example::Counter::Add, 1);
}
```

方法的两个隐式参数是 `vmctx` 和 `self`，**不写入 TOML 的 `parameters`**。业务函数不需要逐个导出；`CI_REGISTER_PROVIDER` 提供固定的登记入口。

本例省略构造声明，由 CI 提供默认无参构造并转发父类无参构造。新对象的 `Value` 初值为 0；显式构造的规则见第 5 节。

### 4.3 安装并运行

将 `CounterPlugin.dll` 与 `CounterPlugin.toml` 一同放入 `reframework/plugins/`，然后完整重启游戏。

将下面的脚本保存为 `reframework/autorun/CounterExample.lua`。脚本查询类型后，在游戏主线程的 `PlayerManager.update()` 原生回调中执行一次计数器操作，使对象创建和方法调用处于原生托管调用作用域内。`UpdateBehavior` 回调只记录主线程上下文，用于选择对应的原生调用。

```lua
local counter_type = assert(sdk.find_type_definition("Example.Counter"))
assert(counter_type:get_method("Add(System.Int32)"))
local player_manager = assert(sdk.find_type_definition("app.PlayerManager"))
local update = assert(player_manager:get_method("update()"))
local pending = true
local main_context

re.on_application_entry("UpdateBehavior", function()
    if pending then main_context = sdk.to_int64(sdk.get_thread_context()) end
end)

sdk.hook(update, function(args)
    if not pending or not main_context or sdk.to_int64(args[1]) ~= main_context then
        return
    end
    pending = false

    local object = assert(sdk.create_instance("Example.Counter"))
    object:add_ref()
    local first = object:call("Add(System.Int32)", 3)
    local second = object:call("Add(System.Int32)", 4)
    local value = object:get_field("Value")
    object:release()
    assert(first == 3 and second == 7 and value == 7)
    log.info("CounterExample: first=3, second=7, Value=7")
end, nil)
```

完整重启游戏后，在 REFramework 日志中查找 `CounterExample: first=3, second=7, Value=7`。连续两次调用作用于同一个实例，`Value` 从 0 累加至 3，再累加至 7。

该脚本每次加载只执行一次计数器操作。完成示例后移除 `CounterExample.lua` 并重启游戏，即可移除示例 hook。正式功能应在自己的组件或其他合适的原生生命周期中使用计数器。

`sdk.create_instance` 使用的创建路径与 `ProviderContext.CreateInstance` 的契约不同：后者只分配，不隐式调用构造，详见第 5.3 节。

## 5. TOML 声明参考

### 5.1 类型、字段与方法

| 配置项 | 含义 | 默认值或要求 |
|---|---|---|
| `schema_version` | 声明格式版本 | 有类型/枚举声明时使用 `1` |
| `types.name` | 完整类型名 | 必填；建议使用扩展专属命名空间 |
| `types.base` | 基类完整名称 | 默认 `System.Object`；新增类的基类须可继承 |
| `types.abstract` | 声明抽象类 | 默认 `false` |
| `types.interfaces` | 实现的已有接口全名数组 | 按需填写 |
| `types.fields` | 字段列表 | 每项包含 `name`、`type` |
| `fields.serialize` | 是否从资源反序列化该字段 | 默认 `false`；仅用于符合条件的新类型字段 |
| `methods.name` | 方法名称 | 构造名称固定为 `.ctor` |
| `methods.kind` | 方法种类 | 默认 `instance`，见下表 |
| `methods.return_type` | 托管返回类型 | 默认 `System.Void` |
| `methods.parameters` | 显式参数列表 | 每项的 `name`、`type` 必填 |
| `parameters.modifier` | 引用传参方向 | 省略为按值；可填 `ref`、`out` |
| `methods.override_of` | 覆盖目标的声明类型 | 按需指定父类或接口来源 |
| `methods.binding` | 原生实现登记键与版本 | 具体方法需要；抽象方法无需提供 |

字段和参数使用托管完整类型名，如 `System.Int32`，而不是 `int`。方法签名由名称、返回类型和参数列表分开声明，不要把 `Add(System.Int32)` 整段写入 `methods.name`。

### 5.2 方法种类

| `kind` | 用途 |
|---|---|
| `constructor` | 构造函数，名称为 `.ctor`，返回 `System.Void` |
| `instance` | 普通实例方法 |
| `virtual` | 可被派生类型覆盖的虚方法 |
| `override` | 覆盖匹配的父类/接口方法 |
| `abstract` | 只声明契约，不提供实现 |

具体类应实现其继承链上仍未实现的抽象方法。声明 `override` 时，参数类型、方向及返回类型必须匹配目标，不是仅凭同名替换。

### 5.3 构造规则

- **完全省略构造**：生成默认 `.ctor()`，转发父类无参构造；父类需要有适用的默认构造路径。
- **显式声明构造**：进入你的 C++ 实现。由实现自行调用正确的父构造并初始化业务字段；CI 不额外补调父构造，也不在每次构造调用前重新清零字段。
- **带参构造和构造重载**：通过 `parameters` 区分，每个具体构造都需要绑定实现。
- **只声明带参构造**：不会额外生成无参构造。创建端应分配对象后调用匹配的构造，或使用与该类型兼容的原生创建路径。
- 分配、构造、持有是三个不同操作。不要对已构造对象无意重复调用构造函数。

### 5.4 参数与原生签名

以第 4 节 Counter.Add 的完整原生签名为例：

```cpp
#include <cstdint>

int32_t Add(void* vmContext, void* self, int32_t delta) noexcept;
```

`self` 可以在使用 SDK 的实现中声明为对应的托管对象指针类型。它不指向普通 C++ 业务类；类型声明也不从 C++ 函数指针自动推导。

标量调用支持 Boolean、有符号/无符号整数、Char、Single、Double、指针宽度整数、已有枚举以及托管引用；也支持多参数及 `ref/out`。复杂 ValueType 的按值传递、结构体返回等须匹配相应类型的引擎原生 ABI 和可用调用路径。

`ref/out` 示例：

```toml
[[types.methods]]
name = "TryGetValue"
return_type = "System.Boolean"
parameters = [{ name = "value", type = "System.Int32", modifier = "out" }]
binding = { function = "Example.Counter.TryGetValue(System.Int32)", version = 1 }
```

对应 C++ 的显式参数是 `int32_t* value`。托管对象的 `ref/out` 参数是对象指针槽，例如 `void**`，不是 `void*`；`out` 实现应在各条返回路径写入输出。

方法查询写 `TryGetValue(System.Int32)`，不在元素类型后加 `&`。`ref` 与 `out` 的方向差异不用于声明两个独立重载。`in` 参数、引用返回、静态成员注册不在当前公开支持范围。

### 5.5 绑定版本

`binding.version` 与 `.register_func(..., version)` 的数值必须完全相等。它是**单个方法的契约版本**，不是 TOML 格式版本，也不是 DLL 的整体版本。

修改参数、返回类型或调用契约时，应同步调整声明、实现和版本，并核对 C++ 签名与托管声明的对应关系。

## 6. 继承、接口与组件

### 6.1 普通继承与覆盖

新类型可以继承已有的可继承 class，也可以继承其他 provider 声明的新 class。完整类型名应保持唯一，并显式交付所依赖的 provider。

父类定义 `virtual` 方法后，派生类使用 `override` 和匹配签名提供实现。调用效果还取决于入口：**原生虚调用按对象分派，而查询某个声明的方法入口后直接调用，不等于进行虚分派。**

### 6.2 已有接口

通过 `interfaces = ["接口完整名称"]` 声明接口，并实现匹配的方法。显式接口实现的方法名使用完整接口前缀，例如 `System.IDisposable.Dispose`。

接口接入需要匹配实际调用方的方法契约，并通过该调用方检查方法执行结果。接口声明限 TDB 中已有的接口，不包含新接口定义、默认接口方法或通用泛型变型。

### 6.3 自定义 AppBehavior 组件

使用 `base = "app.AppBehavior"` 声明组件，通过原生 GameObject 的 `createComponent(System.Type)` 等正规组件创建路径挂载。

| 需求 | 对应覆盖方法 |
|---|---|
| 唤醒 | `doAwake` |
| 启动 | `doStart` |
| 销毁通知 | `doOnDestroy` |
| 每帧更新 | `update` |
| 延迟更新 | `lateUpdate` |

这些方法使用 `kind = "override"`。组件通过 GameObject 创建和挂载后，由引擎调用生命周期方法，并通过组件系统管理启停与销毁。`sdk.create_instance` 用于普通托管对象的创建，不替代组件挂载。

自定义 `app.AppBehavior` 可以追加字段并覆盖上述生命周期方法。选择其他 native-backed 基类时，还应核对该基类的创建方式、原生资源所有权及销毁要求。

## 7. 字段、引用与资源

### 7.1 字段访问与所有权

ProviderContext 字段访问返回的是字段槽地址：

```cpp
auto* value = ci::API::ManagedObject{context->sdk, object}.get_field<int32_t>("Value"); // int32_t*
auto** peer = ci::API::ManagedObject{context->sdk, object}.get_field<void*>("Peer");  // void**
```

模板参数由调用方保证正确，不自动校验字段类型。先确认字段契约，再读写数据；字段地址仅在所属对象有效期间使用。

- POD 标量可以按准确类型写入对应槽。
- 托管引用字段使用 `ci::API{*context->sdk}.objects().assign_reference(owner, field, value)`，而不是裸赋值绕过引用管理；field 是引用槽 `void**`，检查返回的成功状态。
- 需要跨回调保存对象时，按取得对象的接口契约持有和释放；裸指针、局部 Lua/C++ 变量不等于永久持有。
- 清理外部持有的引用时，确保相关回调已经停止使用对象，再按取得引用的契约释放。

### 7.2 内嵌 ValueType

字段可以使用 TDB 中已有的 ValueType，例如 `via.vec3` 和 `System.Guid`。它们保存的是内嵌 payload，不是装箱对象指针。

整体复制时，使用真实的 payload 长度和相应字段写入方式。`get_valuetype_size` 返回已闭合值类型的 payload 大小；对非值类型或无布局返回 0。包装对象的大小与内嵌 payload 大小不同；含托管引用的结构体还需要遵循引用管理契约。

### 7.3 资源序列化字段

新类型可以将指定字段标记为可反序列化：

```toml
[[types]]
name = "Example.Settings"
base = "via.UserData"
fields = [
    { name = "Limit", type = "System.Int32", serialize = true },
    { name = "RuntimeCounter", type = "System.Int32" }
]
```

`Limit` 从配套资源读取，`RuntimeCounter` 仅用于运行时。声明本身不会生成资源文件。

制作 USER/PFB 时，RSZ 类型信息使用类型名称 hash 和对应的引擎 TypeInfo CRC，字段数据按序列化声明编码。运行时 type ID、名称 hash 和 TypeInfo CRC 是不同标识，应分别按资源格式填写。字段声明变更后，同步更新资源及编辑工具使用的 schema。

资源请求可能异步完成，取得 USER 对象不意味着字段已经解码。PFB 自定义组件还需要正确的 GameObject/组件关系；普通 USER 对象不会自行挂载到场景。

## 8. 扩展已有类型与枚举

### 8.1 给原版 class 追加字段

使用 `extend = true`，只声明新增字段：

```toml
schema_version = 1

[[types]]
name = "TARGET"
extend = true
fields = [{ name = "ExampleExtraCounter", type = "System.Int32" }]
```

将 `TARGET` 替换为实际目标的完整类型名。目标应是原 TDB 中的**纯托管具体 class**，并满足以下条件之一：

- 已经是 sealed 类型。
- 不是 sealed，但没有原 TDB 已有的派生类。

扩展保留原有字段和方法，只追加运行时字段；不用于更换父类、增加方法或接口。native-backed、抽象类、接口、数组、字符串、ValueType 和开放泛型不适用。追加字段不使用 `serialize = true`，也不自动写入存档。

多个扩展作用于同一个目标时，避免重名字段，使用扩展专属前缀。不要将追加字段的偏移硬编码给其他插件；通过字段名称或布局查询访问。

### 8.2 编辑已有 Int32 枚举

示例使用原有 `System.DayOfWeek`，展示改名与追加；实际项目应选择自己的业务目标，并避免占用其他扩展使用的名称和值。

```toml
schema_version = 1

[[enums]]
name = "System.DayOfWeek"
renames = [
    { fromName = "Tuesday", to = "Example_Tuesday" },
    { fromValue = 4, to = "Example_Thursday" }
]
appends = [{ name = "Example_ExtraDay", value = 7 }]
```

- 改名每项使用 `fromName` 或 `fromValue` 之一；它们只选择源成员，**不修改原数值**。
- 追加使用 `name` 与 Int32 `value`，重名报错；不覆盖原成员。
- 同值别名不会替换原 canonical 名称；按值查名仍取首个匹配成员。
- 单条声明先执行改名，再执行追加。变更后完整重启。
- 也可在 `CI_REGISTER_PROVIDER` 中使用 `registration.register_enum(...)` 提供动态登记。

追加业务 ID 时，同步为使用该 ID 的表、资源或工厂提供对应条目。

## 9. 泛型与数组

### 9.1 启动时显式闭合泛型

支持两种声明方式，二选一：

- `generic_definition`：明确声明需要的闭合类型及其具体字段/方法。
- `generic_template`：使用已有闭合 class 的声明形状，再按新的实参适配。

配合 `generic_arguments` 列出实际类型。类型全名须使用正确的反引号 arity 和实参，例如 ``Example.Box`1<Example.Counter>``。

泛型实例在启动时显式声明，实参限支持的闭合托管引用、`System.Int32` 或已有 Int32 底层枚举。具体非构造方法需要提供对应的 C++ 实现。完整开放泛型成员、约束系统及运行时任意 `MakeGenericType` 不在支持范围内。

### 9.2 新类型的数组

声明字段或方法类型为 `Example.Counter[]` 时，缺失的单维 class 引用数组类型会自动补齐，无需另写一个 `[[types]]` 数组声明。已有数组类型直接复用。

新增数组类型限单维 class 引用数组；新值类型数组、多维数组、jagged 数组、数组协变及完整泛型数组接口不在支持范围内。对于引擎已有的值数组类型，可通过 ProviderContext 的 `CreateArray` 分配实例。

## 10. 初始化与 Provider SDK API

### 10.1 公共头、登记与版本

| 头文件 | 用途 |
|---|---|
| `ci/Provider.h` | `ProviderContext`、`RegistrationAPI` 和枚举登记的 POD 协议 |
| `ci/Provider.hpp` | 本 DLL 内的 `Registration`、`EnumChanges` 和 `CI_REGISTER_PROVIDER` |
| `ci/API.h` | 六组 `SDKAPI` 函数表及不透明句柄 |
| `ci/API.hpp` | 本 DLL 内的 `ci::API` 值视图与 vector 包装 |

每个 provider DLL 只在一个 `.cpp` 中使用一次 `CI_REGISTER_PROVIDER(registration)`。宏生成 `ci_get_sdk_abi` 与 `ci_register_provider` 两个 C 导出。`RegistrationAPI` 和 `Registration` 只在本次登记调用中有效，不保存到后续回调；宿主同步复制输入，登记错误累计后使导出返回失败。

下面是第 8.2 节 TOML 枚举示例的 **C++ 替代写法**，不要同时登记两份相同变更：

```cpp
#include <ci/Provider.hpp>

CI_REGISTER_PROVIDER(registration) {
    registration.register_enum("System.DayOfWeek", {
        .renames = {
            ci::EnumRename::by_name("Tuesday", "Example_Tuesday"),
            ci::EnumRename::by_value(4, "Example_Thursday")
        },
        .additions = {{"Example_ExtraDay", 7}}
    });
}
```

未使用的 `renames` 或 `additions` 可以省略，不传空列表占位。`EnumChanges` 是调用侧参数，不将 initializer_list 或 STL 对象跨 DLL 保存。函数登记仍使用第 4 节的 `registration.register_func("完整绑定键", &实现函数, version)`。

当前 SDK ABI 为 **3**，`ProviderContext` 只含 `const ci::SDKAPI* sdk`。ABI、TOML 的 `schema_version`、单个函数的 `binding.version` 是三个不同版本。ABI1/2 与当前宿主不兼容，须协调重编宿主及该实例所有 provider；本次公共头和 snake_case 命名重组保持 ABI3 布局不变。

### 10.2 两阶段初始化与零类型 provider

```text
扫描同名 TOML/DLL，核 ci_get_sdk_abi，调用 ci_register_provider
  → 登记函数/枚举，核对声明与绑定（此阶段不访问 VM 或 RF SDK）
  → 发布类型，准备原生入口、自定义虚表与 ProviderContext
  → 全部 provider 的 ci_on_initialize
       查询元数据、安装早期接入；向 UDE 提交依赖 TDB 的 JSON
  → 全部 provider 的 ci_on_post_initialize
       UDE 关闭登记、验证声明并安装解码 hook
  → 继续游戏全局静态构造
```

两轮都是**全局静态构造之前**，不是每个 provider 的两个通知连续执行。不依赖 TOML 文件名排序，也没有通用依赖图。需要在 UDE 关闭登记前提交的调用方使用第一轮 `ci_on_initialize`，不要把提交放到自己的第二轮回调。

两个通知导出均可省略；只导出后置通知的 UDE 也会被通知。签名如下：

```cpp
extern "C" __declspec(dllexport)
void ci_on_initialize(const ci::ProviderContext* context) noexcept;

extern "C" __declspec(dllexport)
void ci_on_post_initialize(const ci::ProviderContext* context) noexcept;
```

不新增类型也可作为早期 loader 使用：放置空的同名 TOML（或只有注释），并提供无业务登记的宏体，另按需实现通知：

```cpp
#include <ci/Provider.hpp>

CI_REGISTER_PROVIDER(registration) {
    // 零类型 provider；此处不访问 VM 或 RF SDK。
}
```

早期通知不是 RF 插件初始化，也不表示游戏单例、业务缓存或资源已就绪。`ProviderContext`/SDK 表为宿主期借用数据，与只活到登记返回的 `RegistrationAPI` 不同。

### 10.3 查询、字段和所有权

公共查询不依赖 RF 初始化。以下函数只查询元数据与入口，不在早期通知中创建业务对象或执行构造：

```cpp
#include <ci/Provider.hpp>
#include <ci/API.hpp>

static bool HasObjectConstructor(const ci::ProviderContext& context) {
    const ci::API api{*context.sdk};
    const auto type = api.tdb().find_type("System.Object");
    if (!type)
        return false;
    const auto method = type.find_method(".ctor()");
    if (!method)
        return false;
    using FnConstructor = void (*)(void* vmContext, void* object);
    return method.get_function<FnConstructor>() != nullptr;
}
```

| 分组 | POD 表 / C++ 视图 | 契约 |
|---|---|---|
| 元数据 | `tdb`、`type_definition`、`field`、`method` / `api.tdb()` 及类型、字段、方法视图 | `find_method` 返回元数据；`get_function<准确签名>()` 取得原生入口 |
| 对象 | `managed_object` / `api.objects()` | `create_instance` 不执行 .ctor、不 retain；`retain/release` 管理调用方持有；引用槽用 `assign_reference` |
| 资源 | `resource_manager` / `api.resources()` | `create_userdata` 返回一份 owned 引用，但不承诺已解码；holder 的原生请求由其 finalizer 管理 |

- `Field::get_data<T>()` 返回 **`T&`**；`ManagedObject::get_field<T>()` 返回 **`T*`**。模板参数由调用方核对，二者不要混用。
- 实例字段按实际对象寻址；unboxed 的内嵌 payload 使用 `get_data_raw(payload, true)`。字段视图按值保存，字段地址只在对应对象有效期间使用；没有公共 `FieldLayout` 或 UDE 全局字段布局缓存。
- `get_init_data()` 返回元数据 initializer 的借用字节地址，缺失为 `nullptr`；它不是运行时 static storage，也不保证每个字段都有数据。枚举常量先核 literal、类型与宽度再读；数值 0 不是“无 initializer”。
- 底层 `get_fields/get_methods` 的 `out_size` 是**字节数**，`out_count` 是写入条目数；`ci::Result` 沿用 REF 整数约定（`ERROR_NONE == 0`）。C++ 包装在本 DLL 内管理 vector，不需自建 visitor。
- `create_action(vmContext, actionType, target, method)` 接受真实 Method 句柄，不传 C++ 函数地址代替或改写委托函数槽。
- API 不建立 VM 托管作用域。对象创建、方法执行沿真实原生生命周期使用；主线程、取得 VM context、裸 IPC 或 `UpdateBehavior` 本身都不是外层作用域证明。引用数组先初始化元素，再首次发布给持有者。
- 可选入口未解析时可能为 `nullptr`，按实际所需服务检查。查询成功不代表业务对象、资源或 RF SDK 可用。

方法查找仍遵循完整签名、逗号空格、父链和 ref/out 元素类型规则，不自动虚分派或执行构造。实现入口见 [Provider 公共头](include/ci/Provider.hpp)、[SDK 视图](include/ci/API.hpp)；完整 ABI 见 [SDK 函数表](include/ci/API.h)，提交示例见 [UDE 指南第 10 节](https://github.com/kassent/UserDataExtension/blob/main/README.zh-CN.md#10-插件开发者接口)。

## 11. 兼容性、诊断与交付

### 11.1 兼容性原则

- 使用配套发行的运行时与公共头文件，并明确 provider 所需的运行时版本。
- 类型声明更新时，同步检查 provider、依赖扩展、资源和编辑器 schema。
- 类型名称和方法签名是协作契约。避免使用原版已有名称或其他扩展的命名空间。
- 自定义 type ID 不宜作为持久化或跨版本协议；使用明确的类型名和业务 ID。
- 游戏更新后，检查类型注册、方法调用、资源加载和对象生命周期。

### 11.2 排查顺序

1. **检查文件配对**：同名 TOML/DLL 是否位于 `reframework/plugins/` 顶层，是否误放进子目录。
2. **检查启动日志**：运行时日志位于 `logs/HIDWrapper.log`；使用 Mod 管理器时，按其文件重定向设置查找。确认日志来自本次启动。
3. **检查声明与绑定**：查看 TOML 错误、缺失函数、版本错配和依赖失效信息。
4. **检查就绪阶段**：确认类型注册记录、原生入口就绪及 CI 初始化 通知完成。
5. **检查实际行为**：检查实例创建、方法返回、字段变化、业务调用及退出清理。

常见日志含义：

| 日志片段 | 应检查的方向 |
|---|---|
| `Type injection hooks ready` | 启动入口安装结果，后续应出现注册及就绪记录 |
| `Plugin declarations skipped` | 当前文件的声明解析失败，查看相邻错误原因 |
| `function missing` / `function version mismatch` | TOML 的 binding 与 DLL 登记不一致 |
| `Plugin skipped` | 该 provider 被跳过，依赖它的新类型也可能受影响 |
| `SDK ABI identity missing` / `SDK ABI mismatch` | DLL 缺少版本导出或 ABI 不匹配；核宿主与该实例全部 provider 的配套版本 |
| `Startup window missed` | 当前运行时未赶上启动注册窗口，检查安装版本及加载方式 |
| `Published metadata` / `Registered` | 已推进到类型发布/注册阶段，仍需确认入口就绪 |
| `CI provider initialization complete; injection ready=1` | CI 已完成注册及就绪通知，可继续排查具体业务或资源加载 |

单份 provider 绑定缺失或版本错配时，其整份声明会被跳过；依赖其中自定义类型的 provider 也会被跳过。按日志先解决上游声明或绑定错误，再检查依赖扩展。

### 11.3 发布自己的扩展

建议交付：

- 安装到 `reframework/plugins/` 的同名 provider DLL 与 TOML。
- 本扩展需要的资源、脚本及它们的正确目录结构。
- 所需 ClassInjector/REFramework 版本和其他依赖说明。
- 类型命名空间、对外方法契约、配置说明和升级要求。
- 支持的游戏版本、主要功能及已知限制。

发布前检查：**启动游戏 → 类型注册 → 创建对象并通过业务入口调用 → 检查字段和结果 → 销毁、切场景及正常退出**。涉及组件、资源异步加载或多人行为时，分别检查相应功能。发行包仅包含运行所需文件。

运行时是多个扩展共享的依赖。卸载单个扩展时只移除它自己的文件；保留其他扩展仍依赖的共享运行时。卸载及更新均在退出游戏后进行。

## 12. 常见问题

### 它和 REFramework 是什么关系？

CI 同时提供早期插件 loader 和类型扩展；REFramework 提供脚本、反射访问及插件能力。普通 RF 插件进入依赖 SDK/TDB 的业务阶段时，部分初始化可能已经结束；CI 提供更早的 CI 初始化 接入点，让插件赶在后续关键初始化之前准备逻辑。它不要求先使用新类型，也不提前初始化 RF SDK。使用 Lua 验证或 SDK 字段接口时，仍需要相应 REFramework 环境。

### 不新增 class，也能使用 CI 吗？

可以。提供同名 TOML、固定登记入口和 `ci_on_initialize`，即可将 CI 用作早期 loader，具体见第 10.2 节。早期查询与接入准备使用 ProviderContext。

### “接近 Unity 的创作体验”具体指什么？

指按“自己的组件 + 自己的数据 + 原生生命周期”组织 Mod。例如用自定义 AppBehavior 实现行为，用自定义 UserData 保存可配置参数，再由正规组件或 PFB 路径创建和组合它们。它不是 Unity API、编辑器或 C# 环境的移植；目标是让开发者充分依托 RE Engine 的原生机制创作新增功能，而不是始终围绕已有函数做外部修补。

### 它能替代所有 hook 吗？

hook 与原生扩展各有用途。新增独立行为时，可以优先使用自己的组件、数据和原生调用路径，避免把全部逻辑挂在原版更新函数上；需要调整原版规则或连接缺少开放入口的系统时，仍可使用适量 hook。CI 的早期 loader 也能让这类 hook 在关键初始化之前生效。目标是让 hook 成为必要的连接手段，而不是所有新增功能唯一的实现方式。

### 只改 TOML 就能实现新行为吗？

TOML 描述契约，不包含方法体。已有 C++ 实现可以被声明引用；新的方法逻辑仍需要 provider 提供。Lua 可以消费已注册类型，但不是当前方法 binding 的实现后端。

### 类型已查到，为什么还不出效果？

依次确认：对象是否通过正确路径创建、构造是否执行、是否进入真实业务注册表、资源是否引用正确类型、生命周期是否真的调用到目标方法。注册成功只是第一步。

### 新增字段会自动保存吗？

运行时字段不会自动进入存档。新类型的 `serialize = true` 用于配套资源的字段读取，也不等同于接通游戏存档写入。原版 class 的追加字段只用于运行时。

### 可以运行中更新类型或卸载 DLL 吗？

类型和 provider 的有效期是整个游戏进程。更新、移除或修改布局后完整重启游戏，不在现有对象仍存活时卸载实现。

### 能直接加载 C# DLL 吗？

当前接入方式是 TOML 类型声明加 C++ provider，不包含通用 C# 程序集加载器或 JIT。

---

**接入要点：在合适的初始化时机进入，用 TOML 定义自己的类型，用 C++ 实现行为，用原生组件、数据资源和生命周期组织功能，用真实消费者验证结果。**
