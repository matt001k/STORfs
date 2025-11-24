cmake_minimum_required(VERSION 3.29)

project(STORfs VERSION 0.1.0 LANGUAGES C)

include(FetchContent)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED YES)
set(CMAKE_C_EXTENSIONS NO)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include_directories(
    ../src
    ../include
    ..
    .
)

enable_language(CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)
set(CMAKE_CXX_FLAGS "-g -Wall")
set(CMAKE_C_FLAGS "-g -Wall -W")
set(CMAKE_EXE_LINKER_FLAGS "-fprofile-arcs -ftest-coverage")

include(CTest)

FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG        v1.15.0
)
# For Windows: Prevent overriding the parent project's compiler/linker settings
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)
FetchContent_Declare(
  fff
  GIT_REPOSITORY https://github.com/meekrosoft/fff.git
  GIT_TAG        5111c61
)
FetchContent_MakeAvailable(fff)
include(GoogleTest)

link_libraries(
    gmock
    gtest_main
)

# - Create Unit Test
#
# This function creates a unit test with required source files.
# Coverage report information is compiled in for the file to be tested.
#
# test_name - the name to call the test
# srcs - list of sources necessary to build the test including the file to be tested

function(create_gtest test_name  srcs)
    set(GTEST_NAME ${test_name}_test)
    file(GLOB TEST_STUBS stubs/*.c)
    list(FILTER TEST_STUBS EXCLUDE REGEX "${test_name}")
    add_executable(${GTEST_NAME})
    target_include_directories(${GTEST_NAME}
        PRIVATE
        ${includes})
    target_sources(${GTEST_NAME}
        PRIVATE
        ${srcs}
        src/${test_name}_test.cpp
    )
    gtest_discover_tests(${GTEST_NAME})
    # Generate artifacts for coverage report using lcov
    file(GLOB_RECURSE FILES "../*")
    foreach(FILE ${FILES})
        if (NOT FILE MATCHES "^test/")
            get_filename_component(FILENAME "${FILE}" NAME)
            if (FILENAME MATCHES ".*${test_name}.c")
                set_source_files_properties(${FILE} PROPERTIES
                    COMPILE_FLAGS
                    "-fprofile-arcs -ftest-coverage"
                )
            endif()
        endif()
    endforeach()
endfunction()

# HASH unit test
set (HASH_TEST_SRCS
    ${COMMON_DIR}/hash.c
)
create_gtest("hash" "${HASH_TEST_SRCS}")

enable_testing()
add_dependencies(gtest update_submodules)
