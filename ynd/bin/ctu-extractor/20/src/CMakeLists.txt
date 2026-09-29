cmake_minimum_required(VERSION 3.20)

# Built as an LLVM external project by the IX recipe.  Keeping the extractor
# as a CMake target makes schema generation an explicit build-graph edge and
# lets it link the Clang libraries from the same source tree as the compiler.
project(ctu_extractor LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Protobuf REQUIRED CONFIG)
find_library(CTU_ZSTD_LIBRARY NAMES zstd REQUIRED)
find_path(CTU_ZSTD_INCLUDE_DIR NAMES zstd.h REQUIRED)
find_program(CTU_PROTOC_EXECUTABLE NAMES protoc REQUIRED)

set(CTU_PROTO ${CMAKE_CURRENT_SOURCE_DIR}/factspb/facts.proto)
set(CTU_PROTO_CC ${CMAKE_CURRENT_BINARY_DIR}/factspb/facts.pb.cc)
set(CTU_PROTO_H ${CMAKE_CURRENT_BINARY_DIR}/factspb/facts.pb.h)

add_custom_command(
  OUTPUT ${CTU_PROTO_CC} ${CTU_PROTO_H}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/factspb
  COMMAND ${CTU_PROTOC_EXECUTABLE}
          --proto_path=${CMAKE_CURRENT_SOURCE_DIR}
          --cpp_out=${CMAKE_CURRENT_BINARY_DIR}
          ${CTU_PROTO}
  DEPENDS ${CTU_PROTO}
  COMMENT "Generating CTU extractor protobuf bindings"
  VERBATIM)

set(LLVM_LINK_COMPONENTS Support)

add_clang_tool(ctu-extractor
  driver.cpp
  source_utils.cpp
  fact_visitor_decls.cpp
  fact_visitor_exprs.cpp
  fact_visitor_control_flow.cpp
  fact_visitor_pointers.cpp
  ${CTU_PROTO_CC})

target_include_directories(ctu-extractor PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_BINARY_DIR}
  # An LLVM external project is a sibling of clang, so AddClang does not
  # propagate clang's directory-scoped include paths to this target.
  ${LLVM_MAIN_SRC_DIR}/../clang/include
  ${LLVM_BINARY_DIR}/tools/clang/include
  ${CTU_ZSTD_INCLUDE_DIR})

# IX builds both Clang and protobuf without C++ RTTI/exceptions.  Inherit those
# ABI settings from LLVM; enabling RTTI here would emit references to Clang
# typeinfo which the linked Clang libraries intentionally do not provide.
target_compile_options(ctu-extractor PRIVATE -fno-strict-aliasing)

clang_target_link_libraries(ctu-extractor PRIVATE
  clangAnalysis
  clangAST
  clangBasic
  clangDriver
  clangFrontend
  clangIndex
  clangLex
  clangRewrite
  clangSerialization
  clangTooling)

target_link_libraries(ctu-extractor PRIVATE
  protobuf::libprotobuf
  ${CTU_ZSTD_LIBRARY})
