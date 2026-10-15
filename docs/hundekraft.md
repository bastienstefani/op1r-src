# roll et drive par-dessus hundekraft

[hundekraft](https://hundekraft.github.io/op1-workshop/manual.html) est un firmware assemblé par le « workshop » de
Hundekraft. Il contient :
- op1r 30 et Musica Obscura (Mixtur, Arc, Palme, In a room, Oracle, Peal) ;
- la boîte à outils #249 de maiko (annulation de la bande, noms des sons, mode gamme, stockage) ;
- les synthés Wavetable, Taffy, Pew et Elg, les effets Washer et Snowglobe, et le mode accords de Hundekraft.

Les moteurs de Hundekraft ne sont publiés qu'en binaire : on ne peut pas reconstruire hundekraft depuis des sources.
On prend donc **son image comme base**, et on y ajoute roll et drive avec un **overlay** compilé depuis nos sources.

## Construire

```sh
# 1. l'image hundekraft : depuis le workshop (navigateur), ou en ligne de commande avec sa release publiée
git clone https://github.com/Hundekraft/op1-workshop
python3 tools/workshop_build.py op1-workshop 7ff op1_246.op1 hundekraft-2.7.1-7ff.op1   # 7ff = tout coché

# 2. roll + drive par-dessus
cd synth_sdk
python3 make_overlay.py ../hundekraft-2.7.1-7ff.op1 --native     # -> ../hundekraft-2.7.1-7ff+roll+drive.op1
```

- Le texte de démarrage devient `hundekraft 2.7.1` / `feat. op1r 30 + maiko #249 + roll + drive`.
- Les presets de Hundekraft (`presets.zip` du workshop) s'installent comme d'habitude.
- **Ne pas diffuser** l'image obtenue : elle contient le travail de Hundekraft et de maiko.

## État

| | État |
|---|---|
| `workshop_build.py` | redonne l'image du workshop octet pour octet (variante 7ff : sha256 `456eb1a0…`) |
| `make_overlay.py` sur hundekraft 2.7.1 (7ff) | construite et vérifiée hors ligne (voir plus bas) ; **pas encore flashée** |
| `make_overlay.py` sur op1r 27 officiel | construite (contrôle de généralité), pas flashée |

## Comment ça marche

`make_overlay.py` lit l'image de base et regarde ce qu'elle a à chaque site dont roll et drive ont besoin :

| Site | Ce que la base doit avoir | Ce que fait l'overlay |
|---|---|---|
| `CALL` `01a42a2e`, `01a42bdc`, `01a42ce0` (`SeqIds::from_name`) | un `CALL` vers son propre hook | « roll » → type 5 + drapeau, sinon son hook |
| `JUMP.L` `01a422ec` (`SeqIds::name`) | un `JUMP.L` vers son hook | 5 + drapeau → « roll », sinon son hook |
| `CALL` `01a0d1b0` (lecteur de l'arpeggio) | le `CALL` d'origine | lecteur roll ou arpeggio |
| `01a5e7d0` (remise à zéro de l'horloge) | les octets d'origine | `rl_clock_reset` |
| `CALL` `01a3bd58` / `019f07d2` (écrans) | ses hooks | ses écrans, puis les nôtres (roll en `0x2c`, drive en `0x0b`) |
| `CALL` `019c2d3e` / `019c2e94` (touches, encodeurs) | ses hooks (chez hundekraft : maiko, puis op1r) | les pages roll et drive, puis sa chaîne |
| `JUMP.L` `019aad8e` (noms des effets) | son hook | « drive » = « filter », sinon son hook |
| mots `01910ca8/b0/c8/d0` (filter mono / master) | ses fonctions | drive si son sélecteur, sinon ses fonctions |
| mot `018f7ff8` (`ScreenId` fx, cas 5) | son stub `CALL x ; P0 = [FP+4] ; UNLINK ; JUMP (P0)` | appelle `x`, puis met l'écran drive |

Chaque cible trouvée devient un maillon `next_*`, transmis à l'éditeur de liens (`--defsym`). Le code de l'overlay ne
contient que des adresses d'origine R246. L'outil s'arrête si un site n'a pas la forme attendue : par exemple, si une
nouvelle version de la base accroche elle-même l'arpeggio ou l'horloge.

L'overlay est un seul bloc LDR, placé dans le premier trou libre après le bloc d'op1r. Sur hundekraft 2.7.1 :
`01e2c000-01e34520` (34 Ko), sous le module de maiko (`01e40000`).

Base de données : `seq_types` « roll » et `fx_types` « drive » prennent les ids libres suivants (11 et 20 sur
hundekraft 2.7.1).

## Vérifications faites sur l'image produite

- `fwcheck.py diff` contre la base hundekraft :
  - fichiers : seulement `OP1_vdk.ldr`, `op1_factory.db` et les ajouts `roll.svg` et `drive.svg` ;
  - mémoire : les 16 sites, le bloc de l'overlay et le texte de démarrage ;
  - bootloader inchangé.
- Chaque site décodé pointe sur la bonne fonction de l'overlay (`build/overlay/overlay.sym`).
- Le code de l'overlay n'appelle, de la base, que les 12 maillons `next_*`.

## Différences avec la build depuis les sources

- **Sélecteur de drive** : `0x5340` ici, hors des numéros d'op1r, pour ne prendre la place d'aucun effet de la base
  (Washer y a le numéro 4). Dans la build op1r 27 depuis les sources, drive a le numéro 4 (`0x5304`).
  - Un son réglé avec drive dans l'une des deux builds n'est pas reconnu dans l'autre.
  - Sur hundekraft, un ancien son « drive » en `0x5304` (images de test précédentes) s'ouvre sur Washer : rechoisir drive.
- **Retour à un effet de la base** : quand on repasse de drive à l'effet qu'une position avait juste avant, la base ne
  le réinitialise pas (elle ne sait pas que drive a tourné entre-temps). Cet effet repart donc avec son état
  précédent (par exemple une ancienne traîne d'écho).
- **Notes de roll** : elles passent par la même entrée que le clavier. Le mode accords de Hundekraft, s'il est actif,
  peut les transformer en accords. Le mode gamme de maiko agit sur le clavier : à vérifier sur la saisie pas à pas.

## Nouvelle version de hundekraft

Reconstruire la base (workshop ou `workshop_build.py` sur la nouvelle release), puis relancer `make_overlay.py`. Si
un contrôle échoue, la base a changé l'un des sites : il faut regarder lequel et adapter l'overlay.

## Revenir en arrière

Comme pour roll et drive (voir [roll.md](roll.md), [drive.md](drive.md)) : avant de flasher une image sans eux, choisir
un autre séquenceur en synth et en drum, et un autre effet sur chaque son qui utilise drive.
