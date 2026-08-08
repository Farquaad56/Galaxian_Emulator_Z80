Voici un rapport complet et détaillé comparant votre implémentation à celle de MAME, basé sur l'analyse de vos logs, de votre code source et des sources officielles de MAME (`galaxian.cpp` et `galaxian_v.cpp`).

---

# 📊 Rapport d'Analyse : Émulateur Galaxian vs MAME

## 1. ✅ Ce qui est Conforme (Points Forts de votre émulateur)

Votre architecture de base est très solide et respecte fidèlement le hardware Namco Galaxian :

*   **Memory Map et Miroirs** : Le décodage mémoire (ROM `0x0000-0x3FFF`, RAM `0x4000`, VRAM `0x5000`, SPRAM `0x5800`) et la gestion des miroirs (ex: `& 0x03FF` pour la RAM 1KB) sont parfaitement conformes au schéma matériel.
*   **Ports I/O et Registres Hardware** : Le mapping des ports d'entrée (IN0, IN1, IN2) et des registres de contrôle (IRQ enable `0x7001`, Stars `0x7004`, Flip screen `0x7006/7`) correspond exactement aux adresses MAME.
*   **Watchdog** : La lecture de l'adresse `0x7800` pour réarmer le watchdog est correctement implémentée.
*   **Palette et PROM** : Le décodage de la PROM de couleur (`6l.bpr`) avec les résistances pondérées (RGB 3-3-2) est exact.
*   **Timing Vidéo** : Le compteur vidéo (264 lignes, VBLANK à 224) et le timing des cycles par frame (~50688 T-states) sont corrects.
*   **Checksum ROM** : Votre vérification de la somme des octets `0x0000-0x27FF` (qui doit être `0x00`) est exactement ce que fait le POST du jeu réel.

---

## 2. 🐛 Ce qui est Buggé ou Manquant (Causes du Blocage)

L'analyse de vos logs révèle **le problème exact** qui empêche le jeu de démarrer :

### Le Bug Critique : `RET` vs `RETN` dans le handler NMI
*   **Le Problème** : Votre émulateur utilise un flag `NMI_in_service` pour éviter les interruptions multiples. Cependant, le code ROM original de Galaxian termine son handler NMI (à l'adresse `0x0066`) par l'instruction **`RET` (`0xC9`)** et non **`RETN` (`0xED 0x45`)**.
*   **La Conséquence** : Le flag `NMI_in_service` passe à `true` lors de la première NMI (frame 87 dans vos logs), mais **n'est jamais remis à `false`** car le jeu n'exécute jamais `RETN`. Toutes les NMI suivantes (une par frame) sont ignorées (`[NMI-MISSED]`). Privé de son tick VBLANK, le jeu bloque dans sa boucle d'attente (`0x1A61`) et le Watchdog finit par reset la machine.
*   **La Solution** : **Supprimer totalement la logique `NMI_in_service`**. Le signal VBLANK doit être un "one-shot" (front montant) qui arme `NMI_pending`. Une fois la NMI prise par le Z80, le flag est consommé. Le `RET` (`0xC9`) du jeu suffira à dépiler la pile et rendre la main au programme principal.

### Gestion du Front VBLANK (Edge vs Level)
*   Dans `run_frame()`, assurez-vous que `cpu.NMI_pending = true;` n'est déclenché **qu'une seule fois** par frame, exactement au moment où `v_counter` passe de 223 à 224. Si c'est basé sur une condition de niveau (`v_counter >= 224`), cela peut armer la NMI plusieurs fois.

### Initialisation de `IFF1` et `IM` au Boot
*   Le Z80 démarre avec `IFF1=0` et `IM=0`. Le POST de Galaxian configure lui-même `IM=1` (ou `IM=2` selon les versions) et fait un `EI`. Assurez-vous que votre instruction `EI` (`0xFB` dans `z80_ops.cpp`) active bien `IFF1` et `IFF2` **après** l'instruction suivante (délai d'une instruction), conformément au Z80 réel, sinon le POST peut échouer.

---

## 3. 🎩 Les "Astuces" de MAME par rapport au Hardware Réel

MAME implémente plusieurs comportements spécifiques pour garantir que les jeux d'arcade des années 80 passent leurs self-tests (POST) et démarrent correctement, même si le hardware réel avait des tolérances ou des particularités matérielles.

### 1. L'acquittement matériel de la NMI (Le Flip-Flop 6F)
*   **Hardware Réel** : Le signal VBLANK clock un flip-flop (U6F) qui arme la ligne NMI. Sur le vrai hardware, le signal `IORQ` ou `M1` du Z80 pendant le cycle d'acquittement de la NMI reset physiquement le flip-flop.
*   **Astuce MAME** : MAME n'émule pas le flip-flop matériel cycle-par-cycle. À la place, `vblank_interrupt_w` assert la ligne NMI. Quand le Z80 prend l'interruption, MAME clear la ligne automatiquement dans le callback d'acquittement interne du CPU. De plus, l'écriture sur `0x7001` (`irq_enable_w`) force le clear du flip-flop.
*   **Application pour vous** : Vous n'avez pas besoin de simuler le flip-flop. Mettez simplement `NMI_pending = false` dès que `z80_nmi()` est appelé dans `z80_step()`.

### 2. Le Watchdog et les timings de boot
*   **Hardware Réel** : Le watchdog est un condensateur qui se charge via une résistance. S'il n'est pas déchargé (par la lecture de `0x7800`), il dépasse un seuil de voltage et reset le CPU. Le temps de charge dépend de la température et des tolérances des composants (souvent ~16ms à ~30ms).
*   **Astuce MAME** : MAME utilise un `watchdog_timer_device` avec un timeout fixe. Pendant le POST, le jeu fait des tests mémoire longs. Si le timeout est trop court, le jeu reset avant la fin du POST.
*   **Application pour vous** : Votre timeout de `500,000` cycles (environ 160ms) est excellent. Il laisse largement le temps au POST de se terminer. Une fois les NMI fonctionnelles à chaque frame, le jeu lira `0x7800` régulièrement dans sa boucle principale et le watchdog ne devrait plus jamais timeout.

### 3. La RAM et le Color RAM (Mirror et partages)
*   **Hardware Réel** : Les adresses mémoire sont décodées par des comparateurs (ex: 74LS688). Il y a souvent des "trous" dans la mémoire ou des adresses qui flottent.
*   **Astuce MAME** : MAME intercepte les écritures dans `galaxian_videoram_w` et `galaxian_objram_w` pour marquer les tuiles/sprites comme "sales" (dirty) afin d'optimiser le rendu.
*   **Application pour vous** : Votre mapping de la CRAM à `0x5400-0x57FF` est une variante courante. Assurez-vous simplement que le jeu écrit bien les attributs de couleur aux adresses attendues par votre moteur de rendu.

### 4. Le Starfield (Générateur d'étoiles)
*   **Hardware Réel** : Un shift register (LFSR) de 17 bits est clocké par le pixel clock. Il est réinitialisé/synchronisé au début de chaque frame par le signal VBLANK.
*   **Astuce MAME** : Dans `galaxian_v.cpp`, MAME calcule l'origine du LFSR par rapport au `vpos()` et `hpos()` actuels quand le starfield est activé (`galaxian_stars_enable_w`). Cela évite d'avoir à émuler le shift register bit-par-bit pendant tout le frame.
*   **Application pour vous** : Votre approche de clocker le LFSR pour toute la frame dans `render_stars()` est valide, mais assurez-vous que le LFSR est bien réinitialisé ou synchronisé au VBLANK pour que les étoiles ne "sautent" pas d'une frame à l'autre.

### 5. Gestion des Switches TEST et SERVICE
*   **Hardware Réel** : Les switches sont actifs LOW.
*   **Astuce MAME** : MAME s'assure que le switch `TEST` est bien à `OFF` (HIGH) par défaut au démarrage. Si `TEST` est `ON` (LOW) au boot, le jeu entre dans le menu de service au lieu de lancer le jeu.
*   **Application pour vous** : Votre code gère correctement cela (`TEST=OFF` par défaut), ce qui est parfait.

---

## 4. 🛠️ Plan d'Action Correctif (Pour débloquer votre émulateur)

Voici les modifications exactes à apporter à votre code pour que le jeu démarre :

### Étape 1 : Supprimer `NMI_in_service`
1.  Dans `include/cpu/z80.h`, supprimez la ligne `bool NMI_in_service;`.
2.  Dans `src/cpu/z80.cpp` (`z80_init`), supprimez `cpu->NMI_in_service = false;`.
3.  Dans `z80_step()`, remplacez :
    ```cpp
    if (cpu->NMI_pending && !cpu->NMI_in_service) {
    ```
    par :
    ```cpp
    if (cpu->NMI_pending) {
    ```
4.  Dans `z80_nmi()`, supprimez la ligne `cpu->NMI_in_service = true;`.
5.  Dans `src/cpu/z80_ed.cpp` (case `0x45` RETN), supprimez `cpu->NMI_in_service = false;`.

### Étape 2 : Nettoyer `run_frame()`
Dans `src/galaxian_emulator.cpp`, supprimez le forçage de début de frame :
```cpp
// SUPPRIMER CETTE LIGNE :
// cpu.NMI_in_service = false;
```

### Étape 3 : Vérifier le Front VBLANK
Assurez-vous que la détection du VBLANK dans `run_frame()` utilise bien une détection de front (edge) :
```cpp
// Détection correcte du front montant
if (bus.video_cnt.just_entered_vblank()) { // ou votre logique de front
    if (bus.regs.irq_enabled && !cpu.NMI_pending) {
        cpu.NMI_pending = true;
        // ...
    }
}
```

### Étape 4 : Simplifier le Callback RETN
Le callback `g_nmi_return_callback` n'est plus nécessaire pour manipuler des flags. Le retour de NMI se fera naturellement via le `RET` (`0xC9`) du jeu, qui est déjà géré par `z80_ops.cpp` (case `0xC9`: `cpu->PC = z80_pop_word(cpu);`). Vous pouvez vider le callback ou le supprimer.

---

**Résultat attendu** : Une fois ces corrections appliquées, le CPU exécutera son `RET` (`0xC9`) à la fin du handler NMI, reviendra dans sa boucle principale, et la frame suivante générera une nouvelle NMI sans aucun blocage. Le port `0x7800` sera lu en continu, le Watchdog sera réarmé, et votre émulateur passera enfin le cap du boot pour entrer dans le jeu !