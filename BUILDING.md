# Building from source / 从源码构建

## Checkout

```powershell
git clone --recurse-submodules https://github.com/kassent/REClassInjector.git
cd REClassInjector
```

For an existing clone / 已有克隆：

```powershell
git submodule update --init --recursive
```

`common/` is pinned to a commit in [WildsCommonLib](https://github.com/kassent/WildsCommonLib), not a copied directory or an automatically moving branch. Updating common requires testing and committing the changed gitlink in this repository.

`common/` 通过 submodule 固定公共库的具体提交；公共库更新不会自动改变已发布版本。本仓库只保留对应插件及必要构建文件，不引入原开发仓库或其它插件历史。

## Requirements / 环境

- Windows x64; Visual Studio 2022 C++ tools and Windows SDK.
- CMake 3.21+; vcpkg dependencies: spdlog, fmt, nlohmann-json and zlib.
- Set `VCPKG_ROOT` to your vcpkg checkout. No developer-specific path is embedded in the CMake files.
- Runtime support is Monster Hunter Wilds / TDB81; offline builds and tests do not prove game-version compatibility.

```powershell
cmake -S . -B build -A x64 `
  "-DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
  -DVCPKG_HIDWrapper_TRIPLET=x64-windows-static
cmake --build build --config Release --target HIDWrapper
cmake --build build --config Release --target check
```

`check` builds and runs the included offline tests. Tests use temporary fixtures and do not launch the game. The default configuration **never deploys to a game directory**.

`check` 会构建并执行随包离线测试，不启动游戏、不自动部署。构建产物在 `build/bin/Release/`；开发验证或最终部署由使用者显式复制到相应游戏相对路径。

## Layout / 目录

```text
common/                         WildsCommonLib submodule
include/ClassProvider.h          convenient public SDK entry
plugins/ClassInjector/           core, SDK, examples and offline fixtures
plugins/HIDWrapper/              integrated hid.dll startup entry
```

The deliverable is the integrated `hid.dll`, not a standalone `ClassInjector.dll`. `include/ClassProvider.h` and `plugins/ClassInjector/include/ClassProvider.h` are identical SDK copies for existing links and build layout. The offline suite includes synthetic metadata, field layout, enums, provider ABI rejection, HID forwarding and common contracts. Captured game snapshots and live-game resource fixtures are not distributed.

运行产物为集成宿主 `hid.dll`；示例、诊断与测试 Provider 为显式构建目标，不随生产构建自动部署。

## Source synchronization / 源码同步

Production C++/headers and fixtures are copied without behavioral changes. Public root CMake files remove machine-specific toolchain/deployment assumptions. The common library has independent history and is shared only through the pinned submodule.

运行实现不做另一套重写。公开构建入口移除了开发机固定路径和默认自动部署；公共库只有一份独立 Git 历史。`SOURCE_SYNC.json` records upstream source identities and file hashes; build outputs, logs, game/TDB dumps, resources and private review documents are excluded.

Third-party notices already present in bundled dependencies are preserved. No new license is assigned to original code by this source organization; select the project license separately before advertising licensing terms.
