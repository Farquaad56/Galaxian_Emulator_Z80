cmake_minimum_required(VERSION 3.20)
project(Galaxian_Emulator CXX C)

set(CMAKE_CXX_STANDARD 17)

# ============================================================================
# Raylib — compilation depuis les sources locales (third_party/raylib/src/)
# ============================================================================
set(RAYLIB_DIR "${CMAKE_SOURCE_DIR}/third_party/raylib")

# Core + modules obligatoires pour Galaxian
# ⚠️ Avec PLATFORM_DESKTOP_WIN32, on utilise Win32 natif — PAS besoin de rglfw.c
set(RAYLIB_SOURCES
    ${RAYLIB_DIR}/src/rcore.c
    ${RAYLIB_DIR}/src/rshapes.c
    ${RAYLIB_DIR}/src/rtext.c
    ${RAYLIB_DIR}/src/raudio.c
    ${RAYLIB_DIR}/src/rmodels.c
    ${RAYLIB_DIR}/src/rtextures.c
    ${RAYLIB_DIR}/src/external/stb_vorbis.c
)

add_library(raylib_static STATIC ${RAYLIB_SOURCES})

# ⚠️ DEFINITION OBLIGATOIRE — sans cela, rcore.c ne compile PAS les fonctions de fenêtre/input
# PLATFORM_DESKTOP_WIN32 utilise Win32 API native (pas besoin de GLFW externe)
target_compile_definitions(raylib_static PRIVATE
    PLATFORM_DESKTOP_WIN32
    USE_EXAMPLES_BUILD
)

# PUBLIC pour que rlimgui_lib trouve raylib.h automatiquement
target_include_directories(raylib_static PUBLIC
    ${RAYLIB_DIR}/src
    ${RAYLIB_DIR}/src/external
)

# Windows : liaison GDI32 + opengl32 + winmm (gdi.lib n'existe pas, utiliser gdi32.lib)
if(WIN32)
    target_link_libraries(raylib_static PRIVATE gdi32 opengl32 winmm imm32)
endif()

# ============================================================================
# Dear ImGui — compilation depuis third_party/imgui/
# ============================================================================
set(IMGUI_DIR "${CMAKE_SOURCE_DIR}/third_party/imgui")

set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
)

add_library(imgui_lib STATIC ${IMGUI_SOURCES})
# PUBLIC pour que rlimgui_lib et debug_ui_lib trouvent imgui.h automatiquement
target_include_directories(imgui_lib PUBLIC ${IMGUI_DIR})

# ============================================================================
# rlImGui — bridge Raylib + ImGui (raylib-extras)
# ============================================================================
set(RLIMGUI_DIR "${CMAKE_SOURCE_DIR}/third_party/rlImGui")

set(RLIMGUI_SOURCES
    ${RLIMGUI_DIR}/rlImGui.cpp
)

add_library(rlimgui_lib STATIC ${RLIMGUI_SOURCES})
target_include_directories(rlimgui_lib PRIVATE
    ${RLIMGUI_DIR}
    ${RLIMGUI_DIR}/extras
)
# rlimgui hérite déjà de raylib_static et imgui_lib via PUBLIC links
target_link_libraries(rlimgui_lib PRIVATE imgui_lib raylib_static)

# Inclusion DIRECTE — nécessaire car le héritage PUBLIC ne fonctionne pas avec MSVC dans CLion
target_include_directories(rlimgui_lib PRIVATE
    ${RAYLIB_DIR}/src
    ${IMGUI_DIR}
)

# ============================================================================
# Z80 CPU — bibliothèque existante (Farquaad56)
# ============================================================================
set(Z80_SOURCES
    src/cpu/z80.cpp
    src/cpu/z80_ops.cpp
    src/cpu/z80_cb.cpp
    src/cpu/z80_ed.cpp
    # z80_ddcb.cpp supprimé — doublon de z80_exec_xycb() dans z80_cb.cpp avec bugs (lecture registre au lieu de mémoire, flags BIT faux)
    src/cpu/z80_dd.cpp
    src/cpu/z80_fd.cpp
)

set(Z80_SYSTEM_SOURCES
    src/system/z80_bdos.cpp
)

add_library(z80_lib ${Z80_SOURCES} ${Z80_SYSTEM_SOURCES})
target_include_directories(z80_lib PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)
# ============================================================================
# Galaxian — système principal (bus + émulateur + rendu vidéo)
# ============================================================================
set(GALAXIAN_SOURCES
    src/galaxian_bus.h
    src/galaxian_emulator.h
    src/galaxian_emulator.cpp
)

add_library(galaxian_lib ${GALAXIAN_SOURCES})
target_link_libraries(galaxian_lib z80_lib)
target_include_directories(galaxian_lib PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)

# ============================================================================
# Debug UI — panels ImGui de débogage
# ============================================================================
set(DEBUG_UI_SOURCES
    src/ui/debug_ui.h
    src/ui/debug_ui.cpp
)

add_library(debug_ui_lib ${DEBUG_UI_SOURCES})
# debug_ui hérite de imgui via PUBLIC link à imgui_lib
target_link_libraries(debug_ui_lib PRIVATE galaxian_lib z80_lib rlimgui_lib raylib_static)

# Inclusion DIRECTE — nécessaire car le héritage PUBLIC ne fonctionne pas avec MSVC dans CLion
target_include_directories(debug_ui_lib PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/imgui_club
    ${CMAKE_SOURCE_DIR}/third_party/imgui_club/imgui_memory_editor
    ${IMGUI_DIR}
    ${RAYLIB_DIR}/src
)

# ============================================================================
# Executable principal — main_galaxian.cpp
# ============================================================================
add_executable(galaxian_emu main_galaxian.cpp)
# galaxian_emu hérite de raylib.h et imgui.h via PUBLIC links
# ⚠️ raylib_static DOIT être linké DIRECTEMENT car le PRIVATE linking transitif ne fonctionne pas sur plusieurs niveaux
target_link_libraries(galaxian_emu PRIVATE galaxian_lib debug_ui_lib rlimgui_lib z80_lib raylib_static)

# Inclusion DIRECTE — nécessaire car le héritage PUBLIC ne fonctionne pas avec MSVC dans CLion
target_include_directories(galaxian_emu PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/imgui_club
    ${CMAKE_SOURCE_DIR}/third_party/imgui_club/imgui_memory_editor
    ${CMAKE_SOURCE_DIR}/third_party/rlImGui
    ${CMAKE_SOURCE_DIR}/third_party/rlImGui/extras
    ${RAYLIB_DIR}/src
    ${IMGUI_DIR}
)

# MSVC runtime + Windows SDK headers — CMake/ninja ne source pas vcvars64.bat,
# donc les chemins par défaut de cl.exe (__INCLUDE_PATH__) sont vides.
# On les ajoute explicitement à toutes les cibles.
set(MSVC_INCLUDE "C:/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/MSVC/14.50.35717/include")
set(WINDOWS_KIT "C:/Program Files (x86)/Windows Kits/10/Include/10.0.26100.0")

# Ajouter les chemins à toutes les bibliothèques et l'exécutable
target_compile_options(z80_lib           PRIVATE "/I${MSVC_INCLUDE}")
target_compile_options(galaxian_lib      PRIVATE "/I${MSVC_INCLUDE}")
target_compile_options(imgui_lib         PRIVATE "/I${MSVC_INCLUDE}")
target_compile_options(raylib_static     PRIVATE "/I${MSVC_INCLUDE}")
target_compile_options(debug_ui_lib      PRIVATE "/I${MSVC_INCLUDE}")
target_compile_options(galaxian_emu      PRIVATE "/I${MSVC_INCLUDE}")

# SDK headers pour toutes les cibles C/C++
foreach(_tgt z80_lib galaxian_lib imgui_lib raylib_static debug_ui_lib galaxian_emu)
    target_compile_options(${_tgt} PRIVATE
        "/I${WINDOWS_KIT}/ucrt"
        "/I${WINDOWS_KIT}/shared"
        "/I${WINDOWS_KIT}/um"
        "/I${WINDOWS_KIT}/winrt"
    )
endforeach()

# Chemins LIB pour le linker — nécessaires car CMake/ninja ne source pas vcvars64.bat
set(MSVC_LIB "C:/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/MSVC/14.50.35717/lib/x64")
set(WINDOWS_SDK_LIB "C:/Program Files (x86)/Windows Kits/10/Lib/10.0.26100.0")
target_link_options(galaxian_emu PRIVATE "/LIBPATH:${MSVC_LIB}" "/LIBPATH:${WINDOWS_SDK_LIB}/ucrt/x64" "/LIBPATH:${WINDOWS_SDK_LIB}/um/x64")

# ============================================================================
# Copie automatique des ROMs dans le répertoire de sortie après build
# ============================================================================
add_custom_command(TARGET galaxian_emu POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/assets/roms"
        $<TARGET_FILE_DIR:galaxian_emu>/assets/roms
    COMMENT "Copying ROMs to output directory (assets/roms)")

add_custom_command(TARGET galaxian_emu POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/assets/roms"
        $<TARGET_FILE_DIR:galaxian_emu>/roms
    COMMENT "Copying ROMs to output directory (roms/)")

# ============================================================================
# Copie automatique du répertoire Debug_log dans le dossier de sortie après build
# ============================================================================
add_custom_command(TARGET galaxian_emu POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/Debug_log"
        $<TARGET_FILE_DIR:galaxian_emu>/Debug_log
    COMMENT "Copying Debug_log directory to output")
