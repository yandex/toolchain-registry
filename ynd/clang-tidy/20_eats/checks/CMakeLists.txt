cmake_minimum_required(VERSION 3.13)
project(eats-clang-tidy-plugin CXX)

set(LLVM_DIR "/usr/lib/llvm-20/lib/cmake/llvm" CACHE PATH "Directory with LLVMConfig.cmake")
set(Clang_DIR "/usr/lib/llvm-20/lib/cmake/clang" CACHE PATH "Directory with ClangConfig.cmake")

find_package(LLVM REQUIRED CONFIG)
# Checks must match the clang-tidy 20 that the ix package builds them into.
# find_package(LLVM 20) is not used: LLVMConfigVersion requires an exact minor match.
if(NOT LLVM_VERSION_MAJOR EQUAL 20)
    message(FATAL_ERROR "LLVM 20 is required, found LLVM ${LLVM_PACKAGE_VERSION}")
endif()
find_package(Clang REQUIRED CONFIG)

# Non-recursive: test inputs in tests/ must not be compiled into the plugin
file(GLOB EATS_CHECK_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)

add_library(EatsTidyPlugin MODULE ${EATS_CHECK_SOURCES})

target_include_directories(EatsTidyPlugin PRIVATE ${LLVM_INCLUDE_DIRS} ${CLANG_INCLUDE_DIRS})
target_compile_definitions(EatsTidyPlugin PRIVATE ${LLVM_DEFINITIONS})
target_compile_features(EatsTidyPlugin PRIVATE cxx_std_17)
if(NOT LLVM_ENABLE_RTTI)
    target_compile_options(EatsTidyPlugin PRIVATE -fno-rtti)
endif()

target_link_libraries(EatsTidyPlugin
  PRIVATE
  clangTidy
  clangTidyUtils
  clangAST
  clangASTMatchers
  clangBasic
  clangLex
)
