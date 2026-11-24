cmake_minimum_required(VERSION 3.20)
project(WinterDesktop LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(WinterDesktop WIN32 src/main.cpp)

target_compile_definitions(WinterDesktop PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00
)

target_link_libraries(WinterDesktop PRIVATE
    user32
    gdi32
    gdiplus
    dwmapi
    shell32
    ole32
    advapi32
)

if(MSVC)
    set_property(TARGET WinterDesktop PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(WinterDesktop PRIVATE /O2 /W3)
else()
    target_compile_options(WinterDesktop PRIVATE -O2 -Wall)
    target_link_options(WinterDesktop PRIVATE -static -static-libgcc -static-libstdc++ -mwindows -municode)
endif()
