cmake_minimum_required(VERSION 3.16)
project(VasyanLang C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Runtime: value, bytecode, validator and the VM.
add_library(vasyan_runtime STATIC
    src/value.cpp
    src/opcodes.cpp
    src/module.cpp
    src/validator.cpp
    src/vm.cpp
)

target_include_directories(vasyan_runtime
    PUBLIC  include
)

# Toolchain: the compiler front end plus the command-line tool.
add_executable(vasyan
    src/lexer.cpp
    src/parser.cpp
    src/compiler.cpp
    src/main.cpp
)

target_link_libraries(vasyan PRIVATE vasyan_runtime)

# The source and the language's keywords/output are UTF-8 (Cyrillic). Tell MSVC
# so string literals and identifiers keep their bytes and console output is
# correct; harmless/ignored on other compilers.
if (MSVC)
    target_compile_options(vasyan_runtime PRIVATE /utf-8)
    target_compile_options(vasyan PRIVATE /utf-8)
endif()
