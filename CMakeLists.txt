cmake_minimum_required(VERSION 3.16)
project(jetedge-agent VERSION 0.3.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ---- Dependencies ----------------------------------------------------------
find_package(PkgConfig REQUIRED)
pkg_check_modules(GSTREAMER REQUIRED IMPORTED_TARGET gstreamer-1.0)
pkg_check_modules(GLIB REQUIRED IMPORTED_TARGET glib-2.0)
pkg_check_modules(YAMLCPP REQUIRED IMPORTED_TARGET yaml-cpp)

# ---- Source files ----------------------------------------------------------
set(JETEDGE_SOURCES
  apps/jetedge_server/main.cpp
  src/common/config_loader.cpp
  src/pipeline/pipeline.cpp
  src/pipeline/source_bin.cpp
  src/pipeline/source_manager.cpp
)

# ---- Headers (for IDE integration) -----------------------------------------
set(JETEDGE_HEADERS
  include/jetedge/common/logging.h
  include/jetedge/common/config_loader.h
  include/jetedge/pipeline/pipeline.h
  include/jetedge/pipeline/pipeline_config.h
  include/jetedge/pipeline/source_bin.h
  include/jetedge/pipeline/source_manager.h
  include/jetedge/pipeline/stream_config.h
)

# ---- Executable ------------------------------------------------------------
add_executable(jetedge_server ${JETEDGE_SOURCES} ${JETEDGE_HEADERS})
target_include_directories(jetedge_server PRIVATE
  ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(jetedge_server PRIVATE
  PkgConfig::GSTREAMER
  PkgConfig::GLIB
  PkgConfig::YAMLCPP
)
