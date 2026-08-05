# Install script for directory: /mnt/d/Dev/tmp1/game/third_party/mongo-c-driver/src/libmongoc

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
          
            # Installation of pkg-config for target mongoc_static
            message(STATUS "Generating pkg-config file: mongoc2-static.pc")
            file(READ [[/mnt/d/Dev/tmp1/game/src/server/mongo/_mongo_vendor/mongo-c-driver/src/libmongoc/_pkgconfig/mongoc_static-debug-for-install.txt]] content)
            # Insert the install prefix:
            string(REPLACE "%INSTALL_PLACEHOLDER%" "${CMAKE_INSTALL_PREFIX}" content "${content}")
            # Write it before installing again. Lock the file to sync with parallel installs.
            file(LOCK [[/mnt/d/Dev/tmp1/game/src/server/mongo/_mongo_vendor/mongo-c-driver/src/libmongoc/mongoc_static-pkg-config-tmp.txt.lock]] GUARD PROCESS)
            file(WRITE [[/mnt/d/Dev/tmp1/game/src/server/mongo/_mongo_vendor/mongo-c-driver/src/libmongoc/mongoc_static-pkg-config-tmp.txt]] "${content}")
        
        
    
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/pkgconfig" TYPE FILE RENAME "mongoc2-static.pc" FILES "/mnt/d/Dev/tmp1/game/src/server/mongo/_mongo_vendor/mongo-c-driver/src/libmongoc/mongoc_static-pkg-config-tmp.txt")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib" TYPE STATIC_LIBRARY FILES "/mnt/d/Dev/tmp1/game/src/server/mongo/_mongo_vendor/mongo-c-driver/src/libmongoc/libmongoc2.a")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/mongoc-2.3.3/mongoc" TYPE FILE FILES
    "/mnt/d/Dev/tmp1/game/src/server/mongo/_mongo_vendor/mongo-c-driver/src/libmongoc/src/mongoc/mongoc-config.h"
    "/mnt/d/Dev/tmp1/game/src/server/mongo/_mongo_vendor/mongo-c-driver/src/libmongoc/src/mongoc/mongoc-version.h"
    "/mnt/d/Dev/tmp1/game/third_party/mongo-c-driver/src/libmongoc/src/mongoc/mongoc.h"
    "/mnt/d/Dev/tmp1/game/third_party/mongo-c-driver/src/libmongoc/src/mongoc/mongoc-apm.h"
    "/mnt/d/Dev/tmp1/game/third_party/mongo-c-driver/src/libmongoc/src/mongoc/mongoc-bulk-operation.h"
    "/mnt/d/Dev/tmp1/game/third_party/mongo-c-driver/src/libmongoc/src/mongoc/mongoc-bulkwrite.h"
    "/mnt/d/Dev/tmp1/game/third_party/mongo-c-driver/src/libmongoc/src/mongoc/mongoc-change-stream.h"
    "/mnt/d/Dev/tmp1/game/third_party/mongo-c-driver/src/libmongoc/src/mongoc/mongoc-client.h"
    "/mnt/d/Dev/tmp1/game/third_party/mongo-c-driver/src/libmongoc/src/mongoc/mongoc-client-pool.h"
    "/mnt/d/Dev/tmp1/game/third_party/mongo-c-driver/src/libmongoc/src/mongoc/mongoc-client-side-encryption.h"
    "/mnt/d/Dev/tmp1/game/third_party/mongo-c-driver/src/libmongoc/src/mongoc/mongoc-collection.h"
    "/mnt/d/Dev/tmp1/game/third_party/mongo-c-driver/src/libmongoc/src/mongoc/mongoc-cursor.h"
    "/mnt/d/Dev/tmp1/game/third_party/mongo-c-driver/src/libmongoc/src/mongoc/mongoc-database.h"
    "/mnt/d/Dev/tmp1/game/third_party/mongo-c-driver/src/libmongoc/src/mongoc/mongoc-error.h"
    "/mnt/d/Dev/tmp1/game/third_party/mongo-c-driver/src/libmongoc/src/mongoc/mongoc-flags.h"
    "/mnt/d/Dev/tmp1/game/third_party/mongo-c-driver/src/libmongoc/src/mongoc/mongoc-find-and-modify.h"
    "/mnt/d/Dev/tmp1/game/third_party/mongo-c-driver/src/libmongoc/src/mongoc/mongoc-gridfs.h"
    "/mnt/d/Dev/tmp1/game/third_party/mongo-c-driver/src/libmongoc/src/mongoc/mongoc-gridfs-bucket.h"
    "/mnt/d/Dev/tmp1/game/third_party/mongo-c-driver/src/libmongoc/src/mongoc/mongoc-gridfs-file.h"
    "/mnt/d/Dev/tmp1/game/third_party/mongo-c-driver/src/libmongoc/src/mongoc/mongoc-gridfs-file-page.h"
    "/mnt/d/Dev/tmp1/game/third_party/mongo-c-driver/src/libmongoc/src/mongoc/mongoc-gridfs-file-list.h"
    "/mnt/d/Dev/tmp1/game/third_party/mongo-c-driver/src/libmongoc/src/mongoc/mongoc-handshake.h"
    "/mnt/d/Dev/tmp1/game/third_party/mongo-c-driver/src/libmongoc/src/mongoc/mongoc-host-list.h"
    "/mnt/d/Dev/tmp1/game/third_party/mongo-c-driver/src/libmongoc/src/mongoc/mongoc-init.h"
    "/mnt/d/Dev/tmp1/game/third_party/mongo-c-driver/src/libmongoc/src/mongoc/mongoc-iovec.h"
    "/mnt/d/Dev/tmp1/game/third_party/mongo-c-driver/src/libmongoc/src/mongoc/mongoc-log.h"
    "/mnt/d/Dev/tmp1/game/third_party/mongo-c-driver/src/libmongoc/src/mongoc/mongoc-macros.h"
    "/mnt/d/Dev/tmp1/game/third_party/mongo-c-driver/src/libmongoc/src/mongoc/mongoc-oidc-callback.h"
    "/mnt/d/Dev/tmp1/game/third_party/mongo-c-driver/src/libmongoc/src/mongoc/mongoc-opcode.h"
    "/mnt/d/Dev/tmp1/game/third_party/mongo-c-driver/src/libmongoc/src/mongoc/mongoc-optional.h"
    "/mnt/d/Dev/tmp1/game/third_party/mongo-c-driver/src/libmongoc/src/mongoc/mongoc-prelude.h"
    "/mnt/d/Dev/tmp1/game/third_party/mongo-c-driver/src/libmongoc/src/mongoc/mongoc-read-concern.h"
    "/mnt/d/Dev/tmp1/game/third_party/mongo-c-driver/src/libmongoc/src/mongoc/mongoc-read-prefs.h"
    "/mnt/d/Dev/tmp1/game/third_party/mongo-c-driver/src/libmongoc/src/mongoc/mongoc-server-api.h"
    "/mnt/d/Dev/tmp1/game/third_party/mongo-c-driver/src/libmongoc/src/mongoc/mongoc-server-description.h"
    "/mnt/d/Dev/tmp1/game/third_party/mongo-c-driver/src/libmongoc/src/mongoc/mongoc-client-session.h"
    "/mnt/d/Dev/tmp1/game/third_party/mongo-c-driver/src/libmongoc/src/mongoc/mongoc-sleep.h"
    "/mnt/d/Dev/tmp1/game/third_party/mongo-c-driver/src/libmongoc/src/mongoc/mongoc-socket.h"
    "/mnt/d/Dev/tmp1/game/third_party/mongo-c-driver/src/libmongoc/src/mongoc/mongoc-stream-tls-openssl.h"
    "/mnt/d/Dev/tmp1/game/third_party/mongo-c-driver/src/libmongoc/src/mongoc/mongoc-stream.h"
    "/mnt/d/Dev/tmp1/game/third_party/mongo-c-driver/src/libmongoc/src/mongoc/mongoc-stream-buffered.h"
    "/mnt/d/Dev/tmp1/game/third_party/mongo-c-driver/src/libmongoc/src/mongoc/mongoc-stream-file.h"
    "/mnt/d/Dev/tmp1/game/third_party/mongo-c-driver/src/libmongoc/src/mongoc/mongoc-stream-gridfs.h"
    "/mnt/d/Dev/tmp1/game/third_party/mongo-c-driver/src/libmongoc/src/mongoc/mongoc-stream-socket.h"
    "/mnt/d/Dev/tmp1/game/third_party/mongo-c-driver/src/libmongoc/src/mongoc/mongoc-structured-log.h"
    "/mnt/d/Dev/tmp1/game/third_party/mongo-c-driver/src/libmongoc/src/mongoc/mongoc-topology-description.h"
    "/mnt/d/Dev/tmp1/game/third_party/mongo-c-driver/src/libmongoc/src/mongoc/mongoc-uri.h"
    "/mnt/d/Dev/tmp1/game/third_party/mongo-c-driver/src/libmongoc/src/mongoc/mongoc-version-functions.h"
    "/mnt/d/Dev/tmp1/game/third_party/mongo-c-driver/src/libmongoc/src/mongoc/mongoc-write-concern.h"
    "/mnt/d/Dev/tmp1/game/third_party/mongo-c-driver/src/libmongoc/src/mongoc/mongoc-rand.h"
    "/mnt/d/Dev/tmp1/game/third_party/mongo-c-driver/src/libmongoc/src/mongoc/mongoc-stream-tls.h"
    "/mnt/d/Dev/tmp1/game/third_party/mongo-c-driver/src/libmongoc/src/mongoc/mongoc-ssl.h"
    "/mnt/d/Dev/tmp1/game/third_party/mongo-c-driver/src/libmongoc/src/mongoc/mongoc-bulkwrite.h"
    )
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/mongoc-2.3.3/mongoc-targets.cmake")
    file(DIFFERENT _cmake_export_file_changed FILES
         "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/mongoc-2.3.3/mongoc-targets.cmake"
         "/mnt/d/Dev/tmp1/game/src/server/mongo/_mongo_vendor/mongo-c-driver/src/libmongoc/CMakeFiles/Export/3e6ef2058ae16cf119a32c8533545804/mongoc-targets.cmake")
    if(_cmake_export_file_changed)
      file(GLOB _cmake_old_config_files "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/mongoc-2.3.3/mongoc-targets-*.cmake")
      if(_cmake_old_config_files)
        string(REPLACE ";" ", " _cmake_old_config_files_text "${_cmake_old_config_files}")
        message(STATUS "Old export file \"$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/mongoc-2.3.3/mongoc-targets.cmake\" will be replaced.  Removing files [${_cmake_old_config_files_text}].")
        unset(_cmake_old_config_files_text)
        file(REMOVE ${_cmake_old_config_files})
      endif()
      unset(_cmake_old_config_files)
    endif()
    unset(_cmake_export_file_changed)
  endif()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/mongoc-2.3.3" TYPE FILE FILES "/mnt/d/Dev/tmp1/game/src/server/mongo/_mongo_vendor/mongo-c-driver/src/libmongoc/CMakeFiles/Export/3e6ef2058ae16cf119a32c8533545804/mongoc-targets.cmake")
  if(CMAKE_INSTALL_CONFIG_NAME MATCHES "^([Dd][Ee][Bb][Uu][Gg])$")
    file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/mongoc-2.3.3" TYPE FILE FILES "/mnt/d/Dev/tmp1/game/src/server/mongo/_mongo_vendor/mongo-c-driver/src/libmongoc/CMakeFiles/Export/3e6ef2058ae16cf119a32c8533545804/mongoc-targets-debug.cmake")
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/mongoc-2.3.3" TYPE FILE FILES
    "/mnt/d/Dev/tmp1/game/src/server/mongo/_mongo_vendor/mongo-c-driver/src/libmongoc/mongocConfig.cmake"
    "/mnt/d/Dev/tmp1/game/src/server/mongo/_mongo_vendor/mongo-c-driver/src/libmongoc/mongocConfigVersion.cmake"
    )
endif()

