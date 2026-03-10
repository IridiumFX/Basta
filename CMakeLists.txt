cmake_minimum_required(VERSION 3.20)

project(basta
    VERSION 0.1.0
    DESCRIPTION "Binary And Simple Text Archive"
    LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

option(BASTA_BUILD_TESTS "Build the test executable" ON)

# ---- Shared library ----

add_library(basta SHARED
    src/main/c/basta_lexer.c
    src/main/c/basta_parser.c
    src/main/c/basta_value.c
    src/main/c/basta_writer.c
)

target_include_directories(basta
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/main/h>
            $<INSTALL_INTERFACE:include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/main/c
)

target_compile_definitions(basta PRIVATE BASTA_BUILDING)

if(NOT WIN32)
    target_compile_options(basta PRIVATE -fvisibility=hidden -Wall -Wextra)
endif()

set_target_properties(basta PROPERTIES
    VERSION   ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
)

if(UNIX)
    target_link_libraries(basta PRIVATE m)
endif()

# ---- Tests ----

if(BASTA_BUILD_TESTS)
    add_executable(basta_test
        src/test/c/basta_test.c
    )
    target_include_directories(basta_test PRIVATE
        src/main/h
        src/main/c
    )
    target_link_libraries(basta_test PRIVATE basta)
    # basta_test links the shared library, so let the default
    # dllimport/dllexport decoration apply — no BASTA_STATIC.

    enable_testing()
    add_test(NAME basta_test COMMAND basta_test)
endif()

# ---- Install ----

include(GNUInstallDirs)

install(TARGETS basta
    EXPORT  bastaTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(FILES src/main/h/basta.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(EXPORT bastaTargets
    FILE        bastaTargets.cmake
    NAMESPACE   basta::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/basta
)

include(CMakePackageConfigHelpers)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/bastaConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)
