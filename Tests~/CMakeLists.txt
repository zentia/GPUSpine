# =====================================================================
# GPUSpine 测试
# ---------------------------------------------------------------------
# 全部不依赖任何引擎（ZEngine / Unity）——核心只吃标准库。
# 测试夹具用手写 json（比手写二进制好读），所以测试用的核心副本额外带一个
# json 解析器（GPUSPINE_ENABLE_TEST_JSON），需要 rapidjson：
#     -DGPUSPINE_RAPIDJSON_DIR=<含 rapidjson/document.h 的 include 目录>
# 产品库 GPUSpine::Core 不带 json、不依赖 rapidjson。
#
#   cmake -S . -B build -DGPUSPINE_BUILD_TESTS=ON -DGPUSPINE_RAPIDJSON_DIR=.../rapidjson/include
#   cmake --build build --config Debug
#   ctest --test-dir build -C Debug
# =====================================================================

if(NOT GPUSPINE_RAPIDJSON_DIR OR NOT EXISTS "${GPUSPINE_RAPIDJSON_DIR}/rapidjson/document.h")
    message(FATAL_ERROR "GPUSpine 测试需要 -DGPUSPINE_RAPIDJSON_DIR=<含 rapidjson/document.h 的目录>")
endif()

set(_core_dir "${CMAKE_CURRENT_SOURCE_DIR}/../Runtime/GPUSpine")
if(MSVC)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
endif()

# 核心 + 测试 json 解析器。独立副本，不污染产品库。
# 文件名都是 SpineGpu*.cpp 但在不同 target 里各编一份——这是故意的：
# 这些测试要么数分配（不能链别人的 operator new），要么要 json 入口。
add_library(GPUSpineTestable STATIC
    ${_core_dir}/SpineGpuImporter.cpp
    ${_core_dir}/SpineGpuAnimSolver.cpp
    ${_core_dir}/SpineGpuAnimationState.cpp
    ${_core_dir}/SpineGpuSkeletonState.cpp
    ${_core_dir}/SpineGpuPoseSolver.cpp
    ${_core_dir}/SpineGpuClipper.cpp
    ${_core_dir}/SpineGpuBatch.cpp
    ${_core_dir}/SpineGpuAssetFile.cpp)
target_include_directories(GPUSpineTestable PUBLIC "${_core_dir}")
target_include_directories(GPUSpineTestable PRIVATE "${GPUSPINE_RAPIDJSON_DIR}")
target_compile_definitions(GPUSpineTestable PUBLIC GPUSPINE_ENABLE_TEST_JSON=1)
set_target_properties(GPUSpineTestable PROPERTIES CXX_STANDARD 20 FOLDER "GPUSpine/Tests")
if(MSVC)
    target_compile_options(GPUSpineTestable PRIVATE /W4 /utf-8)
endif()

function(gpuspine_add_test name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE GPUSpineTestable)
    target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    set_target_properties(${name} PROPERTIES CXX_STANDARD 20 FOLDER "GPUSpine/Tests")
    if(MSVC)
        target_compile_options(${name} PRIVATE /W4 /utf-8)
    endif()
endfunction()

# ---- 导入器 / 容器 / 解算器自测 ----
gpuspine_add_test(SpineGpuImportTest SpineGpuImportTest.cpp)
add_test(NAME SpineGpuImportTest COMMAND SpineGpuImportTest)

# ---- 热路径：稳态每帧 0 堆分配（规格 §17）----
# 这条断言要数全局 operator new；MSVC Debug 的迭代器调试会给每个容器多发一次 16 B 分配，
# 所以整目标（含它自己编的核心副本）统一 _ITERATOR_DEBUG_LEVEL=0，不与别的库混链。
add_executable(SpineGpuHotLoopTest SpineGpuHotLoopTest.cpp
    ${_core_dir}/SpineGpuImporter.cpp
    ${_core_dir}/SpineGpuAnimSolver.cpp
    ${_core_dir}/SpineGpuAnimationState.cpp
    ${_core_dir}/SpineGpuSkeletonState.cpp
    ${_core_dir}/SpineGpuPoseSolver.cpp
    ${_core_dir}/SpineGpuClipper.cpp
    ${_core_dir}/SpineGpuBatch.cpp)
target_include_directories(SpineGpuHotLoopTest PRIVATE "${_core_dir}" "${CMAKE_CURRENT_SOURCE_DIR}" "${GPUSPINE_RAPIDJSON_DIR}")
target_compile_definitions(SpineGpuHotLoopTest PRIVATE GPUSPINE_ENABLE_TEST_JSON=1)
set_target_properties(SpineGpuHotLoopTest PROPERTIES CXX_STANDARD 20 FOLDER "GPUSpine/Tests")
if(MSVC)
    target_compile_options(SpineGpuHotLoopTest PRIVATE /W4 /utf-8)
    target_compile_definitions(SpineGpuHotLoopTest PRIVATE _ITERATOR_DEBUG_LEVEL=0)
endif()
add_test(NAME SpineGpuHotLoopTest COMMAND SpineGpuHotLoopTest)

# ---- 与参考 spine-cpp 3.8 对拍 / 性能基准（可选）----
# 参考实现不随本仓库：-DGPUSPINE_REF_ROOT=<spine-cpp 根，含 src/spine/Skeleton.cpp>
set(GPUSPINE_REF_ROOT "" CACHE PATH "Root of the reference spine-cpp (3.8) sources")
if(GPUSPINE_REF_ROOT AND EXISTS "${GPUSPINE_REF_ROOT}/src/spine/Skeleton.cpp")
    # Atlas.cpp / AtlasAttachmentLoader.cpp 排除：吃 Unity 引擎内部头，独立编不过。
    # 对拍只比骨骼，用自己的空 AttachmentLoader；SkeletonBinary 构造函数里 new 的
    # AtlasAttachmentLoader 由测试内的“永不调用”桩实现解析。
    file(GLOB _ref_src_all "${GPUSPINE_REF_ROOT}/src/spine/*.cpp")
    set(_ref_src "")
    foreach(_f IN LISTS _ref_src_all)
        get_filename_component(_n "${_f}" NAME)
        if(NOT _n STREQUAL "Atlas.cpp" AND NOT _n STREQUAL "AtlasAttachmentLoader.cpp")
            list(APPEND _ref_src "${_f}")
        endif()
    endforeach()
    add_library(SpineReference STATIC EXCLUDE_FROM_ALL ${_ref_src})
    set_target_properties(SpineReference PROPERTIES CXX_STANDARD 20 FOLDER "GPUSpine/Tests")
    # 按 config 改名：历史上上层工程把 ARCHIVE_OUTPUT_DIRECTORY_<CONFIG> 统一设成
    # 同一个扁平目录，Debug/Release 的 .lib 会互相覆盖 ⇒ LNK2038。上层已不再覆盖
    # 按 config 的变量（多配置生成器会自行追加 <Config>），这里保留改名仅作防御。
    set_target_properties(SpineReference PROPERTIES OUTPUT_NAME "SpineReference_$<CONFIG>")
    # 参考实现内部互相 include 写的是 `Modules/Spine/spine-cpp/include/spine/X.h`
    # （相对 Unity 引擎根）⇒ 除 include/ 外还要挂引擎根（spine-cpp 的上三级）。
    get_filename_component(_ref_engine_root "${GPUSPINE_REF_ROOT}" DIRECTORY)
    get_filename_component(_ref_engine_root "${_ref_engine_root}" DIRECTORY)
    get_filename_component(_ref_engine_root "${_ref_engine_root}" DIRECTORY)
    target_include_directories(SpineReference PUBLIC "${GPUSPINE_REF_ROOT}/include" "${_ref_engine_root}")
    target_compile_definitions(SpineReference PUBLIC SPINE_USE_STD_FUNCTIONS=0)
    if(MSVC)
        target_compile_options(SpineReference PRIVATE /W0)
    endif()

    gpuspine_add_test(SpineGpuRefParityTest SpineGpuRefParityTest.cpp)
    target_link_libraries(SpineGpuRefParityTest PRIVATE SpineReference)
    # 需要传 OSGame 资产目录作参数 ⇒ 不进 ctest，手动跑：SpineGpuRefParityTest <Assets 目录>

    # Release 才有意义；不进 ctest（是测量不是断言）。
    gpuspine_add_test(SpineGpuBenchmark SpineGpuBenchmark.cpp)
    target_link_libraries(SpineGpuBenchmark PRIVATE SpineReference)
    set_target_properties(SpineGpuBenchmark PROPERTIES EXCLUDE_FROM_ALL TRUE)
    message(STATUS "GPUSpine: 参考对拍已注册 (ref: ${GPUSPINE_REF_ROOT})")
else()
    message(STATUS "GPUSpine: 未提供 GPUSPINE_REF_ROOT ⇒ 不注册参考对拍与基准")
endif()

# ---------------------------------------------------------------------
# 曾经这里有"跨语言对拍"（C++ 写 → C# 读 → C# 写，必须逐字节相同），2026-10-30
# 随 Runtime/CSharp/ 一起删除：Unity 侧的导入器改成引擎内 C++，直接调本 core，
# 不再需要"复刻一份 C# 再对拍"。跨端一致性现在由"同一个子模块 = 同一份代码"
# 结构性保证（与 GPUParticle 同一条理由）。
# ---------------------------------------------------------------------
