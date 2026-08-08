# Guide d'Intégration du GUI — Émulateur Arcade

## Vue d'ensemble de l'architecture

Ce document explique comment l'interface graphique est implémentée dans l'émulateur Galaxian et comment intégrer ce système GUI dans un autre projet.

### Technologies utilisées

| Technologie | Rôle | Source |
|-------------|------|--------|
| **Dear ImGui** | GUI immediate mode (widgets, fenêtres, boutons, etc.) | `third_party/imgui/` |
| **imgui_club** | Extensions ImGui (imgui_memory_editor, etc.) | `third_party/imgui_club/` |
| **Raylib** | Fenêtrage, rendu OpenGL, gestion des entrées | `third_party/raylib/` |
| **rlImGui** | Bridge Raylib ↔ ImGui | `third_party/rlImGui/` |

### Schéma d'architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                     main_galaxian.cpp                          │
│  ┌──────────────┐   ┌──────────────┐   ┌──────────────────┐   │
│  │  Raylib      │   │  rlImGui     │   │  DebugUI         │   │
│  │  (fenêtre,  │──▶│  (bridge     │──▶│  (panels ImGui  │   │
│  │  input,      │   │   render)    │   │   personnalisés) │   │
│  │  rendu)      │   └──────────────┘   └──────────────────┘   │
│  └──────────────┐                                          │   │
│                 │                                          │   │
│         emu.run_frame()                                    │   │
└─────────────────┼──────────────────────────────────────────┘
                  │
          ┌───────▼────────┐
          │ GalaxianBus    │
          │ (input state,  │
          │  VRAM, CRAM,   │
          │  registers)    │
          └────────────────┘
```

---

## 1. Structure des dépendances

```
third_party/
├── imgui/                  # Dear ImGui core
│   ├── imgui.cpp
│   ├── imgui.h
│   ├── imgui_tables.cpp
│   ├── imgui_widgets.cpp
│   ├── imgui_draw.cpp
│   └── imgui_demo.cpp
│
├── imgui_club/             # Extensions ImGui
│   └── imgui_memory_editor/
│       └── imgui_memory_editor.h
│
├── raylib/                 # Raylib source
│   └── src/
│       ├── rcore.c
│       ├── rshapes.c
│       ├── rtext.c
│       ├── raudio.c
│       ├── rmodels.c
│       └── rtextures.c
│
└── rlImGui/                # Bridge Raylib ↔ ImGui
    ├── rlImGui.cpp
    ├── rlImGui.h
    └── extras/
```

---

## 2. Configuration CMake

### 2.1 Compilation de chaque bibliothèque

```cmake
# ---------------------------------------------------------------------------
# Dear ImGui — bibliothèque statique
# ---------------------------------------------------------------------------
set(IMGUI_DIR "${CMAKE_SOURCE_DIR}/third_party/imgui")
set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
)
add_library(imgui_lib STATIC ${IMGUI_SOURCES})
target_include_directories(imgui_lib PUBLIC ${IMGUI_DIR})

# ---------------------------------------------------------------------------
# Raylib — bibliothèque statique
# ---------------------------------------------------------------------------
set(RAYLIB_DIR "${CMAKE_SOURCE_DIR}/third_party/raylib")
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
target_compile_definitions(raylib_static PRIVATE PLATFORM_DESKTOP_WIN32)
target_include_directories(raylib_static PUBLIC
    ${RAYLIB_DIR}/src
    ${RAYLIB_DIR}/src/external
)
if(WIN32)
    target_link_libraries(raylib_static PRIVATE gdi32 opengl32 winmm imm32)
endif()

# ---------------------------------------------------------------------------
# rlImGui — bridge Raylib ↔ ImGui
# ---------------------------------------------------------------------------
set(RLIMGUI_DIR "${CMAKE_SOURCE_DIR}/third_party/rlImGui")
set(RLIMGUI_SOURCES ${RLIMGUI_DIR}/rlImGui.cpp)
add_library(rlimgui_lib STATIC ${RLIMGUI_SOURCES})
target_include_directories(rlimgui_lib PRIVATE
    ${RLIMGUI_DIR}
    ${RLIMGUI_DIR}/extras
    ${RAYLIB_DIR}/src
    ${IMGUI_DIR}
)
target_link_libraries(rlimgui_lib PRIVATE imgui_lib raylib_static)

# ---------------------------------------------------------------------------
# imgui_club — extensions (imgui_memory_editor)
# ---------------------------------------------------------------------------
# Pas de bibliothèque séparée : include direct dans le code source.
# Le chemin d'inclusion est ajouté au target qui en a besoin.
```

### 2.2 Linking final

```cmake
# Bibliothèque UI personnalisée
add_library(debug_ui_lib ${DEBUG_UI_SOURCES})
target_link_libraries(debug_ui_lib PRIVATE
    galaxian_lib z80_lib rlimgui_lib raylib_static
)
target_include_directories(debug_ui_lib PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/imgui_club
    ${CMAKE_SOURCE_DIR}/third_party/imgui_club/imgui_memory_editor
    ${IMGUI_DIR}
    ${RAYLIB_DIR}/src
)

# Executable principal
add_executable(galaxian_emu main_galaxian.cpp)
target_link_libraries(galaxian_emu PRIVATE
    galaxian_lib debug_ui_lib rlimgui_lib z80_lib raylib_static
)
target_include_directories(galaxian_emu PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/imgui_club
    ${CMAKE_SOURCE_DIR}/third_party/imgui_club/imgui_memory_editor
    ${CMAKE_SOURCE_DIR}/third_party/rlImGui
    ${CMAKE_SOURCE_DIR}/third_party/rlImGui/extras
    ${IMGUI_DIR}
    ${RAYLIB_DIR}/src
)
```

> **Note importante** : Avec MSVC/CLion, le `PUBLIC` linking transitif ne fonctionne pas toujours sur plusieurs niveaux. Il faut ajouter les includes directement au target final.

---

## 3. Cycle de vie rlImGui

### 3.1 Initialisation (appelé une fois au démarrage)

```cpp
#include "rlImGui.h"

rlImGuiSetup(true);  // true = charger polices FontAwesome
```

### 3.2 Boucle principale (appelé chaque frame)

```cpp
while (!WindowShouldClose()) {
    // 1. Traitement des entrées

    // 2. Début du rendering ImGui
    rlImGuiBegin();

    // 3. Appel des panels ImGui personnalisés
    debug_ui.draw();

    // 4. Fin du rendering ImGui
    rlImGuiEnd();

    // 5. Fin de la frame Raylib
}
```

### 3.3 Shutdown (appelé une fois à la fermeture)

```cpp
rlImGuiShutdown();
```

### 3.4 Fonctions rlImGui

| Fonction | Rôle |
|----------|------|
| `rlImGuiSetup(bool fontAwesome)` | Initialise le contexte ImGui, lie les callbacks Raylib, charge FontAwesome |
| `rlImGuiBegin()` | Prépare le rendu ImGui (set viewport, scissor, shader) |
| `rlImGuiDraw()` | Dessine les draw calls ImGui (optionnel, appelé automatiquement) |
| `rlImGuiEnd()` | Present le rendu (swap buffers, reset state) |
| `rlImGuiShutdown()` | Libère les ressources ImGui |

---

## 4. Structure d'un panel ImGui personnalisé

### 4.1 En-tête (`debug_ui.h`)

```cpp
#pragma once
#include "imgui.h"
#include "imgui_memory_editor.h"   // Extension imgui_club
#include "galaxian_emulator.h"

class DebugUI {
public:
    void init(GalaxianEmulator* emu);   // Initialisation
    void draw();                         // Point d'entrée appelé chaque frame

private:
    GalaxianEmulator* emu = nullptr;

    // Panels individuels
    void draw_cpu_panel(const Z80& cpu, const GalaxianBus& bus);
    void draw_debug_panel(GalaxianEmulator& emu);
    void draw_tilemap_panel(const GalaxianBus& bus, const uint32_t* palette);
    void draw_palette_panel(const uint32_t* palette);
    void draw_dipsw_panel(GalaxianBus& bus);
    void draw_inputs_panel(GalaxianBus& bus);
    void draw_memory_editors(GalaxianBus& bus);

private:
    // Instances persistantes d'imgui_memory_editor
    MemoryEditor mem_ed_rom;
    MemoryEditor mem_ed_ram;
    MemoryEditor mem_ed_vram;
    MemoryEditor mem_ed_cram;
    MemoryEditor mem_ed_sp;
};
```

### 4.2 Implémentation d'un panel (`debug_ui.cpp`)

```cpp
void DebugUI::draw_cpu_panel(const Z80& cpu, const GalaxianBus& bus) {
    // 1. Position et taille de la fenêtre (une seule fois)
    ImGui::SetNextWindowPos({1248, 0}, ImGuiCond_Once);
    ImGui::SetNextWindowSize({340, 310}, ImGuiCond_Once);

    // 2. Ouverture du panel
    ImGui::Begin("Z80 CPU");

    // 3. Contenu — widgets ImGui classiques
    ImGui::SeparatorText("Registres principaux");
    ImGui::Text("A  = %02X    F  = %02X    AF = %04X",
        cpu.A, cpu.F, cpu.AF);
    ImGui::Text("B  = %02X    C  = %02X    BC = %04X",
        cpu.B, cpu.C, cpu.BC);
    ImGui::Text("PC = %04X     SP = %04X", cpu.PC, cpu.SP);

    // 4. Flags avec indicateur visuel
    ImGui::SeparatorText("Flags (registre F)");
    ImGui::Text("S=%d  Z=%d  H=%d  PV=%d  N=%d  C=%d",
        (cpu.F & FLAG_S) ? 1 : 0,
        (cpu.F & FLAG_Z) ? 1 : 0,
        (cpu.F & FLAG_H) ? 1 : 0,
        (cpu.F & FLAG_PV)? 1 : 0,
        (cpu.F & FLAG_N) ? 1 : 0,
        (cpu.F & FLAG_C) ? 1 : 0);

    // 5. Couleur conditionnelle
    ImVec4 col_on  = ImVec4(0.0f, 1.0f, 0.0f, 1.0f);  // vert
    ImVec4 col_off = ImVec4(1.0f, 0.3f, 0.0f, 1.0f);  // orange
    ImGui::PushStyleColor(ImGuiCol_Text, cpu.IFF1 ? col_on : col_off);
    ImGui::Text("IFF1 = %s", cpu.IFF1 ? "ON" : "OFF");
    ImGui::PopStyleColor();

    // 6. Fermeture du panel (NE PAS oublier !)
    ImGui::End();
}
```

### 4.3 Panel avec imgui_memory_editor

```cpp
void DebugUI::draw_memory_editors(GalaxianBus& bus) {
    ImGui::SetNextWindowPos({1248, 740}, ImGuiCond_Once);
    ImGui::SetNextWindowSize({830, 420}, ImGuiCond_Once);
    ImGui::Begin("Mémoire");

    // Configurer l'éditeur ROM (lecture seule)
    mem_ed_rom.ReadOnly = true;

    // Onglets avec ImGuiTabBar
    if (ImGui::BeginTabBar("MemTabs", ImGuiTabBarFlags_None)) {
        if (ImGui::BeginTabItem("ROM")) {
            mem_ed_rom.DrawContents((void*)bus.rom, 0x4000, 0x0000);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("RAM")) {
            mem_ed_ram.DrawContents((void*)bus.ram, 0x0800, 0x0000);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("VRAM")) {
            mem_ed_vram.DrawContents((void*)bus.vram, 0x0400, 0x0000);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("CRAM")) {
            mem_ed_cram.DrawContents((void*)bus.cram, 0x0400, 0x0000);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("SPRAM")) {
            mem_ed_sp.DrawContents((void*)bus.spram, 0x0040, 0x0000);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::End();
}
```

### 4.4 Panel avec rendu personnalisé (tilemap)

```cpp
void DebugUI::draw_tilemap_panel(const GalaxianBus& bus, const uint32_t* palette) {
    ImGui::SetNextWindowPos({768, 0}, ImGuiCond_Once);
    ImGui::SetNextWindowSize({480, 360}, ImGuiCond_Once);
    ImGui::Begin("Tilemap 32x28");

    float TS = 8.0f;  // taille d'une tuile
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p     = ImGui::GetCursorScreenPos();

    for (int col = 0; col < 32; col++) {
        for (int row = 0; row < 28; row++) {
            int addr = col * 32 + row;
            uint8_t tile = bus.vram[addr];
            uint8_t cgr  = bus.cram[addr] & 0x07;

            ImVec2 tl = { p.x + col * TS, p.y + row * TS };
            ImVec2 br = { tl.x + TS - 1.0f, tl.y + TS - 1.0f };

            uint32_t col_val = palette[cgr * 4 + 1];
            // Convertir ARGB → ABGR pour ImGui
            uint32_t imgui_col = ((col_val & 0xFF000000)) |
                                 ((col_val & 0x00FF0000) >> 16) |
                                 ((col_val & 0x0000FF00)) |
                                 ((col_val & 0x000000FF) << 16);

            dl->AddRectFilled(tl, br, imgui_col);
            dl->AddRect(tl, br, IM_COL32(150,150,150,200), 0.0f, ImDrawFlags_None, 1.0f);

            // Tooltip au survol
            if (ImGui::IsMouseHoveringRect(tl, br)) {
                ImGui::SetTooltip("Tuile [%d,%d] Code: 0x%02X Couleur: %d",
                    col, row, tile, cgr);
            }
        }
    }

    ImGui::End();
}
```

---

## 5. Intégration dans un nouveau projet

### Étape 1 : Copier les dépendances

```
votre_projet/
├── third_party/
│   ├── imgui/              # ← Copier depuis third_party/imgui/
│   ├── imgui_club/         # ← Copier depuis third_party/imgui_club/
│   ├── raylib/             # ← Copier depuis third_party/raylib/
│   └── rlImGui/            # ← Copier depuis third_party/rlImGui/
├── src/
│   └── ui/
│       ├── debug_ui.h      # ← Copier depuis src/ui/debug_ui.h
│       └── debug_ui.cpp    # ← Copier depuis src/ui/debug_ui.cpp
├── CMakeLists.txt
└── main.cpp
```

### Étape 2 : Configuration CMake

```cmake
cmake_minimum_required(VERSION 3.20)
project(MyEmulator CXX C)
set(CMAKE_CXX_STANDARD 17)

# ---------------------------------------------------------------------------
# Dear ImGui
# ---------------------------------------------------------------------------
set(IMGUI_DIR "${CMAKE_SOURCE_DIR}/third_party/imgui")
add_library(imgui_lib STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
)
target_include_directories(imgui_lib PUBLIC ${IMGUI_DIR})

# ---------------------------------------------------------------------------
# Raylib
# ---------------------------------------------------------------------------
set(RAYLIB_DIR "${CMAKE_SOURCE_DIR}/third_party/raylib")
add_library(raylib_static STATIC
    ${RAYLIB_DIR}/src/rcore.c
    ${RAYLIB_DIR}/src/rshapes.c
    ${RAYLIB_DIR}/src/rtext.c
    ${RAYLIB_DIR}/src/raudio.c
    ${RAYLIB_DIR}/src/rmodels.c
    ${RAYLIB_DIR}/src/rtextures.c
    ${RAYLIB_DIR}/src/external/stb_vorbis.c
)
target_compile_definitions(raylib_static PRIVATE PLATFORM_DESKTOP_WIN32)
target_include_directories(raylib_static PUBLIC
    ${RAYLIB_DIR}/src ${RAYLIB_DIR}/src/external
)
if(WIN32)
    target_link_libraries(raylib_static PRIVATE gdi32 opengl32 winmm imm32)
endif()

# ---------------------------------------------------------------------------
# rlImGui
# ---------------------------------------------------------------------------
set(RLIMGUI_DIR "${CMAKE_SOURCE_DIR}/third_party/rlImGui")
add_library(rlimgui_lib STATIC ${RLIMGUI_DIR}/rlImGui.cpp)
target_include_directories(rlimgui_lib PRIVATE
    ${RLIMGUI_DIR} ${RLIMGUI_DIR}/extras
    ${RAYLIB_DIR}/src ${IMGUI_DIR}
)
target_link_libraries(rlimgui_lib PRIVATE imgui_lib raylib_static)

# ---------------------------------------------------------------------------
# Bibliothèque UI personnalisée (copiée de src/ui/)
# ---------------------------------------------------------------------------
add_library(custom_ui_lib
    src/ui/debug_ui.h
    src/ui/debug_ui.cpp
)
target_include_directories(custom_ui_lib PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/imgui_club
    ${CMAKE_SOURCE_DIR}/third_party/imgui_club/imgui_memory_editor
    ${IMGUI_DIR}
    ${RAYLIB_DIR}/src
)
target_link_libraries(custom_ui_lib PRIVATE rlimgui_lib raylib_static)

# ---------------------------------------------------------------------------
# Executable principal
# ---------------------------------------------------------------------------
add_executable(my_emu main.cpp)
target_link_libraries(my_emu PRIVATE custom_ui_lib rlimgui_lib raylib_static)
target_include_directories(my_emu PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/imgui_club
    ${CMAKE_SOURCE_DIR}/third_party/imgui_club/imgui_memory_editor
    ${CMAKE_SOURCE_DIR}/third_party/rlImGui
    ${CMAKE_SOURCE_DIR}/third_party/rlImGui/extras
    ${IMGUI_DIR}
    ${RAYLIB_DIR}/src
)
```

### Étape 3 : Configuration du main.cpp

```cpp
#include "raylib.h"
#include "rlImGui.h"
#include "imgui.h"
#include "ui/debug_ui.h"   // Votre bibliothèque UI personnalisée

int main() {
    // 1. Créer la fenêtre
    InitWindow(1920, 1080, "Mon Émulateur");
    SetTargetFPS(60);

    // 2. Initialiser rlImGui
    rlImGuiSetup(true);

    // 3. Initialiser l'émulateur et le debug UI
    MyEmulator emu;
    DebugUI debug_ui;
    debug_ui.init(&emu);

    // 4. Boucle principale
    while (!WindowShouldClose()) {
        // Entrées clavier/souris → émulateur
        // ...

        // Exécution de l'émulateur
        emu.run_frame();

        // Rendu
        BeginDrawing();
        ClearBackground(BLACK);

        // Écran émulé
        DrawTextureEx(screen_tex, {0, 0}, 0.0f, 3.0f, WHITE);

        // Panels ImGui
        rlImGuiBegin();
        debug_ui.draw();       // ← Appel des panels personnalisés
        rlImGuiEnd();

        EndDrawing();
    }

    // 5. Nettoyage
    rlImGuiShutdown();
    CloseWindow();
    return 0;
}
```

### Étape 4 : Créer des panels personnalisés

Copier le pattern de `debug_ui.h` / `debug_ui.cpp` :

```cpp
// my_panel.h
#pragma once
#include "imgui.h"
#include "imgui_memory_editor.h"

class MyPanel {
public:
    void init(MyEmulator* emu);
    void draw();
private:
    MyEmulator* emu = nullptr;
    void draw_state_panel();
    void draw_registers_panel();
    MemoryEditor mem_ed;
};
```

```cpp
// my_panel.cpp
#include "my_panel.h"

void MyPanel::init(MyEmulator* emu) {
    this->emu = emu;
}

void MyPanel::draw() {
    if (!emu) return;
    draw_state_panel();
    draw_registers_panel();
}

void MyPanel::draw_state_panel() {
    ImGui::SetNextWindowPos({800, 0}, ImGuiCond_Once);
    ImGui::SetNextWindowSize({300, 250}, ImGuiCond_Once);
    ImGui::Begin("État");

    ImGui::Text("Frame: %d", emu->frame_count);
    ImGui::Text("FPS: %.1f", GetFPS());

    if (ImGui::Button("Reset")) {
        emu->reset();
    }

    ImGui::End();
}
```

---

## 6. Référence rapide des widgets ImGui utilisés

| Widget | Usage | Description |
|--------|-------|-------------|
| `ImGui::Begin/End` | Fenêtre panel | Crée un panneau flottant |
| `ImGui::Text()` | Texte brut | Affiche du texte formaté |
| `ImGui::Button()` | Bouton ponctuel | Action momentané (poussoir) |
| `ImGui::Checkbox()` | Toggle maintenu | État ON/OFF persistant |
| `ImGui::Combo()` | Liste déroulante | Sélection parmi options |
| `ImGui::SeparatorText()` | Titre de section | Séparateur avec label |
| `ImGui::ColorButton()` | Échantillon couleur | Bouton couleur carré |
| `ImGui::Dummy()` | Espace vide | Réservedu espace dans la fenêtre |
| `ImGui::SameLine()` | Alignement horizontal | Place le widget suivant sur la même ligne |
| `ImGui::SetTooltip()` | Info-bulle | Tooltip au survol |
| `ImGui::PushStyleColor/PopStyleColor` | Style conditionnel | Modifie la couleur temporairement |
| `ImGui::BeginTabBar/EndTabBar` | Onglets | Container d'onglets |
| `ImGui::BeginTabItem/EndTabItem` | Onglet individuel | Onglet dans un TabBar |
| `ImGui::GetWindowDrawList()` | Rendu personnalisé | Accès au draw list pour shapes |
| `ImDrawList::AddRectFilled()` | Rectangle rempli | Dessine un rectangle coloré |
| `ImDrawList::AddRect()` | Rectangle contour | Dessine un contour |

---

## 7. Points d'attention

### ⚠️ Immediate Mode — Penser en frames

ImGui est un GUI **immediate mode**. Chaque frame, tout doit être redraw complet. Ne pas stocker d'état GUI dans les widgets — l'état est dans les données C++ :

```cpp
// ✅ CORRECT : lecture de l'état actuel
ImGui::Text("PC = %04X", cpu.PC);

// ❌ WRONG : tentative de stockage d'état ImGui
static int saved_pc;  // Ne PAS faire ça pour des données émulateur
```

### ⚠️ ImGui::End() obligatoire

Chaque `ImGui::Begin()` doit avoir un `ImGui::End()` correspondant. Oublier `End()` provoque des crashes.

### ⚠️ Position/size des fenêtres

Utiliser `SetNextWindowPos/Size` avec `ImGuiCond_Once` pour figer la position la première frame :

```cpp
ImGui::SetNextWindowPos({768, 0}, ImGuiCond_Once);  // Colonne droite
ImGui::SetNextWindowSize({480, 360}, ImGuiCond_Once);
ImGui::Begin("Tilemap");
// ...
ImGui::End();
```

### ⚠️ Format de couleur

ImGui utilise `IM_COL32(r, g, b, a)` ou `ImVec4(r, g, b, a)` avec des valeurs **0.0–1.0**.

Pour convertir une valeur ARGB 32-bit :

```cpp
uint32_t argb = palette[index];
float r = ((argb >> 16) & 0xFF) / 255.0f;
float g = ((argb >>  8) & 0xFF) / 255.0f;
float b = ((argb >>  0) & 0xFF) / 255.0f;
ImVec4 imcol = {r, g, b, 1.0f};
ImGui::ColorButton("##color", imcol);
```

### ⚠️ MSVC et le linking PUBLIC transitif

Avec MSVC dans CLion, le `PUBLIC` linking ne fonctionne pas sur plusieurs niveaux de bibliothèques. Il faut **ajouter manuellement** les includes à chaque target final :

```cmake
# Ne PAS compter sur le transitif — ajouter directement :
target_include_directories(galaxian_emu PRIVATE
    ${IMGUI_DIR}
    ${RAYLIB_DIR}/src
    ${RLIMGUI_DIR}
    ${CMAKE_SOURCE_DIR}/third_party/imgui_club
)
```

---

## 8. Raccourcis clavier de l'émulateur

| Touche | Action |
|--------|--------|
| `P` | Pause / Reprendre |
| `R` | Reset émulateur |
| `F1` | Toggle TEST switch |
| `F2` | Service button (momentané) |
| `Space/Up` | Fire (Joueur 1) |
| `Left/Right` | Direction (Joueur 1) |
| `1/2` | Start Joueur 1/2 |
| `5` | Coin |

---

## 9. Structure des fenêtres du panel

```
┌──────────────────────────────────────────────────────────────────┐
│  [Écran émulé 256×224 ×3]         │ Z80 CPU          │
│                                   │ [Debug / Trace]    │
│                                   │ [Entrees Joueur]   │
│                                   │ [DIP Switches]     │
│                                   │ [Mémoire]           │
├───────────────────────────────────┤                      │
│  [Tilemap 32×28]                  │  [Palette]          │
│                                   │                      │
└───────────────────────────────────┴──────────────────────┘
  ← 768px →                      ← 480px →  ← 830px →
  (écran)                         (tilemap)   (panels)