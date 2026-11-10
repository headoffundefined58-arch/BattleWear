# BattleWear - graded, always-attached garment damage for NSUNSC (NUCC / XFBIN / CPK).
#
# Configure + build (VS2022 generator needs no vcvars):
#   cmake -B build -S . -G "Visual Studio 17 2022" -A x64
#   cmake --build build --config Release
#
# Outputs: build/bin/<cfg>/BattleWear.dll  and  build/bin/<cfg>/bw_cli.exe
cmake_minimum_required(VERSION 3.20)
project(BattleWear LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8 /EHsc)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX)
    # 静态 CRT (/MT)：产物不依赖 vcruntime140/msvcp140，注入游戏进程更稳，
    # 也不要求使用者装 VC redist。链接期所有目标必须一致，故在此统一定义。
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    # Release 也带 CodeView：崩溃帧可符号化。/FS 必须有，否则并行编译抢 vc143.pdb (C1041)
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /Zi /FS")
    set(CMAKE_SHARED_LINKER_FLAGS_RELEASE "${CMAKE_SHARED_LINKER_FLAGS_RELEASE} /DEBUG:FULL")
else()
    add_compile_options(-Wall -Wextra)
endif()

set(BW_CORE_SOURCES
    src/config.cpp
    src/damage_model.cpp
    src/costume_registry.cpp
    src/visual_mapper.cpp
    src/invariants.cpp
)

# --- static core: no engine dependency, link this into any loader plugin ---------
add_library(battlewear_core STATIC ${BW_CORE_SOURCES})
target_include_directories(battlewear_core PUBLIC include)

# --- shared core for hot-reload loaders -----------------------------------------
add_library(battlewear_shared SHARED ${BW_CORE_SOURCES})
target_include_directories(battlewear_shared PUBLIC include)
# 注意：Windows 文件名不区分大小写，这里不能叫 battlewear.dll —— 会与插件目标
# BattleWear.dll 撞成同一个文件，两个 target 互抢链接，报 LNK1104。
set_target_properties(battlewear_shared PROPERTIES OUTPUT_NAME battlewear_shared)

# --- JAPI / ModdingAPI plugin DLL -----------------------------------------------
add_library(BattleWear SHARED plugin/bw_plugin.cpp)
target_link_libraries(BattleWear PRIVATE battlewear_core)
target_include_directories(BattleWear PRIVATE plugin include)
set_target_properties(BattleWear PROPERTIES
    PREFIX ""
    OUTPUT_NAME "BattleWear"
    SUFFIX ".dll")

# --- headless conformance driver: same core, full flow + edge cases -------------
add_executable(bw_tests tests/test_main.cpp)
target_link_libraries(bw_tests PRIVATE battlewear_core)
target_include_directories(bw_tests PRIVATE include)
if(MSVC)
    # 测试一次性持有 Config/DamageState/VisualFrame/CostumeHandle 等大对象，
    # 默认 1 MB 栈太紧，显式放大到 8 MB。
    set_target_properties(bw_tests PROPERTIES LINK_FLAGS "/STACK:8388608")
endif()

set(OUTDIR "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")
foreach(t BattleWear battlewear_shared bw_tests)
    set_target_properties(${t} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${OUTDIR}"
                                          LIBRARY_OUTPUT_DIRECTORY "${OUTDIR}")
endforeach()

# --- copy the shipped config next to the binaries -------------------------------
add_custom_command(TARGET BattleWear POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${OUTDIR}/BattleWear"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/config/battlewear.config.json"
            "${OUTDIR}/BattleWear/battlewear.config.json"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/config/costume_bindings.example.json"
            "${OUTDIR}/BattleWear/costume_bindings.json")

enable_testing()
add_test(NAME bw_conformance COMMAND bw_tests)
set_tests_properties(bw_conformance PROPERTIES
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")
