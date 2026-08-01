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

# DeepStream 7.1 (installed system-wide on the Jetson).
set(DEEPSTREAM_ROOT /opt/nvidia/deepstream/deepstream-7.1)
set(DEEPSTREAM_INCLUDE ${DEEPSTREAM_ROOT}/sources/includes)
set(DEEPSTREAM_LIB ${DEEPSTREAM_ROOT}/lib)
set(CUDA_INCLUDE /usr/local/cuda/include)

# ---- Source files ----------------------------------------------------------
set(JETEDGE_SOURCES
  apps/jetedge_server/main.cpp
  src/common/config_loader.cpp
  src/inference/metadata_probe.cpp
  src/metrics/metrics_registry.cpp
  src/pipeline/pipeline.cpp
  src/pipeline/source_bin.cpp
  src/pipeline/source_manager.cpp
)

# ---- Headers (for IDE integration) -----------------------------------------
set(JETEDGE_HEADERS
  include/jetedge/common/logging.h
  include/jetedge/common/config_loader.h
  include/jetedge/inference/inference_config.h
  include/jetedge/inference/metadata_probe.h
  include/jetedge/metrics/metrics_registry.h
  include/jetedge/pipeline/pipeline.h
  include/jetedge/pipeline/source_bin.h
  include/jetedge/pipeline/source_manager.h
  include/jetedge/pipeline/stream_config.h
)

# ---- YOLO11 custom parser (loaded by nvinfer at runtime) -------------------
# DeepStream 7.1 ships no YOLO parser for the raw 1x84x5040 output; this
# standalone .so implements NvDsInferParseCustomYolo11.
add_library(nvds_yolo11_parser SHARED src/inference/yolo11_parser.cpp)
target_include_directories(nvds_yolo11_parser PRIVATE
  ${DEEPSTREAM_INCLUDE}
  ${CUDA_INCLUDE}
)
target_link_libraries(nvds_yolo11_parser PRIVATE nvinfer)
set_target_properties(nvds_yolo11_parser PROPERTIES
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# ---- Executable ------------------------------------------------------------
add_executable(jetedge_server ${JETEDGE_SOURCES} ${JETEDGE_HEADERS})
target_include_directories(jetedge_server PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${DEEPSTREAM_INCLUDE}
)
target_link_libraries(jetedge_server PRIVATE
  PkgConfig::GSTREAMER
  PkgConfig::GLIB
  PkgConfig::YAMLCPP
  nvdsgst_meta
  nvds_meta
)
target_link_directories(jetedge_server PRIVATE ${DEEPSTREAM_LIB})
