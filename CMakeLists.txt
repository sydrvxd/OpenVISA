cmake_minimum_required(VERSION 3.15)
project(OpenVISA VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)

# Sources
set(SOURCES
    src/core/session.c
    src/transport/transport.c
    src/transport/tcpip_raw.c
)

# Shared library (visa32.dll / libvisa.so)
add_library(visa SHARED ${SOURCES})
target_include_directories(visa PUBLIC include PRIVATE src)
target_compile_definitions(visa PRIVATE OPENVISA_EXPORTS)

# Platform-specific
if(WIN32)
    target_link_libraries(visa PRIVATE ws2_32)
    set_target_properties(visa PROPERTIES
        OUTPUT_NAME "visa64"
        PREFIX ""
    )
    # Also build 32-bit name for compat
    # TODO: conditional on architecture
endif()

if(UNIX)
    set_target_properties(visa PROPERTIES
        OUTPUT_NAME "visa"
        VERSION ${PROJECT_VERSION}
        SOVERSION 0
    )
endif()

# Static library (for embedding)
add_library(visa_static STATIC ${SOURCES})
target_include_directories(visa_static PUBLIC include PRIVATE src)
set_target_properties(visa_static PROPERTIES OUTPUT_NAME "visa_static")

# Example program
add_executable(example_idn examples/idn_query.c)
target_link_libraries(example_idn PRIVATE visa)
target_include_directories(example_idn PRIVATE include)

# Tests
enable_testing()
add_executable(test_parser tests/test_parser.c)
target_link_libraries(test_parser PRIVATE visa_static)
target_include_directories(test_parser PRIVATE include src)
add_test(NAME parser_tests COMMAND test_parser)
