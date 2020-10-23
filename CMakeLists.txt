project(vperfetto)
cmake_minimum_required(VERSION 3.7)
find_package(Threads)

include(ExternalProject)

set(CMAKE_CXX_STANDARD 17)

include(protobuf.cmake)
include(gtest.cmake)

set(OPTION_PERFETTO_USE_SDK TRUE)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

add_subdirectory(proto)

set(perfetto "${CMAKE_CURRENT_SOURCE_DIR}/perfetto-min")

add_library(
  perfetto-libperfettobase
  ${perfetto}/src/base/logging.cc
  ${perfetto}/src/base/metatrace.cc
  ${perfetto}/src/base/paged_memory.cc
  ${perfetto}/src/base/string_splitter.cc
  ${perfetto}/src/base/string_utils.cc
  ${perfetto}/src/base/string_view.cc
  ${perfetto}/src/base/subprocess.cc
  ${perfetto}/src/base/thread_checker.cc
  ${perfetto}/src/base/time.cc
  ${perfetto}/src/base/uuid.cc
  ${perfetto}/src/base/virtual_destructors.cc
  ${perfetto}/src/base/waitable_event.cc
  ${perfetto}/src/base/watchdog_posix.cc)

target_include_directories(
    perfetto-libperfettobase PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${perfetto}/include)

add_executable(
  perfetto-protozero-protoc-plugin
  ${perfetto}/src/protozero/protoc_plugin/protozero_plugin.cc)
target_link_libraries(
    perfetto-protozero-protoc-plugin PRIVATE perfetto-libperfettobase ${Protobuf_PROTOC_LIBRARY} ${Protobuf_LIBRARY})

add_library(
  perfetto-libprotozero
  ${perfetto}/src/protozero/field.cc
  ${perfetto}/src/protozero/message.cc
  ${perfetto}/src/protozero/message_arena.cc
  ${perfetto}/src/protozero/message_handle.cc
  ${perfetto}/src/protozero/packed_repeated_fields.cc
  ${perfetto}/src/protozero/proto_decoder.cc
  ${perfetto}/src/protozero/scattered_heap_buffer.cc
  ${perfetto}/src/protozero/scattered_stream_null_delegate.cc
  ${perfetto}/src/protozero/scattered_stream_writer.cc
  ${perfetto}/src/protozero/static_buffer.cc
  ${perfetto}/src/protozero/virtual_destructors.cc)

target_include_directories(
  perfetto-libprotozero PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${perfetto}/include)

target_link_libraries(
  perfetto-libprotozero PUBLIC perfetto-libperfettobase)

function(protobuf_generate_with_plugin)
  include(CMakeParseArguments)

  set(_options APPEND_PATH)
  set(_singleargs LANGUAGE OUT_VAR EXPORT_MACRO PROTOC_OUT_DIR PLUGIN PLUGINOUT PROTOPATH HEADERFILEEXTENSION)
  if(COMMAND target_sources)
    list(APPEND _singleargs TARGET)
  endif()
  set(_multiargs PROTOS IMPORT_DIRS GENERATE_EXTENSIONS)

  cmake_parse_arguments(protobuf_generate_with_plugin "${_options}" "${_singleargs}" "${_multiargs}" "${ARGN}")

  if(NOT protobuf_generate_with_plugin_PROTOS AND NOT protobuf_generate_with_plugin_TARGET)
    message(SEND_ERROR "Error: protobuf_generate_with_plugin called without any targets or source files")
    return()
  endif()

  if(NOT protobuf_generate_with_plugin_OUT_VAR AND NOT protobuf_generate_with_plugin_TARGET)
    message(SEND_ERROR "Error: protobuf_generate called without a target or output variable")
    return()
  endif()

  if(NOT protobuf_generate_with_plugin_LANGUAGE)
    set(protobuf_generate_with_plugin_LANGUAGE cpp)
  endif()
  string(TOLOWER ${protobuf_generate_with_plugin_LANGUAGE} protobuf_generate_with_plugin_LANGUAGE)

  if(NOT protobuf_generate_with_plugin_PROTOC_OUT_DIR)
    set(protobuf_generate_with_plugin_PROTOC_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR})
  endif()

  if(protobuf_generate_with_plugin_EXPORT_MACRO AND protobuf_generate_with_plugin_LANGUAGE STREQUAL cpp)
    set(_dll_export_decl "dllexport_decl=${protobuf_generate_with_plugin_EXPORT_MACRO}:")
  endif()

  if(NOT protobuf_generate_with_plugin_GENERATE_EXTENSIONS)
    if(protobuf_generate_with_plugin_LANGUAGE STREQUAL cpp)
        set(protobuf_generate_with_plugin_GENERATE_EXTENSIONS ${HEADERFILEEXTENSION} .pb.cc)
    elseif(protobuf_generate_with_plugin_LANGUAGE STREQUAL python)
      set(protobuf_generate_with_plugin_GENERATE_EXTENSIONS _pb2.py)
    else()
      message(SEND_ERROR "Error: protobuf_generatewith_plugin given unknown Language ${LANGUAGE}, please provide a value for GENERATE_EXTENSIONS")
      return()
    endif()
  endif()

  if(protobuf_generate_with_plugin_TARGET)
    get_target_property(_source_list ${protobuf_generate_with_plugin_TARGET} SOURCES)
    foreach(_file ${_source_list})
      if(_file MATCHES "proto$")
        list(APPEND protobuf_generate_with_plugin_PROTOS ${_file})
      endif()
    endforeach()
  endif()

  if(NOT protobuf_generate_with_plugin_PROTOS)
    message(SEND_ERROR "Error: protobuf_generate_with_plugin could not find any .proto files")
    return()
  endif()

  if(protobuf_generate_with_plugin_APPEND_PATH)
    # Create an include path for each file specified
    foreach(_file ${protobuf_generate_with_plugin_PROTOS})
      get_filename_component(_abs_file ${_file} ABSOLUTE)
      get_filename_component(_abs_path ${_abs_file} PATH)
      list(FIND _protobuf_include_path ${_abs_path} _contains_already)
      if(${_contains_already} EQUAL -1)
          list(APPEND _protobuf_include_path -I ${_abs_path})
      endif()
    endforeach()
  else()
    set(_protobuf_include_path -I ${CMAKE_CURRENT_SOURCE_DIR})
  endif()

  foreach(DIR ${protobuf_generate_with_plugin_IMPORT_DIRS})
    get_filename_component(ABS_PATH ${DIR} ABSOLUTE)
    list(FIND _protobuf_include_path ${ABS_PATH} _contains_already)
    if(${_contains_already} EQUAL -1)
        list(APPEND _protobuf_include_path -I ${ABS_PATH})
    endif()
  endforeach()

  set(_generated_srcs_all)
  foreach(_proto ${protobuf_generate_with_plugin_PROTOS})
    get_filename_component(_abs_file ${_proto} ABSOLUTE)
    get_filename_component(_abs_dir ${_abs_file} DIRECTORY)
    get_filename_component(_basename ${_proto} NAME_WE)
    file(RELATIVE_PATH _rel_dir ${CMAKE_CURRENT_SOURCE_DIR} ${_abs_dir})

    set(_generated_srcs)
    foreach(_ext ${protobuf_generate_with_plugin_GENERATE_EXTENSIONS})
      list(APPEND _generated_srcs "${protobuf_generate_with_plugin_PROTOC_OUT_DIR}/${_rel_dir}/${_basename}${_ext}")
    endforeach()
    list(APPEND _generated_srcs_all ${_generated_srcs})

    add_custom_command(
      OUTPUT ${_generated_srcs}
      COMMAND  protobuf::protoc
      ARGS --${protobuf_generate_with_plugin_LANGUAGE}_out ${_dll_export_decl}${protobuf_generate_with_plugin_PROTOC_OUT_DIR} ${_protobuf_include_path} ${PROTOPATH} ${_abs_file} --plugin=${PLUGIN} --plugin_out=${PLUGINOUT}
      DEPENDS ${_abs_file} protobuf::protoc
      COMMENT "Running ${protobuf_generate_with_plugin_LANGUAGE} protocol buffer compiler on ${_proto} protopath [${PROTOPATH}]"
      VERBATIM )
  endforeach()

  set_source_files_properties(${_generated_srcs_all} PROPERTIES GENERATED TRUE)
  if(protobuf_generate_with_plugin_OUT_VAR)
    set(${protobuf_generate_with_plugin_OUT_VAR} ${_generated_srcs_all} PARENT_SCOPE)
  endif()
  if(protobuf_generate_with_plugin_TARGET)
    target_sources(${protobuf_generate_with_plugin_TARGET} PRIVATE ${_generated_srcs_all})
  endif()

endfunction()

function(PROTOBUF_GENERATE_CPP_WITH_PLUGIN HEADERFILEEXTENSION PLUGIN PLUGINOUT PROTOPATH SRCS HDRS)
  cmake_parse_arguments(protobuf_generate_cpp_with_plugin "" "EXPORT_MACRO" "" ${ARGN})

  set(_proto_files "${protobuf_generate_cpp_with_plugin_UNPARSED_ARGUMENTS}")
  if(NOT _proto_files)
      message(SEND_ERROR "Error: PROTOBUF_GENERATE_CPP_WITH_PLUGIN() called without any proto files")
    return()
  endif()

  if(PROTOBUF_GENERATE_CPP_WITH_PLUGIN_APPEND_PATH)
    set(_append_arg APPEND_PATH)
  endif()

  if(DEFINED Protobuf_IMPORT_DIRS)
    set(_import_arg IMPORT_DIRS ${Protobuf_IMPORT_DIRS})
  endif()

  set(_outvar)
  protobuf_generate_with_plugin(${_append_arg} LANGUAGE cpp EXPORT_MACRO ${protobuf_generate_cpp_with_plugin_EXPORT_MACRO} OUT_VAR _outvar ${_import_arg} PROTOS ${_proto_files} PLUGIN ${PLUGIN} PLUGINOUT ${PLUGINOUT} PROTOPATH ${PROTOPATH} HEADERFILEEXTENSION ${HEADERFILEEXTENSION})

  set(${SRCS})
  set(${HDRS})
  foreach(_file ${_outvar})
    if(_file MATCHES "cc$")
      list(APPEND ${SRCS} ${_file})
    else()
      list(APPEND ${HDRS} ${_file})
    endif()
  endforeach()
  set(${SRCS} ${${SRCS}} PARENT_SCOPE)
  set(${HDRS} ${${HDRS}} PARENT_SCOPE)
endfunction()

# Adds a protozero library with the given plugin name.
function(add_protobuf_with_plugin name headerfileextension plugin pluginout protolib PROTOPATH protofile genccpath)
    message(
      WARNING "protozero protofile: ${protofile}")
  protobuf_generate_cpp_with_plugin(${headerfileextension} ${plugin} ${pluginout} ${PROTOPATH} PROTO_SRCS PROTO_HDRS ${protofile})
  get_filename_component(PROTO_SRCS_ABS ${PROTO_SRCS} ABSOLUTE)
  set(genccpath2 ${CMAKE_CURRENT_BINARY_DIR}/${genccpath})
  set_source_files_properties(${genccpath} PROPERTIES GENERATED TRUE)
  set_source_files_properties(${genccpath2} PROPERTIES GENERATED TRUE)
  add_library(${name} ${PROTO_HDRS})
  target_link_libraries(${name} PUBLIC ${protolib})
  target_include_directories(${name} PUBLIC ${CMAKE_CURRENT_BINARY_DIR})
  # Disable generation of information about every class with virtual functions
  # for use by the C++ runtime type identification features (dynamic_cast and
  # typeid). If you don't use those parts of the language, you can save some
  # space by using this flag. Note that exception handling uses the same
  # information, but it will generate it as needed. The  dynamic_cast operator
  # can still be used for casts that do not require runtime type information,
  # i.e. casts to void * or to unambiguous base classes.
  target_compile_options(${name} PRIVATE -fno-rtti)
  # This needs to be public, as we don't want the headers to start exposing
  # exceptions.
  target_compile_definitions(${name} PUBLIC -DGOOGLE_PROTOBUF_NO_RTTI)
endfunction()

function(add_pbzero_library directory name)
    set(add_pbzero_library_protosrc "${directory}/${name}.proto")
    set(add_pbzero_library_protosrc_generated "${name}.pbzero.cc")
    get_filename_component(add_pbzero_library_protosrc_absdir ${directory} ABSOLUTE)

    if (MSVC)
        set(EXE_SUFFIX ".exe")
    else()
        set(EXE_SUFFIX "")
    endif()

    message("Adding pbzero for ${name}. Tracing protobuf src absdir: ${add_pbzero_library_protosrc_absdir}")

    add_protobuf_with_plugin(
      pbzero-${name}
      .pbzero.h
      "protoc-gen-plugin=${CMAKE_BINARY_DIR}/perfetto-protozero-protoc-plugin${EXE_SUFFIX}"
      "wrapper_namespace=pbzero:${CMAKE_CURRENT_BINARY_DIR}"
      perfetto-libprotozero
      "-I${perfetto}"
      "${add_pbzero_library_protosrc}"
      "${add_pbzero_library_protosrc_generated}")

    add_dependencies(
      pbzero-${name}
      perfetto-protozero-protoc-plugin)
endfunction()

add_pbzero_library(${perfetto}/protos/perfetto/common trace_stats)
add_pbzero_library(${perfetto}/protos/perfetto/config trace_config)
add_pbzero_library(${perfetto}/protos/perfetto/trace trace_packet)
add_pbzero_library(${perfetto}/protos/perfetto/trace trace)
add_pbzero_library(${perfetto}/protos/perfetto/trace clock_snapshot)
add_pbzero_library(${perfetto}/protos/perfetto/trace/interned_data interned_data)
add_pbzero_library(${perfetto}/protos/perfetto/trace/track_event counter_descriptor)
add_pbzero_library(${perfetto}/protos/perfetto/trace/track_event process_descriptor)
add_pbzero_library(${perfetto}/protos/perfetto/trace/track_event thread_descriptor)
add_pbzero_library(${perfetto}/protos/perfetto/trace/track_event track_descriptor)
add_pbzero_library(${perfetto}/protos/perfetto/trace/track_event track_event)

if (OPTION_PERFETTO_USE_SDK)
    set(VPERFETTO_FULL_SOURCES
        perfetto.cc
        vperfetto-sdk.cpp)
    set(VPERFETTO_FULL_INCLUDE
        ${CMAKE_CURRENT_SOURCE_DIR})
    set(VPERFETTO_FULL_LIBRARIES
        perfetto_trace
        ${Protobuf_LIBRARIES}
        Threads::Threads)
else()
    set(VPERFETTO_FULL_SOURCES
        vperfetto.cpp)
    set(VPERFETTO_FULL_INCLUDE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${perfetto}/include)
    set(VPERFETTO_FULL_LIBRARIES
        pbzero-trace_stats
        pbzero-trace_config
        pbzero-clock_snapshot
        pbzero-trace
        pbzero-trace_packet
        pbzero-interned_data
        pbzero-process_descriptor
        pbzero-counter_descriptor
        pbzero-thread_descriptor
        pbzero-track_descriptor
        pbzero-track_event
        Threads::Threads)
endif()

# Build targets:
# vperfetto.so, the library that does it all (callbacks on start/stop, set guest time, trace combining)
add_library(
    vperfetto
    SHARED
    ${VPERFETTO_FULL_SOURCES})
target_include_directories(
    vperfetto PUBLIC
    ${VPERFETTO_FULL_INCLUDE})
target_link_libraries(vperfetto
    PRIVATE
    ${VPERFETTO_FULL_LIBRARIES})

# vperfetto_unittests, a set of test traces
add_executable(
    vperfetto_unittests
    vperfetto_unittest.cpp)

target_link_libraries(vperfetto_unittests PUBLIC vperfetto gtest_main)

# vperfetto_merge, a tool to combine guest/host traces
add_executable(
    vperfetto_merge
    vperfetto_merge.cpp)
target_link_libraries(vperfetto_merge PUBLIC vperfetto)

# vperfetto_min, a shared library that's essentially a thin layer over the SDK
# and only does trace start/stop.
add_library(
    vperfetto_min
    SHARED
    vperfetto-min.cpp
    perfetto.cc)
target_include_directories(
    vperfetto_min PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(
    vperfetto_min
    PRIVATE
    Threads::Threads)

# Unittests for vperfetto_min
add_executable(
    vperfetto_min_unittests
    vperfetto_min_unittest.cpp)
target_link_libraries(vperfetto_min_unittests PUBLIC vperfetto_min gtest_main)
