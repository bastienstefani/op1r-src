# Étude du firmware OP-1 R246 d'origine (et de la façon dont op1r s'y greffe)

Document d'étude : comment fonctionne le firmware d'origine Teenage Engineering R246, à la lumière de ce que
modifie le patcher op1r, et par où passer pour y ajouter des fonctionnalités. Tout ce qui suit a été obtenu par
analyse statique (désassemblage, chaînes, tables) ; rien n'a été testé sur un OP-1. Les adresses sont des adresses
de chargement (celles du désassemblage), pas des offsets dans le fichier.

Légende : **vérifié** = lu directement dans les octets / le code ; *déduit* = interprétation plausible, à confirmer.

> **Origine de ce document.** Il a été écrit dans le dépôt du patcher (`op1r`, branche
> `claude/brave-galileo-8ytbnt`), à partir de l'image binaire d'op1r 12, avant la publication de ces sources. Il
> est repris ici pour sa partie sur le firmware **d'origine** (formats, sous-systèmes, adresses R246), qui reste
> valable. Deux choses ont changé :
> - les adresses dans le bloc d'op1r (`01e0xxxx`-`01e2xxxx`) sont celles d'op1r 12 ; dans ce dépôt, ces fonctions
>   sont dans les sources (`synth_sdk/`) et leurs adresses sortent de l'éditeur de liens à chaque build ;
> - les outils cités (`tools/fwtool.py`, `fwbuild.py`, `fwcheck.py`, `analyze.sh`) et les `examples/` sont restés
>   dans cette branche du dépôt `op1r` : ils travaillent sur l'image binaire. Ici, on construit avec
>   `synth_sdk/make_firmware.py` (voir [developpement.md](developpement.md)). `examples/pianoroll` et
>   `examples/drive` sont devenus `synth_sdk/seq/roll.c` et `synth_sdk/plugins/drive/`.

Le dépôt ne contient aucun fichier de Teenage Engineering : l'image, l'arborescence décompressée et le
désassemblage restent hors de git.

## 1. Récupérer le firmware

L'image est publiée par Teenage Engineering :

```sh
curl -LO https://teenage.engineering/_software/op-1/op1_246.op1
sha256sum op1_246.op1   # c5315218f825f143b415ca554516541898abee843d3a236df0b54c04e1fb13a9
```

Le SHA-256 est celui que `patch_op1.py` exige (vérifié le 2026-10-10 : 13 039 128 octets). Puis :

```sh
tools/build_bfin_toolchain.sh --binutils-only  # binutils bfin-elf -> work/bfin/bin (quelques minutes)
export BFIN_OBJDUMP=$PWD/work/bfin/bin/bfin-elf-objdump
tools/analyze.sh op1_246.op1 work              # décompresse, applique op1r à part, désassemble, indexe
```

## 2. Le conteneur `.op1`

**Vérifié** : `CRC-32 (LE, 4 octets) + flux LZMA « alone » d'une archive tar` (format GNU). 117 entrées :

| Chemin | Taille | Rôle |
|---|---|---|
| `te-boot.ldr` | 261 604 | bootloader TE (fichier LDR Blackfin) |
| `OP1_vdk.ldr` | 2 171 624 | le programme principal (LDR Blackfin) — seul binaire modifié par op1r |
| `content/op1_factory.db` | | SQLite : **registre** des moteurs/FX/LFO/séquenceurs + presets d'usine |
| `content/op1.db` | | SQLite : état utilisateur par défaut (slots, séquenceurs, master, réglages) |
| `content/tape.db`, `content/kerntable.db` | | SQLite : état de la bande ; crénage de la police |
| `content/display/*.svg` (62) | | écrans 320×160, dessinés en vectoriel par le firmware |
| `content/audio/**.raw` (47) | 22 Mo | échantillons d'usine (drum, synth, speech) |

## 3. `OP1_vdk.ldr` : format LDR Blackfin (BF52x)

**Vérifié** sur les 801 blocs (et les 118 de `te-boot.ldr`). Chaque bloc a un en-tête de 16 octets
(4 mots LE32) suivi de sa charge utile :

| Mot | Contenu |
|---|---|
| 0 block code | bits 0-3 code DMA (toujours 6), bits 4-15 drapeaux, bits 16-23 **HDRCHK**, bits 24-31 signature `0xAD` |
| 1 | adresse cible |
| 2 | nombre d'octets |
| 3 argument | FILL : motif de remplissage ; bloc FIRST : taille du DXE |

- Drapeaux : `FILL 0x100` (pas de charge utile, la zone est remplie avec l'argument — c'est le BSS),
  `CALLBACK 0x400`, `INIT 0x800`, `IGNORE 0x1000`, `INDIRECT 0x2000`, `FIRST 0x4000`, `FINAL 0x8000`.
- **HDRCHK** = XOR des 4 mots (octet de checksum à 0) replié sur 8 bits (`x ^= x>>16; x ^= x>>8`). Vrai pour 100 %
  des en-têtes.
- Premier bloc (`FIRST|IGNORE`, cible `ffa00000`) : argument = **taille du fichier − 16**. Quand op1r ajoute son
  bloc de code, il ne change que cet argument et le HDRCHK du premier en-tête (`0x2122d8 → 0x22a170`, soit
  +97 944 = 16 + 0x17e88).
- Le bloc `FINAL` (cible `ffa00000`, 0 octet) termine le chargement ; l'exécution démarre en L1 à `ffa00000`
  (*déduit*, comportement standard du BF52x). Un dernier
  bloc `CALLBACK|IGNORE|INDIRECT` de 0xf0 octets le suit (métadonnées, non chargées).
- Répartition : 340 blocs de données SDRAM (2,08 Mo), 413 FILL (21 Mo de zones mises à zéro), 45 blocs
  `CALLBACK|INDIRECT` pour la L1 (80 Ko).

**Le code et les données L1 sont encodés.** Les blocs `CALLBACK|INDIRECT` (L1 code `ffa00000-ffa0bfb4`, L1 data)
ont une entropie de 7,4 bits/octet et se désassemblent mal (alors que le crt0 du bootloader, lui, est en clair et
partage quelques octets identiques avec le début du bloc). Le bootloader les décode au chargement via la routine de
callback. Le code SDRAM, lui, est **en clair** : c'est là qu'op1r pose tous ses hooks. Ce document ne s'occupe
pas de la L1 (~48 Ko, *déduit* : les noyaux DSP les plus chauds et le gestionnaire d'interruption audio ; le
code SDRAM y appelle beaucoup, par exemple `ffa0a1e0` est la cible d'au moins 393 `CALL (P1)`).

## 4. Carte mémoire

Reconstruite à partir des blocs (`tools/fwtool.py mem`) — **vérifié** :

| Plage | Contenu |
|---|---|
| `00000000-01000000` | rien de chargé par `OP1_vdk.ldr` ; `te-boot` y tourne (`00000004-00029294`) pendant le chargement |
| `01000000-01001ea8` | table de 256 pointeurs de fonction terminée par 0 (`01000000`, *déduit* : constructeurs statiques C++) et données |
| `01001ea8-018ab7cc` | BSS (8,7 Mo mis à zéro), dont le **tas** : une arène de `0x840000` octets, de descripteur `0106b7cc`, créée par `01a5e364` (*déduit* : de `0106b7cc` jusqu'à `018ab7cc`) |
| `018ab7cc-01aa7078` | **données en lecture + code** (2 Mo) : chaînes, tables, vtables, puis le code C++ |
| `01aa7078-01dfff34` | BSS (3,4 Mo) |
| `01dfff34-01f00000` | **libre** (~1 Mo) : op1r 12 y charge `01e00000-01e2a1d0` (BSS comprise), les exemples roll/drive `01ee0000` ; carte détaillée dans [developpement.md](developpement.md) §6.2 |
| `01f00000-01f21308` | données/BSS de `te-boot` (actif pendant le chargement : ne rien y charger) |
| `02000000-02025800`, `02040000-027ffb04` | grandes zones mises à zéro (buffers : *déduit* audio/bande/écran) |
| `03000000-0303ffff` | zone mise à zéro |
| `ff800000-ff808000`, `ff900000-ff908000` | L1 données A et B (encodées) : paramètres courants, buffers DMA |
| `ffb00000-ffb01000` | scratchpad L1 : objets voix / FX / LFO construits sur place |
| `ffa00000-ffa0bfb4` | L1 code (encodé), point d'entrée |

Conséquence pratique : du code ajouté doit être chargé dans `01dfff34-01f00000`, au-dessus d'op1r si on part
d'op1r. midi-panic utilise `01e80000`, roll et drive `01ee0000`, parce qu'une image tierce (op1_249) occupe
`01e40000-01eb7464`. Jamais dans les zones de `te-boot`, qui serait écrasé pendant qu'il charge l'image.

## 5. Organisation du programme

Pas de symboles, mais VisualDSP++ a laissé ~1 300 chaînes d'assertion `..\src\<dossier>\<fichier>.cpp:<ligne>
<condition>` qui nomment les fichiers source. `tools/fwtool.py xref` s'en sert pour nommer ~1 265 des ~8 650
fonctions repérées. Arborescence reconstituée :

| Dossier | Fichiers (extraits) | Rôle |
|---|---|---|
| `src/bf` | `audio.cpp codec.cpp nand.cpp usb.cpp usb_ms_yaffs.cpp gpio.cpp twi.cpp SPI.cpp RGB.cpp vu.cpp xyz.cpp fm_radio.cpp powermgr.cpp flasher.cpp tar.cpp` | pilotes Blackfin : codec audio, NAND, USB, accéléromètre, radio FM, mise à jour |
| `src/dsp` | `BodeAudioEngine Core VoiceCtl Adsr` + moteurs `Cls DrWave Dssd Fm Ftwo Id Iter Pd Pls St T10 dsynth dbox DrumSampler SynthSampler` + FX `Grid Mllp Mmmf Ptch Rymd Slump` + LFO `LfoBend LfoCrank LfoDual LfoMidi LfoReRoute LfoRnd LfoSingle` + `lfo_factory producer_factory tape_c metronome modem` | moteur audio |
| `src/seq` | `PlayerManager Timing tempo pattern_data EndlessPlayer TombolaPhysics TombolaPlayer EtchASketch* ok_player` | séquenceurs |
| `src/main/ctls` | `synth_controller DrumController MasterController SeqManager TapeController ModeSwitcher CtlKnob com_controller global_controller …` | contrôleurs (logique des modes) |
| `src/gui` | `MainInterface Screen ScreenId svg_loader`, `lib/{Node Parser Raster Shape AGG}`, `screens/*Screen.cpp fx_bypass lfo_bypass` | interface : SVG → formes, rendu AGG |
| `src/shared` | `instr_types fx_types lfo_types SeqIds Keys Presets bg_saver base_dir` | tables nom ↔ identifiant, touches, presets |
| `src/db2`, `src/disk` | `db2 patch Clips fat_interface`, `fat_manager filesync_yaffs Defragmenter` | presets/patchs, synchronisation FAT ↔ yaffs (mode disque USB) |
| `src/midi` | `midi_interface.cpp` | MIDI USB |
| `ext/` | `sqlite3-bf`, `jsoncpp-src-0.5.0`, `Box2D` (physique du tombola), `yaffs2` (NAND), `libaiff-5.0`, `smx_usbd` (pile USB) | bibliothèques tierces |

## 6. Données : un firmware piloté par SQLite, JSON et SVG

### `op1_factory.db` = registre des types

**Vérifié.** Le firmware ne liste pas lui-même ses moteurs : il lit les tables `synth_types`, `drum_types`,
`fx_types`, `lfo_types`, `seq_types` (`id`, `type` = nom, `default_params` = 8 valeurs). Le nom est ensuite
traduit en identifiant interne par une table C (`instr_types.cpp`, `fx_types.cpp`, `lfo_types.cpp`,
`SeqIds.cpp`). Le firmware R246 contient des moteurs **cachés seulement par leur absence de la base** :

| Table | Stock R246 | Ajouté par op1r |
|---|---|---|
| `synth_types` | cluster digital drwave fm phase pulse dna string dsynth voltage | `iter` (moteur d'origine caché), supersaw, fmbell, granular |
| `fx_types` | grid punch delay phone spring cwo nitro (ids 3-5, 7-10) | `filter` (FX d'origine caché, id 2), tape, clouds, reverb |
| `lfo_types` | tremolo value random element midi bend crank | quad |
| `seq_types` | pattern endless tombola finger sketch arpeggio | euclid |

Les moteurs ajoutés par op1r portent un marqueur dans leur 5e paramètre par défaut : `21249/21250/21251` =
`0x5301/0x5302/0x5303` (synth et FX), et le quad LFO `0x4080…0x4c80` dans les paramètres 5 à 8.

### Patchs = JSON dans SQLite

**Vérifié.** Un son synth est un objet JSON (jsoncpp) : `type`, `knobs[8]`, `adsr[8]`, `fx_type`, `fx_params[8]`,
`fx_active`, `lfo_type`, `lfo_params[8]`, `lfo_active`, `octave`, `name`, `synth_version`. Les valeurs sont des
entiers 16 bits signés (0…32767 pour la plupart des réglages). `op1.db` stocke l'état (tables `synth_slots`,
`drum_slots`, `sequencer` clé/valeur, `endless`, `drumseq`, `okseq`, `master`, `knobs`…) ; par exemple
`sequencer = {synth_sequencer: finger, drum_sequencer: finger}`. Détail intéressant : les données du séquenceur
endless contiennent déjà des champs `euclidean` et `euclidean_offset` (chaînes `018d59e0`, `018d5c90`).

### Écrans = SVG

**Vérifié.** 320×160 px, exportés d'Illustrator (`line`, `path`, `g`, `polyline`, `circle`, `rect`…). Le
firmware les parse (`svg_loader.cpp`, `gui/lib/Parser.cpp`) et anime les groupes par leur `id` (`blue`, `green`,
`white`, `red`, `static`, `bar%i`, `base%i`…). `colors.svg` définit la palette. op1r ajoute ses écrans de la même
façon (`content/display/euclid.svg`, …) et dessine le reste par code.

## 7. Conventions du code C++ (VisualDSP++)

- **Appels** (vérifié) : arguments dans `R0`, `R1`, `R2`, la suite sur la pile à `[SP+12]…` (l'appelant réserve
  12 octets) ; retour dans `R0` ; `R4-R7`, `P3-P5`, `FP` préservés. C'est la convention de GCC `bfin-elf`, on
  peut donc écrire du code en C qui appelle le firmware et est appelé par lui (testé : `bfin-elf-gcc 14.2`).
- **Vtables** (vérifié) : entrées de 8 octets `{int16 ajustement de this, pointeur de fonction}` ; entrée 0
  vide, entrée 1 (`+0x0c`) = destructeur. Un appel virtuel : `P0 = [vptr+8k+4]; R0 = this + W[vptr+8k];
  CALL (P0)`. Les vtables sont dans la zone `0190xxxx-0191xxxx`.
- **Tables nom ↔ id** : remplies au démarrage par des initialiseurs statiques, dans le BSS (*déduit* : appelés
  via la table de 256 pointeurs de fonction à `01000000`).
- **`std::string`** : chaîne interne à `+4` si capacité (`+0x18`) < 16, longueur à `+0x14`. Les comparaisons
  « C string == std::string » sont des petites fonctions isolées — exactement ce qu'op1r détourne pour créer des
  **alias de noms** (section 10).
- **Switch** : tables de pointeurs de code dans les données (`018f5a04`, `018f7fe4`, …). Changer une entrée
  redirige un seul `case`.
- **Objets audio en L1** : voix, FX et LFO sont construits *in place* dans la scratchpad L1 (`ffb00000` voix,
  `ffb00600` FX instrument, `ffb00790` FX master, `ffb00a00` LFO par voix, `ffb00c10` LFO global).
- **Paramètres** : tableaux d'int16 en L1 données, 8 slots × 8 valeurs : moteur `ff8013c4` (synth) /
  `ff801544` (drum), FX `ff801444` / `ff8015c4`, master FX `ff8016c4`, LFO `ff8014c4` / `ff801644`. Slot courant :
  `ff8016d4` (synth), `ff8016d8` (drum).

## 8. Sous-systèmes

### 8.1 Moteurs synth (instruments)

**Vérifié.**

1. La base donne le nom (`SELECT type, default_params FROM synth_types WHERE id=%i`, `0193ffb0`).
2. `instr_types.cpp` traduit le nom en id interne via une table `{nom, id}` de 15 entrées à `01dd4d98`
   (remplie par `019b988c`) ; la comparaison de chaînes est `019b975c`, la recherche `019b9810`.
3. `producer_factory.cpp` « synth reset » (`01a1b7d8`) fait un `switch` sur l'id (table `018f5a04`) et construit
   6 voix (`kSynthVoiceCount = 6`) dans la scratchpad L1 à `ffb00000 + i*taille`.

| id | nom | source | id | nom | source |
|---|---|---|---|---|---|
| 0 | fm | Fm.cpp | 8 | drum | (fabrique drum `01a1b9c4`, 12 voix DrumSampler) |
| 1 | digital | T10.cpp | 9 | dbox | (fabrique drum, dbox.cpp) |
| 2 | drwave | DrWave | 10 | **ls** | caché, voir plus bas |
| 3 | string | St.cpp | 11 | dna | Id.cpp |
| 4 | cluster | Cls.cpp | 12 | dsynth | dsynth.cpp |
| 5 | pulse | Pls.cpp (rendu en L1) | 13 | voltage | Slump.cpp |
| 6 | sampler | SynthSampler | 14 | **iter** | Iter.cpp — caché, réactivé par op1r |
| 7 | phase | Pd.cpp | | | |

Vtable d'une voix (6 entrées, ex. iter `019103cc`) : `+0x0c` destructeur, `+0x14` **render(this, int16 *out,
int16 knobs[8])**, `+0x1c` **note-on(this, knobs)**, `+0x24` état (0 pour un synth), `+0x2c` **note-off**.
Une voix iter fait 0x24 octets. Les sorties sont en int16 fractionnaire saturé ; les potards en int16 0…32767.

- `iter` est un moteur complet (case de fabrique, vtable, écran `iter.svg` livré) : il n'est caché que parce
  qu'il manque une ligne dans `synth_types`.
- `sampler` et `drum` n'ont pas de ligne non plus mais sont atteints par les presets.
- *Déduit* : **`ls`** (id 10) est un moteur caché monophonique (vtable `019109ec`, code `019d3370-019d5800`), sans
  assertion, sans SVG, sans ligne de base ni preset. Rôle inconnu : piste d'étude.

**Patchs** : parseur JSON `01a037ac`. Il renvoie des codes d'erreur (9, 10, 11, 12, 19, 27, 28, 29, 99, 100,
109, 115) affichés par « patch.cpp:390 ». Les bornes sont vérifiées par tables, en BSS :

- `01a1deb0` : potards par type d'instrument, 15×8 bornes à `01de5890` ;
- `01de5710` et `01de56c8` : enveloppes ;
- `01a1df4e` : paramètres FX (`01de5c50`) ;
- `01a1df9c` : paramètres LFO. Il lit `01de5740 + 48*type + 6*i` (`{min, max, …}`) ; un échec donne l'erreur 27.

### 8.2 Effets et chaîne audio

**Vérifié.** Table `fx_types.cpp` à `018dd810` : delay 0, grid 1, punch 2, phone 3, spring 4, **filter 5**,
nitro 6, cwo 7.

- L'id de la base n'est qu'un id d'interface : base → nom → id interne (`019aae44`, comparaison `019aad8e`).
- **Création** : `create_mono_fx(id, pos)` (`01a1b6d0`, table `018f59ac`) pour l'instrument, objet à `ffb00600`,
  positions 0/1. `create_stereo_fx(id)` (`01a1b5f0`, table `018f5910`) pour le master, objet à `ffb00790`. Les
  pointeurs vont dans `s_fx[]` (`01aaad60` synth, `…64` drum, `…68` master).
- **Vtable FX** : `+0x14` **process** (mono : `(this, int16 *buf, const int16 *params)`, stéréo : `(this, L, R,
  params)`) et `+0x1c` **set_position(pos)**. Chaque FX publie ses valeurs d'écran dans `ff801774 + pos*16`.

Écrans : delay, filter et cwo vérifiés ; les autres *déduits* du nom de fichier.

| FX | source | écran | FX | source | écran |
|---|---|---|---|---|---|
| delay | Dssd.cpp | delay.svg | spring | Rymd.cpp | rymd.svg |
| grid | Grid.cpp | grid.svg | **filter** | Mmmf.cpp | mmmf.svg (caché, réactivé par op1r) |
| punch | Mllp.cpp | mllp.svg | nitro | Ftwo.cpp | ftwo.svg |
| phone | Ptch.cpp | ptch.svg | cwo | BodeAudioEngine.cpp | bode.svg |

**Matériel audio** : SPORT0 en I2S 32 bits, DMA par listes de descripteurs, interruption IVG11 (gestionnaire en
L1 `ffa0047c`), 64 trames stéréo par interruption, 44,1 kHz. Le pointeur du callback de bloc est à `[ff80036c]`
(défaut `ffa04804`, L1). Le firmware contient aussi un passthrough (`0191a7a8`) et un générateur sinus
(`0191a880`) de test.

**Non résolu** : la boucle de rendu iter fait 128 itérations, celle du filtre 64. La boucle qui enchaîne
voix → FX → LFO → mixeur est en L1 (encodée), donc non lisible ici. Ordre *déduit* : voix → FX instrument
(paramètres modulés par le LFO) → bande/mixeur → FX master → sortie.

#### Héberger un nouvel effet par-dessus op1r (vérifié avec `examples/drive`)

- **Aiguillage d'op1r** : les mots `process` / `set_position` du filter pointent vers op1r.
  - Le marqueur est `params[4] = 0x5300 + n`. op1r n'accepte que `1 ≤ n < N`, avec `N` = 4 en op1r 12 (une
    valeur dans ses données) ; sinon il appelle le filter d'origine.
  - op1r garde un cache « dernier effet op1r » (mono et stéréo), qu'il remet à 0 quand l'effet n'est pas le sien.
    drive fait de même avant de traiter un bloc.
- **`process` mono** : `(this, int16 *buf, const int16 *p)`. Il reçoit 64 échantillons Q15, traités sur place.
  - Pour un instrument, `p` est la copie modulée par le LFO (`ff801724`) ; pour le master, `ff8016c4`.
  - Il tourne en interruption IVG11.
- **`process` stéréo** (master) : `(this, L, R, p)`, avec le 4e argument sur la pile.
- **`set_position(this, pos)`** : `pos & 1` donne l'instance (0 synth, 1 drum) ; les positions stéréo sont 2/3.
  Elle tourne dans le fil de l'interface.
- **Effet courant** :
  - le contexte est `[01df4e3c]` : 0 synth, 1 drum, 3 master ;
  - paramètres enregistrés : `ff801444 + 16 * ([ff8016d4] & 7)` (synth), `ff8015c4 + 16 * ([ff8016d8] & 7)`
    (drum), `ff8016c4` (master) ;
  - id interne de l'effet : `ff801734[slot]`, `ff801754[slot]`, `[ff801090]`. Il vaut 5 pour filter.
- **Bornes** :
  - `{min, max, pas}` en int16 à `01de5c50 + id * 0x30 + i * 6` ; la ligne de filter est en `01de5d40` ;
  - elles sont calculées au démarrage, d'où le repli d'op1r si `min ≥ max` : `{64, 14528}`, `{0, 24576}`,
    `{0, 22528}`, `{8192, 32767}` ;
  - *Déduit* : les paramètres 4 à 7 acceptent 0..32767, puisque la page 2 d'op1r en dépend ;
  - une valeur hors bornes fait échouer le patch (erreur 24) ou déclenche une assertion fatale à l'écran.
- **Sauvegarder une modification** : `019ff664(patch, p)`, avec `patch = 01ddd7e8 + slot * 0x38` (synth) ou
  `01ddd9a8 + slot * 0x38` (drum), écrit `fx_params` dans le JSON du son. Pour le master, seul le tableau est
  écrit (comme op1r).
- **Nom** : la comparaison de `fx_types` (`019aad8e`) prend `(char *nom_table, std::string *nom_base)` et renvoie
  1 si les deux noms sont égaux.
- **Écran** :
  - le `case` 5 de ScreenId fx s'exécute dans le cadre du `switch` ; il se termine par `P0 = [FP + 4]; UNLINK;
    JUMP (P0)` ;
  - tous les effets op1r partagent l'écran `0xb` ; le choix d'op1r (`01e02018` en op1r 12) réécrit
    l'emplacement `0xb` du registre à chaque appel.
- **Encodeurs** : la page 1 est gérée par le firmware d'origine (bornes et pas de la table). Les appuis
  d'encodeurs sont les touches `0x3b`-`0x3e`.
- **LFO** : *déduit*, il ne module que `p[0..3]` de la copie.

### 8.3 LFO

**Vérifié.** Table `lfo_types.cpp` (`01dd58e4`) : tremolo 0, value 1, random 2, element 3, midi 4, bend 5, crank 6
(les ids de la base commencent à 1). `lfo_factory.cpp` a trois `switch` (fx `019c99ac`, synth `019c9a24`, drum
`019c9ac0`) ; un LFO existe en une instance par voix (6 synth / 12 drum, à `ffb00a00 + i*taille`) plus une
instance globale (`ffb00c10`).

| type | classe | vtable synth / process |
|---|---|---|
| tremolo | LfoDual | `019107fc` / `019cbe48` |
| value | LfoSingle | `0191097c` / `019cfaf8` |
| random | LfoRnd | `0191090c` / `019cea70` |
| element | LfoReRoute | `0191089c` / `019cda14` |
| midi | LfoMidi | `0191082c` / `019cce68` |
| bend | LfoBend | `0191071c` / `019c9d44` |
| crank | LfoCrank | `0191078c` / `019cb0d4` |

- **Vtable LFO** : `+0x14` **process(this, int16 params[8], passe)**, `+0x1c` **note-on(params)** (appelé par
  VoiceCtl si `lfo_active`).
- La modulation passe par :
  - `01a1dfec` (potard moteur synth, `Ranges.cpp:810`) et `01a1e57c` (drum) ;
  - `019f69b0` (enveloppe) ;
  - les objets FX par slot (`ff801734`, `ff801754`) ;
  - deux buffers par bloc, `ff801950` (128 × int32) et `ff801d50` (128 × int16) — *déduit* : hauteur
    (vibrato) et amplitude (trémolo).
- Le LFO midi a 4 voies (valeurs CC à `01dff13c`). La classe d'une voie vaut `params[4+i] >> 11` ; *déduit* :
  0 = moteur, 1 = enveloppe, 2 = FX, 3 = autre.
- **Écrans LFO** (`01a3bab0(this, id, svg)` enregistre l'écran dans `01df4e54[id]`) : 0x18 duallfo,
  0x19 singlelfo, 0x1a rndlfo, 0x1b reroutelfo, 0x1c midilfo, 0x1d bendlfo, 0x1e cranklfo. Les valeurs affichées
  viennent de `ReadInterface::getLfoScreenValues` (`01a293bc`), qui normalise via la table de bornes et lève une
  assertion hors bornes.

### 8.4 Interface

- `ScreenId.cpp` traduit un type en numéro d'écran par des `switch` :
  - fx : `018f7fe4`, delay 8 … spring 0xf ;
  - synth : 15 cas, iter 0x14, dna 0x32, dsynth 0x12, voltage 0x13 ;
  - drum : 0x10, dbox 0x11 ;
  - lfo : `018f80e8`, 0x18 … 0x1e.
- Au démarrage :
  - `01a3bce4` pré-charge environ 70 écrans ; chaque singleton charge son SVG (iter : `019ba2f8`, objet
    `01dd4f60`) ;
  - `01a3ba50` (appelé par `OP_Main.cpp`) appelle l'`init` (`+0x24`) des 68 écrans listés à `018f7f64`.
- Un écran d'effet récupère ses 8 valeurs via `ReadInterface::fx_values` (`01a28da0`). L'élément SVG cherché par
  nom a son drapeau de visibilité à `B[elem+0x50]`.
- **Clavier / encodeurs** : tous les événements passent par le fil `KeyHandler` (`019c2cc0`), qui appelle
  `key_dispatch` (`019b6c74`) et `encoder_dispatch` (`019b6c0c`) ; c'est le point de passage obligé pour un
  raccourci global (détails en 8.6).

#### Navigateurs de presets (vérifié)

- **Chargement** : au démarrage, `01a05680` lit `synth_presets` (base d'usine) puis `synth_user` (`op1.db`).
  - Les presets vont dans le tableau `01dddb68` (0x38 octets par preset : JSON en +4, dossier en +0x14).
  - Limites : sampler ≤ 42, synth ≤ 176, user ≤ 100, total ≤ 318.
  - Un patch invalide est seulement signalé dans le journal ; il reste dans la liste.
- **Choix du moteur** (Shift + T1, constructeur `01a02444`) :
  - les entrées viennent de `synth_types`, donc avec les noms de types (« DRWAVE ») ;
  - il n'y range des presets que pour le dossier « sampler » (`01a024f6`). Les autres moteurs ont donc une
    colonne vide, c'est le comportement d'origine.
- **Navigateur de presets** (Shift + 1…8, constructeur `01a02794`) : les entrées sont les dossiers des presets
  (« DR WAVE »), avec tous leurs presets. Un moteur sans preset d'usine (fmbell, supersaw, granular, iter) n'y
  apparaît pas.
- **Sélection** : `01a50f8c` → `0193ffb0`. Un id > 0 charge les valeurs par défaut du moteur depuis
  `synth_types` ; un id ≤ 0 charge le preset n° `-id` du tableau.

### 8.5 Séquenceurs

**Vérifié.**

- `SeqIds.cpp` (`018fa634`) : pattern 0, endless 1, tombola 2, finger 3, sketch 4, arpeggio 5. Deux fonctions :
  `idFromName` (`01a42344`, nom inconnu → 1) et `nameFromId` (`01a422ec`).
- `SeqManager` « select sequencer » (`01a429d0`, entrée de vtable `0191158c`), dans l'ordre :
  1. `SELECT type FROM seq_types` puis `idFromName` ;
  2. désactive l'ancien contrôleur (`01df5018[type]`) ;
  3. `UPDATE sequencer SET value='%s' WHERE key='synth_sequencer'` (ou `drum_sequencer`) dans `op1.db` ;
  4. `PlayerManager::setSequencer(id)` (`01a0d050`) ;
  5. réactive le contrôleur.

  Au démarrage, `01a42ba0` refait ces étapes pour le mode synth (0) et le mode drum (1) (mode courant :
  `[01de4c40]`).
- `PlayerManager::setSequencer` est un `switch` sur 6 cas. Chaque cas appelle un singleton paresseux : pattern
  `01a08840`, endless `01987cd8`, tombola `01a65ae0`, finger `019ed460`, sketch `0198c754`, arp `01a46400`. Le
  lecteur actif est rangé dans `01ab313c` (synth) / `01ab3138` (drum).
- **Horloge** : le fil séquenceur (`01a4318c`) attend un sémaphore (`[01df50b8]`) que le cœur DSP poste à chaque
  bloc. Il appelle `tick` (`01a0d2c0`), qui lit le MIDI entrant, gère l'horloge externe (« midi_ext_clock_in ») et
  appelle le tick virtuel des lecteurs actifs.
- **Horloge** (vérifié avec `examples/pianoroll`) :
  - 96 ticks par noire (les constantes 96.0 et 2646000.0 apparaissent dans le calcul du tempo) ;
  - le tick passé aux lecteurs est le compteur `018c1884` **modulo 48** (une demi-noire) ; `018c187c` compte
    modulo 4 (horloge MIDI) ;
  - `01a5e7d0` remet les trois compteurs (`018c187c/80/84`) à zéro. Elle est appelée par PLAY (`019ac780`),
    Start MIDI (`019e31a8`, qui remet aussi le métronome à zéro par `019de510`) et le départ des séquenceurs
    d'origine ;
  - le métronome lui-même est dans la L1 encodée.
- **Vtable d'un lecteur** :
  - `+0x0c` destructeur ;
  - `+0x14` `key_down(self, touche, shift)`, qui renvoie 1 pour avaler la touche ;
  - `+0x1c` `key_up` ;
  - `+0x24` sortie du mode ;
  - `+0x2c` `tick(self, tick48)` ;
  - `+0x34` image ;
  - `+0x3c` `stop`, qui doit pouvoir être appelé plusieurs fois.
- **Lecteurs** : celui d'arpeggio est obtenu en `01a0d1b0` (`01a46400`), celui de finger en `01a0d150`. Écrans :
  arpeggio `0x2c`, finger `0x2d`.
- **Jouer des notes** depuis un lecteur :
  - `01a85750(MidiNote *, demi-tons batterie)` et `01a85840(MidiNote *)`, avec
    `MidiNote = {note, vélocité, midi = 1, pad, user}` ;
  - octave du synthé courant : `01dfe4e0[slot]`.

### 8.6 Démarrage et boucle principale

**Vérifié.** `te-boot.ldr` = TE-BOOT v2.30 (« Rev. 00244; 2022/03/15 »). Il :

- propose le menu Upload firmware (disque USB), Function test, Factory reset, Format drive ;
- vérifie le CRC-32 du `.op1`, le décompresse (LZMA), le détarre dans `/yaffs2` puis charge `OP1_vdk.ldr` ;
- ne contient aucune chaîne de vérification de signature (*déduit* : rien n'empêche un firmware modifié de
  démarrer, ce que confirme op1r).

Le fil `OP_Main` (`019efef0`) chronomètre chaque étape d'initialisation, dans l'ordre :

1. matériel : TWI, audio, GPIO ;
2. montage yaffs et éventuelle mise à jour de `te-boot` ;
3. synchro PC et vérification du contenu ;
4. bases et FileWorker, clips, FAT, USB ;
5. cœur DSP (`Core.cpp`) ;
6. `01a3bce4` construit ~69 singletons dont les écrans ;
7. activation des entrées, `01a3ba50` (init des écrans), callbacks de touches.

La boucle principale (`019f09fe-019f0b94`) :

- vérifie les canaris de pile (`0xdeadbeef`, « stack is busted! ») ;
- interroge audio_clipboard, album, `global_controller` (`019ad090`) et la gestion d'alimentation/USB ;
- dort quand il n'y a rien à faire.

**Entrées** :

1. Les callbacks GPIO (`019efe40`) poussent `{touche, état}` dans une file (`01dd5044`) et postent un sémaphore.
2. Le fil KeyHandler (`019c2cc0`) dépile et appelle `key_dispatch(touche, appui)` (`019b6c74`) en `019c2d3e`,
   ou `encoder_dispatch(encodeur, delta)` (`019b6c0c`) en `019c2e94`. Les deltas d'encodeurs s'accumulent dans
   `01dd5250[4]`.
3. Le répartiteur passe l'événement au contrôleur courant (`[01dd8bc8]`) :
   - appui sans Shift → vtable `+0x14` ;
   - appui avec Shift → `+0x1c` ;
   - relâchement → `+0x24` ;
   - repli sur `global_controller` (`019ac4b4` / `019ac9b8`).
4. **Shift** = touche `0x26`, état dans l'octet `01dd7f78` (lu par `019d7c6c`, 26 appelants). Shift + encodeur
   est transmis comme encodeur + 4.

Codes de touches du clavier (**vérifié**, d'après la table de `Keys.cpp`, `018e0c0c`, qui traduit un code en
note 0-23) :

| Blanches | F | G | A | B | C | D | E | F | G | A | B | C | D | E |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| code | 27 | 28 | 29 | 2a | 1c | 1d | 1e | 1f | 30 | 31 | 32 | 33 | 34 | 35 |

| Noires | F# | G# | A# | C# | D# | F# | G# | A# | C# | D# |
|---|---|---|---|---|---|---|---|---|---|---|
| code | 0d | 0e | 0f | 20 | 16 | 17 | 18 | 19 | 1a | 1b |

Autres codes : `0x24`/`0x25` flèches gauche/droite, `0x26` Shift. `0x22` est la touche qu'op1r simule pour lancer
la bande, *déduit* PLAY.

### 8.7 MIDI et mode contrôleur OP-Z

**Vérifié.** MIDI USB (pile smxUSBD, « OP-1 Midi »).

- **Réception** : le parseur `019e31a8` lit un anneau de 1024 paquets USB-MIDI (`01ab0510`) et assemble les SysEx.
- **Envoi** :
  - note : `019e2e64` ;
  - **control change : `019e3004(cc, valeur)`**, sur le canal courant `B[01aadb20]`, ignoré si l'USB n'est pas
    connecté ;
  - pitch bend : `019e3c08`.
- **SysEx** : en plus de l'identity request (`F0 7E 7F 06 01 F7`), il existe des **commandes TE d'origine**
  `F0 00 20 76 00 <cmd> … F7` :

  | cmd | effet |
  |---|---|
  | 01 vv | vv=1 → drapeau `018b6014`, vv=2 → `018b6015` |
  | 02 | valeur 32 bits en 5 octets → `ff802968` |
  | 03 | chaîne → `01ab2b4c` (copiée en `ff907d08`) |
  | 04 | triplets → `01ab2b6c` |

- **Le mode contrôleur OP-Z est une fonction d'origine cachée.**
  - `com_controller.cpp` et `PlayerManager` testent un accesseur `019e0cfc` qui est un bouchon (`R0 = 0; RTS`).
  - Les réglages `opz_synth_channel_temp`, `opz_drum_channel_temp`, `opz_knob_mode_temp` et la table
    `opzcontroller_temp` existent.
  - `com.svg` contient déjà `<g id="opz">`.
  - op1r n'ajoute que deux choses : l'accesseur renvoie `B[018b6016]`, et la commande `01 03` met ce drapeau.

### 8.8 Persistance

**Vérifié.**

- Les bases sont ouvertes par `0196f6b8`. Poignées : `op1.db` `[01bab720]`, factory `[01bab724]`, kerntable
  `[01bab728]`, tape `[01bab72c]`. `op1.db` est copié du contenu vers `/yaffs2/user/` au premier démarrage.
- L'état est sauvé en tâche de fond (`bg_saver`) :
  1. un objet s'enregistre dans une liste (`0193f010`) ;
  2. `markDirty` (`0193ef3c`, 66 appelants) poste un sémaphore ;
  3. le fil FileWorker appelle, au repos, les méthodes de sauvegarde (vtable `+0x10/+0x14`) des objets sales,
     par exemple finger → JSON → `UPDATE okseq SET data='%s' WHERE is_synth=%i`.
- L'euclid d'op1r utilise ce même mécanisme avec sa propre table `euclidseq`.

## 9. Comment op1r s'insère : carte complète des modifications

Cette carte a été dressée sur **op1r 5**. Les versions suivantes ont ajouté quelques sites (par exemple
`019deef2` et `01a85c8a` pour dots). Surtout, **les adresses dans le bloc op1r changent à chaque version** : les
valeurs actuelles de ce que les exemples utilisent sont dans [developpement.md](developpement.md) §6.3, et
`fwtool.py hooks` donne la liste complète pour une version donnée.

`tools/fwtool.py hooks work/orig --funcs work/funcs.json` liste toutes les modifications de `OP1_vdk.ldr` :
45 remplacements (2 dans l'en-tête LDR, le texte de démarrage, 9 tables, 33 branchements) et 1 insertion (le bloc
de 0x17e88 octets chargé en `01e00000`). Leur sens :

**Données (tables)**

| Adresse | Quoi | Effet |
|---|---|---|
| `018f738a` | texte « R. 00246 » | « op1r 5 » sur l'écran de démarrage |
| `018f7ff8` | ScreenId fx, cas 5 (filter) | écrans des FX op1r |
| `018f8074` | ScreenId synth, cas 14 (iter) | écrans des synthés op1r et écran « lab » d'iter |
| `018f80f8` | ScreenId lfo, cas 4 (midi) | écran du quad LFO |
| `019103e0-fb` | vtable voix iter : render, note-on, état, note-off | aiguillage vers supersaw / fmbell / granular / iter |
| `01910840/60/80` | `process` des 3 classes LfoMidi (synth, drum, global) | quad LFO |
| `01910ca8/b0`, `01910cc8/d0` | `process` + `set_position` du filter (mono, stéréo) | tape / clouds / reverb, aussi en FX master |

**Code (CALL ou JUMP.L redirigés)**

| Site(s) | Fonction d'origine | Rôle du détournement |
|---|---|---|
| `019b975c` (JUMP.L) | comparaison de noms `instr_types` | alias supersaw / fmbell / granular → iter |
| `019aad8e` (JUMP.L) | comparaison de noms `fx_types` | alias tape / clouds / reverb → filter |
| `019c9be2`, `019c9c3a` | comparaison de noms `lfo_types` | alias quad → midi |
| `01a42a2e`, `01a42bdc`, `01a42ce0` | `SeqIds::idFromName` | euclid → id 3 (finger) + drapeau |
| `01a422ec` (JUMP.L) | `SeqIds::nameFromId` | renvoie « euclid » si le drapeau est mis |
| `01a0d150` | `FingerPlayer::instance` | renvoie l'EuclidPlayer |
| `01a042a2` | contrôle des bornes LFO (`01a1df9c`) | accepte les paramètres du quad (erreur 27) |
| 7 écrans LFO | `getLfoScreenValues` (`01a293bc`) | valeurs factices pour le quad (évite les assertions) |
| 8 écrans FX | `fx_values` (`01a28da0`) | borne les paramètres des FX op1r (« fx param out of range ») |
| `019b9ef8` | valeurs d'écran moteur (`01a28210`) | écran iter réutilisé par les écrans op1r |
| `01a3bd58` | init de l'écran iter (`019ba2f8`) | construit les écrans op1r (copie de vtable en RAM) |
| `019f07d2` | init de tous les écrans (`01a3ba50`) | init des écrans op1r |
| `01a01844` | `type == "sampler"` (`01a01094`) | granular charge l'échantillon du slot |
| `019c2d3e` | `key_dispatch` (`019b6c74`) | raccourcis Shift globaux (euclid) |
| `019c2e94` | `encoder_dispatch` (`019b6c0c`) | encodeurs sur les écrans op1r |
| `019e0cfc` (JUMP.L) | bouchon « mode OP-Z » | renvoie le drapeau OP-Z |
| `019e3540` (JUMP.L) | parseur SysEx, commande 01 | `01 03` active le mode OP-Z |

## 10. La recette d'op1r (réutilisable)

1. **Se greffer sur un type caché.** Le nouveau nom est déclaré dans `op1_factory.db`, puis rendu équivalent à un
   type d'origine *caché* (iter pour les synthés, filter pour les FX, midi pour les LFO, finger pour le
   séquenceur) en détournant la petite fonction de comparaison de chaînes. Tout le reste de la chaîne d'origine
   fonctionne alors sans modification : base, JSON, validation, fabrique, sauvegarde. Les sons stock qui
   utilisent ces types ne sont pas touchés.
2. **Marquer l'instance dans ses paramètres** : 5e valeur `0x53nn` (synth/FX) ou bits `0x4000` (quad). Le
   marqueur voyage avec le patch JSON, donc avec les presets.
3. **Aiguiller par vtable** : les entrées `render` / `process` / `note-on` du type hôte pointent vers un
   répartiteur. Il lit le marqueur, exécute le nouveau code, sinon saute à la fonction d'origine.
4. **Écrans** : on détourne le `case` de `ScreenId` du type hôte et l'init des écrans. On construit ses propres
   objets écran avec `01a3bab0(this, id, "xxx.svg")` en copiant une vtable d'origine en RAM pour remplacer
   quelques méthodes.
5. **Désamorcer les validations** (tables de bornes, assertions des écrans) par des enveloppes qui appellent
   l'original puis tolèrent le marqueur.
6. **Entrées globales** : envelopper les deux appels du fil KeyHandler.
7. **Persistance** : s'enregistrer auprès de `bg_saver` avec sa propre table SQL.

## 11. Ajouter des fonctionnalités

### Trois niveaux

| Niveau | Outils | Exemple |
|---|---|---|
| Données seules | `fwbuild.py --sql`, `--file`, `--text` | réactiver iter et filter sur le firmware d'origine |
| Tables | `fwbuild.py --word` (vtable, `case` de switch) | rediriger une méthode vers une autre existante |
| Code | `bfin-elf-gcc` + `fwbuild.py --blob --call/--jump` | [`examples/midi-panic`](../examples/midi-panic) |

Exemple « données seules » (sans code, les mêmes lignes qu'op1r) :

```sh
python3 tools/fwbuild.py op1_246.op1 -o op1_246-unhide.op1 \
  --sql "content/op1_factory.db=INSERT INTO synth_types VALUES (11, 'iter', '[1516, 16704, 0, 15168, 0, 0, 0, 0]')" \
  --sql "content/op1_factory.db=INSERT INTO fx_types VALUES (2, 'filter', '[7548, 0, 8272, 19572, 8000, 8000, 8000, 8000]')"
```

### Exemple de code : MIDI panic

[`examples/midi-panic`](../examples/midi-panic) : **Shift + F du milieu** (code `0x1f`, modifiable par
`PANIC_KEY`) envoie All Sound Off (CC 120) et All Notes Off (CC 123) sur le canal MIDI courant. 104 octets de C,
liés à `01e80000`. Le `CALL` de `019c2d3e` est redirigé vers `key_hook()`, qui renvoie tous les autres événements
à la cible précédente. Sur l'image d'origine, la cible précédente est `019b6c74` ; avec `--op1r`, c'est le hook
d'op1r (`01e010f0`), ce qui chaîne les deux.

```sh
tools/build_bfin_toolchain.sh                      # binutils + gcc bfin-elf (~30 min, une fois)
export PATH=$PWD/work/bfin/bin:$PATH
examples/midi-panic/build.sh op1_246.op1 op1_246-panic.op1          # sur le firmware d'origine
examples/midi-panic/build.sh op1_246.op1 op1r-panic.op1 --op1r      # par-dessus op1r
```

Vérifié hors matériel :

- l'image se dépaquette ;
- les 802/803 blocs LDR ont des checksums valides ;
- l'argument du premier bloc est cohérent ;
- les seuls octets changés sont l'en-tête, le `CALL` et le texte de démarrage (« R246 pnc » / « op1r5pnc ») ;
- le bloc ajouté est à `01e80000`.

`fwbuild.py --op1r` sans autre option redonne exactement l'image de référence op1r (`d4daaea0…`), et
`fwbuild.py` sans option redonne un contenu identique à l'original : la mécanique d'insertion est celle d'op1r,
qui est testée sur matériel. **L'exemple lui-même n'a pas été flashé.**

### Contraintes

- **Où charger** : `01dfff34-01f00000` seulement.
  - op1r 12 occupe `01e00000-01e2a1d0` (`fwcheck.py refs` sur son code) ; d'autres patchs construits sur op1r
    peuvent occuper davantage (op1_249 : `01e40000-01eb7464`).
  - Le code SDRAM d'origine ne référence jamais `01e0xxxx-01ffxxxx`.
  - Carte détaillée dans [developpement.md](developpement.md) §6.2.
- **Portée des branchements** : `CALL`/`JUMP.L` pcrel24 = ±16 Mo, suffisant de `01e8xxxx` vers tout le code
  `018a-01aa`. Pour la L1 (`ffa0xxxx`), il faut un appel indirect `CALL (P1)`.
- **Contexte d'exécution** : clavier = fil KeyHandler ; séquenceur = fil séquenceur ; audio = interruption
  IVG11, 64 trames (1,45 ms) par bloc, en grande partie dans la L1 illisible. Un rendu trop lent fait décrocher
  l'audio.
- **Taille** : l'OP-1 accepte moins de 15 Mo d'image (op1r : 12,8 Mo).
- **Retour arrière** : le bootloader n'est jamais modifié ; l'`op1_246.op1` d'origine se reflashe toujours.
- **Pas de symboles** : nommer une fonction suppose de la retrouver par ses chaînes, ses appelants et ses tables
  (`funcs.json`, `fwtool.py ptr`).

### Pistes

| Idée | Point d'entrée | Difficulté |
|---|---|---|
| Activer le moteur caché **`ls`** (id 10) | ligne `synth_types` + `case` 10 de ScreenId synth (aujourd'hui une assertion) vers un écran générique ; comprendre d'abord `019d3370-019d5800` (objet unique, pas 6 voix) | moyenne, à risque |
| Mode **euclidien du séquenceur endless** | champs `euclidean` / `euclidean_offset` déjà présents dans les données endless (`018d59e0`) : chercher qui les lit dans `EndlessPlayer` | étude |
| **Commandes SysEx TE** 02/03/04 | parseur `019e31a8` ; ajouter une commande (ex. charger un preset, régler un paramètre à distance) au même endroit qu'op1r (`019e3540`) | moyenne |
| **LFO → MIDI CC** (envoyer la modulation vers l'extérieur) | `process` d'un LFO (vtable) + `019e3004(cc, v)` à débit limité | moyenne |
| Nouveau moteur / FX / LFO / séquenceur | recette de la section 10 ; faits : séquenceur [`examples/pianoroll`](../examples/pianoroll), effet [`examples/drive`](../examples/drive) | élevée |
| Presets pour les moteurs d'op1r | lignes `synth_presets` (dossier = nom du moteur), sans code ; ou presets par moteur dans Shift + T1 (constructeur `01a02444`, 8.4) | faible / moyenne |
| Raccourcis Shift supplémentaires | `key_hook()` de l'exemple + table des codes (section 8.6) | faible |

## 12. Outils

Tous en Python 3 standard, sauf le désassembleur et le compilateur (GNU, construits par
`tools/build_bfin_toolchain.sh`).

| Commande | Rôle |
|---|---|
| `tools/analyze.sh op1_246.op1 [work]` | tout en une fois : dépaquetage, application d'op1r à part, mémoire, chaînes, désassemblage (`sdram.dis`, `l1code.dis`, `op1r_block.dis`), carte des fonctions (`funcs.json`), liste des hooks (`hooks.txt`) |
| `tools/fwtool.py unpack / apply` | `.op1` → fichiers ; applique le manifeste op1r sans recompresser |
| `tools/fwtool.py blocks / mem` | liste des blocs LDR ; image mémoire (`regions.json` + `mem_*.bin`) |
| `tools/fwtool.py addr / offset` | offset du fichier LDR ↔ adresse de chargement |
| `tools/fwtool.py strings / rd / ptr / dump` | chaînes avec adresse ; dump de mots ; recherche de pointeurs (vtables, tables, références) ; extraction d'une plage |
| `tools/fwtool.py branch` | cible du `CALL`/`JUMP.L` à une adresse |
| `tools/fwtool.py xref` | carte heuristique des fonctions : prologues + cibles d'appel, chaînes référencées, fichier source d'après les assertions, appelants |
| `tools/fwtool.py hooks` | toutes les modifications d'op1r décodées (ancienne / nouvelle cible) |
| `tools/fwbuild.py` | construit un `.op1` : `--op1r` ou `--base IMAGE`, `--blob F@ADDR`, `--call`, `--jump`, `--word`, `--bytes`, `--text`, `--file`, `--sql` |
| `tools/fwcheck.py diff / calls / refs / blockhash` | vérifie une image avant de la flasher : ce qu'elle change par rapport à une référence (fichiers, structure LDR, mémoire, bases) ; appels sortants et adresses utilisées par un bloc de code ; empreinte d'un bloc (voir [developpement.md](developpement.md) §5) |
| `tools/build_bfin_toolchain.sh [--binutils-only]` | binutils 2.42 + GCC 14.2 pour `bfin-elf` |

Pour lire le désassemblage : objdump affiche en commentaire la valeur complète d'un registre chargé en deux
moitiés (`P1.H = 0x1ab; /* P1=0x1ab31b4 */`) ; c'est fiable quand `.L` et `.H` se suivent. La chaîne d'une
assertion donne le fichier et la ligne ; `funcs.json` regroupe tout cela par fonction. Le désassemblage est
linéaire : dans les zones de données il produit des `ILLEGAL`, c'est normal.
