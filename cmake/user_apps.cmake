# user_apps.cmake — Single source of truth for first-party userland apps.
#
# Listing every app (and its multi-source pieces) here means each target's
# build only needs to iterate this list to know what to build and install.
# Targets that can't build a given app on their toolchain opt out via a
# per-target skip set (see USER_APPS_<TARGET>_SKIP below).
#
# Included by cmake/user.cmake (romfs targets: arm/m68k/riscv/xtensa) and
# by src/target/pcxt/CMakeLists.txt (ia16 has its own build pipeline).

include_guard(GLOBAL)

# Application programs (sources in src/user/). Single canonical list.
set(USER_APPS hello getty init trace pdb push cat ls ps df top pi pile calc uname sleep mkdir reset rmdir rm kill touch date cp mv chmod ln wc head tail printf basename dirname yes cut tr free mount umount grep sort sed ttyctl)
# Install destinations: init -> sbin, ttyctl -> usr/bin, others -> bin

# Optional per-app extra sources (for multi-file user programs).
get_filename_component(_PPAP_USER_APPS_ROOT ${CMAKE_CURRENT_LIST_DIR}/.. ABSOLUTE)

set(PPAP_USER_MAIN_SOURCE_pdb ${_PPAP_USER_APPS_ROOT}/src/user/pdb/pdb.c)
set(PPAP_USER_EXTRA_SOURCES_pdb
    ${_PPAP_USER_APPS_ROOT}/src/user/pdb/pdb_util.c
    ${_PPAP_USER_APPS_ROOT}/src/user/pdb/pdb_trace_util.c
    ${_PPAP_USER_APPS_ROOT}/src/user/pdb/pdb_cmd.c
    ${_PPAP_USER_APPS_ROOT}/src/user/pdb/pdb_regs.c
    ${_PPAP_USER_APPS_ROOT}/src/user/pdb/pdb_target.c
    ${_PPAP_USER_APPS_ROOT}/src/user/pdb/pdb_inspect.c
    ${_PPAP_USER_APPS_ROOT}/src/user/pdb/pdb_break.c
)
set(PPAP_USER_EXTRA_SOURCES_push
    ${_PPAP_USER_APPS_ROOT}/src/user/push_line.c
)
set(PPAP_USER_MAIN_SOURCE_pi ${_PPAP_USER_APPS_ROOT}/src/user/pi/pi.c)
set(PPAP_USER_EXTRA_SOURCES_pi
    ${_PPAP_USER_APPS_ROOT}/src/user/pi/pi_buf.c
    ${_PPAP_USER_APPS_ROOT}/src/user/pi/pi_term.c
    ${_PPAP_USER_APPS_ROOT}/src/user/pi/pi_ui.c
    ${_PPAP_USER_APPS_ROOT}/src/user/pi/pi_menu.c
)
set(PPAP_USER_MAIN_SOURCE_pile ${_PPAP_USER_APPS_ROOT}/src/user/pile/pile.c)
set(PPAP_USER_EXTRA_SOURCES_pile
    ${_PPAP_USER_APPS_ROOT}/src/user/pile/pile_pane.c
    ${_PPAP_USER_APPS_ROOT}/src/user/pile/pile_draw.c
    ${_PPAP_USER_APPS_ROOT}/src/user/pile/pile_ops.c
    ${_PPAP_USER_APPS_ROOT}/src/user/pile/pile_view.c
)
set(PPAP_USER_MAIN_SOURCE_calc ${_PPAP_USER_APPS_ROOT}/src/user/calc/calc.c)
set(PPAP_USER_EXTRA_SOURCES_calc
    ${_PPAP_USER_APPS_ROOT}/src/user/calc/calc_state.c
    ${_PPAP_USER_APPS_ROOT}/src/user/calc/calc_render.c
    ${_PPAP_USER_APPS_ROOT}/src/user/calc/calc_segdisp.c
)
