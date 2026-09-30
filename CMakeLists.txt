cmake_minimum_required(VERSION 3.16)

# customrounds — MetaMod:Source 2 plugin (CS2)
# Mechanics for VIP custom rounds: «Один патрон», «Без звука».
#
# Output: build/addons/customrounds/bin/linuxsteamrt64/customrounds.so

project(customrounds LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(CR_OPTIMIZE "Enable -O3 in Release" ON)

function(cr_try_path OUT_VAR)
  set(${OUT_VAR} "" PARENT_SCOPE)
  foreach(candidate IN LISTS ARGN)
    if(candidate AND EXISTS "${candidate}")
      set(${OUT_VAR} "${candidate}" PARENT_SCOPE)
      return()
    endif()
  endforeach()
endfunction()

if(NOT HL2SDK_PATH AND DEFINED ENV{HL2SDKCS2})
  set(HL2SDK_PATH "$ENV{HL2SDKCS2}")
endif()
if(NOT MMS_PATH AND DEFINED ENV{MMSOURCE112})
  set(MMS_PATH "$ENV{MMSOURCE112}")
elseif(NOT MMS_PATH AND DEFINED ENV{MMSOURCE_DEV})
  set(MMS_PATH "$ENV{MMSOURCE_DEV}")
endif()

set(_cr_roots
  "${CMAKE_SOURCE_DIR}"
  "${CMAKE_SOURCE_DIR}/.."
  "${CMAKE_SOURCE_DIR}/../.."
)
foreach(_root IN LISTS _cr_roots)
  list(APPEND _cr_hl2sdk_candidates "${_root}/deps/hl2sdk-cs2")
  list(APPEND _cr_mms_candidates "${_root}/deps/metamod-source")
endforeach()

if(NOT HL2SDK_PATH OR NOT EXISTS "${HL2SDK_PATH}/public/tier0/dbg.h")
  cr_try_path(_found_hl2sdk ${_cr_hl2sdk_candidates})
  if(_found_hl2sdk)
    set(HL2SDK_PATH "${_found_hl2sdk}")
  endif()
endif()
if(NOT MMS_PATH OR NOT EXISTS "${MMS_PATH}/core/ISmmPlugin.h")
  cr_try_path(_found_mms ${_cr_mms_candidates})
  if(_found_mms)
    set(MMS_PATH "${_found_mms}")
  endif()
endif()

set(HL2SDK_PATH "${HL2SDK_PATH}" CACHE PATH "Path to hl2sdk-cs2")
set(MMS_PATH "${MMS_PATH}" CACHE PATH "Path to metamod-source")

if(NOT HL2SDK_PATH OR NOT EXISTS "${HL2SDK_PATH}/public/tier0/dbg.h")
  message(FATAL_ERROR "HL2SDK not found. Run: bash build_cmake.sh")
endif()
if(NOT MMS_PATH OR NOT EXISTS "${MMS_PATH}/core/ISmmPlugin.h")
  message(FATAL_ERROR "Metamod:Source not found. Run: bash build_cmake.sh")
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(MMS_CORE "${MMS_PATH}/core")

# Bake git metadata into the binary for `cr_status`.
set(CR_GIT_COMMIT "unknown")
set(CR_GIT_BRANCH "unknown")
set(CR_GIT_COMMIT_DATE "unknown")
set(CR_GIT_COMMIT_SUBJECT "unknown")
find_package(Git QUIET)
if(GIT_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" rev-parse --short HEAD
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    OUTPUT_VARIABLE CR_GIT_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
  )
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" rev-parse --abbrev-ref HEAD
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    OUTPUT_VARIABLE CR_GIT_BRANCH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
  )
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" log -1 --format=%ci
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    OUTPUT_VARIABLE CR_GIT_COMMIT_DATE
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
  )
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" log -1 --format=%s
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    OUTPUT_VARIABLE CR_GIT_COMMIT_SUBJECT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
  )
endif()
string(TIMESTAMP CR_BUILD_TIMESTAMP "%Y-%m-%d %H:%M:%S")
foreach(_cr_git_var IN ITEMS CR_GIT_COMMIT CR_GIT_BRANCH CR_GIT_COMMIT_DATE CR_GIT_COMMIT_SUBJECT CR_BUILD_TIMESTAMP)
  string(REPLACE "\\" "\\\\" ${_cr_git_var} "${${_cr_git_var}}")
  string(REPLACE "\"" "\\\"" ${_cr_git_var} "${${_cr_git_var}}")
endforeach()

add_library(customrounds_flags INTERFACE)

target_compile_definitions(customrounds_flags INTERFACE
  META_IS_SOURCE2
  SOURCE_ENGINE=25
  SE_CS2=25
  GAME_DLL
  RAD_TELEMETRY_DISABLED
  stricmp=strcasecmp
  _stricmp=strcasecmp
  _snprintf=snprintf
  _vsnprintf=vsnprintf
  HAVE_STDINT_H
  GNUC
  CR_GIT_COMMIT="${CR_GIT_COMMIT}"
  CR_GIT_BRANCH="${CR_GIT_BRANCH}"
  CR_GIT_COMMIT_DATE="${CR_GIT_COMMIT_DATE}"
  CR_GIT_COMMIT_SUBJECT="${CR_GIT_COMMIT_SUBJECT}"
  CR_BUILD_TIMESTAMP="${CR_BUILD_TIMESTAMP}"
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(customrounds_flags INTERFACE
    -pipe
    -fno-strict-aliasing
    -Wall
    -Wno-uninitialized
    -Wno-parentheses
    -Wno-unused
    -Wno-switch
    -Wno-sign-compare
    -Wno-attributes
    -Wno-invalid-offsetof
    -Wno-non-virtual-dtor
    -Wno-overloaded-virtual
    -fno-threadsafe-statics
    -fvisibility=hidden
    -fvisibility-inlines-hidden
    -fno-omit-frame-pointer
    $<$<COMPILE_LANGUAGE:CXX>:-std=c++17>
  )
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_compile_definitions(customrounds_flags INTERFACE
    LINUX
    _LINUX
    POSIX
    _FILE_OFFSET_BITS=64
    X64BITS
    PLATFORM_64BITS
    COMPILER_GCC
    _GLIBCXX_USE_CXX11_ABI=0
  )
  target_compile_options(customrounds_flags INTERFACE -fPIC -msse)
  if(CMAKE_BUILD_TYPE STREQUAL "Release" AND CR_OPTIMIZE)
    target_compile_options(customrounds_flags INTERFACE -O3)
    target_compile_definitions(customrounds_flags INTERFACE NDEBUG)
  endif()
  if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(customrounds_flags INTERFACE -g3)
    target_compile_definitions(customrounds_flags INTERFACE DEBUG _DEBUG)
  endif()
endif()

target_include_directories(customrounds_flags INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${MMS_CORE}"
  "${MMS_PATH}/third_party/khook/include"
  "${HL2SDK_PATH}/thirdparty/protobuf-3.21.8/src"
  "${HL2SDK_PATH}/public"
  "${HL2SDK_PATH}/public/engine"
  "${HL2SDK_PATH}/public/mathlib"
  "${HL2SDK_PATH}/public/tier0"
  "${HL2SDK_PATH}/public/tier1"
  "${HL2SDK_PATH}/public/entity2"
  "${HL2SDK_PATH}/public/game/server"
  "${HL2SDK_PATH}/public/schemasystem"
  "${HL2SDK_PATH}/game/shared"
  "${HL2SDK_PATH}/game/server"
  "${HL2SDK_PATH}/common"
)

# ---------------------------------------------------------------------------
# Protobuf (network_connection.pb.h etc.)
# ---------------------------------------------------------------------------

set(CR_PROTO_FILES
  "${HL2SDK_PATH}/common/valveextensions.proto"
  "${HL2SDK_PATH}/common/network_connection.proto"
  "${HL2SDK_PATH}/common/networkbasetypes.proto"
  "${HL2SDK_PATH}/game/shared/usercmd.proto"
  "${HL2SDK_PATH}/game/shared/cs/cs_usercmd.proto"
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(CR_PROTOC "${HL2SDK_PATH}/devtools/bin/linux/protoc")
else()
  set(CR_PROTOC "${HL2SDK_PATH}/devtools/bin/protoc.exe")
endif()

if(NOT EXISTS "${CR_PROTOC}")
  message(FATAL_ERROR "protoc not found: ${CR_PROTOC}")
endif()

set(CR_PROTO_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${CR_PROTO_GEN_DIR}")

set(CR_PROTO_SRCS "")
set(CR_PROTO_HDRS "")

foreach(proto ${CR_PROTO_FILES})
  get_filename_component(proto_name "${proto}" NAME_WE)
  if(proto_name STREQUAL "cs_usercmd")
    set(out_cc "${CR_PROTO_GEN_DIR}/cs/cs_usercmd.pb.cc")
    set(out_h "${CR_PROTO_GEN_DIR}/cs/cs_usercmd.pb.h")
  else()
    set(out_cc "${CR_PROTO_GEN_DIR}/${proto_name}.pb.cc")
    set(out_h "${CR_PROTO_GEN_DIR}/${proto_name}.pb.h")
  endif()
  list(APPEND CR_PROTO_SRCS "${out_cc}")
  list(APPEND CR_PROTO_HDRS "${out_h}")

  add_custom_command(
    OUTPUT "${out_cc}" "${out_h}"
    COMMAND "${CR_PROTOC}"
      --cpp_out=${CR_PROTO_GEN_DIR}
      "-I${HL2SDK_PATH}/common"
      "-I${HL2SDK_PATH}/game/shared"
      "-I${HL2SDK_PATH}/game/shared/cs"
      "-I${HL2SDK_PATH}/thirdparty/protobuf-3.21.8/src"
      "${proto}"
    DEPENDS "${proto}"
    COMMENT "Generating ${proto_name}.pb.cc"
    VERBATIM
  )
endforeach()

add_custom_target(customrounds_protoc DEPENDS ${CR_PROTO_SRCS} ${CR_PROTO_HDRS})

set(CR_PLUGIN_SRCS
  src/customrounds.cpp
  src/commands.cpp
  src/schema.cpp
  src/vtable_finder.cpp
  src/players.cpp
  src/events.cpp
  src/rounds.cpp
  src/sound_hook.cpp
  src/invert_hook.cpp
  src/entity_utils.cpp
  src/grenade_shot_hook.cpp
)

set(CR_SDK_SRCS
  "${HL2SDK_PATH}/public/tier0/memoverride.cpp"
  "${HL2SDK_PATH}/entity2/entitysystem.cpp"
  "${HL2SDK_PATH}/entity2/entityidentity.cpp"
  "${HL2SDK_PATH}/entity2/entitykeyvalues.cpp"
  "${HL2SDK_PATH}/tier1/convar.cpp"
  "${HL2SDK_PATH}/tier1/keyvalues3.cpp"
)

add_library(customrounds SHARED
  ${CR_PLUGIN_SRCS}
  ${CR_SDK_SRCS}
  ${CR_PROTO_SRCS}
)

add_dependencies(customrounds customrounds_protoc)

target_link_libraries(customrounds PRIVATE customrounds_flags)
target_include_directories(customrounds PRIVATE "${CR_PROTO_GEN_DIR}")

set_target_properties(customrounds PROPERTIES
  PREFIX ""
  OUTPUT_NAME "customrounds"
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(HL2SDK_LIB "${HL2SDK_PATH}/lib/linux64")
  set(CR_SDK_LIBS
    "${HL2SDK_LIB}/mathlib.a"
    "${HL2SDK_LIB}/interfaces.a"
    "${HL2SDK_LIB}/release/libprotobuf.a"
  )
  foreach(lib ${CR_SDK_LIBS})
    if(NOT EXISTS "${lib}")
      message(FATAL_ERROR "Missing HL2SDK library: ${lib}")
    endif()
  endforeach()

  target_link_libraries(customrounds PRIVATE
    ${CR_SDK_LIBS}
    dl
    pthread
    m
  )

  target_link_options(customrounds PRIVATE
    -static-libgcc
    -static-libstdc++
  )

  set(CR_OUT_DIR "${CMAKE_BINARY_DIR}/addons/customrounds/bin/linuxsteamrt64")
  set_target_properties(customrounds PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CR_OUT_DIR}"
  )

  set(CR_PKG_METAMOD "${CMAKE_BINARY_DIR}/addons/metamod")
  add_custom_target(customrounds_package ALL
    DEPENDS customrounds
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CR_PKG_METAMOD}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${CMAKE_SOURCE_DIR}/customrounds.vdf" "${CR_PKG_METAMOD}/"
    COMMENT "Packaging addons/customrounds layout"
  )
else()
  message(FATAL_ERROR "Unsupported platform: ${CMAKE_SYSTEM_NAME}")
endif()

message(STATUS "customrounds: HL2SDK=${HL2SDK_PATH}")
message(STATUS "customrounds: MMS=${MMS_PATH}")
