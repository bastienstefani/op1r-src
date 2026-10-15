# Notes pour reprendre le travail (agents et humains)

Ce dépôt contient les sources d'**op1r** (amont : `pcppcp/op1r-src`, branche `main`) et, par-dessus, nos ajouts :
le séquenceur piano roll **roll** (`synth_sdk/seq/roll.c`) et l'effet de distorsion **drive**
(`synth_sdk/plugins/drive/`). Ils étaient avant des blocs binaires greffés sur l'image d'op1r (dépôt `op1r`,
branche `claude/brave-galileo-8ytbnt`) ; ici ils sont compilés avec op1r, sans adresse d'op1r à suivre.

À lire d'abord :
- [docs/developpement.md](docs/developpement.md) : état, construction, tests, vérifications, points d'accroche,
  identifiants réservés, procédures (nouvelle version amont, nouvelle fonction), pièges ;
- [docs/roll.md](docs/roll.md), [docs/drive.md](docs/drive.md) : mode d'emploi, branchement, état ; au format de
  l'amont (op1r 34), pour une PR vers op1r : drive sur la branche `claude/drive-fx`, roll (synthé seulement) sur
  `claude/roll-seq` ;
- [docs/op1_249.md](docs/op1_249.md) : chargement du module binaire d'op1_249 (maiko) avec `--module` ;
- [docs/hundekraft.md](docs/hundekraft.md) : roll et drive en overlay par-dessus l'image hundekraft (`make_overlay.py`) ;
- [synth_sdk/PLUGINS.md](synth_sdk/PLUGINS.md) : l'API des plugins ;
- [docs/firmware-r246.md](docs/firmware-r246.md) : étude du firmware d'origine.

## Règles

- **Ne jamais commiter de firmware** (TE ou tiers) ni rien qui en sort : `*.op1`, `*.ldr`, `op1_246*`,
  `op1repacker/` sont ignorés. Pour un module tiers (`MODULES`), seuls son sha256 et des adresses ; une image
  construite avec `--module` ne se diffuse pas sans l'accord de son auteur.
- Seules des adresses **d'origine** R246 apparaissent dans le code ; un nouveau site patché va dans `HOOKS` de
  `make_firmware.py` avec ses octets d'origine (vérifiés au build).
- L'`order` d'un plugin est enregistré dans chaque son (sélecteur `0x5300 + order`) : ne jamais le renuméroter
  (drive = 4 dans la build op1r, `0x5340` dans l'overlay, hors des numéros d'op1r). Les noms (« roll », « drive »)
  sont enregistrés aussi : ne pas les changer.
- L'overlay (`overlay/overlay.c`) n'écrit aucune adresse du code de la base : ses maillons `next_*` sont lus dans
  l'image par `make_overlay.py`, qui s'arrête si un site n'a pas la forme attendue.
- Ne pas dire « testé » sans flash sur l'OP-1 ; l'utilisateur teste et fait ses retours.
- `docs/` et ce fichier en français ; code, commentaires, `README.md`, `PLUGINS.md` en anglais.

## Commandes utiles

```sh
toolchain/build_native.sh && export PATH=$PWD/toolchain/bfin/bin:$PATH    # chaîne bfin-elf sans Docker (une fois)
op1repacker/.venv/bin/op1repacker unpack op1_246.op1                         # -> op1_246/ (une fois)
cd synth_sdk && python3 make_firmware.py --opz --native                      # -> ../op1_246_custom-repacked.op1
python3 make_firmware.py --opz --native --module op1_249.op1                 # + module binaire d'op1_249 (docs/op1_249.md)
python3 ../tools/workshop_build.py op1-workshop 7ff op1_246.op1 hk.op1       # image hundekraft complète (docs/hundekraft.md)
python3 make_overlay.py hk.op1 --native                                      # + roll + drive par-dessus
python3 harness/audio.py drive && python3 harness/render.py drive            # drive sur PC
python3 harness/render_roll.py                                               # roll : tests + aperçu
```

Firmware d'origine attendu : `op1_246.op1`, sha256 `c5315218f825f143b415ca554516541898abee843d3a236df0b54c04e1fb13a9`.
