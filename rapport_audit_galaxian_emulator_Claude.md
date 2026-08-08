# Rapport d'audit — Galaxian_Emulator_Z80 vs MAME (galaxian.cpp / galaxian_v.cpp)

Dépôt audité : `Farquaad56/Galaxian_Emulator_Z80` (commit unique, branche `main`)
Référence : `mamedev/mame/src/mame/galaxian/galaxian.cpp` + `galaxian_v.cpp`
Set de ROMs utilisé : `galmidw.u/v/w/y` + `7l` + `1h.bin/1k.bin` + `6l.bpr`
→ correspond **exactement** au set MAME `ROM_START( galaxian )` (le set "parent", pas un clone/bootleg). Bon point de départ : vous n'émulez pas un mauvais dump ou un jeu de la famille (Moon Cresta, Scramble…) qui a une carte mémoire différente.

---

## 0. Diagnostic principal (résumé pour aller vite)

Votre symptôme — "reste dans une boucle avant d'atteindre le programme principal" — colle avec **deux régressions concrètes** que j'ai trouvées en comparant votre code à MAME (pas des suppositions, ce sont des diffs de comportement vérifiables) :

1. **`bus.regs.irq_enabled = true` au reset** (`galaxian_emulator.cpp`, fonction `reset()`, commentée "CORRECTION CRITIQUE (08/08/2026)"). C'est faux. Dans `galaxian.h` de MAME : `uint8_t m_irq_enabled = 0;` — le flip-flop "NMI ON" est **désactivé** au power-on, le jeu doit l'activer lui-même en écrivant en `0x7001`. En le forçant à `true` dès le reset, votre émulateur envoie des NMI au CPU **avant** que le programme ait fini d'initialiser sa pile (`SP`) et ses variables de boot. Une NMI qui tombe pendant les toutes premières instructions du POST (avant que `SP` soit positionné correctement) peut faire planter/boucler tout le début d'exécution. C'est très probablement votre bug n°1.

2. **Une "Color RAM" (CRAM) de 1 Ko à `0x5400-0x57FF` qui n'existe pas sur le vrai hardware Galaxian.** Dans `galaxian_map_base()` de MAME :
   ```cpp
   map(0x5000, 0x53ff).mirror(0x0400).ram().w(FUNC(...galaxian_videoram_w)).share("videoram");
   ```
   `0x5400-0x57FF` est un **mirror** (répétition physique) de la VRAM `0x5000-0x53FF`, pas une mémoire indépendante. Il n'y a **aucune** RAM de couleur par tuile sur Galaxian de base : la couleur d'un tuile vient de l'octet d'attribut de sa **colonne** dans l'OBJRAM (`0x5800-0x583F`, voir `bg_get_tile_info` dans `galaxian_v.cpp` : `uint8_t attrib = m_spriteram[x*2+1]; uint8_t color = attrib & 7;`). Chez vous, `0x5000-0x53FF` (vram) et `0x5400-0x57FF` (cram) sont deux tableaux **séparés** (`vram[0x400]` et `cram[0x400]` dans `galaxian_bus.h`). Résultat : toute routine de boot qui s'appuie sur le fait que ces deux zones sont physiquement la même mémoire (test RAM par écriture/lecture croisée, initialisations qui comptent sur le mirroring) va lire des valeurs incohérentes. C'est un classique des routines de POST d'époque (tester qu'une zone mirror répond bien comme un mirror). Bug d'architecture n°2, quasi certainement lié au blocage observé.

Les deux bugs ci-dessus sont prioritaires. Je détaille tout le reste plus bas, classé par sévérité.

---

## 1. Ce qui est conforme au hardware réel

| Élément | Votre implémentation | MAME | Verdict |
|---|---|---|---|
| Choix des ROMs | `galmidw.u/v/w/y + 7l + 1h.bin/1k.bin + 6l.bpr` | `ROM_START( galaxian )` identique | ✅ conforme |
| ROM programme | linéaire 0x0000-0x3FFF, code réel seulement jusqu'à 0x27FF, reste à 0xFF | `map(0x0000,0x3fff).rom()`, région déclarée 0x4000 mais seuls 0x2800 octets chargés | ✅ conforme |
| Ports IN0/IN1/IN2 | `0x6000-0x67FF` / `0x6800-0x6FFF` / `0x7000-0x77FF`, mirroring correct | `portr("IN0")` mirror 0x07ff sur 0x6000, idem IN1/IN2 | ✅ conforme |
| Watchdog à `0x7800` | lecture réarme le compteur | `map(0x7800,0x7800).mirror(0x07ff).r("watchdog",...)` | ✅ zone correcte (mécanisme interne différent, voir §3.9) |
| Bits palette | R=bits2:0, G=bits5:3, B=bits7:6 | `galaxian_palette()` : rouge=bits0-2, vert=bits3-5, bleu=bits6-7 | ✅ conforme (regroupement correct, calcul de gain différent — cosmétique, voir §3.10) |
| Interruption = NMI (pas INT maskable) | Vous utilisez bien `cpu.NMI_pending`, prioritaire sur `IFF1` | `int m_irq_line = INPUT_LINE_NMI;` par défaut dans `galaxian.h` | ✅ conforme — **et c'est un point que vous aviez visiblement mal compris au début** (les commentaires parlent encore d'"IM2" par endroits, résidu d'une ancienne version, cf. §3.8) mais l'implémentation actuelle dans `run_frame()` est correcte : NMI edge-triggered, indépendante de IFF1/IM. Bon travail sur ce point-là. |
| Sprites : 8 sprites, 4 octets (Y, tile, attr, X), OBJRAM à `0x5840` | conforme à la structure attendue | Structure similaire (schéma HPOSI/VPL/OBJ DATA L décrit dans `galaxian_v.cpp`) | ✅ conforme dans les grandes lignes |
| Timing CPU/vidéo | 3.072 MHz (18.432/6), 264 lignes, VBLANK à la ligne 224, ~50688 cycles/frame | `Z80(config, m_maincpu, GALAXIAN_PIXEL_CLOCK/3/2)`, VTOTAL=264, VBSTART=224 | ✅ conforme |

---

## 2. Bugs critiques (probablement responsables du blocage)

### 2.1 `irq_enabled = true` au reset — inversion du power-on state
- **Fichier** : `src/galaxian_emulator.cpp`, `GalaxianEmulator::reset()`
- **MAME** : `galaxian.h:429` → `uint8_t m_irq_enabled = 0;` (flip-flop clear au reset, `irq_enable_w()` documente : *"the latched D0 bit here goes to the CLEAR line on the interrupt flip-flop"*)
- **Impact** : NMI envoyée dès la frame 0, potentiellement avant que le programme ait exécuté `LD SP,xxxx`. Une NMI avec une pile pas encore initialisée pousse `PC` sur une adresse RAM aléatoire/non initialisée → comportement indéfini, quasi toujours une boucle ou un plantage silencieux.
- **Correctif** : remettre `bus.regs.irq_enabled = false;` au reset, et laisser le jeu l'activer lui-même via l'écriture `0x7001` (ce que votre `write_hw_reg()` gère déjà correctement pour la partie écriture).

### 2.2 CRAM fictive à 0x5400-0x57FF (pas de couleur par tuile sur Galaxian)
- **Fichier** : `src/galaxian_bus.h` (`uint8_t cram[0x0400]`, lecture/écriture dédiées) + usage dans `galaxian_emulator.cpp` (`decode_pixel`/rendu, `log_tilemap_analysis`)
- **MAME** : `map(0x5000, 0x53ff).mirror(0x0400).ram()...share("videoram")` — un seul buffer de 1 Ko, mirroré. La couleur du fond vient de `m_spriteram[x*2+1] & 7` (attribut de colonne dans l'OBJRAM `0x5800-0x583F`), **pas** d'un octet par tuile en `0x5400+`.
- **Impact double** :
  1. Rendu couleur incorrect par rapport au vrai jeu même une fois le boot débloqué (le fond n'aura jamais la bonne couleur car vous lisez une RAM qui n'existe pas).
  2. Toute routine de POST qui suppose que `0x5000-0x53FF` et `0x5400-0x57FF` sont **la même mémoire physique** échouera silencieusement chez vous (écriture d'un motif de test à une adresse, lecture à l'adresse "miroir", comparaison — cas classique de test RAM d'époque).
- **Correctif** :
  - Supprimer `cram[]`, faire de `0x5400-0x57FF` un vrai mirror : `vram[addr & 0x03FF]` (exactement comme MAME).
  - Pour la couleur des tuiles de fond, aller lire l'attribut de colonne dans `spram[col*2 + 1]` (bits 2:0 = couleur), avec `col = x / 8` (32 colonnes), **pas** un octet par tuile individuelle.

---

## 3. Autres non-conformités trouvées (par ordre de sévérité)

### 3.1 Polarité des entrées inversée (COIN/joystick/fire/TEST/SERVICE)
- **Fichier** : `galaxian_bus.h`, `build_in0()`. Le commentaire dit *"BUG P0 #2 CORRIGÉ : TEST et SERVICE sont actifs LOW"* — c'est l'inverse de MAME.
- **MAME** (`INPUT_PORTS_START( galaxian )`) :
  ```
  PORT_BIT( 0x01, IP_ACTIVE_HIGH, IPT_COIN1 )
  PORT_BIT( 0x04, IP_ACTIVE_HIGH, IPT_JOYSTICK_LEFT )
  PORT_BIT( 0x10, IP_ACTIVE_HIGH, IPT_BUTTON1 )
  PORT_SERVICE( 0x40, IP_ACTIVE_HIGH )
  PORT_BIT( 0x80, IP_ACTIVE_HIGH, IPT_SERVICE1 )
  ```
  Tout est **actif HIGH** : bit = 0 au repos, bit = 1 quand pressé/actif. Votre code part d'un `v = 0xFF` (tout à 1 par défaut) et **efface** le bit quand l'entrée est active — c'est le schéma inverse.
- **Impact** : la lecture de `TEST`/`SERVICE`/pièces/joystick est logiquement inversée. Si le POST ou l'attract mode teste l'état de `TEST` à un moment donné (entrée en mode diagnostic, lecture DIP), il verra l'état opposé de ce qui est attendu.
- **Correctif** : partir de `v = 0x00` et **positionner** le bit à 1 quand l'entrée est active, pour les bits `IP_ACTIVE_HIGH` (coin1/coin2/left/right/fire/test/service1). Les bits DIP (cabinet, coinage, bonus, lives) restent des valeurs brutes directes (cf. 3.2), pas soumis à cette logique active-high/low.

### 3.2 DIP switches "coinage" et "bonus life" mal encodés
- **Fichier** : `galaxian_bus.h`, `build_in1()` / `build_in2()`
- **MAME** : les valeurs des `PORT_DIPSETTING` sont les valeurs **brutes** du port, pas un signal actif-bas :
  - Coinage (IN1 bits 7:6) : `0x00`=1C/1C (défaut), `0x40`=2C/1C, `0x80`=1C/2C, `0xC0`=Free Play.
  - Bonus (IN2 bits 1:0) : `0x00`=7000 (défaut), `0x01`=10000, `0x02`=12000, `0x03`=20000.
- **Votre code** part de `v=0xFF` et fait `v &= ~(1<<6)` pour "2C/1C" (donc bit6=0), alors que MAME veut bit6=**1** pour 2C/1C. Le cas par défaut (`case 0`, 1C/1C) ne touche à rien et laisse les bits à 1 alors que MAME attend `0x00`. C'est l'inverse de bout en bout, pour les deux DIP switches (coinage et bonus).
- **Correctif** : ne pas partir de `0xFF` pour ces bits-là ; écrire directement la valeur MAME (`v |= (coinage_val << 6)` avec `coinage_val` 0-3 correspondant directement à 00/01/10/11 → 1C1C/2C1C/1C2C/Free, et pareil pour bonus avec un simple `v |= bonus_val`).
- Le DIP "Lives" (bit2 de IN2) est en revanche **correct** chez vous (bit=1 pour 3 vies = défaut, conforme à `PORT_DIPNAME(0x04,0x04,Lives)`).

### 3.3 Décodage des registres hardware par `addr & 0x0F` — collisions entre 0x6000 et 0x7000
- **Fichier** : `galaxian_bus.h`, `write_hw_reg()`
- **Problème** : vous décodez uniquement sur les 4 bits bas de l'adresse, sans distinguer si l'écriture vient de la zone `/DRIVER` (0x6000-0x67FF) ou `/LATCH` (0x7000-0x77FF). Or ce sont deux registres physiquement différents sur le vrai hardware (deux chip-selects distincts, cf. le schéma en tête de `galaxian.cpp`) :
  ```
  /DRIVER (6000-67ff): 6000=1P START, 6001=2P START, 6002=COIN LOCKOUT, 6003=COIN COUNTER, 6004-6007=résistances son (555 timer)
  /LATCH  (7000-77ff): 7001=NMI ON, 7004=STARS ON, 7006=HFLIP, 7007=VFLIP
  ```
  Chez vous, `case 0x01` traite indifféremment `0x6001` (2P START LAMP, normalement ignoré) et `0x7001` (NMI ON) : une écriture à `0x6001` va donc **modifier `irq_enabled` par erreur**. Même souci pour `0x6004` (résistance son) qui tombe dans le même `case` que `0x7004` (stars enable), et `0x6006` (résistance son) vs `0x7006` (flip screen X).
- **Correctif** : décoder sur `addr` complet (ou au moins sur `addr & 0x0800` en plus du nibble bas, pour distinguer 0x6xxx de 0x7xxx), comme le fait MAME avec des `map()` séparées par plage complète.

### 3.4 Sur-mapping de la RAM principale (0x4800-0x4FFF)
- **Fichier** : `galaxian_bus.h`, `read()`/`write()` : `if (addr < 0x5000) return ram[(addr - 0x4000) & 0x03FF];`
- **MAME** : `map(0x4000, 0x43ff).mirror(0x0400).ram();` → seuls `0x4000-0x47FF` répondent (1 Ko physique mirroré une fois). `0x4800-0x4FFF` est **non mappé** (commentaire du schéma : *"4800-4fff -> n/c"*), donc bus flottant (0xFF côté lecture avec `unmap_value_high()`).
- **Impact** : mineur pour le POST standard, mais non conforme : chez vous cette zone se comporte comme de la RAM valide (mirroir supplémentaire), alors qu'elle devrait être flottante sur le hardware "galaxian" de base. À corriger pour la rigueur, mais probablement pas bloquant pour le boot.

### 3.5 Flip screen X/Y fusionnés en un seul bool
- **Fichier** : `galaxian_bus.h`, `HardwareRegs::flip_screen`
- **MAME** : deux registres indépendants, `galaxian_flip_screen_x_w` (0x7006) et `galaxian_flip_screen_y_w` (0x7007).
- **Impact** : pas bloquant pour le boot (le jeu en upright n'active jamais ces bits), mais faux en mode cocktail/joueur 2 et pour l'écran de test si celui-ci teste indépendamment X et Y.

### 3.6 `NMI_in_service` jamais réarmé si le handler ne finit pas par `RETN`
- **Fichier** : `include/cpu/z80.h` / `src/cpu/z80.cpp`
- Votre implémentation ne remet `NMI_in_service` à `false` que via l'interception de l'opcode `RETN` (0xED 0x45). Si pour une raison quelconque le handler d'interruption réel utilise une autre séquence de retour avant d'atteindre le `RETN` final (jump conditionnel vers un point de sortie différent, etc. — peu probable mais pas à exclure sans avoir désassemblé la ROM), la NMI resterait bloquée "in service" indéfiniment et plus aucune VBLANK ne serait délivrée après la première. À vérifier en traçant le PC autour de `0x0066` (vecteur NMI standard) pour confirmer que chaque passage se termine bien par `RETN`.

### 3.7 Watchdog basé sur un compteur de cycles, pas sur un compteur de VBLANK
- **Fichier** : `galaxian_bus.h`, `check_watchdog()` (limite à 500000 cycles ≈ 160 ms)
- **MAME** : `WATCHDOG_TIMER(config, "watchdog").set_vblank_count("screen", 8);` → reset si le CPU ne relit pas `0x7800` pendant **8 VBLANK** (~132 ms), indépendamment du nombre de cycles CPU consommés entre-temps.
- Ce n'est pas un bug en soi (votre approximation cycles↔temps est raisonnable), mais notez que vous avez déjà dû **augmenter artificiellement** ce timeout ("Timeout diagnostic augmenté à 500000... pour permettre au boot de progresser", commentaire dans le code) — c'est un signe que le boot met plus de temps que prévu à progresser, cohérent avec un CPU qui boucle sur les bugs 2.1/2.2 au lieu d'avancer normalement et de relire `0x7800` périodiquement. Une fois 2.1 et 2.2 corrigés, il vaut la peine de repasser à une valeur proche du hardware réel (~8 VBLANK) pour vérifier que le POST relit bien le watchdog à temps.

### 3.8 Résidus de commentaires "IM2" trompeurs
- **Fichier** : `galaxian_emulator.cpp`, commentaires dans `load_roms()` (*"7l — Video/IRQ handler + IM2 setup"*) et `reset()` (*"Le boot Galaxian configure lui-même IM2 + I pendant le POST"*).
- Ces commentaires datent visiblement d'une version antérieure de l'émulateur où l'auteur pensait, à tort, que Galaxian utilisait une interruption maskable en mode IM2 avec table de vecteurs. Ce n'est pas le cas (voir §1, `INPUT_LINE_NMI`). Le code actuel a l'air d'avoir été corrigé pour utiliser NMI correctement, mais ces commentaires obsolètes risquent d'induire en erreur toute personne qui reprend le projet — à nettoyer.

### 3.9 Palette : DAC linéaire au lieu du mélange par résistances pondérées
- **Fichier** : `galaxian_emulator.cpp`, `build_palette()` (LUT linéaire `0x00,0x24,0x49,...`)
- **MAME** utilise `compute_resistor_weights()` avec des résistances réelles (1kΩ/470Ω/220Ω pour R/G, 470Ω/220Ω pour B), ce qui donne une réponse **non linéaire**. Purement cosmétique, sans impact sur le boot — juste des teintes légèrement différentes du hardware réel.

---

## 4. "Astuces" employées par MAME (hardware réel) pour que le self-test passe et que le jeu démarre

Ce sont des points où MAME ne modélise pas le silicium à 100 % au niveau transistor, mais reproduit fidèlement le comportement observable — ce sont ces "astuces" qui permettent aux ROMs originales de passer leur propre self-test sans modification :

1. **Bus flottant = 0xFF, pas 0x00.** `map.unmap_value_high()` en tête de `galaxian_map_base()` : toute adresse non décodée par un composant renvoie 0xFF (le bus de données est tiré au niveau haut par les résistances de pull-up des bus drivers 74LS368/74LS367, jamais à zéro). Vous faites déjà ça correctement pour votre zone `0x7800+` — assurez-vous que c'est cohérent partout, y compris pour `0x4800-0x4FFF` une fois corrigé (§3.4).

2. **IM0 par défaut, mais ça n'a aucune importance car l'interruption est câblée sur NMI, pas INT.** MAME ne configure explicitement **aucun** mode d'interruption particulier pour le Z80 de Galaxian (`Z80(config, m_maincpu, ...)` sans `set_irq_acknowledge_callback` ni vecteur). C'est voulu : la ligne d'interruption maskable (INT) du Z80 n'est **même pas câblée** sur le PCB Galaxian d'origine. Tout le mécanisme d'IRQ passe par la broche NMI, réputée non maskable et indépendante d'IFF1/IM. C'est ce qui permet au hardware de garantir qu'une VBLANK sera toujours prise en compte même si le programme a oublié de faire `EI`, ou est dans un état IM bizarre — un vrai "filet de sécurité" matériel que le POST original exploite implicitement (il n'a jamais besoin de se soucier d'activer les interruptions maskables).

3. **Flip-flop "NMI ON" initialisé à l'état clear (désactivé) au reset**, et c'est le programme lui-même qui l'active au bon moment (`irq_enable_w`, écriture `0x7001`). C'est l'"astuce" (en fait juste la réalité du composant D flip-flop au reset) que votre §2.1 viole actuellement : le vrai hardware **garantit** qu'aucune NMI n'arrive tant que le programme ne l'a pas explicitement demandé, ce qui laisse au POST le temps de configurer sa pile et ses variables avant la première interruption.

4. **Watchdog cadencé sur le VBLANK (8 trames), pas sur une horloge indépendante.** `set_vblank_count("screen", 8)` : cela permet à un jeu qui boucle correctement dans sa boucle principale (qui relit `0x7800` typiquement une fois par frame pendant l'attente VBLANK) de ne jamais déclencher le watchdog, même si sa boucle de jeu est lente en cycles CPU — le comptage est calé sur l'affichage, pas sur le temps CPU brut. Une implémentation par comptage de cycles (comme la vôtre) est une approximation raisonnable, mais peut diverger si votre boucle d'attente NMI (voir bug 2.1) consomme les cycles différemment de ce qu'attend le vrai timing.

5. **Pas de RAM de couleur dédiée — la couleur vient du même mécanisme que le scroll.** L'astuce hardware ici est d'avoir réutilisé l'OBJRAM (mémoire des sprites) pour stocker, dans ses 64 premiers octets, un couple **(scroll Y, couleur)** par colonne de 8 pixels de large (32 colonnes de large d'écran), au lieu d'une vraie RAM de couleur par caractère comme sur des hardwares plus tardifs (Scramble et dérivés Konami ont, eux, une vraie logique de couleur par caractère). C'est une économie de composants typique de 1979 : une seule mémoire fait double emploi (scroll + couleur de fond), ce qui explique le mirroring "gratuit" de la VRAM en `0x5400-0x57FF` (le concepteur n'a simplement pas dépensé de décodeur d'adresse supplémentaire pour distinguer les deux moitiés du bloc de 1 Ko réservé). Voir §2.2 pour l'implication sur votre émulateur.

6. **Le "vrai" reset du Z80 met IFF1/IFF2 à false et I/IM à 0**, ce qui n'a pas d'incidence sur la première interruption puisque celle-ci est une NMI. Votre `reset()` fait déjà ça correctement (`cpu.IFF1=false; cpu.IM=0; cpu.I=0x00;`) — c'est bon, gardez-le, ce n'est que le point 2.1 (`irq_enabled`) qu'il faut aligner sur MAME.

---

## 5. Plan d'action recommandé (par ordre de priorité)

1. **`bus.regs.irq_enabled = false;`** au lieu de `true` dans `reset()`. Testez immédiatement — c'est le correctif le plus susceptible de débloquer le POST à lui seul.
2. **Supprimer `cram[]`** et faire de `0x5400-0x57FF` un vrai mirror de `vram[]` ; relire la couleur de fond depuis `spram[col*2+1] & 0x07` (attribut de colonne), pas depuis une RAM couleur par tuile.
3. **Inverser la polarité** de `build_in0()` pour COIN1/COIN2/LEFT/RIGHT/BUTTON1/TEST/SERVICE1 (actif HIGH, base `0x00` puis `|=`).
4. **Corriger l'encodage des DIP** coinage (IN1) et bonus life (IN2) pour utiliser les valeurs brutes MAME plutôt qu'une logique "clear depuis 0xFF".
5. **Séparer le décodage 0x6000 vs 0x7000** dans `write_hw_reg()` pour éviter les collisions `6001/7001`, `6004/7004`, `6006/7006`.
6. Une fois 1-5 en place, relancer avec `LOG_BOOT_SEQUENCE`/`LOG_IRQ_EVENTS` actifs et vérifier que la première NMI survient bien **après** que le programme ait écrit en `0x7001`, et que le PC atteint une zone > `0x2000` (votre propre marqueur `seen_main` dans `run_frame()`) sans revenir boucler sur `0x1A60-0x1A70`.
7. Nettoyage mineur : RAM `0x4800-0x4FFF` non mappée, flip X/Y séparés, commentaires "IM2" obsolètes, palette par résistances pondérées (cosmétique, à faire en dernier).

Bon courage pour la suite — l'architecture générale (bus, découpage fichiers, cœur Z80 avec support NMI correct, timing vidéo) est saine ; les bugs identifiés sont localisés et corrigeables un par un sans réécriture profonde.
