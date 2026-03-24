# stage_romfs.cmake — Assemble a romfs staging directory (build-time script)
#
# Invoked via: cmake -P stage_romfs.cmake -D STAGING=... -D PROJECT_ROOT=...
#
# Required variables:
#   STAGING       — output staging directory
#   PROJECT_ROOT  — project root for src/common headers
#   USER_ELFS     — semicolon-separated list of user ELF paths
#   BB_DIR        — busybox build directory
#   BB_APPLETS    — semicolon-separated list of busybox applets
#   BB_SBIN_APPLETS  — semicolon-separated list of sbin applets
#   ROGUE         — path to rogue binary
#   ETC_DIR       — base /etc directory
#
# Optional:
#   OVERLAY_DIR       — target-specific overlay directory
#   EXTRA_OVERLAY_DIR — user-specified overlay (applied last, highest priority)

# --- Clean and create directory structure ---
file(REMOVE_RECURSE "${STAGING}")
file(MAKE_DIRECTORY
    "${STAGING}/bin"
    "${STAGING}/sbin"
    "${STAGING}/etc"
    "${STAGING}/dev"
    "${STAGING}/proc"
    "${STAGING}/tmp"
    "${STAGING}/usr/bin"
    "${STAGING}/usr/include"
    "${STAGING}/mnt/sd"
    "${STAGING}/mnt/ufs"
)

# --- Install user programs ---
# EXCLUDE_APPS uses ":" as separator (avoids cmake list-separator conflicts)
if(DEFINED EXCLUDE_APPS)
    string(REPLACE ":" ";" EXCLUDE_APPS "${EXCLUDE_APPS}")
endif()
foreach(elf IN LISTS USER_ELFS)
    get_filename_component(name "${elf}" NAME_WE)
    list(FIND EXCLUDE_APPS "${name}" _excl_idx)
    if(NOT _excl_idx EQUAL -1)
        continue()
    endif()
    if(name STREQUAL "init")
        file(COPY "${elf}" DESTINATION "${STAGING}/sbin")
        file(RENAME "${STAGING}/sbin/${name}.elf" "${STAGING}/sbin/${name}")
    elseif(name STREQUAL "ttyctl")
        file(COPY "${elf}" DESTINATION "${STAGING}/usr/bin")
        file(RENAME "${STAGING}/usr/bin/${name}.elf" "${STAGING}/usr/bin/${name}")
    else()
        file(COPY "${elf}" DESTINATION "${STAGING}/bin")
        file(RENAME "${STAGING}/bin/${name}.elf" "${STAGING}/bin/${name}")
    endif()
endforeach()

# --- /bin/sh → push ---
file(CREATE_LINK "push" "${STAGING}/bin/sh" SYMBOLIC)

# --- Install busybox (if available) ---
if(BB_DIR AND EXISTS "${BB_DIR}/busybox")
    file(COPY "${BB_DIR}/busybox" DESTINATION "${STAGING}/bin")
    foreach(a IN LISTS BB_APPLETS)
        file(CREATE_LINK "busybox" "${STAGING}/bin/${a}" SYMBOLIC)
    endforeach()
    foreach(a IN LISTS BB_SBIN_APPLETS)
        file(CREATE_LINK "../bin/busybox" "${STAGING}/sbin/${a}" SYMBOLIC)
    endforeach()
endif()

# --- Install rogue (if available) ---
if(ROGUE AND EXISTS "${ROGUE}")
    file(COPY "${ROGUE}" DESTINATION "${STAGING}/bin")
endif()

# --- Install ZEXDOC/ZEXALL for CP/M subsystem testing ---
if(EXISTS "${PROJECT_ROOT}/third_party/zexall/zexdoc.com")
    file(MAKE_DIRECTORY "${STAGING}/lib")
    file(COPY "${PROJECT_ROOT}/third_party/zexall/zexdoc.com"
         DESTINATION "${STAGING}/lib")
endif()
if(EXISTS "${PROJECT_ROOT}/third_party/zexall/zexall.com")
    file(MAKE_DIRECTORY "${STAGING}/lib")
    file(COPY "${PROJECT_ROOT}/third_party/zexall/zexall.com"
         DESTINATION "${STAGING}/lib")
endif()

# --- Install shared ABI headers ---
file(GLOB _headers "${PROJECT_ROOT}/src/common/*.h")
foreach(h IN LISTS _headers)
    file(COPY "${h}" DESTINATION "${STAGING}/usr/include")
endforeach()

# --- Copy base /etc files ---
if(ETC_DIR AND IS_DIRECTORY "${ETC_DIR}")
    file(GLOB _etc_files "${ETC_DIR}/*")
    foreach(f IN LISTS _etc_files)
        file(COPY "${f}" DESTINATION "${STAGING}/etc")
    endforeach()
endif()

# --- Apply target-specific overlay ---
if(DEFINED OVERLAY_DIR AND IS_DIRECTORY "${OVERLAY_DIR}")
    file(GLOB_RECURSE _overlay_files RELATIVE "${OVERLAY_DIR}" "${OVERLAY_DIR}/*")
    foreach(f IN LISTS _overlay_files)
        get_filename_component(_dir "${f}" DIRECTORY)
        file(MAKE_DIRECTORY "${STAGING}/${_dir}")
        file(COPY "${OVERLAY_DIR}/${f}" DESTINATION "${STAGING}/${_dir}")
    endforeach()
endif()

# --- Apply user-specified extra overlay (highest priority) ---
if(DEFINED EXTRA_OVERLAY_DIR AND IS_DIRECTORY "${EXTRA_OVERLAY_DIR}")
    file(GLOB_RECURSE _extra_files RELATIVE "${EXTRA_OVERLAY_DIR}" "${EXTRA_OVERLAY_DIR}/*")
    foreach(f IN LISTS _extra_files)
        get_filename_component(_dir "${f}" DIRECTORY)
        file(MAKE_DIRECTORY "${STAGING}/${_dir}")
        file(COPY "${EXTRA_OVERLAY_DIR}/${f}" DESTINATION "${STAGING}/${_dir}")
    endforeach()
endif()
