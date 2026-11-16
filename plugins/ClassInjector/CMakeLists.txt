include(FetchContent)
FetchContent_Declare(tomlplusplus
    URL https://codeload.github.com/marzer/tomlplusplus/tar.gz/refs/tags/v3.4.0
    URL_HASH SHA256=8517f65938a4faae9ccf8ebb36631a38c1cadfb5efa85d9a72e15b9e97d25155
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(tomlplusplus)

# 生产核心只链接进 hid.dll；测试 EXE 各自使用隔离实例，不再生成独立 host DLL。
add_library(ClassInjectorCore STATIC EXCLUDE_FROM_ALL
    src/ProviderConfig.cpp src/ClassProviders.cpp src/TypeDatabaseBuilder.cpp src/ClassFields.cpp src/ClassMethods.cpp src/GenericTypes.cpp src/EnumMembers.cpp
    src/MethodTrampolines.cpp src/InvokeAdapters.cpp src/ClassVirtualTable.cpp
    src/TypeDatabase.cpp src/ProviderMetadataAPI.cpp src/ProviderObjectAPI.cpp src/ProviderResourceAPI.cpp src/ClassProviderContext.cpp src/TypeInjection.cpp)
target_include_directories(ClassInjectorCore PUBLIC include PRIVATE src)
target_link_libraries(ClassInjectorCore PUBLIC common_lib PRIVATE tomlplusplus::tomlplusplus)
target_compile_definitions(ClassInjectorCore PRIVATE NOMINMAX)
target_compile_options(ClassInjectorCore PRIVATE /utf-8)

# Examples and fixtures are explicit build targets; production builds do not
# deploy declarations or test providers into the user's game.
add_library(CIExampleProvider SHARED EXCLUDE_FROM_ALL examples/CppMethods.cpp)
set_target_properties(CIExampleProvider PROPERTIES OUTPUT_NAME CIExampleProvider_ci)
add_library(ManagedTypes_ci SHARED EXCLUDE_FROM_ALL examples/CppMethods.cpp)
add_library(ClassInjectorTests_ci SHARED EXCLUDE_FROM_ALL tests/ClassInjectorTypes.cpp)
add_library(InterfaceTests_ci SHARED EXCLUDE_FROM_ALL tests/InterfaceTypes.cpp)
add_library(ClassInjectorClosedTypes_ci SHARED EXCLUDE_FROM_ALL tests/ClosedTypes.cpp)
foreach(PROVIDER CIExampleProvider ManagedTypes_ci ClassInjectorTests_ci InterfaceTests_ci ClassInjectorClosedTypes_ci)
    target_include_directories(${PROVIDER} PRIVATE include ${CMAKE_SOURCE_DIR}/common/include)
    target_compile_options(${PROVIDER} PRIVATE /utf-8)
endforeach()

# 测试输出中也保持一份 TOML 对应一个 DLL；不向游戏自动部署测试清单。
function(CopyCIManifest TARGET MANIFEST DIRECTORY)
    set(DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/fixtures/$<CONFIG>/${DIRECTORY}")
    set_property(TARGET ${TARGET} APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${MANIFEST}")
    add_custom_command(TARGET ${TARGET} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${DESTINATION}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:${TARGET}> "${DESTINATION}/"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/${MANIFEST}" "${DESTINATION}/$<TARGET_FILE_BASE_NAME:${TARGET}>.toml")
endfunction()
CopyCIManifest(CIExampleProvider examples/sample.toml basic)
CopyCIManifest(ManagedTypes_ci examples/ManagedTypes_ci.toml types)
CopyCIManifest(ClassInjectorTests_ci tests/ClassInjectorTests_ci.toml class)
CopyCIManifest(InterfaceTests_ci tests/InterfaceTests_ci.toml interface)
CopyCIManifest(ClassInjectorClosedTypes_ci tests/ClosedTypes_ci.toml closed)

# 可选 RF/Lua 诊断通过现有 provider Context 访问已发布的 TDB，不链接第二份核心。
add_library(ClassInjectorDiagnostics SHARED EXCLUDE_FROM_ALL tests/Diagnostics.cpp)
target_include_directories(ClassInjectorDiagnostics PRIVATE include ${CMAKE_SOURCE_DIR}/common/include/sol2/single/include)
target_link_libraries(ClassInjectorDiagnostics PRIVATE common_lib)
target_compile_definitions(ClassInjectorDiagnostics PRIVATE NOMINMAX)
target_compile_options(ClassInjectorDiagnostics PRIVATE /utf-8)
CopyCIManifest(ClassInjectorDiagnostics tests/Diagnostics.toml diagnostics)

# 显式构造 ABI / 真机反射探针，与 host 隔离；同目录 TOML/DLL 供 CI 和 RF 复用一个模块。
add_library(ClassInjectorConstructorTests SHARED EXCLUDE_FROM_ALL tests/ConstructorTypes.cpp)
target_include_directories(ClassInjectorConstructorTests PRIVATE include ${CMAKE_SOURCE_DIR}/common/include/sol2/single/include)
target_link_libraries(ClassInjectorConstructorTests PRIVATE common_lib)
target_compile_options(ClassInjectorConstructorTests PRIVATE /utf-8)
target_compile_definitions(ClassInjectorConstructorTests PRIVATE NOMINMAX)
CopyCIManifest(ClassInjectorConstructorTests tests/ConstructorTypes.toml constructors)

# 浮点返回 ABI / 反射测试独立部署，不向生产 host 添加探针。
add_library(ClassInjectorFloatingReturnTests SHARED EXCLUDE_FROM_ALL tests/FloatingReturnTypes.cpp)
target_include_directories(ClassInjectorFloatingReturnTests PRIVATE src include ${CMAKE_SOURCE_DIR}/common/include/sol2/single/include)
target_link_libraries(ClassInjectorFloatingReturnTests PRIVATE common_lib)
target_compile_options(ClassInjectorFloatingReturnTests PRIVATE /utf-8)
target_compile_definitions(ClassInjectorFloatingReturnTests PRIVATE NOMINMAX)
CopyCIManifest(ClassInjectorFloatingReturnTests tests/FloatingReturnTypes.toml floating-returns)

# 引用赋值复用 common_lib；所有示例/测试 DLL 的 SDK 初始化由调用方完成。
target_link_libraries(ClassInjectorTests_ci PRIVATE common_lib)
target_link_libraries(ClassInjectorClosedTypes_ci PRIVATE common_lib)
add_library(ClassInjectorTestsBootstrap SHARED EXCLUDE_FROM_ALL tests/ProviderBootstrap.cpp)
target_include_directories(ClassInjectorTestsBootstrap PRIVATE ${CMAKE_SOURCE_DIR}/common/include)
target_compile_options(ClassInjectorTestsBootstrap PRIVATE /utf-8)
add_library(ClassInjectorClosedBootstrap SHARED EXCLUDE_FROM_ALL tests/ClosedBootstrap.cpp)
target_include_directories(ClassInjectorClosedBootstrap PRIVATE ${CMAKE_SOURCE_DIR}/common/include)
target_compile_options(ClassInjectorClosedBootstrap PRIVATE /utf-8)

# 显式 RSZ 原生加载探针；不进入生产 host，不自动部署。
add_library(ClassInjectorRszProbe SHARED EXCLUDE_FROM_ALL tests/RszProbe.cpp)
target_link_libraries(ClassInjectorRszProbe PRIVATE common_lib)
target_include_directories(ClassInjectorRszProbe PRIVATE ${CMAKE_SOURCE_DIR}/common/include/sol2/single/include)
target_compile_options(ClassInjectorRszProbe PRIVATE /utf-8)
target_compile_definitions(ClassInjectorRszProbe PRIVATE NOMINMAX)

# 显式 ref 真机探针：只读参数/元数据，不给生产 host 添加诊断接口。
add_library(ClassInjectorRefProbe SHARED EXCLUDE_FROM_ALL tests/RefParameterProbe.cpp)
target_link_libraries(ClassInjectorRefProbe PRIVATE common_lib)
target_include_directories(ClassInjectorRefProbe PRIVATE src include
    ${CMAKE_SOURCE_DIR}/common/include/sol2/single/include)
target_compile_options(ClassInjectorRefProbe PRIVATE /utf-8)
target_compile_definitions(ClassInjectorRefProbe PRIVATE NOMINMAX)

add_executable(ClassInjectorTests EXCLUDE_FROM_ALL
    tests/MetadataTests.cpp tests/InvokeSignatureTests.cpp)
target_include_directories(ClassInjectorTests PRIVATE include src)
target_compile_definitions(ClassInjectorTests PRIVATE NOMINMAX
    CI_EXAMPLES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/examples"
    CI_TEST_MANIFESTS="${CMAKE_CURRENT_BINARY_DIR}/fixtures/$<CONFIG>"
    CI_TEST_PROVIDER="${CMAKE_CURRENT_BINARY_DIR}/fixtures/$<CONFIG>/basic/CIExampleProvider_ci.dll"
    CI_TEST_TYPE_PLUGIN="${CMAKE_CURRENT_BINARY_DIR}/fixtures/$<CONFIG>/types/ManagedTypes_ci.dll"
    CI_TEST_CLASS_PLUGIN="${CMAKE_CURRENT_BINARY_DIR}/fixtures/$<CONFIG>/class/ClassInjectorTests_ci.dll"
    CI_TEST_FLOAT_MANIFEST="${CMAKE_CURRENT_BINARY_DIR}/fixtures/$<CONFIG>/floating-returns/ClassInjectorFloatingReturnTests.toml"
    CI_TEST_INTERFACE_PLUGIN="${CMAKE_CURRENT_BINARY_DIR}/fixtures/$<CONFIG>/interface/InterfaceTests_ci.dll")
add_dependencies(ClassInjectorTests CIExampleProvider ManagedTypes_ci ClassInjectorTests_ci InterfaceTests_ci
    ClassInjectorTestsBootstrap ClassInjectorClosedTypes_ci ClassInjectorFloatingReturnTests)
target_compile_options(ClassInjectorTests PRIVATE /utf-8)
target_link_libraries(ClassInjectorTests PRIVATE ClassInjectorCore)

# Standalone tracked query regression; no provider DLLs or live VM required.
add_executable(ClassInjectorFieldLayoutTests EXCLUDE_FROM_ALL
    tests/FieldLayoutTests.cpp)
target_include_directories(ClassInjectorFieldLayoutTests PRIVATE include src)
target_compile_definitions(ClassInjectorFieldLayoutTests PRIVATE NOMINMAX)
target_compile_options(ClassInjectorFieldLayoutTests PRIVATE /utf-8)
target_link_libraries(ClassInjectorFieldLayoutTests PRIVATE ClassInjectorCore)

# 枚举元数据能力验证，独立于所有敌人插件与资源。
add_executable(ClassInjectorEnumMetadataTests EXCLUDE_FROM_ALL tests/EnumMetadataTests.cpp)
target_include_directories(ClassInjectorEnumMetadataTests PRIVATE include src)
target_compile_definitions(ClassInjectorEnumMetadataTests PRIVATE NOMINMAX)
target_compile_options(ClassInjectorEnumMetadataTests PRIVATE /utf-8)
target_link_libraries(ClassInjectorEnumMetadataTests PRIVATE ClassInjectorCore)

# SDK 混版本测试只加载本地 DLL；不部署或访问游戏。
add_library(ClassInjectorLegacyProvider SHARED EXCLUDE_FROM_ALL tests/IncompatibleProvider.cpp)
add_library(ClassInjectorMismatchedProvider SHARED EXCLUDE_FROM_ALL tests/IncompatibleProvider.cpp)
foreach(PROVIDER ClassInjectorLegacyProvider ClassInjectorMismatchedProvider)
    target_include_directories(${PROVIDER} PRIVATE include)
    target_compile_options(${PROVIDER} PRIVATE /utf-8)
endforeach()
target_compile_definitions(ClassInjectorLegacyProvider PRIVATE CI_TEST_LEGACY_SDK)
add_executable(ClassInjectorProviderAbiTests EXCLUDE_FROM_ALL tests/ProviderAbiTests.cpp)
target_include_directories(ClassInjectorProviderAbiTests PRIVATE include src)
target_compile_definitions(ClassInjectorProviderAbiTests PRIVATE NOMINMAX)
target_compile_options(ClassInjectorProviderAbiTests PRIVATE /utf-8)
target_link_libraries(ClassInjectorProviderAbiTests PRIVATE ClassInjectorCore)
add_dependencies(ClassInjectorProviderAbiTests ClassInjectorLegacyProvider ClassInjectorMismatchedProvider CIExampleProvider)

add_library(ClassInjectorEnumProbe SHARED EXCLUDE_FROM_ALL tests/EnumProbe.cpp)
target_include_directories(ClassInjectorEnumProbe PRIVATE include ${CMAKE_SOURCE_DIR}/common/include/sol2/single/include)
target_compile_definitions(ClassInjectorEnumProbe PRIVATE NOMINMAX)
target_compile_options(ClassInjectorEnumProbe PRIVATE /utf-8)
target_link_libraries(ClassInjectorEnumProbe PRIVATE common_lib)
CopyCIManifest(ClassInjectorEnumProbe tests/EnumProbe.toml enum-members)

# 五参数 JIT/原 recipe 对照，显式部署的独立 provider，不给生产插件增加测试接口。
add_library(ClassInjectorFiveArgumentTests SHARED EXCLUDE_FROM_ALL tests/FiveArgumentTypes.cpp)
target_include_directories(ClassInjectorFiveArgumentTests PRIVATE src include ${CMAKE_SOURCE_DIR}/common/include/sol2/single/include)
target_link_libraries(ClassInjectorFiveArgumentTests PRIVATE common_lib)
target_compile_options(ClassInjectorFiveArgumentTests PRIVATE /utf-8)
target_compile_definitions(ClassInjectorFiveArgumentTests PRIVATE NOMINMAX)
CopyCIManifest(ClassInjectorFiveArgumentTests tests/FiveArgumentTypes.toml five-arguments)

add_library(ClassInjectorSignatureTests SHARED EXCLUDE_FROM_ALL tests/InvokeSignatureTests.cpp)
target_include_directories(ClassInjectorSignatureTests PRIVATE src include ${CMAKE_SOURCE_DIR}/common/include/sol2/single/include)
target_link_libraries(ClassInjectorSignatureTests PRIVATE common_lib)
target_compile_options(ClassInjectorSignatureTests PRIVATE /utf-8)
target_compile_definitions(ClassInjectorSignatureTests PRIVATE NOMINMAX CI_INVOKE_SIGNATURE_PROVIDER)
CopyCIManifest(ClassInjectorSignatureTests tests/InvokeSignatureTypes.toml invoke-signatures)

add_executable(ClassInjectorProviderSDKTests EXCLUDE_FROM_ALL tests/ProviderSDKTests.cpp)
target_include_directories(ClassInjectorProviderSDKTests PRIVATE include src)
target_compile_definitions(ClassInjectorProviderSDKTests PRIVATE NOMINMAX)
target_compile_options(ClassInjectorProviderSDKTests PRIVATE /utf-8)
target_link_libraries(ClassInjectorProviderSDKTests PRIVATE ClassInjectorCore)

add_library(ClassInjectorFinalPhaseProvider SHARED EXCLUDE_FROM_ALL tests/FinalPhaseProvider.cpp)
target_include_directories(ClassInjectorFinalPhaseProvider PRIVATE include)
target_compile_options(ClassInjectorFinalPhaseProvider PRIVATE /utf-8)
add_dependencies(ClassInjectorProviderSDKTests ClassInjectorFinalPhaseProvider)

add_library(ClassInjectorAbiOneProvider SHARED EXCLUDE_FROM_ALL tests/IncompatibleProvider.cpp)
target_include_directories(ClassInjectorAbiOneProvider PRIVATE include)
target_compile_definitions(ClassInjectorAbiOneProvider PRIVATE CI_TEST_ABI_ONE)
target_compile_options(ClassInjectorAbiOneProvider PRIVATE /utf-8)

add_library(ClassInjectorAbiTwoProvider SHARED EXCLUDE_FROM_ALL tests/IncompatibleProvider.cpp)
target_include_directories(ClassInjectorAbiTwoProvider PRIVATE include)
target_compile_definitions(ClassInjectorAbiTwoProvider PRIVATE CI_TEST_ABI_TWO)
target_compile_options(ClassInjectorAbiTwoProvider PRIVATE /utf-8)
add_dependencies(ClassInjectorProviderAbiTests ClassInjectorAbiOneProvider ClassInjectorAbiTwoProvider)
