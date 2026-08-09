// ============================================================================
// Programme principal — Émulateur Galaxian (Raylib + Dear ImGui + rlImGui)
// ============================================================================

#include "raylib.h"
#include "rlImGui.h"
#include "imgui.h"
#include "galaxian_emulator.h"
#include "ui/debug_ui.h"

// Rotation 90° horaire (MAME ROT90) : brut(768×224) → portrait(224×768)
// Transpose l'axe H brut en axe vertical écran, l'axe V brut en axe horizontal écran.
static std::vector<uint32_t> rot_buf;

void blit_rotated(const uint32_t* src, uint32_t* dst) {
    // src = framebuffer brut [ry * FB_W + rx * 3 + s]
    // dst = portrait [yp * PORT_W + ry] où yp = rx*3+s (axe H → vertical)
    for (int ry = 0; ry < GalaxianEmulator::FB_H; ry++)
        for (int rx = 0; rx < 256; rx++)
            for (int s = 0; s < 3; s++) {
                int yp = rx * 3 + s;                      // axe H brut → vertical écran
                dst[yp * GalaxianEmulator::PORT_W + ry] = src[ry * GalaxianEmulator::FB_W + rx * 3 + s];
            }
}

int main() {
    SetTraceLogLevel(LOG_WARNING);

    // Configuration fenêtre : écran portrait 224×768 pivoté, + panneaux ImGui
    constexpr int SCALE = 3;
    constexpr int FB_W = GalaxianEmulator::FB_W;   // 768 sous-pixels (axe H brut)
    constexpr int FB_H = GalaxianEmulator::FB_H;   // 224 lignes (axe V brut, zone visible)
    constexpr int PORT_W = 224;                    // largeur écran portrait (= axe V brut)
    constexpr int PORT_H = 768;                    // hauteur écran portrait (= 256×3)
    constexpr int SCREEN_X = 20;
    constexpr int SCREEN_Y = 20;
    constexpr int PANELS_WIDTH = 900;
    constexpr int WIN_W = SCREEN_X + PORT_W * SCALE + PANELS_WIDTH;
    constexpr int WIN_H = SCREEN_Y + PORT_H + 120;

    InitWindow(WIN_W, WIN_H, "Galaxian Emulator");
    SetTargetFPS(60);

    // Initialiser rlImGui (setup + fonts FontAwesome)
    rlImGuiSetup(true);

    // ========================================================================
    // Audio — Stream float stéréo 44100 Hz
    // ========================================================================
    constexpr int AUDIO_SAMPLE_RATE = 44100;
    constexpr int AUDIO_FRAMES = 728;

    InitAudioDevice();
    AudioStream audio_stream = LoadAudioStream(AUDIO_SAMPLE_RATE, 32, 2);
    PlayAudioStream(audio_stream);

    // ========================================================================
    // Émulateur Galaxian
    // ========================================================================
    GalaxianEmulator emu;

    if (!emu.load_roms("assets/roms")) {
        fprintf(stderr, "Erreur : ROMs manquantes.\n");
        fprintf(stderr, "Assurez-vous que les ROMs sont dans ./assets/roms/\n");
        UnloadAudioStream(audio_stream);
        CloseAudioDevice();
        rlImGuiShutdown();
        CloseWindow();
        return 1;
    }

    emu.reset();

    // ========================================================================
    // Texture Raylib pour l'écran émulé — portrait 256×768 (après rotation ROT90)
    // Le framebuffer interne est 768×256 ; on le pivot à l'affichage.
    // ========================================================================
    rot_buf.resize(PORT_W * PORT_H);   // portrait 224 × 768

    Image img = GenImageColor(PORT_W, PORT_H, BLACK);
    Texture2D screen_tex = LoadTextureFromImage(img);
    UnloadImage(img);

    SetTextureFilter(screen_tex, TEXTURE_FILTER_POINT);

    // ========================================================================
    // Interface de débogage ImGui
    // ========================================================================
    DebugUI debug_ui;
    debug_ui.init(&emu);

    bool paused = false;
    float audio_buffer[AUDIO_FRAMES * 2];

    // ========================================================================
    // Boucle principale
    // ========================================================================
    while (!WindowShouldClose()) {
        InputState& inp = emu.bus.input;

        inp.left    = IsKeyDown(KEY_LEFT);
        inp.right   = IsKeyDown(KEY_RIGHT);
        inp.fire    = IsKeyDown(KEY_SPACE) || IsKeyDown(KEY_UP);
        inp.start1  = IsKeyPressed(KEY_ONE);
        inp.start2  = IsKeyPressed(KEY_TWO);
        inp.coin1   = IsKeyPressed(KEY_FIVE);

        inp.left2   = IsKeyDown(KEY_N);
        inp.right2  = IsKeyDown(KEY_M);
        inp.fire2   = IsKeyDown(KEY_COMMA);
        inp.coin2   = IsKeyPressed(KEY_SIX);

        if (IsKeyPressed(KEY_F1)) {
            inp.test_switch = !inp.test_switch;
        }
        inp.service = IsKeyDown(KEY_F2);

        if (IsKeyPressed(KEY_P)) {
            paused = !paused;
            if (paused) PauseAudioStream(audio_stream);
            else        ResumeAudioStream(audio_stream);
        }
        if (IsKeyPressed(KEY_R)) emu.reset();

#ifdef GALAXIAN_DEBUG_TOOLS
        if (IsKeyPressed(KEY_F4)) {
            emu.bus.regs.star_enable = !emu.bus.regs.star_enable;
        }
#endif

        if (!paused) {
            emu.run_frame();

            if (IsAudioStreamProcessed(audio_stream)) {
                emu.bus.render_audio(audio_buffer, AUDIO_FRAMES);
                UpdateAudioStream(audio_stream, audio_buffer, AUDIO_FRAMES);
            }
        }

        // Rotation ROT90 : brut(768×224) → portrait(224×768)
        blit_rotated(emu.get_framebuffer(), rot_buf.data());
        UpdateTexture(screen_tex, rot_buf.data());

        BeginDrawing();
        ClearBackground(BLACK);

        // Écran émulé — rotation 90° : texture 256×768 → stretch ×3 vertical
        Rectangle src = { 0.0f, 0.0f, (float)PORT_W, (float)PORT_H };
        Rectangle dst = { (float)SCREEN_X, (float)SCREEN_Y,
                          (float)PORT_W * SCALE, (float)PORT_H };
        DrawTexturePro(screen_tex, src, dst, {0.0f, 0.0f}, 0.0f, WHITE);

        if (paused) {
            DrawText("PAUSE (P)", SCREEN_X + 6, SCREEN_Y + 6, 20, RED);
        }

        DrawText("P=Pause  R=Reset  F1=Test  F2=Service  5=Coin  1/2=Start",
                 SCREEN_X + 4, SCREEN_Y + PORT_H - 16, 10, GRAY);

        rlImGuiBegin();
        debug_ui.draw();
        rlImGuiEnd();

        EndDrawing();
    }

    UnloadAudioStream(audio_stream);
    CloseAudioDevice();
    UnloadTexture(screen_tex);
    rlImGuiShutdown();
    CloseWindow();

    return 0;
}