# Guide de développement et de reprise

Ce guide sert à reprendre le travail sur nos ajouts à op1r — le séquenceur **roll** et l'effet **drive** — dans le
dépôt des sources. Il décrit l'état actuel, la façon de construire et de vérifier, où se branchent les ajouts, les
procédures de mise à jour et les pièges rencontrés.

Pages des ajouts : [roll.md](roll.md), [drive.md](drive.md). Étude du firmware d'origine :
[firmware-r246.md](firmware-r246.md). Écrire un plugin : [../synth_sdk/PLUGINS.md](../synth_sdk/PLUGINS.md).

## 1. État des lieux (octobre 2026)

| Élément | Où | État |
|---|---|---|
| op1r (amont) | `main`, `synth_sdk/` | **op1r 27** ; une construction native de `main` redonne l'`OP1_vdk.ldr` publié octet pour octet |
| Effet « drive » | `synth_sdk/plugins/drive/` | plugin ; testé sur PC, construit, **pas encore flashé** dans cette forme |
| Séquenceur « roll » | `synth_sdk/seq/roll.c` + `dispatcher.c`, `hook.S`, `link.ld`, `make_firmware.py` | testé sur PC, construit, **pas encore flashé** dans cette forme |
| Fonctions facultatives des effets | `op1_fx_t` (`op1_plugin.h`) : `render_stereo`, `key`, `knob` | ajoutées pour drive, utilisables par tout effet |
| Construction sans Docker | `make_firmware.py --native`, `toolchain/build_native.sh` | vérifiée (même résultat que l'image Docker) |
| Module d'op1_249 (maiko, binaire) | `make_firmware.py --module op1_249.op1`, [op1_249.md](op1_249.md) | construit et vérifié hors ligne, **pas encore flashé** avec op1r 27 |

Le travail se fait sur la branche `claude/refactor-source-repo-ptx23n`.

**Avant**, roll et drive étaient des blocs binaires ajoutés par-dessus l'image d'op1r 12 (dépôt `op1r`, branche
`claude/brave-galileo-8ytbnt`) : ils appelaient des adresses internes d'op1r, qu'il fallait relever, épingler et
vérifier à chaque version. Ici, ils sont compilés et liés **avec** op1r : l'éditeur de liens résout tout à chaque
build, il n'y a plus d'adresse d'op1r à suivre. Ce qui reste dans cette branche du dépôt `op1r` : les outils
d'étude de l'image (`tools/fwtool.py`, `fwcheck.py`, `analyze.sh`) et l'exemple MIDI panic (jamais flashé). Le
module de l'image tierce op1_249 se charge ici avec `--module` ([op1_249.md](op1_249.md)).

Compatibilité avec les images binaires précédentes : mêmes noms (« roll », « drive »), même ligne `fx_types`
(id 14, sélecteur `0x5304`), même `seq_types` (id 9), même table `rollseq` et même format. Les sons et motifs
enregistrés avec ces images sont repris.

## 2. Prérequis

- **Firmware d'origine** `op1_246.op1` (sha256 `c5315218f825f143b415ca554516541898abee843d3a236df0b54c04e1fb13a9`),
  publié par TE : `curl -LO https://teenage.engineering/_software/op-1/op1_246.op1`.
- **op1repacker** : `python3 -m venv op1repacker/.venv && op1repacker/.venv/bin/pip install op1repacker` (PyPI),
  ou depuis son dépôt comme dans le [README](../README.md). `make_firmware.py` l'attend dans
  `op1repacker/.venv/bin/op1repacker`.
- **Chaîne Blackfin**, au choix :
  - Docker : `docker build -t bfin-elf-toolchain toolchain` ;
  - sans Docker : `toolchain/build_native.sh` (mêmes versions : binutils 2.40, gcc 12.3 ; environ 30 minutes
    sur 4 cœurs), puis `export PATH=$PWD/toolchain/bfin/bin:$PATH` et `--native`.
- **Python 3** ; pour les tests sur PC : un compilateur C, `pycairo` (aperçus d'écran) et `numpy` (quelques tests).

Rien de tout cela n'est commité : `op1_246*`, `op1repacker/`, `*.op1`, `toolchain/bfin/` sont ignorés.

## 3. Construire

```sh
op1repacker/.venv/bin/op1repacker unpack op1_246.op1          # -> op1_246/ (une fois)
cd synth_sdk
python3 make_firmware.py --opz --native                       # ou sans --native avec Docker
# -> ../op1_246_custom-repacked.op1
```

- L'écran de démarrage affiche `op1r <VERSION> <date heure>` ; `--release` enlève la date (c'est ainsi que sont
  construites les releases : `--release --opz` sur `main` redonne l'`OP1_vdk.ldr` d'op1r 27,
  sha256 `e88609cd…`).
- Pour flasher : démarrer l'OP-1 en mode mise à jour du bootloader et copier le `.op1`. Le bootloader n'est jamais
  modifié : toute image se reflashe, l'originale comprise (voir les noms enregistrés en 8).

## 4. Tester sans OP-1

Depuis `synth_sdk/` :

```sh
mkdir -p harness/build
T="cc -O1 -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover -DOP1_HOST -I."
$T harness/roll_test.c -lm -o harness/build/roll_test && harness/build/roll_test
$T harness/drive_test.c -lm -o harness/build/drive_test && harness/build/drive_test     # ~1 min
python3 harness/audio.py drive                 # -> harness/out/audio_drive.wav (à écouter)
python3 harness/render.py drive                # -> display/drive_preview.png
python3 harness/render_roll.py                 # -> display/roll_preview.png (+ relance roll_test)
```

| Test | Ce qu'il vérifie |
|---|---|
| `roll_test.c` | départ calé sur le temps contre une horloge simulée (5 M de ticks, remises à zéro aléatoires, y compris entre la lecture et l'incrément) ; saisie, effacement, lecture, durées, polyphonie, boucle, HOLD ; modes indépendants ; sauvegarde / relecture et lignes invalides |
| `drive_test.c` | DSP : toutes les combinaisons de réglages sous UBSan, filtre stable, chiffres par mode ; fonctions `key` / `knob` / `render_stereo` ; l'écran dessine |
| `audio.py`, `render.py`, `render_roll.py` | le son et l'écran, à écouter et à regarder |

Les autres tests du harnais (`harness/*_test.c`, `render_*.py`) couvrent le reste d'op1r ; la commande de chacun est
en tête du fichier. Les tests compilent le même C que le firmware ; ils ne vérifient pas les adresses du firmware.

## 5. Vérifier une image avant de la flasher

`make_firmware.py` refuse de construire si :
- un site patché ne contient pas les octets d'origine attendus (`HOOKS`, `DATA_PATCHES`, `IMM_PATCHES`, `SLOTS`) ;
- le code chargé dépasse `0x01E40000` ou la BSS `0x01EF0000` ;
- les patches de la L1 chiffrée changent (`l1_patches.json` : seuls les mainteneurs d'op1r peuvent les refaire ;
  nos ajouts n'y touchent pas) ;
- deux plugins ont le même `order`, ou un SVG porte le nom d'un fichier d'origine.

À regarder ensuite :
- la sortie : `code … bytes`, les lignes `fx_types` (drive = 14) ; `seq_types` doit finir par
  `(7, 'euclid'), (8, 'dots'), (9, 'roll')` ;
- la taille de l'image (moins de 15 Mo) ;
- `synth_sdk/build/synth.lst` (désassemblage) pour le code ajouté, par exemple les appels faits dans
  l'interruption audio.

Pour comparer deux images fichier par fichier, `tools/fwcheck.py diff` de la branche `claude/brave-galileo-8ytbnt`
du dépôt `op1r` fonctionne toujours (il ne dépend que des images).

## 6. Architecture

### 6.1 Principe

La recette d'op1r ([firmware-r246.md](firmware-r246.md) §10) : un nouveau type est déclaré dans
`op1_factory.db` puis rendu équivalent à un type d'origine caché ; les points d'entrée de ce type mènent à notre
code. Tout est dans `synth_sdk/` et lié en un seul bloc à `0x01E00000` :

- **drive** est un plugin d'effet (hôte : filter, type 5) : `plugins/drive/plugin.json` suffit à le déclarer,
  `make_firmware.py` génère la table des effets et la ligne `fx_types` ;
- **roll** est un séquenceur du cœur (hôte : arpeggio, type 5), comme euclid et dots sur finger (type 3).

### 6.2 Points d'accroche de roll

| Site | Code |
|---|---|
| `CALL` `01a42a2e`, `01a42bdc`, `01a42ce0` (`SeqIds::from_name`) | `eu_seq_from_name` (`dispatcher.c`) : « roll » → 5 + `rl_active[mode]` |
| `JUMP.L` `01a422ec` (`SeqIds::name`) | `eu_seq_name` : 5 + drapeau → « roll » |
| `CALL` `01a0d1b0` (`create_player`, arpeggio) | `rl_player_get` (`seq/roll.c`) |
| `JUMP.L` `01a5e7d0` (remise à zéro de l'horloge séquenceur) | `rl_clock_reset` (`hook.S`) |
| `CALL` `01a3bd58` / `019f07d2` (écrans) | `screens_ctor_hook` / `screens_load_hook` : objet écran roll dans l'emplacement `0x2c` |
| `CALL` `019c2d3e` / `019c2e94` (touches, encodeurs) | `key_event_hook` / `knob_event_hook` : `roll_page()` |

Les sites partagés (noms de séquenceur, écrans, touches, encodeurs) sont des fonctions d'op1r qu'on complète : il
n'y a plus de chaîne de hooks à maintenir.

### 6.3 Identifiants réservés

| Espace | Origine | op1r | Nos ajouts |
|---|---|---|---|
| `seq_types.id` | 1-6 | 7 euclid, 8 dots | **9 roll** |
| séquenceur hôte (type interne) | 0-5 | euclid et dots sur finger (3) | roll sur arpeggio (**5**) + `rl_active[mode]` |
| `fx_types.id` | 3-10 (+ filter 2 réactivé) | 11 tape, 12 clouds, 13 reverb | **14 drive** |
| `order` des plugins d'effet → sélecteur `0x5300 + order` | – | 1 tape, 2 clouds, 3 reverb | **4 drive** (`0x5304`) |
| écrans (registre `01df4e54`) | – | `0x0b` effets, `0x14` synthés, `0x1c` quad, `0x2a` horloge, `0x2d` euclid / dots, `0x06` / `0x10` éditeur | **`0x2c`** roll ; drive utilise `0x0b` comme les autres effets |
| tables d'`op1.db` | – | `euclidseq`, `dotsseq`, … | **`rollseq`** |

L'`order` d'un plugin est enregistré dans chaque son (sélecteur `0x5300 + order`) : il ne doit **jamais** changer.
Si l'amont publie un quatrième effet, il faudra qu'il prenne un autre numéro, sinon renuméroter le nôtre casserait
les sons réglés avec drive. À surveiller à chaque fusion (7.1).

### 6.4 Mémoire

| Plage | Occupant |
|---|---|
| `01e00000-01e2c3d8` | code + données (op1r 27 + roll + drive : 181 208 octets ; op1r 27 seul : 161 804) ; limite `01e40000` |
| `01e40000-01ec0000` | laissé libre par op1r pour le module d'un autre projet (commentaire de `link.ld`) |
| `01ec0000-01edb7e4` | BSS (bloc FILL) ; limite `01ef0000` |
| `ffa0bfb4-ffa0c000` | trampolines L1 d'op1r |

### 6.5 Contextes d'exécution

| Où | Fil | Règles |
|---|---|---|
| `drive_render`, `drive_render_stereo` | interruption audio, 64 échantillons | pas d'appel au firmware, peu de pile, pas de division par échantillon (exception connue : voir drive.md) |
| `rl_tick` (`rl_ptick`) | fil séquenceur, 96 ticks par noire | court ; notes par `key_down` / `eu_fw_note_off` |
| `rl_screen_draw`, `drive_screen_update` | fil d'affichage | lignes `op1_raster_line`, coordonnées 22.10 |
| `rl_press` / `rl_turn`, `drive_key` / `drive_knob`, `rl_key_down` | fil KeyHandler | rendre la main vite |
| `rl_load` | démarrage (lecteur ou écran, le premier des deux) | le séquenceur sauvé est chargé **avant** les écrans |
| `rl_bg_save` | fil FileWorker (`bg_saver`) | SQL par `op1_db_write` |

## 7. Procédures

### 7.1 Suivre une nouvelle version d'op1r-src

```sh
git remote add upstream https://github.com/pcppcp/op1r-src.git     # une fois
git fetch upstream && git merge upstream/main
```

- Conflits probables : `dispatcher.c` (fonctions communes aux séquenceurs et aux écrans), `make_firmware.py`
  (`HOOKS`, liste des sources, `patch_db`), `hook.S`, `link.ld`, `.gitignore`.
- Vérifier les identifiants (6.3). Les ids SQL sont attribués à la suite par `patch_db` et les sons enregistrent
  les **noms** : un décalage d'id est sans conséquence. En revanche l'`order` d'un plugin est enregistré dans chaque
  son : si l'amont publie un effet d'`order` 4, l'un des deux doit changer et les sons réglés avec lui se perdent.
- Construire (3), lancer les tests (4), contrôler (5), flasher, puis noter la version dans les pages des ajouts.

Plus besoin de relever des adresses d'op1r : seules les adresses **d'origine** R246 (constantes) apparaissent dans
nos sources.

### 7.2 Ajouter une fonctionnalité

- **Un effet ou un moteur** : un plugin (`synth_sdk/PLUGINS.md`). Les fonctions facultatives `render_stereo`,
  `key` et `knob` couvrent la plupart des besoins d'écran et d'encodeurs.
- **Une fonction du cœur** (séquenceur, écran, outil de bande) : un fichier C dans `seq/`, `screens/` ou `tape/`,
  ajouté à la liste `srcs` de `make_firmware.py`, branché dans `dispatcher.c`. Mettre la logique dans des
  fonctions indépendantes du firmware (`#ifndef OP1_HOST` autour de la colle) et l'essayer avec un
  `harness/<nom>_test.c`.
- **Un nouveau point d'accroche** : trouver le site avec l'étude ([firmware-r246.md](firmware-r246.md)) et les
  outils de la branche `claude/brave-galileo-8ytbnt` du dépôt `op1r` ; ajouter une entrée à `HOOKS` (adresse +
  octets d'origine), la section dans `link.ld` et le `call` / `jump.l` dans `hook.S`.
- Prendre les identifiants suivants (6.3) ; rester sous les limites mémoire (6.4).
- Ne dire « testé » qu'après un flash.

## 8. Pièges rencontrés

| Symptôme | Cause | Règle |
|---|---|---|
| Arrêt au démarrage `SeqIds.cpp:41 unknown sequencer name: 'roll'` | image sans roll alors que roll est le séquenceur enregistré | avant de flasher une image sans roll, choisir un autre séquenceur en synth **et** en drum |
| Même chose pour un effet (`fx_types.cpp:42 unknown fx name`) | un son enregistré avec drive et une image sans drive | changer l'effet de chaque son qui utilise drive avant |
| Plantage EXCAUSE 0x21 (lecture à une adresse invalide) | adresse interne d'op1r périmée (exemples binaires) | réglé par le passage aux sources : plus aucune adresse d'op1r en dur |
| Le séquenceur sauvé est actif avant la construction des écrans | ordre de démarrage d'OP_Main | ne pas remettre les drapeaux à zéro à la construction des écrans ; charger à la première demande (`rl_load`) |
| SQL tronqué ou faux | `db2::exec` passe le SQL deux fois par `vsnprintf` et s'arrête sur erreur | pas de `%` ni de `'` dans les données ; longues requêtes par `op1_db_write` |
| Assertion `… fx param out of range` (fatale) | un mot 0-3 de l'effet hôte hors des bornes de filter | `fx_sanitize` (op1r) recadre ; les défauts de `plugin.json` sont des positions 0..32767 converties |
| Le roll démarrait à contretemps | l'horloge passée au lecteur compte modulo 48 (demi-temps) | `rl_on_beat` compte les tours ; `rl_clock_reset` signale chaque remise à zéro |
| Notes bloquées au-delà de 16 notes simultanées (exemple binaire) | notes jouées sans case de suivi | une note sans case libre n'est pas jouée |

## 9. Limites connues et pistes

- **roll** : changer d'octave pendant la lecture peut laisser une note bloquée (comme euclid) ; l'état lecture / HOLD
  n'est pas sauvé ; calage sur le clic du métronome seulement après un Start MIDI ; au démarrage, le séquenceur
  sauvé de chaque mode passe par `eu_seq_from_name` avec le mode **courant** (comme euclid et dots) : à vérifier
  avec roll en batterie seulement.
- **drive** : la modulation par LFO et la course des encodeurs d'origine ne sont pas vérifiées ; en master, la liste
  affiche « filter » après redémarrage (comme tape, clouds, reverb) ; les modes sont nos formules, pas celles de
  Serum ; division par échantillon dans trois modes.
- **op1_249** : son module est chargé en binaire (`--module`, [op1_249.md](op1_249.md)), sans ses sources. Une
  nouvelle version de maiko demande une nouvelle entrée dans `MODULES`, après la même vérification. Les sources
  (que la GPL d'op1r lui demande de fournir) permettraient de le porter comme roll et drive.
- **Pistes** : dossiers de presets pour les moteurs d'op1r (lignes `synth_presets`), presets par moteur dans
  Shift + T1 (constructeur de liste `01a02444`, [firmware-r246.md](firmware-r246.md) §8.4), autres pistes en §11.

## 10. Conventions du dépôt

- **Jamais de firmware TE ni d'image tierce dans git** : `*.op1`, `*.ldr`, `op1_246*`, les arbres dépaquetés sont
  ignorés. Les seuls octets d'origine sont ceux que vérifie `make_firmware.py` et `l1_patches.json` (voir `NOTICE`).
- Langue : ces documents (`docs/`, `CLAUDE.md`) en français ; le code, ses commentaires, `README.md` et
  `PLUGINS.md` en anglais, comme en amont.
- Rien n'est dit « testé » s'il n'a pas été flashé.
