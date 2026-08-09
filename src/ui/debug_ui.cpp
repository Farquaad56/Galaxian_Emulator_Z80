#include "ui/debug_ui.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>

// ============================================================================
// Initialisation
// ============================================================================
void DebugUI::init(GalaxianEmulator* emu) {
    this->emu = emu;
}

// ============================================================================
// draw — point d'entrée principal appelé depuis main_galaxian.cpp
// ============================================================================
void DebugUI::draw() {
    if (!emu) return;

    draw_cpu_panel(emu->cpu, emu->bus);
    draw_dipsw_panel(emu->bus);
    draw_inputs_panel(emu->bus);
    draw_debug_panel(*emu);
    draw_memory_panel(*emu, emu->bus);
    draw_palette_panel(emu->get_palette());
}

// ============================================================================
// Panel CPU Z80 — registres, flags, état interne + info bus
// Position : x=780 (après l'écran émulé à 256*3=768)
// ============================================================================
void DebugUI::draw_cpu_panel(const Z80& cpu, const GalaxianBus& bus) {
    ImGui::SetNextWindowPos({780, 0}, ImGuiCond_Once);
    ImGui::SetNextWindowSize({244, 380}, ImGuiCond_Once);
    ImGui::Begin("Z80 CPU");

    // Registres principaux
    ImGui::SeparatorText("Registres");
    ImGui::Text("A=%02X F=%02X AF=%04X", cpu.A, cpu.F, cpu.AF);
    ImGui::Text("B=%02X C=%02X BC=%04X", cpu.B, cpu.C, cpu.BC);
    ImGui::Text("D=%02X E=%02X DE=%04X", cpu.D, cpu.E, cpu.DE);
    ImGui::Text("H=%02X L=%02X HL=%04X", cpu.H, cpu.L, cpu.HL);

    ImGui::SeparatorText("Index & Pointeurs");
    ImGui::Text("IX=%04X IY=%04X", cpu.IX, cpu.IY);
    ImGui::Text("PC=%04X  SP=%04X", cpu.PC, cpu.SP);
    ImGui::Text("I=%02X   R=%02X   WZ=%04X", cpu.I, cpu.R, cpu.WZ);

    ImGui::SeparatorText("Registres alternatifs");
    ImGui::Text("AF'=%04X BC'=%04X DE'=%04X HL'=%04X",
        cpu.AF_, cpu.BC_, cpu.DE_, cpu.HL_);

    ImGui::SeparatorText("Flags (F)");
    struct FlagInfo { const char* name; uint8_t mask; };
    const FlagInfo flags[] = {
        {"S", FLAG_S}, {"Z", FLAG_Z}, {"H", FLAG_H},
        {"PV", FLAG_PV}, {"N", FLAG_N}, {"C", FLAG_C}
    };
    for (int i = 0; i < 6; i++) {
        bool set = (cpu.F & flags[i].mask) != 0;
        ImVec4 btn_col = set ? ImVec4(0.15f, 0.85f, 0.15f, 1.0f) : ImVec4(0.22f, 0.22f, 0.22f, 1.0f);
        ImVec4 txt_col = set ? ImVec4(0.6f, 1.0f, 0.6f, 1.0f) : ImVec4(0.45f, 0.45f, 0.45f, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Button, btn_col);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, set ? ImVec4(0.25f, 1.0f, 0.25f, 1.0f) : ImVec4(0.35f, 0.35f, 0.35f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, set ? ImVec4(0.1f, 0.7f, 0.1f, 1.0f) : ImVec4(0.15f, 0.15f, 0.15f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, txt_col);
        ImGui::Button(flags[i].name, {24, 24});
        ImGui::PopStyleColor(4);
        ImGui::SameLine();
        ImGui::Text("%s", set ? "1" : "0");
        if (i < 5) ImGui::SameLine();
    }

    ImGui::SeparatorText("Interruptions");
    ImVec4 on_col = {0.2f, 1.0f, 0.2f, 1.0f};
    ImVec4 off_col = {1.0f, 0.35f, 0.1f, 1.0f};
    ImGui::PushStyleColor(ImGuiCol_Text, cpu.IFF1 ? on_col : off_col);
    ImGui::Text("IFF1=%s", cpu.IFF1 ? "ON" : "OFF");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, cpu.IFF2 ? on_col : off_col);
    ImGui::Text("IFF2=%s", cpu.IFF2 ? "ON" : "OFF");
    ImGui::PopStyleColor();
    ImGui::Text("IM=%d   HALT=%s   EI_del=%s",
        cpu.IM, cpu.halted ? "OUI" : "non", cpu.ei_delay ? "OUI" : "non");

    ImGui::SeparatorText("Hardware Galaxian");
    ImGui::Text("IRQ  : %s", bus.regs.irq_enabled ? "ON" : "OFF");
    ImGui::Text("HFLIP/VFLIP: X=%s Y=%s",
        bus.regs.flip_screen_x ? "ON" : "off",
        bus.regs.flip_screen_y ? "ON" : "off");
    ImGui::Text("Star : %s", bus.regs.star_enable ? "ON" : "off");
    ImGui::Text("Sound: 0x%02X", bus.regs.sound_ctrl);

    if (!bus.regs.irq_enabled && cpu.total_cycles > 10000) {
        ImGui::PushStyleColor(ImGuiCol_Text, off_col);
        ImGui::Text("! IRQ inactive depuis boot !");
        ImGui::PopStyleColor();
    }

    ImGui::SeparatorText("Cycles");
    ImGui::Text("Total : %d", cpu.total_cycles);
    ImGui::End();
}

// ============================================================================
// Panel Debug — trace, état VBLANK
// ============================================================================
void DebugUI::draw_debug_panel(GalaxianEmulator& emu) {
    ImGui::SetNextWindowPos({1040, 0}, ImGuiCond_Once);
    ImGui::SetNextWindowSize({368, 420}, ImGuiCond_Once);
    ImGui::Begin("Debug / Trace");

    const Z80& cpu = emu.cpu;
    bool stuck = emu.is_stuck();

    if (stuck) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.2f, 0.2f, 1.0f));
        ImGui::Text("CPU BLOQUE — boucle infinie !");
        ImGui::PopStyleColor();
    }

    // --- Frame & Cycles ---
    int frame = emu.dbg_frame_count;
    ImGui::Text("Frame   : %d", frame);
    ImGui::Text("Cycles  : %d", cpu.total_cycles);

    // --- VBLANK status ---
    const VideoCounter& vc = emu.bus.video_cnt;
    ImGui::SeparatorText("Video Counter");
    ImGui::Text("V=%03d/%d   H=%03d/384", vc.v_counter, 264, vc.h_counter);
    ImVec4 vblank_col = vc.vblank_active ? ImVec4(1.0f, 0.8f, 0.1f, 1.0f) : ImVec4(0.3f, 0.3f, 0.3f, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, vblank_col);
    ImGui::Text("VBLANK: %s", vc.vblank_active ? "ACTIF" : "inactif");
    ImGui::PopStyleColor();

    // --- Force NMI VBLANK button ---
    if (ImGui::Button("Force NMI VBLANK", {160, 28})) {
        emu.bus.video_cnt.vblank_edge = true;
        emu.cpu.NMI_pending = true;
    }

    ImGui::SeparatorText("Trace");
    ImGui::Text("Entrées : %zu / 50000", emu.trace_log.size());

    if (emu.trace_log.size() > 0) {
        int start = std::max(0, (int)emu.trace_log.size() - 20);
        for (int i = start; i < (int)emu.trace_log.size(); i++) {
            const CycleTrace& t = emu.trace_log[i];
            char buf[80];
            snprintf(buf, sizeof(buf), "#%04d PC=%04X OP=%02X CYC=%d",
                i, t.pc, t.opcode, t.total_cycles);

            // Highlight duplicate PC
            bool dup = (i > start && i < (int)emu.trace_log.size() - 1 &&
                        emu.trace_log[i].pc == emu.trace_log[i-1].pc);
            if (dup) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.7f, 0.0f, 1.0f));
            }
            ImGui::TextUnformatted(buf);
            if (dup) ImGui::PopStyleColor();
        }
    }

    // --- Boutons de contrôle ---
    ImGui::Separator();
    if (ImGui::Button("Reset CPU", {160, 28})) {
        emu.reset();
        emu.trace_log.clear();
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear Trace", {160, 28})) {
        emu.trace_log.clear();
    }

    ImGui::End();
}

// ============================================================================
// Panel DIP Switches — configuration complète
// ============================================================================
void DebugUI::draw_dipsw_panel(GalaxianBus& bus) {
    // DIP panel : sous CPU, bord droit (x=780)
    ImGui::SetNextWindowPos({780, 380}, ImGuiCond_Once);
    ImGui::SetNextWindowSize({244, 260}, ImGuiCond_Once);
    ImGui::Begin("DIP Switches");

    // ─── Coinage (IN1 bits 6-7) ──────────────────────────────────────────────
    const char* coin_str[] = {"1c/1cr", "2c/1cr", "1c/2cr", "Free Play"};
    int coin_sel = bus.input.dipsw_coinage & 0x03;
    if (ImGui::Combo("Coinage", &coin_sel, coin_str, 4))
        bus.input.dipsw_coinage = (bus.input.dipsw_coinage & ~0x03) | (coin_sel & 0x03);

    // ─── Bonus Life Score (IN2 bits 0-1) ─────────────────────────────────────
    const char* bonus_str[] = {"7 000 pts", "10 000 pts", "12 000 pts", "20 000 pts"};
    int bonus_sel = bus.input.dipsw_bonus & 0x03;
    if (ImGui::Combo("Bonus Vie", &bonus_sel, bonus_str, 4))
        bus.input.dipsw_bonus = (bus.input.dipsw_bonus & ~0x03) | (bonus_sel & 0x03);

    // ─── Lives (IN2 bit 2) ──────────────────────────────────────────────────
    const char* lives_str[] = {"2 vies", "3 vies"};
    int lives_sel = bus.input.dipsw_lives ? 1 : 0;
    if (ImGui::Combo("Vies", &lives_sel, lives_str, 2))
        bus.input.dipsw_lives = lives_sel ? 1 : 0;

    // ─── Cabinet Type (IN0 bit 5) ────────────────────────────────────────────
    const char* cab_str[] = {"Upright", "Cocktail"};
    int cab_sel = bus.input.dipsw_cabinet ? 1 : 0;
    if (ImGui::Combo("Cabinet", &cab_sel, cab_str, 2))
        bus.input.dipsw_cabinet = cab_sel ? 1 : 0;

    // ─── TEST Switch (IN0 bit 6) — actif HIGH ──────────────────────────────
    ImGui::SeparatorText("TEST / SERVICE");
    ImGui::Checkbox("TEST", &bus.input.test_switch);
    const char* test_label = bus.input.test_switch ? "TEST ACTIVÉ" : "NORMAL";
    ImVec4 test_col = bus.input.test_switch
        ? ImVec4(1.0f, 0.2f, 0.2f, 1.0f)
        : ImVec4(0.2f, 0.9f, 0.25f, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, test_col);
    ImGui::Text("%s", test_label);
    ImGui::PopStyleColor();

    // ─── SERVICE (IN0 bit 7) — actif HIGH ────────────────────────────────
    ImGui::Checkbox("SERVICE", &bus.input.service);
    const char* svc_label = bus.input.service ? "SERVICE ON" : "SERVICE OFF";
    ImVec4 svc_col = bus.input.service
        ? ImVec4(1.0f, 0.7f, 0.0f, 1.0f)
        : ImVec4(0.3f, 0.3f, 0.3f, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, svc_col);
    ImGui::Text("%s", svc_label);
    ImGui::PopStyleColor();

    // ─── Configuration résumé ---
    ImGui::SeparatorText("Configuration");
    ImGui::Text("Coinage : %s", coin_str[coin_sel]);
    ImGui::Text("Bonus   : %s", bonus_str[bonus_sel]);
    ImGui::Text("Vies    : %s", lives_str[lives_sel]);
    ImGui::Text("Cab     : %s", cab_str[cab_sel]);

    // ─── Ports bruts ---
    uint8_t in0 = bus.build_in0();
    uint8_t in1 = bus.build_in1();
    uint8_t in2 = bus.build_in2();
    ImGui::SeparatorText("Ports IN");
    ImGui::Text("IN0 [0x6000] = 0x%02X", in0);
    ImGui::Text("IN1 [0x6800] = 0x%02X", in1);
    ImGui::Text("IN2 [0x7000] = 0x%02X", in2);

    // Affichage bits IN0
    ImGui::SeparatorText("Bits IN0");
    const char* in0_bits[] = {"COIN","COIN2","LEFT","RIGHT","FIRE","CAB", "TEST", "SVC"};
    for (int b = 0; b < 8; b++) {
        bool set = (in0 >> b) & 1;
        ImVec4 bc = set ? ImVec4(0.2f, 0.8f, 0.2f, 1.0f) : ImVec4(0.35f, 0.35f, 0.35f, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, bc);
        ImGui::Text("%s=%d", in0_bits[b], set ? 1 : 0);
        ImGui::PopStyleColor();
    }

    ImGui::End();
}

// ============================================================================
// Panel Entrées joueur — boutons LED
// ============================================================================
void DebugUI::draw_inputs_panel(GalaxianBus& bus) {
    // Inputs panel : bas droit (sous DIP)
    ImGui::SetNextWindowPos({780, 640}, ImGuiCond_Once);
    ImGui::SetNextWindowSize({244, 160}, ImGuiCond_Once);
    ImGui::Begin("Entrees Joueur");

    auto led = [](const char* label, bool state) {
        ImVec4 col = state ? ImVec4(0.0f, 0.85f, 0.1f, 1.0f) : ImVec4(0.25f, 0.25f, 0.25f, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Button, col);
        ImGui::Button(label, {55, 28});
        ImGui::PopStyleColor();
    };

    InputState& inp = bus.input;

    ImGui::SeparatorText("Joueur 1");
    led("LEFT",   inp.left);   ImGui::SameLine();
    led("RIGHT",  inp.right);  ImGui::SameLine();
    led("FIRE",   inp.fire);
    ImGui::SameLine();
    led("START",  inp.start1);

    ImGui::SeparatorText("Joueur 2");
    led("LEFT2",  inp.left2);  ImGui::SameLine();
    led("RIGHT2", inp.right2); ImGui::SameLine();
    led("FIRE2",  inp.fire2);
    ImGui::SameLine();
    led("START2", inp.start2);

    ImGui::SeparatorText("Monnayeur");
    led("COIN1",  inp.coin1);  ImGui::SameLine();
    led("COIN2",  inp.coin2);

    ImGui::End();
}

// ============================================================================
// request_memory_goto — mapper une adresse Galaxian vers un onglet mémoire
// ============================================================================
void DebugUI::request_memory_goto(uint16_t addr) {
    int tab = MEM_TAB_NONE;
    uint16_t display_addr = 0;

    // ROM 0x0000-0x3FFF
    if (addr < 0x4000) {
        tab = MEM_TAB_ROM;
        display_addr = addr;
    }
    // RAM 0x4000-0x4FFF avec miroir 2KB
    else if (addr < 0x5000) {
        tab = MEM_TAB_RAM;
        display_addr = static_cast<uint16_t>(0x4000 + (addr & 0x07FF));
    }
    // VRAM 0x5000-0x53FF
    else if (addr < 0x5400) {
        tab = MEM_TAB_VRAM;
        display_addr = static_cast<uint16_t>(0x5000 + (addr & 0x03FF));
    }
    // 0x5400-0x57FF non mappé
    else if (addr < 0x5800) {
        tab = MEM_TAB_NONE;
        display_addr = 0;
    }
    // OBJRAM 0x5800-0x5FFF, miroir sur 256 octets
    else if (addr < 0x6000) {
        tab = MEM_TAB_OBJ;
        display_addr = static_cast<uint16_t>(0x5800 + (addr & 0x00FF));
    }
    // Ports / registres hardware
    else {
        tab = MEM_TAB_NONE;
        display_addr = 0;
    }

    if (tab == MEM_TAB_NONE) return;

    mem_tab_to_force = tab;
    mem_goto_addr = display_addr;
    mem_goto_pending = true;
}

// ============================================================================
// draw_decoded_objram — vue décodée de l'OBJRAM (colonnes + sprites)
// ============================================================================
void DebugUI::draw_decoded_objram(const GalaxianBus& bus) {
    if (!ImGui::CollapsingHeader("OBJRAM décodé", ImGuiTreeNodeFlags_DefaultOpen)) {
        return;
    }

    // ------------------------------------------------------------------------
    // Colonnes tilemap : scroll + couleur (spram[col*2+0] et spram[col*2+1])
    // ------------------------------------------------------------------------
    ImGui::Text("Colonnes tilemap");

    if (ImGui::BeginTable("##objram_columns", 4,
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp,
        ImVec2(0, 160)))
    {
        ImGui::TableSetupColumn("Col",    ImGuiTableColumnFlags_WidthFixed, 30.0f);
        ImGui::TableSetupColumn("Addr",   ImGuiTableColumnFlags_WidthFixed, 55.0f);
        ImGui::TableSetupColumn("Scroll", ImGuiTableColumnFlags_WidthFixed, 50.0f);
        ImGui::TableSetupColumn("Color",  ImGuiTableColumnFlags_WidthFixed, 45.0f);
        ImGui::TableHeadersRow();

        for (int col = 0; col < 32; col++) {
            uint16_t addr = static_cast<uint16_t>(0x5800 + col * 2);
            uint8_t scroll = bus.spram[col * 2 + 0];
            uint8_t color  = bus.spram[col * 2 + 1] & 0x07;

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%02d", col);

            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%04X", addr);

            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%02X", scroll);

            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%d", color);
        }

        ImGui::EndTable();
    }

    // ------------------------------------------------------------------------
    // Sprites (spram[0x40 + i*4 .. 0x43 + i*4])
    // ------------------------------------------------------------------------
    ImGui::Spacing();
    ImGui::Text("Sprites");

    if (ImGui::BeginTable("##objram_sprites", 8,
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp,
        ImVec2(0, 180)))
    {
        ImGui::TableSetupColumn("Idx",    ImGuiTableColumnFlags_WidthFixed, 28.0f);
        ImGui::TableSetupColumn("Addr",   ImGuiTableColumnFlags_WidthFixed, 55.0f);
        ImGui::TableSetupColumn("Y",      ImGuiTableColumnFlags_WidthFixed, 35.0f);
        ImGui::TableSetupColumn("Code",   ImGuiTableColumnFlags_WidthFixed, 40.0f);
        ImGui::TableSetupColumn("Attr",   ImGuiTableColumnFlags_WidthFixed, 40.0f);
        ImGui::TableSetupColumn("X",      ImGuiTableColumnFlags_WidthFixed, 35.0f);
        ImGui::TableSetupColumn("Screen", ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn("Flip",   ImGuiTableColumnFlags_WidthFixed, 45.0f);
        ImGui::TableHeadersRow();

        for (int i = 0; i < 8; i++) {
            int offset = 0x40 + i * 4;

            uint8_t raw_y = bus.spram[offset + 0];
            uint8_t code  = bus.spram[offset + 1];
            uint8_t attr  = bus.spram[offset + 2];
            uint8_t raw_x = bus.spram[offset + 3];

            int screen_y = 255 - raw_y;
            int screen_x = static_cast<int>(raw_x) + 1;

            bool flip_x = (attr & 0x01) != 0;
            bool flip_y = (attr & 0x02) != 0;
            int  color  = attr & 0x07;

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%d", i);

            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%04X", 0x5800 + offset);

            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%02X", raw_y);

            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%02X", code);

            ImGui::TableSetColumnIndex(4);
            ImGui::Text("%02X:%d", attr, color);

            ImGui::TableSetColumnIndex(5);
            ImGui::Text("%02X", raw_x);

            ImGui::TableSetColumnIndex(6);
            ImGui::Text("%d,%d", screen_x, screen_y);

            ImGui::TableSetColumnIndex(7);
            ImGui::Text("%c%c", flip_x ? 'X' : '-', flip_y ? 'Y' : '-');
        }

        ImGui::EndTable();
    }
}

// ============================================================================
// draw_palette_panel — affichage des 32 couleurs PROM (6l.bpr)
// ============================================================================
void DebugUI::draw_palette_panel(const uint32_t* palette) {
    if (!palette) return;

    ImGui::SetNextWindowPos({1040, 430}, ImGuiCond_Once);
    ImGui::SetNextWindowSize({368, 200}, ImGuiCond_Once);

    if (!ImGui::Begin("Palette 6L.BPR")) {
        ImGui::End();
        return;
    }

    ImGui::Text("32 couleurs PROM (ARGB)");
    ImGui::Separator();

    for (int i = 0; i < 32; i++) {
        uint32_t c = palette[i];
        float r = ((c >> 16) & 0xFF) / 255.0f;
        float g = ((c >> 8) & 0xFF) / 255.0f;
        float b = (c & 0xFF) / 255.0f;

        ImGui::PushID(i);
        ImGui::ColorButton("##pal_color", ImVec4(r, g, b, 1.0f), 0, ImVec2(24, 24));
        ImGui::PopID();

        if ((i % 8) != 7) {
            ImGui::SameLine();
        }
    }

    ImGui::Separator();
    for (int i = 0; i < 32; i += 8) {
        char label[16];
        snprintf(label, sizeof(label), "%d-%d", i, i + 7);
        ImGui::Text("%s", label);
        for (int j = 0; j < 8; j++) {
            uint32_t c = palette[i + j];
            float r = ((c >> 16) & 0xFF) / 255.0f;
            float g = ((c >> 8) & 0xFF) / 255.0f;
            float b = (c & 0xFF) / 255.0f;
            char id[16];
            snprintf(id, sizeof(id), "##pal_%d", i + j);
            ImGui::PushID(id);
            ImGui::ColorButton("", ImVec4(r, g, b, 1.0f), 0, ImVec2(16, 12));
            ImGui::PopID();
        }
    }

    ImGui::End();
}

// ============================================================================
// draw_memory_panel — éditeur mémoire Galaxian avec navigation fonctionnelle
// ============================================================================
void DebugUI::draw_memory_panel(GalaxianEmulator& emu, GalaxianBus& bus) {
    // Memory panel : bas droite (x=1040)
    ImGui::SetNextWindowPos({1040, 230}, ImGuiCond_Once);
    ImGui::SetNextWindowSize({368, 200}, ImGuiCond_Once);
    ImGui::Begin("Mémoire Galaxian");

    // --- Options ---
    ImGui::Checkbox("Édition mémoire", &mem_allow_edit);
    ImGui::SameLine();
    ImGui::Checkbox("Suivre PC", &mem_follow_pc);

    mem_ed_rom.ReadOnly = true;
    mem_ed_ram.ReadOnly = !mem_allow_edit;
    mem_ed_vram.ReadOnly = !mem_allow_edit;
    mem_ed_sp.ReadOnly = !mem_allow_edit;

    // --- Navigation par adresse ---
    ImGui::SeparatorText("Navigation");

    bool goto_enter = ImGui::InputText(
        "##Addr",
        mem_addr_input,
        sizeof(mem_addr_input),
        ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_EnterReturnsTrue
    );

    ImGui::SameLine();
    bool goto_button = ImGui::Button("Aller");

    if (goto_enter || goto_button) {
        uint32_t addr = strtoul(mem_addr_input, nullptr, 16);
        request_memory_goto(static_cast<uint16_t>(addr));
    }

    // Boutons rapides
    if (ImGui::Button("PC")) {
        request_memory_goto(emu.cpu.PC);
    }
    ImGui::SameLine();
    if (ImGui::Button("SP")) {
        request_memory_goto(emu.cpu.SP);
    }
    ImGui::SameLine();
    if (ImGui::Button("HL")) {
        request_memory_goto(emu.cpu.HL);
    }
    ImGui::SameLine();
    if (ImGui::Button("POST 51F3")) {
        request_memory_goto(0x51F3);
    }
    ImGui::SameLine();
    if (ImGui::Button("OBJ 5840")) {
        request_memory_goto(0x5840);
    }

    // Suivre PC automatiquement
    if (mem_follow_pc) {
        static uint16_t last_follow_pc = 0xFFFF;
        if (emu.cpu.PC != last_follow_pc) {
            request_memory_goto(emu.cpu.PC);
            last_follow_pc = emu.cpu.PC;
        }
    }

    // --- Onglets mémoire ---
    if (ImGui::BeginTabBar("##GalaxianMemoryTabs")) {

        // ROM 0x0000-0x3FFF
        ImGuiTabItemFlags rom_flags =
            (mem_tab_to_force == MEM_TAB_ROM) ? ImGuiTabItemFlags_SetSelected : 0;
        if (ImGui::BeginTabItem("ROM 0x0000", nullptr, rom_flags)) {
            mem_ed_rom.DrawContents((void*)bus.rom, 0x4000, 0x0000);
            ImGui::EndTabItem();
        }

        // RAM 0x4000-0x47FF (miroir sur 0x4800-0x4FFF)
        ImGuiTabItemFlags ram_flags =
            (mem_tab_to_force == MEM_TAB_RAM) ? ImGuiTabItemFlags_SetSelected : 0;
        if (ImGui::BeginTabItem("RAM 0x4000", nullptr, ram_flags)) {
            mem_ed_ram.DrawContents((void*)bus.ram, 0x0800, 0x4000);
            ImGui::EndTabItem();
        }

        // VRAM 0x5000-0x53FF
        ImGuiTabItemFlags vram_flags =
            (mem_tab_to_force == MEM_TAB_VRAM) ? ImGuiTabItemFlags_SetSelected : 0;
        if (ImGui::BeginTabItem("VRAM 0x5000", nullptr, vram_flags)) {
            mem_ed_vram.DrawContents((void*)bus.vram, 0x0400, 0x5000);
            ImGui::EndTabItem();
        }

        // OBJRAM 0x5800-0x58FF (256 octets accessibles)
        ImGuiTabItemFlags obj_flags =
            (mem_tab_to_force == MEM_TAB_OBJ) ? ImGuiTabItemFlags_SetSelected : 0;
        if (ImGui::BeginTabItem("OBJRAM 0x5800", nullptr, obj_flags)) {
            // IMPORTANT : 256 octets seulement avec le bus actuel (addr & 0x00FF)
            mem_ed_sp.DrawContents((void*)bus.spram, 0x0100, 0x5800);
            ImGui::Separator();
            draw_decoded_objram(bus);
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    // --- Carte mémoire ---
    ImGui::Separator();
    ImGui::Text("ROM  = 0x0000-0x3FFF (16 KB)");
    ImGui::Text("RAM  = 0x4000-0x47FF (2 KB, miroir 0x4800-0x4FFF)");
    ImGui::Text("VRAM = 0x5000-0x53FF (1 KB)");
    ImGui::Text("     0x5400-0x57FF non mappé");
    ImGui::Text("OBJRAM=0x5800-0x58FF (256 B, miroir 0x5900-0x5FFF)");

    ImGui::End();
}