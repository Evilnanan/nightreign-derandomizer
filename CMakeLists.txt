cmake_minimum_required(VERSION 3.21)

project(nightreign_derandomizer LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "nightreign_derandomizer only supports Windows")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "nightreign_derandomizer must be built for x64")
endif()

function(configure_project_target target)
    target_compile_features(${target} PRIVATE cxx_std_17)
    target_compile_definitions(${target} PRIVATE
        UNICODE
        _UNICODE
    )

    if(MSVC)
        target_compile_options(${target} PRIVATE
            /W4
            /permissive-
            /utf-8
            /Zc:__cplusplus
        )
    endif()

    # Keep the two distributable binaries together while leaving import
    # libraries and other build-only files outside the distribution folder.
    set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/dist/$<CONFIG>"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib/$<CONFIG>"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib/$<CONFIG>"
    )
endfunction()

add_library(derandomizer SHARED
    src/mod/boss_hooks.cpp
    src/mod/boss_hooks.h
    src/mod/config.cpp
    src/mod/config.h
    src/mod/derandomizer.cpp
    src/mod/game_module.cpp
    src/mod/game_module.h
    src/mod/logging.cpp
    src/mod/logging.h
    src/mod/memory_patch.cpp
    src/mod/memory_patch.h
    src/mod/range_diagnostics.cpp
    src/mod/range_diagnostics.h
    src/mod/seed_hook.cpp
    src/mod/seed_hook.h
)
configure_project_target(derandomizer)
target_link_libraries(derandomizer PRIVATE version)
set_target_properties(derandomizer PROPERTIES
    OUTPUT_NAME "derandomizer"
)

add_executable(seed_reroller
    src/app/cli.cpp
    src/app/cli.h
    src/app/main.cpp
    src/app/config.cpp
    src/app/config.h
    src/app/model.cpp
    src/app/model.h
)
configure_project_target(seed_reroller)
target_link_libraries(seed_reroller PRIVATE
    comdlg32
    dwmapi
    gdi32
    user32
    uxtheme
)
set_target_properties(seed_reroller PROPERTIES
    OUTPUT_NAME "Seed Reroller"
)

set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT seed_reroller)
