# Define minimal required version of CMake.
cmake_minimum_required(VERSION "3.25")

# Project definition
project(
	opendisplay-bg22
	VERSION 1.0
	LANGUAGES C CXX ASM
)

option(OD_ENABLE_RTT "Enable SEGGER RTT logging backend" ON)
option(OD_GENERATE_GBL_OTA "After link, run Simplicity Commander to create OTA .gbl next to .s37" ON)

set(OD_APP_VERSION "0x0019" CACHE STRING "16-bit BLE app version (hex, high byte=major, low=minor)")
set(OPENDISPLAY_BUILD_ID "bg22-dev" CACHE STRING "Firmware version string (GATT 0x0043 / build id)")
set(OD_SL_APPLICATION_VERSION "" CACHE STRING "If set, overrides SL_APPLICATION_VERSION (decimal uint32)")

# Include the definition of the slc_opendisplay-bg22 target,
# which contains the content of the SLC project
include(opendisplay-bg22.cmake)

# main.c and other app sources live in target `slc`; they must see OD_ENABLE_RTT when compiled.
target_compile_definitions(slc PUBLIC
	OD_ENABLE_RTT=$<IF:$<BOOL:${OD_ENABLE_RTT}>,1,0>
	SL_APP_PROPERTIES=1
	"OD_APP_VERSION=${OD_APP_VERSION}"
	"OPENDISPLAY_BUILD_ID=\"${OPENDISPLAY_BUILD_ID}\""
)

if(NOT "${OD_SL_APPLICATION_VERSION}" STREQUAL "")
  target_compile_definitions(slc PUBLIC "SL_APPLICATION_VERSION=${OD_SL_APPLICATION_VERSION}u")
endif()

# Silicon Labs application properties (vendored; no Conan SDK_PATH).
set(OPD_APP_PROPERTIES_DIR "${CMAKE_CURRENT_LIST_DIR}/../third_party/silabs_app_properties")
target_sources(slc PRIVATE "${OPD_APP_PROPERTIES_DIR}/app_properties.c")
target_include_directories(slc PUBLIC
  "${OPD_APP_PROPERTIES_DIR}"
  "${CMAKE_CURRENT_LIST_DIR}/../config"
)

if(NOT OD_ENABLE_RTT)
    set_source_files_properties(
        "../third_party/segger_rtt/SEGGER_RTT.c"
        "../od_rtt.c"
        PROPERTIES HEADER_FILE_ONLY TRUE
    )
endif()

add_executable(opendisplay-bg22
    # Add additional sources here
)

target_include_directories(opendisplay-bg22 PUBLIC
    # Add additional include paths here
)

target_compile_definitions(opendisplay-bg22 PUBLIC
    OD_ENABLE_RTT=$<IF:$<BOOL:${OD_ENABLE_RTT}>,1,0>
)

target_compile_options(opendisplay-bg22 PUBLIC
    # Set additional compiler flags here
)

target_link_options(opendisplay-bg22 PUBLIC
    # Set additional linker flags here
)

# Link with the content defined in the SLC project
target_link_libraries(opendisplay-bg22 PRIVATE
    slc
)

set(OPD_BOOTLOADER_S37 "${CMAKE_CURRENT_LIST_DIR}/../bootloader-artifact/opendisplay-bootloader.s37")
if(EXISTS "${OPD_BOOTLOADER_S37}")
  add_custom_command(
    TARGET opendisplay-bg22
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${OPD_BOOTLOADER_S37}"
      "$<TARGET_FILE_DIR:opendisplay-bg22>/bootloader-apploader.s37"
    COMMENT "Stage bootloader-apploader.s37 (from bootloader-artifact/)"
    VERBATIM
  )
else()
  message(WARNING "Missing ${OPD_BOOTLOADER_S37} — full-flash bootloader not staged (see bootloader-artifact/README.md)")
endif()

# Include managed project content if available
include(opendisplay-bg22_project.cmake OPTIONAL RESULT_VARIABLE managed_project)
if(managed_project)
    message(STATUS "Using managed project content from ${managed_project}")
endif()

# Force the gcc linker command
set_target_properties(opendisplay-bg22 PROPERTIES LINKER_LANGUAGE C)

# Create .bin, .hex and .s37 artifacts after building the project
add_custom_command(TARGET opendisplay-bg22
    POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} ${OBJCOPY_SREC_CMD} "$<TARGET_FILE:opendisplay-bg22>" "$<TARGET_FILE_DIR:opendisplay-bg22>/$<TARGET_FILE_BASE_NAME:opendisplay-bg22>.s37"
    COMMAND ${CMAKE_OBJCOPY} ${OBJCOPY_IHEX_CMD} "$<TARGET_FILE:opendisplay-bg22>" "$<TARGET_FILE_DIR:opendisplay-bg22>/$<TARGET_FILE_BASE_NAME:opendisplay-bg22>.hex"
    COMMAND ${CMAKE_OBJCOPY} ${OBJCOPY_BIN_CMD}  "$<TARGET_FILE:opendisplay-bg22>" "$<TARGET_FILE_DIR:opendisplay-bg22>/$<TARGET_FILE_BASE_NAME:opendisplay-bg22>.bin"
)

set(OPD_COMMANDER "")
if(DEFINED ENV{COMMANDER} AND EXISTS "$ENV{COMMANDER}")
  cmake_path(SET OPD_COMMANDER "$ENV{COMMANDER}" NORMALIZE)
elseif(DEFINED POST_BUILD_EXE AND POST_BUILD_EXE AND EXISTS "${POST_BUILD_EXE}")
  set(OPD_COMMANDER "${POST_BUILD_EXE}")
else()
  cmake_path(SET _opd_cmd_candidate "${CMAKE_CURRENT_LIST_DIR}/../../SimplicityCommander-Linux/commander/commander")
  if(EXISTS "${_opd_cmd_candidate}")
    set(OPD_COMMANDER "${_opd_cmd_candidate}")
  endif()
endif()
if(NOT OPD_COMMANDER)
  find_program(OPD_COMMANDER NAMES commander)
endif()

if(OD_GENERATE_GBL_OTA AND OPD_COMMANDER)
  add_custom_command(
    TARGET opendisplay-bg22
    POST_BUILD
    COMMAND "${OPD_COMMANDER}" gbl create
      "$<TARGET_FILE_DIR:opendisplay-bg22>/$<TARGET_FILE_BASE_NAME:opendisplay-bg22>.gbl"
      --app "$<TARGET_FILE_DIR:opendisplay-bg22>/$<TARGET_FILE_BASE_NAME:opendisplay-bg22>.s37"
    COMMENT "OTA: commander gbl create -> $<TARGET_FILE_BASE_NAME:opendisplay-bg22>.gbl"
    VERBATIM
  )
elseif(OD_GENERATE_GBL_OTA)
  message(STATUS "Simplicity Commander not found (slt/COMMANDER/ PATH); CMake will not emit .gbl — use build-and-flash.sh or set COMMANDER / POST_BUILD_EXE")
endif()

# Run post-build pipeline to perform additional post-processing
if(post_build_command)
add_custom_command(TARGET opendisplay-bg22
    POST_BUILD
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}/..
    COMMAND ${post_build_command}
)
endif()