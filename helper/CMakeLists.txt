# SPDX-License-Identifier: GPL-2.0-only
cmake_minimum_required(VERSION 3.13)
project(rsim-card C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
# warnings are errors while developing; a distribution build with a newer
# compiler than this was written against turns that off
option(RSIM_WERROR "treat warnings as errors" ON)
add_compile_options(-Wall -Wextra)
if(RSIM_WERROR)
	add_compile_options(-Werror)
endif()
# cfmakeraw, CRTSCTS and dprintf are outside strict C99/POSIX
add_compile_definitions(_DEFAULT_SOURCE)

# A static binary runs on a machine that has nothing installed — the usual
# case for the SIM host reached over `ssh:<user>@<host>:<reader>`, which only
# needs `rsim-card` in its PATH. ON links with -static and takes the STATIC
# link lines from pkg-config, so the optional backends bring their own
# dependencies (libusb pulls -lpthread, libpcsclite -lpthread -ldl) instead of
# leaving undefined symbols. Nothing here is architecture-specific: the
# toolchain decides, so the same switch works for a cross build.
option(RSIM_STATIC "link rsim-card statically" OFF)
if(RSIM_STATIC)
	# prefer the archives when both are installed next to each other
	set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
endif()

# PC/SC is optional so that the Phoenix-only package has no dependency:
# AUTO builds it when pkg-config finds libpcsclite, ON insists, OFF skips.
set(WITH_PCSC AUTO CACHE STRING "PC/SC backend: AUTO, ON or OFF")
set_property(CACHE WITH_PCSC PROPERTY STRINGS AUTO ON OFF)
set(RSIM_PCSC OFF)
if(NOT WITH_PCSC STREQUAL "OFF")
	find_package(PkgConfig)
	if(PKG_CONFIG_FOUND)
		pkg_check_modules(PCSC libpcsclite)
	endif()
	if(PCSC_FOUND)
		set(RSIM_PCSC ON)
	elseif(WITH_PCSC STREQUAL "ON")
		message(FATAL_ERROR "WITH_PCSC=ON but libpcsclite was not found by pkg-config")
	endif()
endif()
message(STATUS "rsim-card: PC/SC backend ${RSIM_PCSC}")

# libusb only for the WB Electronics Smartmouse USB: its clock and mode are
# set over USB (src/wbsm.c), and its FTDI UART is then driven over libusb as
# well (src/ftdi_usb.c), because the target kernels have no ftdi_sio
set(WITH_LIBUSB AUTO CACHE STRING "Smartmouse USB support (libusb-1.0): AUTO, ON or OFF")
set_property(CACHE WITH_LIBUSB PROPERTY STRINGS AUTO ON OFF)
set(RSIM_LIBUSB OFF)
if(NOT WITH_LIBUSB STREQUAL "OFF")
	find_package(PkgConfig)
	if(PKG_CONFIG_FOUND)
		pkg_check_modules(LIBUSB libusb-1.0)
	endif()
	if(LIBUSB_FOUND)
		set(RSIM_LIBUSB ON)
	elseif(WITH_LIBUSB STREQUAL "ON")
		message(FATAL_ERROR "WITH_LIBUSB=ON but libusb-1.0 was not found by pkg-config")
	endif()
endif()
message(STATUS "rsim-card: Smartmouse USB (libusb) ${RSIM_LIBUSB}")

add_library(rsimcore STATIC src/atr.c src/t0.c src/json.c src/log.c src/ftdi_proto.c)
target_include_directories(rsimcore PUBLIC src)

set(RSIM_SOURCES src/main.c src/phoenix.c src/phx_tty.c src/phoenix_baud.c src/atmodem.c)
if(RSIM_PCSC)
	list(APPEND RSIM_SOURCES src/pcsc.c)
endif()
if(RSIM_LIBUSB)
	list(APPEND RSIM_SOURCES src/wbsm.c src/ftdi_usb.c)
endif()
add_executable(rsim-card ${RSIM_SOURCES})
target_link_libraries(rsim-card rsimcore)
if(RSIM_STATIC)
	target_link_options(rsim-card PRIVATE -static)
endif()
if(RSIM_PCSC)
	target_compile_definitions(rsim-card PRIVATE WITH_PCSC)
	target_include_directories(rsim-card PRIVATE ${PCSC_INCLUDE_DIRS})
	if(RSIM_STATIC)
		target_link_libraries(rsim-card ${PCSC_STATIC_LDFLAGS})
	else()
		target_link_libraries(rsim-card ${PCSC_LDFLAGS})
	endif()
endif()
if(RSIM_LIBUSB)
	target_compile_definitions(rsim-card PRIVATE WITH_LIBUSB)
	target_include_directories(rsim-card PRIVATE ${LIBUSB_INCLUDE_DIRS})
	if(RSIM_STATIC)
		target_link_libraries(rsim-card ${LIBUSB_STATIC_LDFLAGS})
	else()
		target_link_libraries(rsim-card ${LIBUSB_LDFLAGS})
	endif()
endif()
install(TARGETS rsim-card RUNTIME DESTINATION bin)

include(CTest)
if(BUILD_TESTING)
	foreach(t atr t0 json ftdi)
		add_executable(test_${t} tests/test_${t}.c)
		target_link_libraries(test_${t} rsimcore)
		add_test(NAME ${t} COMMAND test_${t})
	endforeach()
	find_package(Python3 COMPONENTS Interpreter)
	if(Python3_FOUND)
		add_test(NAME e2e_phoenix
			COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_e2e.py $<TARGET_FILE:rsim-card>)
		add_test(NAME e2e_at
			COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_e2e_at.py $<TARGET_FILE:rsim-card>)
		set_tests_properties(e2e_at PROPERTIES TIMEOUT 180)
		set_tests_properties(e2e_phoenix PROPERTIES TIMEOUT 120)
	endif()
endif()
