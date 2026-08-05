# Install script for directory: /mnt/d/Dev/tmp1/game/third_party/mongo-c-driver/src/libbson

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
    set(CMAKE_INSTALL_CONFIG_NAME "Debug")
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

# Install shared libraries without execute permission?
if(NOT DEFINED CMAKE_INSTALL_SO_NO_EXE)
  set(CMAKE_INSTALL_SO_NO_EXE "1")
endif()

# Is this installation the result of a crosscompile?
if(NOT DEFINED CMAKE_CROSSCOMPILING)
  set(CMAKE_CROSSCOMPILING "FALSE")
endif()

# Set default install directory permissions.
if(NOT DEFINED CMAKE_OBJDUMP)
  set(CMAKE_OBJDUMP "/usr/bin/objdump")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib" TYPE STATIC_LIBRARY FILES "/mnt/d/Dev/tmp1/game/src/server/mongo/_mongo_vendor/mongo-c-driver/src/libbson/libbson2.a")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/bson-2.3.3/bson_static-targets.cmake")
    file(DIFFERENT _cmake_export_file_changed FILES
         "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/bson-2.3.3/bson_static-targets.cmake"
         "/mnt/d/Dev/tmp1/game/src/server/mongo/_mongo_vendor/mongo-c-driver/src/libbson/CMakeFiles/Export/080ef203c0c1b5dc808ba5f8ae27a5fd/bson_static-targets.cmake")
    if(_cmake_export_file_changed)
      file(GLOB _cmake_old_config_files "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/bson-2.3.3/bson_static-targets-*.cmake")
      if(_cmake_old_config_files)
        string(REPLACE ";" ", " _cmake_old_config_files_text "${_cmake_old_config_files}")
        message(STATUS "Old export file \"$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/bson-2.3.3/bson_static-targets.cmake\" will be replaced.  Removing files [${_cmake_old_config_files_text}].")
        unset(_cmake_old_config_files_text)
        file(REMOVE ${_cmake_old_config_files})
      endif()
      unset(_cmake_old_config_files)
    endif()
    unset(_cmake_export_file_changed)
  endif()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/bson-2.3.3" TYPE FILE FILES "/mnt/d/Dev/tmp1/game/src/server/mongo/_mongo_vendor/mongo-c-driver/src/libbson/CMakeFiles/Export/080ef203c0c1b5dc808ba5f8ae27a5fd/bson_static-targets.cmake")
  if(CMAKE_INSTALL_CONFIG_NAME MATCHES "^([Dd][Ee][Bb][Uu][Gg])$")
    file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/bson-2.3.3" TYPE FILE FILES "/mnt/d/Dev/tmp1/game/src/server/mongo/_mongo_vendor/mongo-c-driver/src/libbson/CMakeFiles/Export/080ef203c0c1b5dc808ba5f8ae27a5fd/bson_static-targets-debug.cmake")
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
          
            # Installation of pkg-config for target bson_static
            message(STATUS "Generating pkg-config file: bson2-static.pc")
            file(READ [[/mnt/d/Dev/tmp1/game/src/server/mongo/_mongo_vendor/mongo-c-driver/src/libbson/_pkgconfig/bson_static-debug-for-install.txt]] content)
            # Insert the install prefix:
            string(REPLACE "%INSTALL_PLACEHOLDER%" "${CMAKE_INSTALL_PREFIX}" content "${content}")
            # Write it before installing again. Lock the file to sync with parallel installs.
            file(LOCK [[/mnt/d/Dev/tmp1/game/src/server/mongo/_mongo_vendor/mongo-c-driver/src/libbson/bson_static-pkg-config-tmp.txt.lock]] GUARD PROCESS)
            file(WRITE [[/mnt/d/Dev/tmp1/game/src/server/mongo/_mongo_vendor/mongo-c-driver/src/libbson/bson_static-pkg-config-tmp.txt]] "${content}")
        
        
    
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/pkgconfig" TYPE FILE RENAME "bson2-static.pc" FILES "/mnt/d/Dev/tmp1/game/src/server/mongo/_mongo_vendor/mongo-c-driver/src/libbson/bson_static-pkg-config-tmp.txt")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/bson-2.3.3" TYPE DIRECTORY FILES
    "/mnt/d/Dev/tmp1/game/third_party/mongo-c-driver/src/libbson/src/"
    "/mnt/d/Dev/tmp1/game/src/server/mongo/_mongo_vendor/mongo-c-driver/src/libbson/src/"
    FILES_MATCHING REGEX "/[^/]*\\.h$" REGEX "/[^/]*\\-private\\.h$" EXCLUDE REGEX "/jsonsl$" EXCLUDE)
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/bson-2.3.3/00-mongo-platform-targets.cmake")
    file(DIFFERENT _cmake_export_file_changed FILES
         "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/bson-2.3.3/00-mongo-platform-targets.cmake"
         "/mnt/d/Dev/tmp1/game/src/server/mongo/_mongo_vendor/mongo-c-driver/src/libbson/CMakeFiles/Export/080ef203c0c1b5dc808ba5f8ae27a5fd/00-mongo-platform-targets.cmake")
    if(_cmake_export_file_changed)
      file(GLOB _cmake_old_config_files "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/bson-2.3.3/00-mongo-platform-targets-*.cmake")
      if(_cmake_old_config_files)
        string(REPLACE ";" ", " _cmake_old_config_files_text "${_cmake_old_config_files}")
        message(STATUS "Old export file \"$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/bson-2.3.3/00-mongo-platform-targets.cmake\" will be replaced.  Removing files [${_cmake_old_config_files_text}].")
        unset(_cmake_old_config_files_text)
        file(REMOVE ${_cmake_old_config_files})
      endif()
      unset(_cmake_old_config_files)
    endif()
    unset(_cmake_export_file_changed)
  endif()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/bson-2.3.3" TYPE FILE FILES "/mnt/d/Dev/tmp1/game/src/server/mongo/_mongo_vendor/mongo-c-driver/src/libbson/CMakeFiles/Export/080ef203c0c1b5dc808ba5f8ae27a5fd/00-mongo-platform-targets.cmake")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/bson-2.3.3" TYPE FILE FILES
    "/mnt/d/Dev/tmp1/game/third_party/mongo-c-driver/src/libbson/etc/bsonConfig.cmake"
    "/mnt/d/Dev/tmp1/game/src/server/mongo/_mongo_vendor/mongo-c-driver/src/libbson/bsonConfigVersion.cmake"
    )
endif()

