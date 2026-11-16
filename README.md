# ClassInjector Developer Guide

[English](README.md) | [简体中文](README.zh-CN.md) | [Provider SDK](include/ci/Provider.hpp)
[Build from source / 从源码构建](BUILDING.md)

**Integrate earlier in game initialization, and give your mod its own components, data, and native behavior.**

ClassInjector (CI) is an **early plugin loader and runtime type-extension framework** for RE Engine mod developers. It provides two complementary capabilities: bringing plugin integration forward to the point where the engine's TDB and native method entries are ready, before subsequent critical initialization runs; and letting developers define their own managed classes, components, and UserData types that participate in the engine's native object creation, method calls, resource loading, and lifecycles.

Developers describe types, fields, and methods in TOML and provide method implementations in C++. CI registers them during game startup and hands control to plugins through an early readiness callback. Mods can prepare their integration at the appropriate initialization stage and organize features around their own types, rather than always modifying results after initialization or fitting every new feature into existing logic through hooks.

Supported environment: **Monster Hunter Wilds, Windows x64, TDB81**.

## Contents

- [1. What Problems Does It Solve?](#1-what-problems-does-it-solve)
- [2. Capabilities and Responsibilities](#2-capabilities-and-responsibilities)
- [3. Installation and File Layout](#3-installation-and-file-layout)
- [4. Quick Start: A Counter Type](#4-quick-start-a-counter-type)
- [5. TOML Declaration Reference](#5-toml-declaration-reference)
- [6. Inheritance, Interfaces, and Components](#6-inheritance-interfaces-and-components)
- [7. Fields, References, and Resources](#7-fields-references-and-resources)
- [8. Extending Existing Types and Enums](#8-extending-existing-types-and-enums)
- [9. Generics and Arrays](#9-generics-and-arrays)
- [10. Initialization and the Provider SDK API](#10-initialization-and-the-provider-sdk-api)
- [11. Compatibility, Troubleshooting, and Distribution](#11-compatibility-troubleshooting-and-distribution)
- [12. Frequently Asked Questions](#12-frequently-asked-questions)

## 1. What Problems Does It Solve?

### 1.1 Problem One: Conventional Plugin Integration Can Happen Too Late

For a plugin that uses the TDB to look up types, fields, and methods, having its DLL loaded is different from being ready to execute logic against the type system. A conventional REFramework plugin must wait until REFramework has successfully acquired and prepared the TDB before relying on its SDK for these operations.

The game's initialization stages are not organized around a mod's needs. By the time an ordinary plugin can use the TDB, some game logic, static data, or initialization results may already exist. For example:

- A type list may already have been read and used to build a cache.
- A table may already have been constructed from its initial inputs, with later code reading only the cached result.
- A system may already have completed its first initialization and decided which objects or entries to create for the process.

A hook installed at that point may affect only **future calls**, not the first initialization that has already occurred. The author may then need to backfill data, rebuild caches, or find additional downstream integration points. The issue is not necessarily a limitation of hooking itself: the required initialization window has already passed.

### 1.2 CI Is Also an Earlier Plugin Loader

CI does more than add classes. It loads participating provider DLLs early and exposes `ci_on_initialize`, a callback for **integrating with subsequent initialization as soon as the type system is available**.

This notification occurs **after TDB types and native method entries are ready, but before global static constructors execute**. A plugin can look up the types and methods it needs, prepare its integration, and install hooks that must take effect before initialization, then let the engine continue normally.

The startup stages relevant to plugin integration are:

```text
Engine startup
  |
  +-- CI loads participating providers early and collects
  |   function registrations and type declarations
  |
  +-- The engine builds the TDB and prepares types and native method entries
  |    |
  |    +-- CI: every provider's ci_on_initialize(context)
  |    |    Query entries, prepare integration, submit TDB-dependent JSON to UDE
  |    +-- CI: every provider's ci_on_post_initialize(context)
  |         UDE closes registration, validates declarations, installs its decode hook
  |
  +-- Global static constructors and other initialization continue
  |   Type lists, static tables, and caches are built by their systems
  |
  +-- Conventional REFramework plugins reach their SDK/TDB-dependent stage
      Some first-time initialization results may already exist
```

**Example: a cache built once at startup.** Suppose a system builds a cache from a type list only once. Changing the list's source after the cache has been created does not automatically rebuild the cache. A CI plugin can integrate with that source in its early callback so that the system reads the extended input when constructing the cache for the first time. This changes how initialization happens, rather than patching its results afterward. The plugin implements the actual list and cache integration.

This is also useful for plugins that **need earlier initialization access without adding any new types**. CI can be used solely as an early loader; see Section 10.

CI advances **the loading and callback timing of plugins participating in its loading flow**. It does not initialize the REFramework SDK earlier. Use CI's ProviderContext during the early stage, and run SDK-dependent logic after the REFramework SDK becomes available.

### 1.3 Problem Two: New Features Should Not Always Be Patches to Existing Logic

Scripts and hooks can modify fields, intercept methods, and store extra state in Lua tables or C++ containers. That extra state does not automatically become an engine-recognized type. Native code cannot create it as a component simply by knowing a type name, and resource files do not automatically know how to instantiate it.

ClassInjector provides another approach:

```text
TOML: declare types, fields, inheritance, and method signatures
                              +
C++ DLL: register the corresponding method implementations
                              |
                              v
Register with the engine's type system during startup
                              |
                              v
Native code / REFramework / resources use the declared type contracts
```

You can define your own configuration objects, behavior parameters, and components instead of borrowing an unrelated built-in type name, repurposing existing fields, or maintaining an external object-address-to-state map for every feature.

### 1.4 A Native Authoring Experience Closer to Unity's Component Model

Developers familiar with engines such as Unity often structure a feature around components for behavior, data objects for parameters, and scenes or prefabs for composition, with the engine driving their lifecycles. CI aims to bring a similar authoring approach to RE Engine mods: **define your own content and let the engine use it through native mechanisms, rather than only modifying existing code paths**.

In RE Engine, this component-based, data-driven approach can be organized as follows:

| Authoring task | Organization with CI | Engine responsibility |
|---|---|---|
| Define behavior | Create a component derived from `app.AppBehavior` | Attachment, enable/disable behavior, and the applicable update and destruction lifecycles |
| Define parameters | Create a type derived from `via.UserData` | Read parameters from matching resources according to the type declaration |
| Compose features | Place components and data references in matching PFB/object relationships | Instantiate and associate objects using the resource and component structure |
| Manage runtime state | Store timers, state, and references in instance fields | Recognize object layouts and managed reference relationships |
| Extend a calling contract | Implement an existing interface or override an extensible virtual method | Invoke the implementation through native dispatch |

For example, a custom periodic behavior for a scene object could use:

```text
Example.PulseSettings : via.UserData
  +-- IntervalSeconds   Period interval, loaded from a resource
  +-- EnabledAtStart    Initial enabled state, loaded from a resource

Example.PulseBehavior : app.AppBehavior
  +-- Settings          Reference to its configuration object
  +-- Elapsed           Per-instance elapsed time
  +-- doStart           Read ready configuration and initialize state
  +-- update            Advance this instance's behavior
  +-- doOnDestroy       Clean up external resources or subscriptions it owns

Matching PFB / standard component creation path
  +-- Attach the component to a GameObject; the engine drives its lifecycle
```

Implement the component's behavior, provide the matching configuration resource, and attach it through the component creation path. Hold and release configuration references and external resources at the appropriate lifecycle stages.

This differs from hooking an existing update function, iterating an external object table, and looking up state by object address. **Behavior belongs to your component, parameters belong to your data, runtime state belongs to your objects, and scheduling is delegated to the engine wherever possible.** Multiple objects can reuse the same component type while retaining independent instance state. Parameter changes can be expressed through resources rather than hard-coded branches.

CI is therefore not just a way to add more modification points. Its purpose is to help mods express new features as content authored with the engine. Hooks remain useful for connecting systems without an exposed integration point or changing existing rules, but they do not have to be the foundation of every new feature.

### 1.5 How the Two Capabilities Work Together

The early loader addresses **when to integrate**; type extension addresses **what form that integration takes**. They can be used independently or together:

- To influence first-time initialization, use the early callback without defining a new class.
- To add an independent component or data object, define a type and use the matching native creation and lifecycle paths.
- To include a new type in startup registration or initial cache construction, register the type and connect it to the relevant system in the early callback, before that system initializes.

CI provides the timing and type capabilities. Extension authors connect new content to each system according to that system's contract.

### 1.6 Benefits for Developers

- **Earlier integration:** prepare logic before critical initialization and reduce the need to patch initialization results afterward.
- **Explicit type identity:** custom objects have their own names, inheritance relationships, fields, and method declarations.
- **State attached to objects:** additional state can live in object fields, with managed reference relationships recognized by the engine.
- **Native extension points:** inheritance, virtual overrides, and existing interfaces connect implementations to matching native call paths.
- **Data-driven design:** serializable fields on new types allow parameters to live in USER/PFB resources while behavior remains in code.
- **Cross-plugin collaboration:** other extensions can access objects using agreed type names and signatures without depending on a provider's private C++ classes.

These capabilities improve both integration timing and feature organization. Authors can use native mechanisms for object creation, reference ownership, and calls, leaving component scheduling to the engine.

## 2. Capabilities and Responsibilities

| Capability | What it enables | Conditions or limits |
|---|---|---|
| Early plugin loading and notification | Integrate after TDB/native entries are ready and before subsequent critical initialization | Use CI's paired loading and `ci_on_initialize`; the RF SDK is not necessarily ready |
| New managed classes | Independent data objects, parameter types, and behavior implementations | The base type and actual call path must be compatible |
| Fields | Scalars, managed references, and embedded existing ValueTypes | This does not include arbitrary new struct definitions |
| Methods and constructors | Instance methods, constructor overloads, virtual methods, overrides, and abstract methods | Implementations use C++ bindings and must match engine calling conventions |
| Existing interfaces | Participate in matching interface consumer paths | New interface definitions and default interface methods are outside the supported scope |
| Custom components | Derive from `app.AppBehavior` and use native component lifecycles | Create and destroy through the GameObject/component system |
| RSZ field declarations | Read selected fields of new types from resources | Resources must be authored separately and match the declarations |
| Fields on existing classes | Append runtime state to eligible built-in types | Limited to concrete, purely managed classes; see Section 8 |
| Existing Int32 enum extensions | Rename members and append members | Preserve existing numeric values; do not define new enum types |
| Explicit closed generics | Register required concrete generic types at startup | This is not a complete CLR generics system |
| One-dimensional class-reference arrays | Use arrays of new types in fields and signatures | New array shapes are limited; see Section 9 |
| ProviderContext services | Query types/methods/fields, access objects, and request resources | Each service has its own lifecycle and ownership contract |

**Division of responsibilities:** CI handles plugin loading, type registration, and public foundational services. Extension authors implement behavior, supply resources, and connect features to game systems. For example, a custom Action needs matching resource references and a creation path, a custom UserData type needs a matching USER file, and a type used by another system needs integration with the corresponding factory or registry.

Type declarations and C++ implementations load at game startup. Fully restart the game when updating or removing an extension. Lua can access registered types; method implementations use C++ providers.

## 3. Installation and File Layout

### 3.1 Runtime and Provider DLLs

The ClassInjector runtime is **`hid.dll`** in the game root directory. A developer's implementation DLL is called a **provider** in this guide. A provider can supply new types or use only early loading and the readiness callback.

Example layout:

```text
Game root/
├─ MonsterHunterWilds.exe
├─ hid.dll                         # ClassInjector runtime
└─ reframework/
   └─ plugins/
      ├─ CounterPlugin.dll         # Provider implementation
      ├─ CounterPlugin.toml        # Same directory and base filename as the DLL
      ├─ BehaviorPlugin.dll        # May also be a REFramework plugin
      └─ BehaviorPlugin.toml
```

Preserve the release package's directory layout. If another package already supplies `hid.dll` in the game root, check that the loading arrangements are compatible before replacing that file.

### 3.2 Pairing TOML Files and DLLs

- Install provider DLLs and TOML files directly in the game root's `reframework/plugins/` directory.
- **The TOML file and DLL must share the same directory and base filename**, such as `CounterPlugin.toml` and `CounterPlugin.dll`.
- A TOML file without its matching DLL does not participate in loading. A DLL without a matching TOML file is not discovered as a CI provider.
- One provider can declare multiple types. Install one DLL and matching TOML file per provider.
- A provider that needs the REFramework SDK must handle its own standard plugin initialization; ClassInjector does not initialize the SDK on its behalf.

### 3.3 Applying Changes

Declarations are read at startup. **Fully exit and restart the game** after changing TOML files, type layouts, or provider DLLs. Reloading Lua scripts does not register types again.

## 4. Quick Start: A Counter Type

This example defines `Example.Counter` with an Int32 field named `Value` and an `Add` method, then uses REFramework Lua to create an instance, call the method, and read the result.

### 4.1 Declare `CounterPlugin.toml`

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

Two different names appear here:

- `name = "Add"` is the engine method name used for lookup, reflection, and calls.
- `binding.function` is the implementation registration key inside this provider. It must exactly match the C++ registration. A fully qualified type name followed by the method signature is a useful naming convention.

### 4.2 Implement the Provider Method

Use the public `ci/Provider.hpp` header supplied with the developer interface. This example uses only ProviderContext and has no REFramework SDK dependency:

```cpp
#include <ci/Provider.hpp>
#include <ci/API.hpp>
#include <cstdint>
#include <cstdio>

namespace Example::Counter {
    static const ci::ProviderContext* g_context{}; // Borrowed; valid for the runtime's lifetime.

    // self is an Example.Counter managed object, not a native C++ Counter instance.
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

// Use this macro in exactly one .cpp file per provider DLL.
CI_REGISTER_PROVIDER(registration) {
    registration.register_func(
        "Example.Counter.Add(System.Int32)", &Example::Counter::Add, 1);
}
```

The method's two implicit parameters are `vmctx` and `self`. **Do not include them in TOML `parameters`.** Individual implementation functions do not need to be exported; `CI_REGISTER_PROVIDER` provides the fixed registration entry point.

This example omits a constructor declaration. CI supplies a default parameterless constructor that forwards to the base type's parameterless constructor. A new object's `Value` starts at 0. Explicit constructor rules are covered in Section 5.

### 4.3 Install and Run

Place `CounterPlugin.dll` and `CounterPlugin.toml` together in `reframework/plugins/`, then fully restart the game.

Save the following script as `reframework/autorun/CounterExample.lua`. After looking up the type, the script performs the counter operation once in a game-main-thread native `PlayerManager.update()` callback, keeping object creation and calls inside a native managed-call scope. The `UpdateBehavior` callback only records the main-thread context so that the script can select the matching native call.

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

After fully restarting the game, look for `CounterExample: first=3, second=7, Value=7` in the REFramework log. Both calls operate on the same instance: `Value` increases from 0 to 3, then to 7.

The script performs the counter operation once per script load. When finished, remove `CounterExample.lua` and restart the game to remove the example hook. In your own feature, use the counter from a component or another suitable native lifecycle.

The creation path used by `sdk.create_instance` differs from `ProviderContext.CreateInstance`: the latter allocates only and does not implicitly invoke a constructor. See Section 5.3.

## 5. TOML Declaration Reference

### 5.1 Types, Fields, and Methods

| Setting | Meaning | Default or requirement |
|---|---|---|
| `schema_version` | Declaration format version | Use `1` when declaring types/enums |
| `types.name` | Fully qualified type name | Required; use an extension-specific namespace |
| `types.base` | Fully qualified base type name | Defaults to `System.Object`; a new class needs an inheritable base |
| `types.abstract` | Declare an abstract class | Defaults to `false` |
| `types.interfaces` | Array of fully qualified existing interface names | As needed |
| `types.fields` | Field list | Each entry contains `name` and `type` |
| `fields.serialize` | Deserialize this field from a resource | Defaults to `false`; applies to eligible fields of new types |
| `methods.name` | Method name | Constructors use `.ctor` |
| `methods.kind` | Method kind | Defaults to `instance`; see below |
| `methods.return_type` | Managed return type | Defaults to `System.Void` |
| `methods.parameters` | Explicit parameter list | Each entry requires `name` and `type` |
| `parameters.modifier` | Reference parameter direction | Omit for by-value; accepts `ref` or `out` |
| `methods.override_of` | Declaring type of the override target | Specify the base/interface source as needed |
| `methods.binding` | Native implementation key and version | Required for concrete methods; not needed for abstract methods |

Use fully qualified managed names for fields and parameters, such as `System.Int32` rather than `int`. Declare a method's name, return type, and parameter list separately; do not put the whole `Add(System.Int32)` signature in `methods.name`.

### 5.2 Method Kinds

| `kind` | Purpose |
|---|---|
| `constructor` | A constructor named `.ctor`, returning `System.Void` |
| `instance` | An ordinary instance method |
| `virtual` | A virtual method that derived types can override |
| `override` | Override a matching base/interface method |
| `abstract` | Declare a contract without an implementation |

A concrete class must implement any remaining abstract methods in its inheritance chain. An `override` must match the target's parameter types, directions, and return type, rather than just its name.

### 5.3 Constructor Rules

- **No constructor declared:** CI generates a default `.ctor()` that forwards to the base type's parameterless constructor. The base must provide an applicable default construction path.
- **Explicit constructor:** execution enters your C++ implementation. Your implementation calls the appropriate base constructor and initializes its fields. CI does not add another base-constructor call or clear the fields again before each constructor invocation.
- **Parameterized and overloaded constructors:** distinguish them using `parameters`; each concrete constructor needs an implementation binding.
- **Only parameterized constructors declared:** no extra parameterless constructor is generated. The creation code must allocate the object and call a matching constructor, or use a native creation path compatible with that type.
- Allocation, construction, and ownership are separate operations. Avoid accidentally invoking a constructor again on an already constructed object.

### 5.4 Parameters and Native Signatures

The full native signature of Counter.Add from Section 4 is:

```cpp
#include <cstdint>

int32_t Add(void* vmContext, void* self, int32_t delta) noexcept;
```

An SDK-based implementation may declare `self` using the corresponding managed-object pointer type. It does not point to an ordinary C++ business object. Managed declarations are not inferred automatically from the C++ function pointer.

Scalar calls support Boolean, signed/unsigned integers, Char, Single, Double, pointer-sized integers, existing enums, and managed references. Multiple parameters and `ref/out` are also supported. Complex ValueTypes passed by value and struct returns must match the corresponding engine-native ABI and available invocation path.

Example `ref/out` declaration:

```toml
[[types.methods]]
name = "TryGetValue"
return_type = "System.Boolean"
parameters = [{ name = "value", type = "System.Int32", modifier = "out" }]
binding = { function = "Example.Counter.TryGetValue(System.Int32)", version = 1 }
```

The corresponding explicit C++ parameter is `int32_t* value`. A managed-object `ref/out` parameter is an object-pointer slot, such as `void**`, not `void*`. An `out` implementation must write the output on every return path.

Look up the method as `TryGetValue(System.Int32)`, without appending `&` to the element type. The direction difference between `ref` and `out` does not define two separate overloads. `in` parameters, reference returns, and static member registration are outside the supported public scope.

### 5.5 Binding Versions

The numeric `binding.version` must exactly match the version in `.register_func(..., version)`. It is a **per-method contract version**, not the TOML format version or the DLL's overall version.

When changing parameters, return types, or calling contracts, update the declaration, implementation, and version together, and check that the C++ signature corresponds to the managed declaration.

## 6. Inheritance, Interfaces, and Components

### 6.1 Class Inheritance and Overrides

New types can derive from an existing inheritable class or from a new class declared by another provider. Keep fully qualified type names unique and explicitly distribute the providers your extension depends on.

When a base class defines a `virtual` method, a derived class supplies an implementation using `override` and a matching signature. The call path also matters: **a native virtual call dispatches through the object, while looking up a declaration's method entry and calling it directly does not perform virtual dispatch**.

### 6.2 Existing Interfaces

Declare interfaces with `interfaces = ["Fully.Qualified.InterfaceName"]` and implement matching methods. Explicit interface implementation names use the full interface prefix, such as `System.IDisposable.Dispose`.

Interface integration must match the actual caller's method contract; check execution through that caller. Interface declarations are limited to interfaces already in the TDB. New interface definitions, default interface methods, and general generic variance are outside the supported scope.

### 6.3 Custom AppBehavior Components

Declare a component with `base = "app.AppBehavior"` and attach it through a standard native component creation path, such as the GameObject's `createComponent(System.Type)`.

| Lifecycle event | Override method |
|---|---|
| Awake | `doAwake` |
| Start | `doStart` |
| Destruction notification | `doOnDestroy` |
| Per-frame update | `update` |
| Late update | `lateUpdate` |

Declare these methods with `kind = "override"`. Once the GameObject creates and attaches the component, the engine invokes its lifecycle methods and manages enabling, disabling, and destruction through the component system. `sdk.create_instance` creates ordinary managed objects; it does not replace component attachment.

A custom `app.AppBehavior` can add fields and override the lifecycle methods above. When choosing another native-backed base class, also check its creation path, native resource ownership, and destruction requirements.

## 7. Fields, References, and Resources

### 7.1 Field Access and Ownership

ProviderContext field access returns the address of a field slot:

```cpp
auto* value = ci::API::ManagedObject{context->sdk, object}.get_field<int32_t>("Value"); // int32_t*
auto** peer = ci::API::ManagedObject{context->sdk, object}.get_field<void*>("Peer");  // void**
```

The caller must supply the correct template argument; field types are not checked automatically. Confirm the field contract before reading or writing. A field address is valid only while its owning object remains valid.

- Write POD scalar values through slots of the correct type.
- Update managed-reference fields using `ci::API{*context->sdk}.objects().assign_reference(owner, field, value)` rather than a raw assignment. The field is a reference slot (`void**`); check the returned success flag.
- To keep an object across callbacks, retain and release it according to the contract of the API that supplied it. A raw pointer or a local Lua/C++ variable is not permanent ownership.
- Before releasing externally held references, make sure the relevant callbacks have stopped using the object, then release according to the acquisition contract.

### 7.2 Embedded ValueTypes

Fields can use ValueTypes already present in the TDB, such as `via.vec3` and `System.Guid`. These fields contain embedded payloads, not pointers to boxed objects.

For whole-value copies, use the actual payload length and the appropriate field-writing mechanism. `get_valuetype_size` returns the payload size of a closed value type, or 0 for non-value types and types without a layout. A wrapper object's size differs from the embedded payload size. Structs containing managed references also require the corresponding reference-management contract.

### 7.3 Resource Serialization Fields

Fields on new types can be marked for deserialization:

```toml
[[types]]
name = "Example.Settings"
base = "via.UserData"
fields = [
    { name = "Limit", type = "System.Int32", serialize = true },
    { name = "RuntimeCounter", type = "System.Int32" }
]
```

`Limit` is read from the matching resource; `RuntimeCounter` is runtime-only. The declaration does not generate a resource file.

When authoring USER/PFB files, RSZ type information uses the type-name hash and the corresponding engine TypeInfo CRC. Field data follows the serialization declaration. Runtime type IDs, name hashes, and TypeInfo CRCs are different identifiers and must be supplied according to the resource format. When field declarations change, update the resources and the schemas used by editing tools together.

Resource requests may complete asynchronously. Receiving a USER object does not mean its fields have been decoded. Custom components in a PFB also need correct GameObject/component relationships; an ordinary USER object does not attach itself to a scene.

## 8. Extending Existing Types and Enums

### 8.1 Appending Fields to a Built-in Class

Use `extend = true` and declare only the new fields:

```toml
schema_version = 1

[[types]]
name = "TARGET"
extend = true
fields = [{ name = "ExampleExtraCounter", type = "System.Int32" }]
```

Replace `TARGET` with the fully qualified target type name. The target must be a **concrete, purely managed class** in the original TDB and meet one of these conditions:

- It is sealed.
- It is not sealed, but has no derived classes already present in the original TDB.

The extension preserves existing fields and methods and appends runtime fields only. It does not change the base class or add methods or interfaces. Native-backed types, abstract classes, interfaces, arrays, strings, ValueTypes, and open generics are not eligible. Appended fields do not use `serialize = true` and are not automatically written to save data.

When multiple extensions target the same class, use extension-specific field prefixes to avoid duplicate names. Access appended fields by name or layout query rather than publishing hard-coded offsets to other plugins.

### 8.2 Editing an Existing Int32 Enum

This example uses the existing `System.DayOfWeek` enum to demonstrate renaming and appending. In your own project, choose the relevant enum and avoid names and values used by other extensions.

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

- Each rename uses either `fromName` or `fromValue`. These select a source member and **preserve its numeric value**.
- Appended members use `name` and an Int32 `value`. Duplicate names are errors; existing members are not overwritten.
- An alias with the same value does not replace the original canonical name. Value-to-name lookup still returns the first matching member.
- Within one declaration, renames are applied before appends. Fully restart after changes.
- Dynamic registration is also available through `registration.register_enum(...)` inside `CI_REGISTER_PROVIDER`.

When appending an ID used by game logic, also supply the corresponding entries in tables, resources, or factories that consume it.

## 9. Generics and Arrays

### 9.1 Explicit Closed Generics at Startup

Choose one of two declaration approaches:

- `generic_definition`: explicitly declare the required closed type and its concrete fields/methods.
- `generic_template`: use the declaration shape of an existing closed class and adapt it to new arguments.

List concrete arguments in `generic_arguments`. The fully qualified name must use the correct backtick arity and argument list, for example ``Example.Box`1<Example.Counter>``.

Generic instances are explicitly declared at startup. Arguments are limited to supported closed managed-reference types, `System.Int32`, or existing enums with an Int32 underlying type. Concrete non-constructor methods require corresponding C++ implementations. Full open-generic members, constraints, and arbitrary runtime `MakeGenericType` are outside the supported scope.

### 9.2 Arrays of New Types

When a field or method uses `Example.Counter[]`, CI automatically supplies the missing one-dimensional class-reference array type. A separate `[[types]]` array declaration is not needed. Existing array types are reused.

New array types are limited to one-dimensional class-reference arrays. New value-type arrays, multidimensional arrays, jagged arrays, array covariance, and complete generic array interfaces are outside the supported scope. ProviderContext's `CreateArray` can allocate instances of value-array types already present in the engine.

## 10. Initialization and the Provider SDK API

### 10.1 Public Headers, Registration, and Versions

| Header | Purpose |
|---|---|
| `ci/Provider.h` | POD protocol for `ProviderContext`, `RegistrationAPI`, and enum registration |
| `ci/Provider.hpp` | DLL-local `Registration`, `EnumChanges`, and `CI_REGISTER_PROVIDER` |
| `ci/API.h` | Six `SDKAPI` tables and opaque handles |
| `ci/API.hpp` | DLL-local `ci::API` value views and vector helpers |

Use `CI_REGISTER_PROVIDER(registration)` once, in one `.cpp` per provider DLL. It generates the `ci_get_sdk_abi` and `ci_register_provider` C exports. `RegistrationAPI` and `Registration` are call-local; do not retain them for initialization callbacks. The host copies inputs synchronously, and accumulated registration errors make the export return failure.

This is a **C++ alternative** to the TOML enum example in Section 8.2. Do not register both copies of the same changes:

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

Omit unused `renames` or `additions` instead of passing empty positional lists. `EnumChanges` is a local argument, not an initializer_list or STL object to retain across DLLs. Register functions as in Section 4: `registration.register_func("full binding key", &implementation, version)`.

The current SDK ABI is **3**. `ProviderContext` contains only `const ci::SDKAPI* sdk`. SDK ABI, TOML `schema_version`, and each function's `binding.version` are independent versions. ABI1/2 are incompatible with this host; rebuild the host and every provider installed in that instance together. The public-header and snake_case reorganization preserves ABI3 layouts.

### 10.2 Two Initialization Phases and Zero-Type Providers

```text
Scan matching TOML/DLL pairs, check ci_get_sdk_abi, call ci_register_provider
  → Register functions/enums and validate bindings (no VM or RF SDK access)
  → Publish types; prepare native entries, custom vtables, and ProviderContext
  → Every provider's ci_on_initialize
       Query metadata, prepare early integration, submit TDB-dependent JSON to UDE
  → Every provider's ci_on_post_initialize
       UDE closes registration, validates declarations, installs its decode hook
  → Continue the game's global static constructors
```

Both rounds run **before global static constructors**. They are not adjacent notifications for each individual provider. They do not depend on manifest filenames or provide a general dependency graph. Submit to UDE from the first round (`ci_on_initialize`), not from your own second-round callback.

Either notification export may be omitted; a post-only provider such as UDE still receives its callback. The signatures are:

```cpp
extern "C" __declspec(dllexport)
void ci_on_initialize(const ci::ProviderContext* context) noexcept;

extern "C" __declspec(dllexport)
void ci_on_post_initialize(const ci::ProviderContext* context) noexcept;
```

To use CI only as an early loader, install an empty same-basename TOML (comments are also fine), supply this no-op registration body, and implement a notification as needed:

```cpp
#include <ci/Provider.hpp>

CI_REGISTER_PROVIDER(registration) {
    // Zero-type provider; no VM or RF SDK access here.
}
```

Early notification is not RF plugin initialization or readiness of game singletons, caches, or resources. The context/SDK tables are borrowed for the host lifetime, unlike the call-local `RegistrationAPI`.

### 10.3 Queries, Fields, and Ownership

Public queries do not require RF initialization. This function checks metadata and an entry only; it does not create business objects or invoke constructors during early notification:

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

| Group | POD tables / C++ views | Contract |
|---|---|---|
| Metadata | `tdb`, `type_definition`, `field`, `method` / `api.tdb()` and type/field/method views | `find_method` returns metadata; `get_function<exact signature>()` obtains its native entry |
| Objects | `managed_object` / `api.objects()` | `create_instance` neither invokes .ctor nor retains; manage caller ownership with `retain/release`; write reference slots with `assign_reference` |
| Resources | `resource_manager` / `api.resources()` | `create_userdata` returns one owned reference, not necessarily decoded; holder finalizers own their native requests |

- `Field::get_data<T>()` returns **`T&`**; `ManagedObject::get_field<T>()` returns **`T*`**. The caller checks the actual field type.
- Address instance fields through the actual object; use `get_data_raw(payload, true)` for unboxed embedded payloads. Field views copy by value, but addresses remain tied to object lifetime. There is no public `FieldLayout` or UDE global field-layout cache.
- `get_init_data()` returns borrowed metadata initializer bytes, or `nullptr` when absent. It is not runtime static storage or present on every field. For enum constants, check literal status, type, and width before reading; zero is not an absent initializer.
- Raw `get_fields/get_methods` `out_size` is in **bytes**; `out_count` counts written handles. `ci::Result` follows REF's integer convention (`ERROR_NONE == 0`). C++ wrappers allocate DLL-local vectors; no custom visitor is needed.
- `create_action(vmContext, actionType, target, method)` takes a real Method handle, not a C++ function address or a manually replaced delegate slot.
- No API establishes a managed VM scope. Create objects and execute methods within real native lifecycles; a main thread, VM context, bare IPC, or `UpdateBehavior` is not proof of an outer scope. Initialize reference-array elements before first publication to an owner.
- Unresolved optional entries may be null; check services actually required. Successful queries do not imply business, resource, or RF SDK readiness.

Method lookup still uses complete signatures, comma spacing, parent lookup, and ref/out element-type rules; it does not perform automatic virtual dispatch or construction. See the [provider header](include/ci/Provider.hpp), [SDK views](include/ci/API.hpp), [ABI tables](include/ci/API.h), and [UDE submission example](https://github.com/kassent/UserDataExtension/blob/main/README.md#10-plugin-developer-api).

## 11. Compatibility, Troubleshooting, and Distribution

### 11.1 Compatibility Guidelines

- Use a matching runtime and public header release, and state the runtime version required by your provider.
- When type declarations change, review the provider, dependent extensions, resources, and editor schemas together.
- Type names and method signatures are collaboration contracts. Avoid built-in names and other extensions' namespaces.
- Avoid persisting custom runtime type IDs or using them as a cross-version protocol. Use explicit type names and stable IDs defined by your feature.
- After a game update, check type registration, method calls, resource loading, and object lifecycles.

### 11.2 Troubleshooting Order

1. **Check file pairing:** verify that matching TOML/DLL files are directly in `reframework/plugins/`, not a subdirectory.
2. **Check startup logs:** runtime logs are written to `logs/HIDWrapper.log`. With a mod manager, locate them according to its file-redirection settings. Use logs from the current launch.
3. **Check declarations and bindings:** look for TOML errors, missing functions, version mismatches, and failed dependencies.
4. **Check readiness:** confirm type registration records, native entry readiness, and completion of CI initialization notifications.
5. **Check behavior:** inspect instance creation, method returns, field changes, game-system calls, and cleanup on exit.

Common log messages:

| Log fragment | What to check |
|---|---|
| `Type injection hooks ready` | Startup entry installation result; registration and readiness records should follow |
| `Plugin declarations skipped` | Declaration parsing failed for that file; inspect the accompanying error |
| `function missing` / `function version mismatch` | TOML bindings disagree with DLL registrations |
| `Plugin skipped` | The provider was skipped; types depending on it may also be affected |
| `SDK ABI identity missing` / `SDK ABI mismatch` | Missing version export or mismatched ABI; check the host and every provider in this instance |
| `Startup window missed` | The runtime missed the startup registration window; check the installed version and loading arrangement |
| `Published metadata` / `Registered` | Type publication/registration has progressed; also confirm entry readiness |
| `CI provider initialization complete; injection ready=1` | CI completed registration and readiness notification; continue with feature-specific or resource-loading diagnostics |

If a provider has a missing binding or version mismatch, all declarations from that provider are skipped. Providers depending on its custom types are skipped as well. Resolve upstream declaration or binding errors first, then check dependent extensions.

### 11.3 Distributing Your Extension

Include:

- A matching provider DLL and TOML file installed in `reframework/plugins/`.
- Required resources and scripts in the correct directory structure.
- Required ClassInjector/REFramework versions and other dependencies.
- Type namespaces, public method contracts, configuration instructions, and upgrade requirements.
- Supported game versions, main features, and known limitations.

Before release, check: **launch the game → register types → create objects and call them through the intended game-system path → check fields and results → destroy objects, change scenes, and exit normally**. Check component behavior, asynchronous resource loading, and multiplayer features separately when applicable. Include only files required at runtime in the release package.

The runtime is a shared dependency. When uninstalling one extension, remove only that extension's files and retain the runtime if other extensions still depend on it. Exit the game before uninstalling or updating.

## 12. Frequently Asked Questions

### How Does CI Relate to REFramework?

CI provides early plugin loading and type extension. REFramework provides scripting, reflection access, and plugin capabilities. Some initialization may already be complete when ordinary RF plugins reach their SDK/TDB-dependent stage. CI's early initialization callback lets plugins prepare integration before subsequent critical initialization. It does not require new types and does not initialize the RF SDK earlier. Lua examples and SDK-based field access still require the appropriate REFramework environment.

### Can I Use CI Without Adding a Class?

Yes. Supply a matching TOML file, the fixed registration entry point, and `ci_on_initialize` to use CI as an early loader; see Section 10.2. Use ProviderContext for early lookups and integration setup.

### What Does an Authoring Experience Closer to Unity Mean?

It means organizing a mod around your own components, your own data, and native lifecycles. For example, implement behavior in a custom AppBehavior, store configurable parameters in a custom UserData type, and create and compose them through standard component or PFB paths. This is an authoring approach, not a port of Unity's APIs, editor, or C# environment. The goal is to build new features on RE Engine's native mechanisms rather than relying entirely on external patches around existing functions.

### Does CI Replace Every Hook?

Hooks and native extensions serve different purposes. For new independent behavior, prefer your own components, data, and native call paths instead of attaching all logic to an existing update function. Hooks remain useful for changing built-in rules or connecting systems without an exposed entry point. CI's early loader also lets those hooks take effect before critical initialization. The goal is to use hooks as integration tools where needed, rather than as the only way to implement every feature.

### Is Editing TOML Enough to Add New Behavior?

TOML describes contracts, not method bodies. Declarations can bind to existing C++ implementations, but new method logic needs a provider implementation. Lua can consume registered types; it is not the implementation backend for method bindings.

### Why Does a Type Exist but Produce No Visible Effect?

Check whether the object is created through the correct path, its constructor executes, it is connected to the intended game-system registry, resources reference the correct type, and the lifecycle actually invokes its methods. Registration is the first step.

### Are New Fields Saved Automatically?

Runtime fields do not automatically become part of save data. `serialize = true` on a new type controls field reading from matching resources; it does not connect the type to the game's save-writing system. Fields appended to built-in classes are runtime-only.

### Can I Update Types or Unload a DLL While the Game Is Running?

Types and providers remain valid for the entire game process. Fully restart after updates, removal, or layout changes; do not unload implementations while existing objects remain alive.

### Can CI Load a C# DLL Directly?

The integration model is TOML declarations plus a C++ provider. It does not include a general C# assembly loader or JIT.

---

**Integration essentials: enter at the right initialization stage, define your types in TOML, implement behavior in C++, organize features with native components, resources, and lifecycles, and check results through actual consumers.**
