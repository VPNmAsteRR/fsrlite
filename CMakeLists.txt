cmake_minimum_required(VERSION 3.20)
project(fsrlite LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT MSVC)
    message(FATAL_ERROR "fsrlite targets Windows/DX11 with C++/WinRT. Build with MSVC.")
endif()

add_executable(fsrlite
    src/main.cpp
    src/upscaler.cpp
)

target_compile_options(fsrlite PRIVATE
    /W4 /permissive- /utf-8 /EHsc /await
    # C++/WinRT needs C++17 coroutines and relaxed conformance bits
    /Zc:twoPhase-
)

# C++/WinRT headers ship with the Windows SDK
target_include_directories(fsrlite PRIVATE
    "$ENV{WindowsSdkDir}Include/$ENV{WindowsSDKVersion}cppwinrt"
)

target_link_libraries(fsrlite PRIVATE
    d3d11
    dxgi
    dxguid
    d3dcompiler
    dwmapi
    shcore
    windowsapp
    user32
)

add_custom_command(TARGET fsrlite POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:fsrlite>/shaders"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/upscale.hlsl"
            "$<TARGET_FILE_DIR:fsrlite>/shaders/upscale.hlsl"
)
