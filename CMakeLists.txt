cmake_minimum_required(VERSION 3.21)
project(jisu_browser VERSION 0.2.0 LANGUAGES CXX RC)

if(NOT MSVC)
  message(FATAL_ERROR "只支持 MSVC 构建（GitHub Actions windows-latest）")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# 静态 CRT：不依赖 VC++ 运行库，启动时也少加载两个 DLL
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(WEBVIEW2_DIR "" CACHE PATH "解压后的 Microsoft.Web.WebView2 NuGet 包目录")
set(WEBVIEW2_VERSION "unknown" CACHE STRING "WebView2 SDK 版本，写进启动时间线")
set(JISU_COMMIT "local" CACHE STRING "构建所用提交，写进启动时间线")

set(WEBVIEW2_NATIVE "${WEBVIEW2_DIR}/build/native")
if(NOT EXISTS "${WEBVIEW2_NATIVE}/include/WebView2.h")
  message(FATAL_ERROR "WEBVIEW2_DIR 没有指向 Microsoft.Web.WebView2 包：${WEBVIEW2_DIR}")
endif()
if(CMAKE_GENERATOR_PLATFORM MATCHES "^[Aa][Rr][Mm]64$")
  set(WEBVIEW2_ARCH arm64)
else()
  set(WEBVIEW2_ARCH x64)
endif()

# 新标签页内置一份进 exe（--ntp inline 用）；默认走随包的 ntp\ 目录
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/ntp/index.html" JISU_NTP_HTML)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/ntp/index.html")
string(LENGTH "${JISU_NTP_HTML}" JISU_NTP_HTML_BYTES)
if(JISU_NTP_HTML_BYTES GREATER 15000)
  # MSVC 单个字符串字面量上限约 16KB（C2026），页面再大就要换内置方式
  message(FATAL_ERROR "ntp/index.html 有 ${JISU_NTP_HTML_BYTES} 字节，超出内置字面量的安全上限 15000")
endif()
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/ntp_html.h"
     CONTENT "#pragma once\n// 由 CMake 从 ntp/index.html 生成，不要手改。\ninline constexpr char kNtpHtml[] = R\"JISU_NTP(@JISU_NTP_HTML@)JISU_NTP\";\n"
     @ONLY)

add_executable(jisu-browser WIN32
  src/main.cpp
  src/common.cpp
  src/options.cpp
  src/metrics.cpp
  src/palette.cpp
  src/graphics.cpp
  src/json.cpp
  src/net.cpp
  src/bookmarks.cpp
  src/agent_bridge.cpp
  src/browser_window.cpp
  src/browser_profile.cpp
  src/favicons.cpp
  src/menu_icons.cpp
  res/app.rc
  res/app.manifest)

target_include_directories(jisu-browser PRIVATE
  src
  "${WEBVIEW2_NATIVE}/include"
  "${CMAKE_CURRENT_BINARY_DIR}/generated")

target_compile_definitions(jisu-browser PRIVATE
  UNICODE _UNICODE NOMINMAX
  _WIN32_WINNT=0x0A00 WINVER=0x0A00
  JISU_COMMIT="${JISU_COMMIT}"
  JISU_VERSION="${PROJECT_VERSION}"
  JISU_WEBVIEW2_SDK="${WEBVIEW2_VERSION}")

# /Zi + /DEBUG：生成 PDB，采 ETW 时用来解析外壳自己的调用栈（PDB 不随包分发）
target_compile_options(jisu-browser PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus /EHsc /Zi /Gy /Gw)

target_link_libraries(jisu-browser PRIVATE
  "${WEBVIEW2_NATIVE}/${WEBVIEW2_ARCH}/WebView2LoaderStatic.lib"
  version d2d1 dwrite d3d11 dwmapi winhttp shell32 ole32 advapi32 delayimp windowscodecs)

# 首帧路径之外的 DLL 延迟到第一次调用才加载：D2D/DWrite/D3D 由后台预热线程触发，与建窗并行；
# WinHTTP（智能体桥）在内核就绪后才用，shell32 只在菜单「设为默认浏览器」时用。
# 都不在 wWinMain 之前由加载器串行装入。WebView2 加载器静态链接，不需要 WebView2Loader.dll。
target_link_options(jisu-browser PRIVATE
  /DELAYLOAD:d2d1.dll /DELAYLOAD:dwrite.dll /DELAYLOAD:d3d11.dll /DELAYLOAD:dwmapi.dll
  /DELAYLOAD:winhttp.dll /DELAYLOAD:shell32.dll
  /DEBUG /OPT:REF /OPT:ICF)

enable_testing()
add_executable(tab-policy-test tests/tab-policy-test.cpp)
target_include_directories(tab-policy-test PRIVATE src)
add_test(NAME native-tab-policy COMMAND tab-policy-test)
add_executable(menu-icons-test tests/menu-icons-test.cpp src/menu_icons.cpp)
target_include_directories(menu-icons-test PRIVATE src)
target_compile_definitions(menu-icons-test PRIVATE UNICODE _UNICODE NOMINMAX)
target_compile_options(menu-icons-test PRIVATE /utf-8)
target_link_libraries(menu-icons-test PRIVATE windowscodecs ole32 gdi32 user32)
add_test(NAME native-bookmark-menu-icons COMMAND menu-icons-test "${CMAKE_CURRENT_SOURCE_DIR}/res/app.ico")

add_custom_command(TARGET jisu-browser POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/ntp" "$<TARGET_FILE_DIR:jisu-browser>/ntp")
