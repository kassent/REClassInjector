enable_language(ASM_MASM)
if(NOT TARGET ClassInjectorCore)
    # MHWILDS_PLUGIN_FILTER=HIDWrapper 时也配置其静态核心依赖。
    add_subdirectory(${CMAKE_SOURCE_DIR}/plugins/ClassInjector ${CMAKE_BINARY_DIR}/plugins/ClassInjector)
endif()

# 显式离线加载回归：在临时目录启动，不接触游戏或自动部署 provider。
add_executable(HIDWrapperTests EXCLUDE_FROM_ALL tests/HIDWrapperTests.cpp)
target_compile_definitions(HIDWrapperTests PRIVATE NOMINMAX)
target_compile_options(HIDWrapperTests PRIVATE /utf-8)
add_library(HIDWrapper SHARED src/hid.cpp hid.asm hid.def)
target_link_libraries(HIDWrapper PRIVATE ClassInjectorCore)
target_compile_definitions(HIDWrapper PRIVATE NOMINMAX)
target_compile_options(HIDWrapper PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/utf-8>)
set_target_properties(HIDWrapper PROPERTIES OUTPUT_NAME hid)
option(HID_WRAPPER_DEPLOY "Deploy the integrated hid.dll after building" ON)
set(HID_WRAPPER_DEPLOY_ROOT ""
    CACHE PATH "Development game root; override explicitly for final delivery")
if(HID_WRAPPER_DEPLOY)
    if(NOT HID_WRAPPER_DEPLOY_ROOT)
        message(FATAL_ERROR "Set HID_WRAPPER_DEPLOY_ROOT explicitly to enable deployment")
    endif()
    add_custom_command(TARGET HIDWrapper POST_BUILD
        COMMAND pwsh -NoLogo -NoProfile -File ${CMAKE_CURRENT_SOURCE_DIR}/tools/deploy_hid.ps1
            -Source $<TARGET_FILE:HIDWrapper> -GameRoot ${HID_WRAPPER_DEPLOY_ROOT}
        VERBATIM)
endif()
