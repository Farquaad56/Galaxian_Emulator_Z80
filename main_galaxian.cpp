// ============================================================================
// Programme principal — Émulateur Galaxian (Raylib + Dear ImGui + rlImGui)
// ============================================================================

#include "raylib.h"
#include "rlImGui.h"
#include "imgui.h"
#include "galaxian_emulator.h"
#include "ui/debug_ui.h"

int main() {
    SetTraceLogLevel(LOG_WARNING);

    // Configuration fenêtre : écran portrait 224×768 (pixels carrés), + panneaux ImGui
    // FB_W = 224, FB_H = 768 — framebuffer déjà en format portrait.
    constexpr int SCALE = 3;
    constexpr int SCREEN_X = 20;
    constexpr int SCREEN_Y = 20;
    constexpr int PANELS_WIDTH = 900;
    // Affichage : 224×SCALE × 768 pixels (pixels carrés) + panneau à droite
    constexpr int WIN_W = SCREEN_X + 224 * SCALE + PANELS_WIDTH;
    constexpr int WIN_H = SCREEN_Y + 768 + 120;

    InitWindow(WIN_W, WIN_H, "Galaxian Emulator");
    SetTargetFPS(60);

    // Initialiser rlImGui (setup + fonts FontAwesome)
    rlImGuiSetup(true);

    // ========================================================================
    // Audio — désactivé temporairement
    // ========================================================================
    constexpr int AUDIO_SAMPLE_RATE = 44100;
    constexpr int AUDIO_FRAMES = 728;

    // InitAudioDevice();
    // AudioStream audio_stream = LoadAudioStream(AUDIO_SAMPLE_RATE, 32, 2);
    // PlayAudioStream(audio_stream);

    // ========================================================================
    // Émulateur Galaxian
    // ========================================================================
    GalaxianEmulator emu;

    // Charger les ROMs depuis assets/roms/ ou roms/ (copiées par CMake POST_BUILD)
    if (!emu.load_roms("assets/roms")) {
        fprintf(stderr, "Erreur : ROMs manquantes.\n");
        fprintf(stderr, "Assurez-vous que les ROMs sont dans ./assets/roms/\n");
        // UnloadAudioStream(audio_stream);
        // CloseAudioDevice();
        rlImGuiShutdown();
        CloseWindow();
        return 1;
    }

    emu.reset();

    // ========================================================================
    // Texture Raylib pour l'écran émulé — 224×768 (portrait natif)
    // Le framebuffer interne est déjà 224×768, plus besoin de rotation.
    // ========================================================================
    Image img = GenImageColor(224, 768, BLACK);   // texture portrait 224×768
    Texture2D screen_tex = LoadTextureFromImage(img);
    UnloadImage(img);

    // Pixel-perfect : pas de filtrage linéaire
    SetTextureFilter(screen_tex, TEXTURE_FILTER_POINT);

    // ========================================================================
    // Interface de débogage ImGui
    // ========================================================================
    DebugUI debug_ui;
    debug_ui.init(&emu);

    bool paused = false;
    float audio_buffer[AUDIO_FRAMES * 2]; // Stéréo interleaved (float 32-bit)

    // ========================================================================
    // Boucle principale
    // ========================================================================
    while (!WindowShouldClose()) {
        // --------------------------------------------------------------------
        // Entrées Raylib → émulateur (mapping clavier)
        // --------------------------------------------------------------------
        InputState& inp = emu.bus.input;

        // Joueur 1 — flèches directionnelles + espace
        inp.left    = IsKeyDown(KEY_LEFT);
        inp.right   = IsKeyDown(KEY_RIGHT);
        inp.fire    = IsKeyDown(KEY_SPACE) || IsKeyDown(KEY_UP);
        inp.start1  = IsKeyPressed(KEY_ONE);
        inp.start2  = IsKeyPressed(KEY_TWO);
        inp.coin1   = IsKeyPressed(KEY_FIVE);

        // Joueur 2 — numpad ou touches alternatives (optionnel)
        inp.left2   = IsKeyDown(KEY_N);
        inp.right2  = IsKeyDown(KEY_M);
        inp.fire2   = IsKeyDown(KEY_COMMA);
        inp.coin2   = IsKeyPressed(KEY_SIX);

        // TEST Switch — toggle au clic (comme un interrupteur physique ON/OFF)
        if (IsKeyPressed(KEY_F1)) {
            inp.test_switch = !inp.test_switch;
        }

        // SERVICE Button — momentané (F2)
        inp.service = IsKeyDown(KEY_F2);

        // Contrôles émulateur
        if (IsKeyPressed(KEY_P)) {
            paused = !paused;
            if (paused) /*PauseAudioStream(audio_stream);*/;
            else        /*ResumeAudioStream(audio_stream);*/;
        }
        if (IsKeyPressed(KEY_R)) emu.reset();

#ifdef GALAXIAN_DEBUG_TOOLS
        // F4 : bascule manuelle du starfield pour test (décommenter GALAXIAN_DEBUG_TOOLS)
        if (IsKeyPressed(KEY_F4)) {
            emu.bus.regs.star_enable = !emu.bus.regs.star_enable;
        }
#endif

        // --------------------------------------------------------------------
        // Exécution d'une frame d'émulation (~60 Hz)
        // --------------------------------------------------------------------
        if (!paused) {
            emu.run_frame();

            // Ne mettre à jour l'audio que si le buffer a été consommé
            // if (IsAudioStreamProcessed(audio_stream)) {
            //     emu.bus.render_audio(audio_buffer, AUDIO_FRAMES);
            //     UpdateAudioStream(audio_stream, audio_buffer, AUDIO_FRAMES);
            // }
        }

        // --------------------------------------------------------------------
        // Mise à jour de la texture Raylib — framebuffer portrait direct (224×768)
        // --------------------------------------------------------------------
        UpdateTexture(screen_tex, emu.get_framebuffer());

        // --------------------------------------------------------------------
        // Rendu
        // --------------------------------------------------------------------
        BeginDrawing();
        ClearBackground(BLACK);

        // Écran émulé — portrait natif : texture 224×768 → scale ×3 vertical
        Rectangle src = { 0.0f, 0.0f, (float)224, (float)768 };
        Rectangle dst = { (float)SCREEN_X, (float)SCREEN_Y,
                          (float)224 * SCALE, (float)768 };
        DrawTexturePro(screen_tex, src, dst, {0.0f, 0.0f}, 0.0f, WHITE);

        // Label PAUSE
        if (paused) {
            DrawText("PAUSE (P)", SCREEN_X + 6, SCREEN_Y + 6, 20, RED);
        }

        // Raccourcis clavier affichés en bas de l'écran émulé
        DrawText("P=Pause  R=Reset  F1=Test  F2=Service  5=Coin  1/2=Start",
                 SCREEN_X + 4, SCREEN_Y + 768 - 16, 10, GRAY);

        // --------------------------------------------------------------------
        // ImGui — panels de débogage (droite)
        // --------------------------------------------------------------------
        rlImGuiBegin();
        debug_ui.draw();
        rlImGuiEnd();

        EndDrawing();
    }

    // ========================================================================
    // Nettoyage
    // ========================================================================
    // UnloadAudioStream(audio_stream);
    // CloseAudioDevice();
    UnloadTexture(screen_tex);
    rlImGuiShutdown();
    CloseWindow();

    return 0;
}