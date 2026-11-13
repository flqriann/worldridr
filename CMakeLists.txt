cmake_minimum_required(VERSION 3.24)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
project(worldridr LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# --- SDL3 (Fenster, Eingabe, Lenkrad/Force-Feedback, Audio) ---
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(SDL3
  URL https://github.com/libsdl-org/SDL/archive/refs/tags/release-3.2.10.tar.gz)

# --- OpenXR (VR ueber Meta Horizon Link) ---
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_API_LAYERS OFF CACHE BOOL "" FORCE)
set(BUILD_CONFORMANCE_TESTS OFF CACHE BOOL "" FORCE)
set(DYNAMIC_LOADER OFF CACHE BOOL "" FORCE)
FetchContent_Declare(openxr
  URL https://github.com/KhronosGroup/OpenXR-SDK/archive/refs/tags/release-1.1.43.tar.gz)

# --- Lua, stb, JSON ---
FetchContent_Declare(lua URL https://www.lua.org/ftp/lua-5.4.7.tar.gz)
FetchContent_Declare(stb URL https://github.com/nothings/stb/archive/refs/heads/master.tar.gz)
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
FetchContent_Declare(json URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz)

FetchContent_MakeAvailable(SDL3 openxr lua stb json)

file(GLOB LUA_SRC ${lua_SOURCE_DIR}/src/*.c)
list(FILTER LUA_SRC EXCLUDE REGEX "/(lua|luac)\\.c$")
add_library(lua STATIC ${LUA_SRC})
target_include_directories(lua PUBLIC ${lua_SOURCE_DIR}/src)

add_executable(worldridr
  src/main.cpp src/gfx.cpp src/net.cpp src/world.cpp src/vehicle.cpp
  src/input.cpp src/audio.cpp src/script.cpp src/vr.cpp
  src/carmodel.cpp src/ui.cpp src/editor.cpp src/netplay.cpp src/traffic.cpp src/worldmap.cpp src/nav.cpp src/pedestrians.cpp src/forest.cpp src/econ.cpp src/keys.cpp src/ragdoll.cpp src/onfoot.cpp src/signs.cpp src/env.cpp)
target_include_directories(worldridr PRIVATE ${stb_SOURCE_DIR})
target_compile_definitions(worldridr PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
target_link_libraries(worldridr PRIVATE SDL3::SDL3-static OpenXR::openxr_loader lua
  nlohmann_json::nlohmann_json opengl32 winhttp ws2_32)
if(MSVC)
  target_compile_options(worldridr PRIVATE /utf-8 /W3 /wd4244 /wd4305 /wd4267)
endif()
# Entwicklungsbau: mit Umgebungsvariable WR_SUFFIX=_dev entsteht worldridr_dev.exe (laeuft ein Spiel, ist worldridr.exe gesperrt)
if(DEFINED ENV{WR_SUFFIX})
  set_target_properties(worldridr PROPERTIES OUTPUT_NAME "worldridr$ENV{WR_SUFFIX}")
endif()
