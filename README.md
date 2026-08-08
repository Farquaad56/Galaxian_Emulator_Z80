# Galaxian_Emulator_Z80

Émulateur arcade Galaxian (Konami, 1979) écrit en C++ avec Z80 CPU cycle-accurate.

## 📋 Prérequis

- CMake 3.20+
- Compilateur C++17 (MSVC, GCC ou Clang)

## 🔧 Compilation

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release
```

Ou via CLion : ouvrez le projet et lancez la configuration CMake.

## 🎮 Installation des ROMs

Les ROMs Galaxian sont la propriété de Konami et ne peuvent pas être distribuées avec ce projet.

Pour obtenir les ROMs nécessaires :
1. Téléchargez le set MAME `galaxian.zip` depuis [MAME romsets](https://www.mamedev.org/release/)
2. Extrayez les fichiers suivants dans le dossier `assets/roms/` :
   - `1h.bin`
   - `1k.bin`
   - `6l.bpr`
   - `7l`
   - `galmidw.u`
   - `galmidw.v`
   - `galmidw.w`
   - `galmidw.y`

## 📁 Structure du projet

```
├── src/
│   ├── cpu/          # Implémentation Z80 (Farquaad56)
│   ├── system/       # Bus, mémoire, BDOS
│   └── ui/           # Interface de débogage ImGui
├── include/
│   ├── cpu/          # Headers Z80
│   └── system/       # Headers système
├── mame/
│   └── galaxian/     # Code MAME de référence (galaxian_v.cpp)
├── assets/
│   └── roms/         # Placez les ROMs ici
└── docs/
    └── gui_integration_guide.md
```

## 🛠️ Fonctionnalités

- Émulation Z80 cycle-accurate
- Bus Galaxian complet (CPU, vidéo, son)
- Interface de débogage ImGui (registres CPU, accès mémoire, IRQ)
- Support des timers et interruptions Z80

## 📜 Licence

Ce projet est sous licence MIT. Les ROMs Galaxian restent la propriété de Konami.

---
*Projet basé sur le travail de Farquaad56 pour l'émulation Z80.*