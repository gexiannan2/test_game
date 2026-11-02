include_guard(GLOBAL)

option(E996_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(E996_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)

add_library(e996_sanitizers INTERFACE)

function(e996_configure_sanitizers)
    if (NOT (E996_ENABLE_ASAN OR E996_ENABLE_UBSAN))
        message(STATUS ">>> Sanitizer 未启用")
        return()
    endif()

    if (NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        message(FATAL_ERROR ">>> Sanitizer 仅支持 Linux / WSL 构建")
    endif()

    if (NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        message(FATAL_ERROR ">>> 当前编译器不支持该 Sanitizer 配置")
    endif()

    set(_san_compile_flags
        -fno-omit-frame-pointer
        -fno-optimize-sibling-calls
    )

    set(_san_link_flags)

    if (E996_ENABLE_ASAN)
        list(APPEND _san_compile_flags
            -fsanitize=address
            -fsanitize-address-use-after-scope
        )
        list(APPEND _san_link_flags
            -fsanitize=address
        )
    endif()

    if (E996_ENABLE_UBSAN)
        list(APPEND _san_compile_flags -fsanitize=undefined)
        list(APPEND _san_link_flags -fsanitize=undefined)
    endif()

    target_compile_options(e996_sanitizers INTERFACE
        $<$<COMPILE_LANGUAGE:C>:${_san_compile_flags}>
        $<$<COMPILE_LANGUAGE:CXX>:${_san_compile_flags}>
    )

    target_link_options(e996_sanitizers INTERFACE
        ${_san_link_flags}
    )

    target_compile_definitions(e996_sanitizers INTERFACE
        E996_USE_SANITIZER=1
    )

    message(STATUS ">>> Sanitizer 已启用")
    message(STATUS ">>> E996_ENABLE_ASAN  = ${E996_ENABLE_ASAN}")
    message(STATUS ">>> E996_ENABLE_UBSAN = ${E996_ENABLE_UBSAN}")
    message(STATUS ">>> C 编译器         = ${CMAKE_C_COMPILER}")
    message(STATUS ">>> CXX 编译器       = ${CMAKE_CXX_COMPILER}")
    message(STATUS ">>> 构建类型         = ${CMAKE_BUILD_TYPE}")
endfunction()

#function(e996_enable_sanitizers target visibility)
#    if (TARGET ${target} AND (E996_ENABLE_ASAN OR E996_ENABLE_UBSAN))
#        target_link_libraries(${target} ${visibility} e996_sanitizers)
#    endif()
#endfunction()

function(e996_enable_sanitizers target visibility)
    if (NOT (E996_ENABLE_ASAN OR E996_ENABLE_UBSAN))
        message(STATUS ">>> [SAN] skip target=${target}, sanitizer disabled")
        return()
    endif()

    if (TARGET ${target})
        message(STATUS ">>> [SAN] attach sanitizer to target=${target}, visibility=${visibility}")
        target_link_libraries(${target} ${visibility} e996_sanitizers)

        get_target_property(_san_link_libs ${target} LINK_LIBRARIES)
        get_target_property(_san_link_opts ${target} LINK_OPTIONS)
        get_target_property(_san_compile_opts ${target} COMPILE_OPTIONS)

        message(STATUS ">>> [SAN] ${target} LINK_LIBRARIES = ${_san_link_libs}")
        message(STATUS ">>> [SAN] ${target} LINK_OPTIONS   = ${_san_link_opts}")
        message(STATUS ">>> [SAN] ${target} COMPILE_OPTIONS= ${_san_compile_opts}")
    else()
        message(WARNING ">>> [SAN] target not found: ${target}")
    endif()
endfunction()