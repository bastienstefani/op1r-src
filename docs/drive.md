# Effet de distorsion « drive »

Entrée « drive » dans la liste des effets (synthé, batterie, master). Le fonctionnement s'inspire du module
Distortion de Serum (modes de waveshaping, drive, filtre avant ou après, mix). L'écran reprend son affichage (courbe
de transfert, onde déformée) dans le langage visuel de l'OP-1 : fond noir, traits fins, une couleur par encodeur.

Code : [`synth_sdk/plugins/drive/`](../synth_sdk/plugins/drive), un plugin comme tape, clouds et reverb
(`order` 4, sélecteur `0x5304`). Sur PC :

```sh
cd synth_sdk
python3 harness/audio.py drive                  # -> harness/out/audio_drive.wav
python3 harness/render.py drive                 # -> display/drive_preview.png
cc -O1 -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover -DOP1_HOST -I. harness/drive_test.c -lm \
   -o harness/build/drive_test && harness/build/drive_test
```

## État

| Fonction | État |
|---|---|
| Entrée, écran, son | **flashé** avec l'image binaire (exemple sur op1r 12, puis sur op1_249) : premiers retours bons |
| Détail des modes, page 2, sauvegarde avec le son, master, batterie | à confirmer un par un sur matériel |
| Modulation par LFO, course des encodeurs d'origine (drive, fréquence, mix) | non vérifiées |
| Portage en plugin (ce dépôt, op1r 27) | testé sur PC (`drive_test.c`, `audio.py`, `render.py`), construit ; **pas encore flashé** |

Les modes sont nos propres formules, nommées d'après ceux de Serum et réglées pour s'en approcher à l'oreille.

## Commandes

| | Page 1 | Page 2 |
|---|---|---|
| **Bleu** | drive | routage du filtre : OFF / PRE / POST |
| **Vert** | mode (un cran = un mode) | type de filtre : LP / BP / HP |
| **Blanc** | fréquence du filtre (30 Hz – 10 kHz) | résonance |
| **Rouge** | mix (sec / traité) | niveau de sortie (−24 à +6 dB) |

- **Appui sur un encodeur** : passe de la page 1 à la page 2 et inversement (deux points sous la courbe). La page
  est propre à chaque position (synthé, batterie, master).
- 13 modes : TUBE, SOFT CLIP, HARD CLIP, DIODE 1, DIODE 2, LIN FOLD, SIN FOLD, ZERO-SQUARE, DOWNSAMPLE, ASYM,
  RECTIFY, X-SHAPER, X-SHAPER ASYM. En DOWNSAMPLE le drive règle la réduction (1 à 64 échantillons). En RECTIFY il
  va du signal propre (0 %) au redressement simple alternance (50 %) puis double alternance (100 %). Les deux FOLD
  vont jusqu'à 10× ; les autres modes jusqu'à 40× (32 dB).
- Chaîne : entrée → filtre PRE → drive → mode → filtre POST → mix → niveau. Un passe-haut à 35 Hz recentre les modes
  asymétriques.
- Tous les réglages sont écrits dans le son : ils sont sauvés avec lui et voyagent avec les presets.

## L'écran

- En haut : le nom du mode (vert) et l'état du filtre (« PRE LP », « FILTER OFF »…).
- Au centre : la courbe de transfert du mode au drive courant, mix compris. Le point blanc montre où se trouve la
  crête du signal réel sur la courbe.
- À gauche l'onde d'entrée, à droite la même après la courbe.
- En bas : les quatre réglages de la page, aux couleurs des encodeurs, avec une barre chacun.

## Comment c'est branché

C'est un plugin d'effet (voir [`synth_sdk/PLUGINS.md`](../synth_sdk/PLUGINS.md)) qui utilise les trois
fonctions facultatives ajoutées à `op1_fx_t` pour lui :

| Membre | Rôle pour drive |
|---|---|
| `render` | synthé et batterie, 64 échantillons mono |
| `render_stereo` | master : gauche et droite traités chacun avec leur état (au lieu de la somme mono des autres effets) |
| `key` | tout appui d'encodeur bascule la page des quatre encodeurs |
| `knob` | vert en page 1 (un mode par cran) et toute la page 2 ; drive, fréquence et mix restent aux encodeurs d'origine |

Mots du son (8 × int16) :

| Mot | Rôle | Plage stockée |
|---|---|---|
| 0 – 3 | drive, mode (13 zones), fréquence, mix | bornes de l'effet hôte filter (`op1_fx_param_q15`) |
| 4 | sélecteur `0x5304` | jamais modifié |
| 5 | routage × type de filtre (9 zones) | 0..32767 |
| 6 | résonance | 0..32767 |
| 7 | niveau | 0..32767 |

Défauts (`plugin.json`) : drive 30 %, TUBE, 550 Hz, mix 100 %, filtre OFF, résonance 20 %, 0 dB : la même ligne
`fx_types` que l'image binaire (id 14), donc les sons déjà réglés avec drive gardent leurs réglages.

Écart aux règles des plugins : les modes TUBE, SOFT CLIP et DIODE font une division 32 bits par échantillon
(`tanh` de Padé, `x / (1 + |x|)`). Le moniteur de ressources (horloge + T1) montre la charge.

## Revenir à un firmware sans drive

Un son qui utilise drive garde `"fx_type": "drive"`. Un firmware qui ne connaît pas drive s'arrête **au démarrage**
sur `fx_types.cpp:42 unknown fx name`. Avant de flasher une autre image, choisir un autre effet sur chaque son
(slots synthé et batterie) qui utilise drive. Le master n'est pas concerné (il est enregistré comme « filter »).
