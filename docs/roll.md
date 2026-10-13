# Séquenceur piano roll « roll »

Entrée « roll » dans la liste des séquenceurs. Chaque note a sa position, sa durée, sa hauteur et sa vélocité ; la
boucle a une longueur réglable. Le roll joue en polyphonie, se sauve et démarre calé sur le temps. Un motif par
mode (synthé, batterie).

Code : [`synth_sdk/seq/roll.c`](../synth_sdk/seq/roll.c). Test sur PC :
[`synth_sdk/harness/roll_test.c`](../synth_sdk/harness/roll_test.c), aperçu de l'écran :
`python3 synth_sdk/harness/render_roll.py` (→ `synth_sdk/display/roll_preview.png`).

## État

| Fonction | État |
|---|---|
| Entrée, écran grille + tête de lecture, édition, lecture polyphonique, sauvegarde, HOLD, départ calé | **testé sur matériel** avec l'image binaire (exemple sur op1r 12) |
| Portage dans les sources (ce dépôt, op1r 27) | testé sur PC (`roll_test.c`), construit ; **pas encore flashé** |

Le portage garde le même comportement, avec deux corrections (voir plus bas) : l'octave du synthé est lue
correctement pour tous les slots, et une note sans voix de suivi libre n'est pas jouée au lieu de rester bloquée.

## Commandes

Sur l'écran roll (séquenceur roll sélectionné) :

| | Rotation | Appui |
|---|---|---|
| **Bleu** | déplace le curseur dans le temps (par pas de grille) | change la grille (1/4 → 1/8 → 1/16 → 1/32) |
| **Vert** | fait défiler les hauteurs affichées | efface les notes sous le curseur |
| **Blanc** | durée des nouvelles notes | change la vélocité (127 / 100 / 64) |
| **Rouge** | longueur de la boucle | **lecture / arrêt** |

- **Touche du clavier** : pose une note de cette hauteur au curseur, avec la durée et la vélocité courantes, puis
  avance le curseur de cette durée (saisie pas à pas). La note sonne en même temps.
- **Shift + touche** : efface la note de cette hauteur sous le curseur (sans la jouer).
- **Octave** : les flèches gauche / droite décalent la hauteur posée, comme d'habitude. En batterie, la touche
  choisit le son (joué à l'octave de batterie courante).
- **HOLD**, l'encadré en haut à droite : rouge atténué à l'arrêt, rouge vif en lecture ou armé. Un arrêt externe
  (séquenceur coupé, changement de séquenceur ou de mode) remet le roll sur HOLD. La lecture repart toujours du
  début de la boucle. L'état lecture / HOLD n'est pas sauvé.
- **Départ calé sur le temps** : l'appui rouge arme la lecture ; le roll démarre sur le prochain temps (noire) de
  l'horloge du séquenceur. Le temps 1 est fixé par la dernière remise à zéro de cette horloge : PLAY de la bande,
  Start MIDI ou départ d'un séquenceur d'origine. L'alignement sur le clic du métronome n'est garanti qu'après un
  Start MIDI (le métronome est dans la partie audio encodée).
- **En-tête** : grille, durée des nouvelles notes (fraction de ronde), longueur de la boucle en pas de grille.

Le roll démarre vide sur une boucle d'une mesure de 16 pas. Au plus 128 notes par mode et 16 notes jouées en même
temps (au-delà, les notes ne sont pas jouées).

## Comment c'est branché

Comme euclid et dots sur finger, roll est greffé sur l'arpeggio d'origine (type 5, écran `0x2c`) :

| Site | Rôle |
|---|---|
| `SeqIds::from_name` (3 `CALL`, `dispatcher.c` `eu_seq_from_name`) | « roll » → type 5 et `rl_active[mode]` |
| `SeqIds::name` (`JUMP.L` `01a422ec`, `eu_seq_name`) | type 5 avec le drapeau → « roll » |
| `CALL 01a46400` en `01a0d1b0` (`create_player`, cas arpeggio) | `rl_player_get` : le lecteur roll si le drapeau est mis, sinon l'arpeggio |
| `JUMP.L` en `01a5e7d0` (remise à zéro de l'horloge séquenceur) | `rl_clock_reset` (`hook.S`) : les trois écritures d'origine + `rl_reset_pending` |
| construction / chargement des écrans (`dispatcher.c`) | objet écran roll échangé dans l'emplacement `0x2c` (`eu_sync_screen`) |
| `key_event_hook` / `knob_event_hook` | appuis et rotations sur l'écran roll |
| `op1_factory.db` | `seq_types` « roll » (id 9 : après euclid 7 et dots 8) |

**Lecteur** : vtable de 7 méthodes comme celle d'euclid (`seq/euclid.c`). `tick(self, compteur)` est appelé 96 fois
par noire avec le compteur de l'horloge modulo 48 ; `rl_on_beat` compte ses tours pour trouver les temps.

**Sauvegarde** : table `rollseq (is_synth INTEGER PRIMARY KEY, data TEXT)` d'`op1.db`, une ligne par mode. La clé
vaut **0 pour le synthé et 1 pour la batterie** (le nom de la colonne est trompeur ; c'est le format de l'image
binaire, gardé pour retrouver les motifs déjà enregistrés). `data` = `boucle,grille,vélocité,haut_de_vue,durée`
puis `début,durée,hauteur,vélocité` par note, en décimal. Écrite en tâche de fond par `bg_saver`, comme euclid.

## Corrections par rapport à l'image binaire

- L'octave du synthé était lue comme un octet par slot à `01dfe4e0` ; c'est un `int` par slot (comme le lit
  euclid). Seul le premier slot était sûr de donner la bonne octave.
- Au-delà de 16 notes simultanées, les notes en trop étaient jouées sans être suivies, donc jamais relâchées.
  Elles ne sont plus jouées.

## Limites connues

- Changer l'octave pendant la lecture peut laisser une note bloquée (comme euclid).
- Au démarrage, le séquenceur sauvé de chaque mode passe par `eu_seq_from_name` avec le mode courant (même
  mécanisme qu'euclid et dots) : à vérifier sur matériel avec roll en batterie seulement.

## Revenir à un firmware sans roll

Quand « roll » est le séquenceur choisi, son nom est enregistré dans `op1.db`. Un firmware qui ne connaît pas
« roll » (op1r officiel, ou l'original) s'arrête alors **au démarrage** sur l'assertion
`SeqIds.cpp:41 unknown sequencer name: 'roll'`. Avant de flasher une autre image :

1. choisir un autre séquenceur (pattern, finger, euclid…) **en mode synth et en mode drum** ;
2. puis flasher l'autre image.

Si c'est trop tard, reflasher une image avec roll et faire l'étape 1 : le mode mise à jour du bootloader ne dépend
pas du système. En dernier recours, le « factory reset » du bootloader remet `op1.db` à zéro (données utilisateur
effacées).
