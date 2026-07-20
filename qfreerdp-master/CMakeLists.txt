cmake_minimum_required(VERSION 3.16)
project(qf-client LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CLIPRDR_REF_DIR "${CMAKE_CURRENT_SOURCE_DIR}/ref-tmp")

# 自动处理 Qt 的元对象和资源系统
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# 使用自编译的 FreeRDP 3.28.0（而非系统 freerdp3-dev 包）
list(PREPEND CMAKE_PREFIX_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../freerdp-3.28.0/install")

# 1. 寻找 Qt 6 的核心与 QML 相关模块
find_package(Qt6 REQUIRED COMPONENTS Core Gui Qml Quick)
find_package(FreeRDP 3 REQUIRED)
find_package(WinPR 3 REQUIRED)
find_package(FreeRDP-Client 3 REQUIRED)
find_package(spdlog REQUIRED)
message(STATUS "FreeRDP_VERSION: ${FreeRDP_VERSION}")

# 2. 定义可执行目标（使用 Qt 专属的包装函数）
qt_add_executable(qf-client
    mini-qf-client.cc
    usb-manager.cc
    qf_channel_client_handler.c
    ${CLIPRDR_REF_DIR}/cliprdr_main.c
    ${CLIPRDR_REF_DIR}/cliprdr_format.c
    ${CLIPRDR_REF_DIR}/cliprdr_common.c
)

# 3. 注册 QML 模块
qt_add_qml_module(qf-client
    URI MyTestApp
    VERSION 1.0
    QML_FILES main.qml
    SOURCES rdp-view-item.h
)

# FreeRDP 3 installs headers under include/freerdp3.
target_include_directories(qf-client PRIVATE
    ${FreeRDP_INCLUDE_DIR}
    ${FreeRDP-Client_INCLUDE_DIR}
    ${WinPR_INCLUDE_DIR}
    ${CLIPRDR_REF_DIR}
)

# We need PulseAudio for audio
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(PULSE libpulse)
    if(PULSE_FOUND)
        target_include_directories(qf-client PRIVATE ${PULSE_INCLUDE_DIRS})
        target_link_libraries(qf-client PRIVATE ${PULSE_LIBRARIES})
        target_compile_definitions(qf-client PRIVATE WITH_PULSE=1)
    endif()
endif()

# Clang 21 trips over fmt's consteval format-string path with this distro fmt.
target_compile_definitions(qf-client PRIVATE
    FMT_CONSTEVAL=
)

# ref-tmp C files use nullptr (C23 keyword)
set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)

# 5. GLESv2 for rendering
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(GLESv2 glesv2)
    if(GLESv2_FOUND)
        target_include_directories(qf-client PRIVATE ${GLESv2_INCLUDE_DIRS})
        target_link_libraries(qf-client PRIVATE ${GLESv2_LIBRARIES})
        message(STATUS "GLESv2 found — GL rendering enabled")
    else()
        message(WARNING "GLESv2 not found — GL rendering disabled")
    endif()
endif()

# 7. libusb for USB device enumeration (used by urbdrc + USBManager)
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(LIBUSB libusb-1.0)
    if(LIBUSB_FOUND)
        target_include_directories(qf-client PRIVATE ${LIBUSB_INCLUDE_DIRS})
        target_link_libraries(qf-client PRIVATE ${LIBUSB_LIBRARIES})
        target_compile_definitions(qf-client PRIVATE WITH_LIBUSB=1)
        message(STATUS "libusb found: ${LIBUSB_LIBRARIES}")
    else()
        message(WARNING "libusb-1.0 not found — USB device enumeration disabled")
    endif()
endif()

# 8. Link Qt modules (must be last to satisfy linker ordering)
target_link_libraries(qf-client PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    freerdp
    freerdp-client
    winpr
    spdlog::spdlog
)
