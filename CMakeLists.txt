# 文件作用：定义 Codex Status Light 的 CMake 构建入口
# 职责范围：
# 1. 配置 C++17 Windows 状态解析和托盘工具
# 2. 生成 StatusLight 可执行文件
#
# 不负责：
# - 安装第三方依赖
# - 生成托盘发布版本
#
# 维护说明：
# - P2 阶段包含控制台解析能力和 Win32 托盘显示
cmake_minimum_required(VERSION 3.12)

project(CodexStatusLight LANGUAGES CXX)

if (MSVC)
    foreach(flag_var
        CMAKE_CXX_FLAGS
        CMAKE_CXX_FLAGS_DEBUG
        CMAKE_CXX_FLAGS_RELEASE
        CMAKE_CXX_FLAGS_MINSIZEREL
        CMAKE_CXX_FLAGS_RELWITHDEBINFO)
        if (${flag_var} MATCHES "/MD")
            string(REPLACE "/MDd" "/MTd" ${flag_var} "${${flag_var}}")
            string(REPLACE "/MD" "/MT" ${flag_var} "${${flag_var}}")
        endif()
    endforeach()
endif()

add_executable(StatusLight
    src/main.cpp
    src/CodexRootLocator.cpp
    src/DirectoryWatcher.cpp
    src/InspectCommand.cpp
    src/StatusCommand.cpp
    src/StatusReader.cpp
    src/ProcessMonitor.cpp
    src/IconRenderer.cpp
    src/TrayApp.cpp
    src/celebration/CelebrationController.cpp
    src/celebration/CelebrationPolicy.cpp
    src/celebration/FireworkAnimator.cpp
    src/celebration/FireworkOverlayWindow.cpp
    src/celebration/FireworkRenderer.cpp
    src/celebration/FireworkScene.cpp
    src/celebration/TrayAnchorLocator.cpp
    src/JsonValue.cpp
    src/web/ChatGptConversationStore.cpp
    src/web/ChatGptAccountAggregator.cpp
    src/web/WebDiagnostics.cpp
    src/web/WebDebugLog.cpp
    src/web/NativeHostProtocol.cpp
    src/web/NativeHostRegistration.cpp
    src/web/NativeMessagingHost.cpp
    src/web/WebBridgePipeServer.cpp
    src/web/WebSourceController.cpp
    src/web/WebSelfTest.cpp
)

target_compile_features(StatusLight PRIVATE cxx_std_17)

target_compile_definitions(StatusLight PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

target_link_libraries(StatusLight PRIVATE advapi32)

if (MSVC)
    target_compile_options(StatusLight PRIVATE /W4 /permissive- /utf-8)
    set_target_properties(StatusLight PROPERTIES
        WIN32_EXECUTABLE TRUE
        LINK_FLAGS "/ENTRY:wmainCRTStartup"
    )
endif()

add_custom_command(TARGET StatusLight POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_SOURCE_DIR}/dist"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:StatusLight>" "${CMAKE_SOURCE_DIR}/dist/StatusLight.exe"
)
