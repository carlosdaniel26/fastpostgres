cmake_minimum_required(VERSION 3.16)
project(v8ext LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# ---------------------------------------------------------------------------
# Locate V8. Point V8_ROOT at a directory containing include/ (v8.h and
# libplatform/) and lib/ (libv8_monolith.a). scripts/get-v8.sh downloads a
# prebuilt V8 and produces exactly this layout under third_party/v8-install.
# ---------------------------------------------------------------------------
if(NOT V8_ROOT)
  set(V8_ROOT "" CACHE PATH "V8 install root containing include/ and lib/")
endif()

find_path(V8_INCLUDE_DIR NAMES v8.h HINTS ${V8_ROOT}/include ${V8_ROOT})
find_library(V8_MONOLITH NAMES v8_monolith
             HINTS ${V8_ROOT}/lib ${V8_ROOT}/out/release/obj)
find_library(V8_LIBPLATFORM NAMES v8_libplatform
             HINTS ${V8_ROOT}/lib ${V8_ROOT}/out/release/obj)
find_library(V8_LIBBASE NAMES v8_libbase
             HINTS ${V8_ROOT}/lib ${V8_ROOT}/out/release/obj)

if(NOT V8_INCLUDE_DIR)
  message(FATAL_ERROR
    "V8 headers not found. Run scripts/get-v8.sh to fetch a prebuilt V8, "
    "then rerun cmake with -DV8_ROOT=<repo>/third_party/v8-install")
endif()

# V8 + system libs, carried as an interface target so every consumer links it.
add_library(v8_deps INTERFACE)
target_include_directories(v8_deps SYSTEM INTERFACE ${V8_INCLUDE_DIR})
set(_v8_libs)
if(V8_MONOLITH)
  list(APPEND _v8_libs ${V8_MONOLITH})
endif()
if(V8_LIBPLATFORM)
  list(APPEND _v8_libs ${V8_LIBPLATFORM})
endif()
if(V8_LIBBASE)
  list(APPEND _v8_libs ${V8_LIBBASE})
endif()
target_link_libraries(v8_deps INTERFACE ${_v8_libs} pthread dl m rt z)

# ---------------------------------------------------------------------------
# The client runtime: decodes a columnar /dev/shm segment into JS typed arrays.
# ---------------------------------------------------------------------------
add_library(v8ext STATIC src/runtime.cpp)
target_include_directories(v8ext PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(v8ext PUBLIC v8_deps)
target_compile_options(v8ext PRIVATE -Wall -Wextra)

# ---------------------------------------------------------------------------
# shm-read — standalone decoder (no V8): reads a segment and prints columns.
# ---------------------------------------------------------------------------
add_executable(shm-read tools/shm_read.cpp)
target_include_directories(shm-read PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(shm-read PRIVATE rt pthread)

# ---------------------------------------------------------------------------
# shm-demo — loads a /dev/shm segment into JS typed arrays (V8 runtime).
# ---------------------------------------------------------------------------
add_executable(shm-demo tools/shm_demo.cpp)
target_link_libraries(shm-demo PRIVATE v8ext)
