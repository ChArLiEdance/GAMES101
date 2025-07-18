# Install script for directory: /Users/charlie/Documents/CharlieCode/GAMES101/assignment8/src

# Set the install prefix
if(NOT DEFINED CMAKE_INSTALL_PREFIX)
  set(CMAKE_INSTALL_PREFIX "/usr/local")
endif()
string(REGEX REPLACE "/$" "" CMAKE_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")

# Set the install configuration name.
if(NOT DEFINED CMAKE_INSTALL_CONFIG_NAME)
  if(BUILD_TYPE)
    string(REGEX REPLACE "^[^A-Za-z0-9_]+" ""
           CMAKE_INSTALL_CONFIG_NAME "${BUILD_TYPE}")
  else()
    set(CMAKE_INSTALL_CONFIG_NAME "")
  endif()
  message(STATUS "Install configuration: \"${CMAKE_INSTALL_CONFIG_NAME}\"")
endif()

# Set the component getting installed.
if(NOT CMAKE_INSTALL_COMPONENT)
  if(COMPONENT)
    message(STATUS "Install component: \"${COMPONENT}\"")
    set(CMAKE_INSTALL_COMPONENT "${COMPONENT}")
  else()
    set(CMAKE_INSTALL_COMPONENT)
  endif()
endif()

# Is this installation the result of a crosscompile?
if(NOT DEFINED CMAKE_CROSSCOMPILING)
  set(CMAKE_CROSSCOMPILING "FALSE")
endif()

# Set default install directory permissions.
if(NOT DEFINED CMAKE_OBJDUMP)
  set(CMAKE_OBJDUMP "/opt/miniconda3/envs/games101/bin/llvm-objdump")
endif()

if("x${CMAKE_INSTALL_COMPONENT}x" STREQUAL "xUnspecifiedx" OR NOT CMAKE_INSTALL_COMPONENT)
  list(APPEND CMAKE_ABSOLUTE_DESTINATION_FILES
   "/Users/charlie/Documents/CharlieCode/GAMES101/assignment8/ropesim")
  if(CMAKE_WARN_ON_ABSOLUTE_INSTALL_DESTINATION)
    message(WARNING "ABSOLUTE path INSTALL DESTINATION : ${CMAKE_ABSOLUTE_DESTINATION_FILES}")
  endif()
  if(CMAKE_ERROR_ON_ABSOLUTE_INSTALL_DESTINATION)
    message(FATAL_ERROR "ABSOLUTE path INSTALL DESTINATION forbidden (by caller): ${CMAKE_ABSOLUTE_DESTINATION_FILES}")
  endif()
file(INSTALL DESTINATION "/Users/charlie/Documents/CharlieCode/GAMES101/assignment8" TYPE EXECUTABLE FILES "/Users/charlie/Documents/CharlieCode/GAMES101/assignment8/build/ropesim")
  if(EXISTS "$ENV{DESTDIR}/Users/charlie/Documents/CharlieCode/GAMES101/assignment8/ropesim" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}/Users/charlie/Documents/CharlieCode/GAMES101/assignment8/ropesim")
    if(CMAKE_INSTALL_DO_STRIP)
      execute_process(COMMAND "/opt/miniconda3/envs/games101/bin/arm64-apple-darwin20.0.0-strip" -u -r "$ENV{DESTDIR}/Users/charlie/Documents/CharlieCode/GAMES101/assignment8/ropesim")
    endif()
  endif()
endif()

