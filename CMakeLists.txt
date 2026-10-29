# =====================================================================
# GPUSpine — 静态库（核心C++，ZEngine/Unity native plugin 公用）
# =====================================================================
cmake_minimum_required(VERSION 3.23)
project(GPUSpine VERSION 1.0 LANGUAGES CXX)

# ---- 选项 ----
option(GPUSPINE_BUILD_TESTS "构建测试" OFF)

# ---- C++ 标准（C++20：std::u8string / char8_t，SpineGpuAssetFile 的路径转换要用） ----
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---- 核心库（只依赖标准库） ----
add_library(GPUSpine STATIC
    Runtime/GPUSpine/SpineGpuFormat.h
    Runtime/GPUSpine/SpineGpuTintBlack.h
    Runtime/GPUSpine/SpineGpuAssetFile.h
    Runtime/GPUSpine/SpineGpuAssetFile.cpp
    Runtime/GPUSpine/SpineGpuImporter.h
    Runtime/GPUSpine/SpineGpuImporter.cpp
    Runtime/GPUSpine/SpineGpuAnimSolver.h
    Runtime/GPUSpine/SpineGpuAnimSolver.cpp
    Runtime/GPUSpine/SpineGpuAnimationState.h
    Runtime/GPUSpine/SpineGpuAnimationState.cpp
    Runtime/GPUSpine/SpineGpuSkeletonState.h
    Runtime/GPUSpine/SpineGpuSkeletonState.cpp
    Runtime/GPUSpine/SpineGpuPoseSolver.h
    Runtime/GPUSpine/SpineGpuPoseSolver.cpp
    Runtime/GPUSpine/SpineGpuClipper.h
    Runtime/GPUSpine/SpineGpuClipper.cpp
    Runtime/GPUSpine/SpineGpuBatch.h
    Runtime/GPUSpine/SpineGpuBatch.cpp
)

target_include_directories(GPUSpine PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Runtime>
    $<INSTALL_INTERFACE:include>
)

# 链进 .so/.dll（ZRuntime::Shared、Unity native plugin）需要 PIC
set_target_properties(GPUSpine PROPERTIES POSITION_INDEPENDENT_CODE ON FOLDER "GPUSpine")

# ---- 导出别名，让 ZEngine 可以用 GPUSpine::Core 来链接 ----
add_library(GPUSpine::Core ALIAS GPUSpine)

# ---- 测试（需要 -DGPUSPINE_RAPIDJSON_DIR，仅测试夹具用 json；核心本身不依赖 rapidjson） ----
if(GPUSPINE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(Tests~)
endif()
