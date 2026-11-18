# ClassInjector Developer Guide

[English](README.md) | [简体中文](README.zh-CN.md) | [Provider SDK](include/ci/Provider.hpp)
[Build from source / 从源码构建](BUILDING.md)

ClassInjector (CI) provides startup callbacks and managed-type extensions for **Monster Hunter Wilds (Windows x64, TDB81)**. Declare types and members in TOML, implement methods in C++, and use those types through the game’s object, component, or resource APIs.

## Contents

- [1. Choose an Integration Path](#1-choose-an-integration-path)
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

## 1. Choose an Integration Path

- **Run before static initialization:** pair a provider DLL with a same-name TOML and use `ci_on_initialize` to query TDB metadata and prepare your integration. An empty TOML is enough for a provider that declares no types. See [startup callbacks](#102-two-initialization-phases-and-zero-type-providers).
- **Add a managed type:** declare fields, methods, and inheritance in TOML and register the C++ implementations in your provider. See the [counter example](#4-quick-start-a-counter-type).
- **Add reusable behavior and data:** derive a component from `app.AppBehavior` and attach it through the GameObject component API. For resource-backed parameters, define a `via.UserData` type and author a matching USER. The engine drives the component’s lifecycle.

Use the CI SDK for startup callbacks. Features that use the REFramework SDK, including the Lua quick start, run after REFramework initializes.

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

The creation path used by `sdk.create_instance` differs from CI’s `api.objects().create_instance(...)`: the latter allocates only and does not implicitly invoke a constructor. See Section 5.3.

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

`Limit` is read from the matching resource; `RuntimeCounter` is runtime-only. Author a matching resource file. To persist fields in a save, integrate with the game’s save system separately.

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
- An alias with the same value does not replace the original canonical name. Value-to-name lookup returns the first matching member.
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

New array types are limited to one-dimensional class-reference arrays. New value-type arrays, multidimensional arrays, jagged arrays, array covariance, and complete generic array interfaces are outside the supported scope. `api.objects().create_array(...)` can allocate instances of value-array types already present in the engine.

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

Build providers with the **SDK ABI 3** headers that match the host. Access SDK services through `ProviderContext::sdk`. The ABI identifies the host interface; TOML `schema_version` identifies the declaration format; `binding.version` identifies a method binding.

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
- Address instance fields through the actual object; use `get_data_raw(payload, true)` for unboxed embedded payloads. Field views copy by value, but addresses remain tied to object lifetime.
- `get_init_data()` returns borrowed metadata initializer bytes, or `nullptr` when absent. It is not runtime static storage or present on every field. For enum constants, check literal status, type, and width before reading; zero is not an absent initializer.
- Raw `get_fields/get_methods` `out_size` is in **bytes**; `out_count` counts written handles. `ci::Result` follows REF's integer convention (`ERROR_NONE == 0`). C++ wrappers allocate DLL-local vectors; no custom visitor is needed.
- `create_action(vmContext, actionType, target, method)` takes a real Method handle, not a C++ function address or a manually replaced delegate slot.
- No API establishes a managed VM scope. Create objects and execute methods within real native lifecycles; a main thread, VM context, bare IPC, or `UpdateBehavior` is not proof of an outer scope. Initialize reference-array elements before first publication to an owner.
- Unresolved optional entries may be null; check services actually required. Successful queries do not imply business, resource, or RF SDK readiness.

Method lookup uses complete signatures, comma spacing, parent lookup, and ref/out element-type rules; it does not perform automatic virtual dispatch or construction. See the [provider header](include/ci/Provider.hpp), [SDK views](include/ci/API.hpp), [ABI tables](include/ci/API.h), and [UDE submission example](https://github.com/kassent/UserDataExtension/blob/main/README.md#10-plugin-developer-api).

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
| `Runtime entries FAILED` | Types or native entries are not ready; check earlier metadata and type-registration logs |
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
