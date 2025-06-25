cmake_minimum_required (VERSION 2.8.11)
project (Transformation)

# Allow CMake to find packages installed in the active Conda environment
if(DEFINED ENV{CONDA_PREFIX})
  list(APPEND CMAKE_PREFIX_PATH "$ENV{CONDA_PREFIX}")
endif()

find_package(Eigen3 REQUIRED)
include_directories(${EIGEN3_INCLUDE_DIR})

add_executable (Transformation main.cpp)
