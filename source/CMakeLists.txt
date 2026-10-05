cmake_minimum_required (VERSION 3.20)

project("OnionHEN")

set(PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(OnionTarget)
get_filename_component(ONIONHEN_ROOT "${PROJECT_ROOT}/.." ABSOLUTE)
set(ONIONHEN_THIRD_PARTY_DIR "${ONIONHEN_ROOT}/third_party" CACHE PATH
	"Third-party dependency source directory")
set(ONIONHEN_COMMON_DIR "${PROJECT_ROOT}/common")
set(ONIONHEN_PS5_STUB_DIR "${PROJECT_ROOT}/platform/ps5/stubs")
set(ONIONHEN_KSTUFF_ELF "${ONIONHEN_ROOT}/.cache/dependencies/kstuff.elf"
	CACHE FILEPATH "Path to the kstuff ELF embedded by the bootstrapper")
set(ONIONHEN_GENERATED_INCLUDE_DIR "${CMAKE_BINARY_DIR}/generated/include")
set(ONIONHEN_VERSION_HEADER
	"${ONIONHEN_GENERATED_INCLUDE_DIR}/onion/version.h")

# Forks set their own release number (e.g. 0.1.0-rc1); the commit is appended.
set(ONION_PRODUCT_VERSION "" CACHE STRING "Product version shown instead of the git tag")

# Re-evaluate Git state on every build. The script preserves the header mtime
# when the version is unchanged, so incremental compilation remains cheap.
add_custom_target(onion_version
	COMMAND "${CMAKE_COMMAND}"
		"-DREPO_ROOT=${ONIONHEN_ROOT}"
		"-DOUTPUT_FILE=${ONIONHEN_VERSION_HEADER}"
		"-DPRODUCT_VERSION=${ONION_PRODUCT_VERSION}"
		-P "${PROJECT_ROOT}/cmake/GenerateVersionHeader.cmake"
	BYPRODUCTS "${ONIONHEN_VERSION_HEADER}"
	COMMENT "Generating OnionHEN build version"
	VERBATIM
)

# ---------------------------------------------------------------------------
# Artifact layout
#   ${CMAKE_BINARY_DIR}/bin  — ELFs (daemon, util, bootstrapper, OnionHEN, …)
#   ${CMAKE_BINARY_DIR}/lib  — built static libs (libonion_*, …)
# PS5 system-library stubs stay under source/platform/ps5/stubs.
#
# Default scripts/build.sh uses -B <repo>/build so products land at repo/build/.
# ---------------------------------------------------------------------------
set(ONIONHEN_OUT_BIN "${CMAKE_BINARY_DIR}/bin" CACHE PATH "ELF output directory")
set(ONIONHEN_OUT_LIB "${CMAKE_BINARY_DIR}/lib" CACHE PATH "Built static-lib output directory")

file(MAKE_DIRECTORY "${ONIONHEN_OUT_BIN}" "${ONIONHEN_OUT_LIB}")

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${ONIONHEN_OUT_BIN})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${ONIONHEN_OUT_LIB})
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${ONIONHEN_OUT_LIB})

# ---------------------------------------------------------------------------
# Branding. The defaults reproduce OnionHEN; forks override these cache
# variables instead of editing paths and strings throughout the tree.
# C/C++ code reads them through <onion/brand.h>, locale strings through the
# {brand} / {data_dir} / {issues_url} placeholders.
# ---------------------------------------------------------------------------
set(ONION_BRAND_NAME "OnionHEN" CACHE STRING
	"Product name shown in the Toolbox, notifications and logs")
set(ONION_DIR_NAME "OnionHEN" CACHE STRING
	"Data directory name: /data/<name> and /mnt/usbN/<name>")
set(ONION_DIR_NAME_LOWER "onionhen" CACHE STRING
	"Lower-case directory name: runtime state and legacy paths")
set(ONION_ISSUES_URL "https://github.com/aydencharles/onionHEN/issues" CACHE STRING
	"Issue tracker shown in crash notifications")
set(ONION_PAYLOAD_NAME "OnionHEN" CACHE STRING
	"Final payload file name, without the .elf extension")
set(ONION_SETTINGS_LABEL "OnionHEN Tools" CACHE STRING
	"Toolbox row in Settings; exactly 14 ASCII characters")
string(LENGTH "${ONION_SETTINGS_LABEL}" _onion_settings_label_len)
if(NOT _onion_settings_label_len EQUAL 14)
	message(FATAL_ERROR "ONION_SETTINGS_LABEL must be exactly 14 characters")
endif()
add_compile_definitions(
	ONION_BRAND_NAME="${ONION_BRAND_NAME}"
	ONION_DIR_NAME="${ONION_DIR_NAME}"
	ONION_DIR_NAME_LOWER="${ONION_DIR_NAME_LOWER}"
	ONION_SETTINGS_LABEL="${ONION_SETTINGS_LABEL}"
)
# kstuff is embedded by default. Forks that ship it separately turn this off:
# the bootstrapper then loads ${ONION_DATA_DIR}/kstuff.elf only and never
# carries a kstuff binary of its own.
option(ONION_EMBED_KSTUFF "Embed kstuff.elf in the bootstrapper" ON)
# Directory with payload ELFs to carry inside the bootstrapper and install
# on first boot (shadowmountplus.elf, ftpsrv.elf, nanodns.elf, neoweb.elf).
# Empty: nothing is bundled. See bootstrapper/source/bundled_payloads.c.
set(ONION_BUNDLE_DIR "" CACHE PATH "Payloads embedded in the bootstrapper")
# Hide Toolbox entries not yet verified on a console (overlay, PKG install).
option(ONION_HIDE_UNTESTED "Hide untested Toolbox features" OFF)
if(ONION_HIDE_UNTESTED)
	add_compile_definitions(ONION_HIDE_UNTESTED=1)
endif()
if(NOT ONION_EMBED_KSTUFF)
	add_compile_definitions(ONION_EXTERNAL_KSTUFF=1)
endif()
# 128x128 RGBA PNG: startup notification, HomeUI top-nav and Settings icon.
set(ONION_STARTUP_ICON "${PROJECT_ROOT}/bootstrapper/assets/onionhen_sicon.png"
	CACHE FILEPATH "Icon embedded by the bootstrapper")
set(ONION_I18N_BRAND_ARGS
	--brand "${ONION_BRAND_NAME}"
	--data-dir "/data/${ONION_DIR_NAME}"
	--issues-url "${ONION_ISSUES_URL}"
)

include_directories(BEFORE "${ONIONHEN_GENERATED_INCLUDE_DIR}")
# First-party shared contracts — not SYSTEM, so warnings in them are visible.
include_directories("${PROJECT_ROOT}/include")
include_directories(SYSTEM "${PS5_PAYLOAD_SDK}")
include_directories(SYSTEM "${PS5_PAYLOAD_SDK}/include")

# Built libraries first, then PS5 system stubs.
link_directories("${ONIONHEN_OUT_LIB}" "${ONIONHEN_PS5_STUB_DIR}")

add_subdirectory(libhijacker)
add_subdirectory(libNidResolver)
# add_subdirectory(libRPI)
# libSelfDecryptor removed from OnionHEN.
# Shared proc/ucred before NineS (inject path links these symbols).
add_subdirectory(libonion_proc)
add_subdirectory(libonion_fps)
add_subdirectory(libonion_platform)
add_subdirectory(libonion_payload)
add_subdirectory(libonion_conflict)
add_subdirectory(libonion_ipc)
add_subdirectory(libonion_settings)
add_subdirectory(libonion_detour)
add_subdirectory(libonion_ready)
add_subdirectory(libonion_lifecycle)
add_subdirectory(libonion_plugin_ui)
add_subdirectory(libonion_plugin_bridge)
add_subdirectory(libonion_plugin_session)
add_subdirectory(libonion_plugin_manager)
# Shared ptrace + ELF inject (authid elevated once at inject entry, not per syscall).
add_subdirectory(libonion_elfldr)
add_subdirectory(libonion_sprx)
add_subdirectory(onion_elfldr_server)
add_subdirectory(libNineS)
add_subdirectory(shellui)
add_subdirectory(util)
add_subdirectory(daemon)
add_subdirectory(bootstrapper)
add_subdirectory(unpacker)
