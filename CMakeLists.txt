cmake_minimum_required(VERSION 3.24)

project(
    EEA_Native
    VERSION 1.0.0
    LANGUAGES CXX
    DESCRIPTION "Dynamic Enhance Enemy Attributes - native SKSE plugin")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# MSVC 运行时库必须与 vcpkg 编出来的 CommonLibSSE-NG 完全一致，
# 否则链接期会报一大片 LNK2038 RuntimeLibrary mismatch + LNK2005 重复定义。
# 规则（严格由 VCPKG_TARGET_TRIPLET 推导，不要再手写死）：
#   x64-windows              -> CommonLibSSE 是 /MD  -> 我们也要 /MD
#   x64-windows-static       -> CommonLibSSE 是 /MT  -> 我们也要 /MT
#   x64-windows-static-md    -> 静态库 + /MD        -> 还是 /MD
if(VCPKG_TARGET_TRIPLET MATCHES "static" AND NOT VCPKG_TARGET_TRIPLET MATCHES "static-md")
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    message(STATUS "EEA: 三元组 ${VCPKG_TARGET_TRIPLET} -> 静态 CRT (/MT)")
else()
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL$<$<CONFIG:Debug>:DebugDLL>")
    message(STATUS "EEA: 三元组 '${VCPKG_TARGET_TRIPLET}' -> 动态 CRT (/MD)")
endif()

# CommonLibSSE-NG (SE) —— 由 vcpkg 端口 commonlibsse-ng-se 提供
find_package(CommonLibSSE CONFIG REQUIRED)

set(EEA_SOURCES
    src/main.cpp
    src/Config.cpp
    src/EnhancementManager.cpp
    src/Menu.cpp
)

add_library(${PROJECT_NAME} SHARED ${EEA_SOURCES})

target_include_directories(${PROJECT_NAME}
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party"
)

target_link_libraries(${PROJECT_NAME} PRIVATE CommonLibSSE::CommonLibSSE)

target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)

target_compile_definitions(${PROJECT_NAME} PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
    UNICODE
    _UNICODE
)

if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE
        /Zc:__cplusplus
        /permissive-
        /W4
        /MP
        /EHsc
        /utf-8
        /wd4200 /wd4201 /wd4324
        $<$<CONFIG:Release>:/O2>
    )
    # 生成独立 PDB，方便线上排查崩溃
    target_compile_options(${PROJECT_NAME} PRIVATE $<$<CONFIG:Release>:/Zi>)
    target_link_options(${PROJECT_NAME} PRIVATE
        $<$<CONFIG:Release>:/DEBUG:FULL>
        $<$<CONFIG:Release>:/OPT:REF>
        $<$<CONFIG:Release>:/OPT:ICF>
    )
endif()

set_target_properties(${PROJECT_NAME} PROPERTIES
    PREFIX ""
    OUTPUT_NAME "EEA_Native"
    DEBUG_POSTFIX ""
)

# 编译产物直接摊到 dist/，方便打包成 MO2 可安装结构
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_SOURCE_DIR}/dist/SKSE/Plugins"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:${PROJECT_NAME}>"
            "${CMAKE_CURRENT_SOURCE_DIR}/dist/SKSE/Plugins/EEA_Native.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/EEA_Native.ini"
            "${CMAKE_CURRENT_SOURCE_DIR}/dist/SKSE/Plugins/EEA_Native.ini"
    COMMENT "EEA: 复制 DLL 与默认配置到 dist/"
)
