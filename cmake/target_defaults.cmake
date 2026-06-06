# target_defaults.cmake — Common target policy helpers

include_guard(GLOBAL)

function(ppap_target_default_init_path target)
    if(PPAP_TESTS_EXTENDED)
        set(_PPAP_INIT_PATH "/bin/runtests_ext")
    elseif(PPAP_TESTS)
        set(_PPAP_INIT_PATH "/bin/runtests")
    else()
        set(_PPAP_INIT_PATH "/sbin/init")
    endif()
    target_compile_definitions(${target} PRIVATE
        PPAP_DEFAULT_INIT_PATH="${_PPAP_INIT_PATH}")
endfunction()
