cmake_minimum_required(VERSION 3.14)

project(scheduler LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 让 clangd 能直接在项目根目录读到最新的 compile_commands.json。
# CMake 只会把它生成到 build/ 目录下，这里建一个指向 build/ 的符号链接，
# 避免根目录残留旧版本的 compile_commands.json 误导 clangd（例如拿不到
# 新加的 OpenCV -I 路径，从而头文件报 "找不到"）。
if(PROJECT_SOURCE_DIR AND NOT PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
    # 若根目录已有普通文件/旧链接，先清掉再建，避免 CREATE_LINK 被拒
    file(REMOVE "${PROJECT_SOURCE_DIR}/compile_commands.json")
    file(CREATE_LINK
        "${PROJECT_BINARY_DIR}/compile_commands.json"
        "${PROJECT_SOURCE_DIR}/compile_commands.json"
        SYMBOLIC
    )
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(GNUInstallDirs)

# ============================================================
# 第三方依赖
# ------------------------------------------------------------
# OpenCV：图像/视觉相关数据结构（cv::Mat 等）会在 DataHub / TaskFlow 中
# 被作为 DataType 使用，因此作为 scheduler 库的 PUBLIC 依赖暴露给下游。
# 若只想给个别组件使用，可改成 PRIVATE 链接。
#
# 可通过 -DOpenCV_DIR=/path/to/opencv/build 指定非标准安装路径；
# 需要特定组件时使用：find_package(OpenCV REQUIRED COMPONENTS core imgproc ...)
# ============================================================
find_package(OpenCV REQUIRED)
message(STATUS "Found OpenCV ${OpenCV_VERSION}")
message(STATUS "  OpenCV_INCLUDE_DIRS = ${OpenCV_INCLUDE_DIRS}")
message(STATUS "  OpenCV_LIBS         = ${OpenCV_LIBS}")

# ============================================================
# 头文件分类
# ------------------------------------------------------------
# SCHEDULER_PRIVATE_HEADERS  仅供内部使用的头文件
#   - 只参与库本身（src/*.cpp）和同项目其它 target（如 examples）的构建
#   - 不会被 install，下游使用方看不到这些头文件
#
# SCHEDULER_PUBLIC_HEADERS   对内对外通用的公共头文件
#   - 同时参与内部构建，并随 .so 一起安装到
#     ${CMAKE_INSTALL_INCLUDEDIR}/scheduler/
#   - 新增对外头文件时在这个列表里追加
# ============================================================
set(SCHEDULER_PRIVATE_HEADERS
    include/DataHub.h
    include/Task.h
    include/Type.h
)

set(SCHEDULER_PUBLIC_HEADERS
    # 目前没有对外头文件；新增时追加，例如：
    # include/Scheduler.h
)

# ---- 源文件 ----
file(GLOB_RECURSE SCHEDULER_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)

# ============================================================
# 构建目标
#   - 有 src/*.cpp 时编译为 SHARED 库（.so）
#   - src/ 为空时退化为 INTERFACE 库，避免 CMake 推断链接语言失败
# ============================================================
if(SCHEDULER_SOURCES)
    add_library(scheduler SHARED ${SCHEDULER_SOURCES})
    set_target_properties(scheduler PROPERTIES
        POSITION_INDEPENDENT_CODE ON
    )

    # 库自身源码可以看到全部头文件（含内部头文件）
    # 对外（BUILD_INTERFACE/INSTALL_INTERFACE）只暴露安装后的 include 目录，
    # 下游拿到的是被安装出去的 public 头文件，看不到内部头文件
    target_include_directories(scheduler
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
        INTERFACE
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    )

    # OpenCV：PUBLIC 传递，库自身与下游都能用
    target_include_directories(scheduler PUBLIC ${OpenCV_INCLUDE_DIRS})
    target_link_libraries(scheduler PUBLIC ${OpenCV_LIBS})
else()
    message(STATUS
        "scheduler: src/ 下没有 .cpp，暂时按 INTERFACE 库构建；"
        "添加源文件后会自动编译为 SHARED (.so)")
    add_library(scheduler INTERFACE)
    target_include_directories(scheduler
        INTERFACE
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    )

    # INTERFACE 库只能通过 INTERFACE 方式传递依赖
    target_include_directories(scheduler INTERFACE ${OpenCV_INCLUDE_DIRS})
    target_link_libraries(scheduler INTERFACE ${OpenCV_LIBS})
endif()

# ============================================================
# 安装
#   通过 CMAKE_INSTALL_PREFIX 控制安装根目录，默认 /usr/local，
#   build.sh 会把它指向 ${SCRIPT_DIR}/install。
# ============================================================
if(SCHEDULER_SOURCES)
    install(TARGETS scheduler
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
else()
    install(TARGETS scheduler)
endif()

# 只安装公共头文件；内部头文件不会被安装
if(SCHEDULER_PUBLIC_HEADERS)
    install(FILES ${SCHEDULER_PUBLIC_HEADERS}
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/scheduler
    )
endif()

# ---- examples ----
option(SCHEDULER_BUILD_EXAMPLES "Build example programs" ON)
if(SCHEDULER_BUILD_EXAMPLES)
    add_executable(example examples/example.cpp
        include/Scheduler.h
        include/Worker.h
        include/TaskContext.h)
    target_link_libraries(example PRIVATE scheduler)
    # examples 作为同项目内部产物，允许访问全部头文件（含内部头文件）
    target_include_directories(example
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    find_package(Threads REQUIRED)
    target_link_libraries(example PRIVATE Threads::Threads)
endif()
