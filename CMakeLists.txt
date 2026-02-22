cmake_minimum_required(VERSION 3.15)
project(OpenVISA VERSION 0.2.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)

# Options
option(OPENVISA_WITH_USB "Enable USBTMC support (requires libusb)" ON)

# Sources
set(SOURCES
    src/core/session.c
    src/core/discovery.c
    src/transport/transport.c
    src/transport/tcpip_raw.c
    src/transport/tcpip_vxi11.c
    src/transport/tcpip_hislip.c
    src/transport/usbtmc.c
    src/transport/serial.c
    src/transport/gpib.c
)

# Shared library (visa32.dll / libvisa.so)
add_library(visa SHARED ${SOURCES})
target_include_directories(visa PUBLIC include PRIVATE src)
target_compile_definitions(visa PRIVATE OPENVISA_EXPORTS)

# Static library (for embedding)
add_library(visa_static STATIC ${SOURCES})
target_include_directories(visa_static PUBLIC include PRIVATE src)
target_compile_definitions(visa_static PRIVATE OPENVISA_EXPORTS)
set_target_properties(visa_static PROPERTIES OUTPUT_NAME "visa_static")

# Platform-specific linking
if(WIN32)
    target_link_libraries(visa PRIVATE ws2_32)
    target_link_libraries(visa_static PRIVATE ws2_32)
    # Output visa32.dll or visa64.dll based on target architecture
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set_target_properties(visa PROPERTIES OUTPUT_NAME "visa64" PREFIX "")
        set_target_properties(visa_static PROPERTIES OUTPUT_NAME "visa64_static" PREFIX "")
    else()
        set_target_properties(visa PROPERTIES OUTPUT_NAME "visa32" PREFIX "")
        set_target_properties(visa_static PROPERTIES OUTPUT_NAME "visa32_static" PREFIX "")
    endif()
    # Generate .lib import library for MSVC
    set_target_properties(visa PROPERTIES
        WINDOWS_EXPORT_ALL_SYMBOLS OFF
    )
    # DEF file for explicit exports (optional, we use __declspec(dllexport))
else()
    target_link_libraries(visa PRIVATE dl)
    target_link_libraries(visa_static PRIVATE dl)
    set_target_properties(visa PROPERTIES
        OUTPUT_NAME "visa"
        VERSION ${PROJECT_VERSION}
        SOVERSION 0
    )
endif()

# Optional: libusb for USBTMC
if(OPENVISA_WITH_USB)
    find_package(PkgConfig QUIET)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(LIBUSB libusb-1.0)
    endif()
    if(LIBUSB_FOUND)
        target_include_directories(visa PRIVATE ${LIBUSB_INCLUDE_DIRS})
        target_link_libraries(visa PRIVATE ${LIBUSB_LIBRARIES})
        target_compile_definitions(visa PRIVATE OPENVISA_HAS_LIBUSB)
        target_include_directories(visa_static PRIVATE ${LIBUSB_INCLUDE_DIRS})
        target_link_libraries(visa_static PRIVATE ${LIBUSB_LIBRARIES})
        target_compile_definitions(visa_static PRIVATE OPENVISA_HAS_LIBUSB)
        message(STATUS "libusb found — USBTMC enabled")
    else()
        message(STATUS "libusb not found — USBTMC will return VI_ERROR_NSUP_OPER")
    endif()
endif()

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

# Install rules
include(GNUInstallDirs)
install(TARGETS visa
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(FILES include/visa.h include/visatype.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
