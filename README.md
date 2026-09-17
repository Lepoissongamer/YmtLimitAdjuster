# YmtLimitAdjuster — GTA V Legacy (.asi)

**Prototype expérimental pour le mode Histoire de GTA V Legacy, Windows x64.**
Il contourne le budget de dépendances de streaming des personnages pour permettre
davantage de packs de vêtements YMT. Le binaire compilé est
`dist/YmtLimitAdjuster.asi` ; le paquet d'installation est
`dist/YmtLimitAdjuster-0.1.2-Legacy-x64.zip`.

**La version 0.1.2 reste à valider dans GTA V.** Les essais des versions 0.1.0 et
0.1.1 sur Legacy 1.0.3586.0 ont refusé l'activation, sans installer de hooks.
Le second journal confirme que les références répétées convergent ; le blocage
restant concerne la validation mémoire du gestionnaire de streaming. La 0.1.2
corrige les exigences de permissions et ajoute les détails nécessaires pour
identifier un éventuel refus restant. Aucun INI n'est nécessaire.

## Fonctionnement et limites

Le plugin demande jusqu'à **256 dépendances au total par modèle**, conserve
les métadonnées YMT en mémoire, puis retire du petit tableau du moteur celles
qui sont déjà chargées et maintenues résidentes. Les autres dépendances gardent
leur ordre. Le tableau original du jeu n'est pas agrandi.

256 n'est **pas** le nombre de YMT supplémentaires : les dépendances du jeu et des
DLC utilisent aussi ce budget. Les limites des drawables, props, textures, pools
et formats de fichiers restent distinctes. Ce n'est donc pas un mode « illimité ».
La rétention des métadonnées augmente l'utilisation mémoire.

Une sonde de 257 entrées permet de détecter le dépassement de 256. Une dépendance
non chargée ne sera jamais volontairement supprimée pour faire tenir le résultat.
Si cela reste impossible, une boîte de dialogue explique le problème et **GTA V
est fermé**. La progression non sauvegardée est alors perdue. Réduire les packs
concernés avant de relancer.

## Compatibilité

| Cible | État |
| --- | --- |
| Legacy à partir de b1604, exécutable `GTA5.exe` | Candidats : activation seulement si toutes les signatures et vérifications correspondent |
| Legacy avant b1604 | Refusé ; aucune base de signatures vérifiée disponible ici |
| Enhanced, FiveM, RedM, autre exécutable | Refusé |
| Legacy 1.0.3586.0 | Activation refusée en 0.1.0 et 0.1.1 ; validation mémoire corrigée en 0.1.2, à retester en jeu |
| Versions validées en jeu avec **ce** binaire | Aucune pour l'instant |

La recherche par signatures évite des adresses absolues spécifiques à un build.
Elle ne garantit pas toutes les versions, ni les futures mises à jour. Le slot
virtuel de `GetDependencies` est lu dans le code du jeu (il change avec b2802).
Les cibles doivent être uniques et avoir les propriétés mémoire attendues.
Le gestionnaire doit être lisible et modifiable ; la table virtuelle doit être
lisible. Des permissions supplémentaires ne constituent pas à elles seules un
motif de refus. Les limites de l'image et des sections, l'alignement, l'appartenance
mémoire à l'exécutable et les cibles de fonctions restent vérifiés.
Plusieurs références vers le même gestionnaire ou la même fonction sont acceptées
si chaque occurrence du motif donne une cible valide et identique. Une référence
invalide ou des cibles distinctes entraînent un refus.
Un échec initial laisse le jeu sans correctif. Les hooks sont installés ensemble
après validation ; un échec d'activation partielle ferme le processus.

## Installation

1. Installer un ASI Loader compatible Legacy, par exemple celui distribué avec
   [Script Hook V](https://www.dev-c.com/gtav/scripthookv/). Avec ce dernier,
   placer `dinput8.dll` et la version appropriée de `ScriptHookV.dll` à côté de
   `GTA5.exe`.
2. Copier uniquement `YmtLimitAdjuster.asi` du dossier `dist/` à côté de
   `GTA5.exe`. Aucun INI n'est nécessaire ; un ancien INI peut être supprimé,
   car il est ignoré.
3. Lancer le mode Histoire, puis consulter `YmtLimitAdjuster.log`, créé à côté
   du plugin, ou dans `%TEMP%` si ce dossier n'est pas accessible en écriture.
   `ACTIVE` indique que les hooks sont installés ; cela ne constitue pas une
   validation de tous les vêtements.

C'est un ASI de modification mémoire compatible avec le chargeur fourni avec
Script Hook V ; il n'appelle pas les natives Script Hook V et n'a pas besoin du
SDK pour compiler. Installer au démarrage, sans injection ni rechargement à chaud.
Les dépendances tierces utilisées pour compiler sont incluses dans les sources.
Le chargeur ASI et ScriptHookV ne sont pas redistribués.

Pour désinstaller, fermer GTA V et retirer le `.asi`. Le jeu n'est jamais
modifié sur disque. Utilisation prévue uniquement en mode Histoire.

## Diagnostic

Le plugin tente automatiquement de s'activer au démarrage. Il journalise le
premier dépassement du budget original par modèle et par session : total,
capacité du demandeur et entrées résidentes retirées. Il n'y a aucune option
à configurer.

`UNSUPPORTED` signifie que le correctif n'a pas été installé. Le journal précise
la signature absente, ambiguë ou incompatible. La 0.1.2 détaille aussi les
caractéristiques de section, l'état et les permissions actuelles des pages ainsi
que leur type pour les cibles examinées. Les autres vérifications indépendantes
continuent après un premier refus pour rendre le diagnostic complet.
Si le journal principal ne peut pas être ouvert, chercher
`%TEMP%\YmtLimitAdjuster.log`. Une alerte signale l'impossibilité d'écrire aussi à
cet emplacement.

Pour diagnostiquer un problème, fournir `YmtLimitAdjuster.log`, le build, les
autres ASI installés et les packs utilisés. `asiloader.log` confirme seulement le
chargement du module, pas l'installation de ses hooks.

Sur 3586, le journal de la 0.1.1 confirme les résultats suivants :

- Les 10 références `StreamingManager` pointent toutes vers `GTA5.exe+0x2F86270`,
  mais cette cible échoue au contrôle mémoire.
- Les 2 références `RequestObject` pointent vers `GTA5.exe+0x168A0F4` et sont
  acceptées.
- La signature affinée de `ShutdownSession` apparaît une fois et sa cible
  `GTA5.exe+0x27138` est acceptée.

Le contrôle précédent refusait notamment des pages à la fois modifiables et
exécutables, bien qu'elles autorisent la lecture et l'écriture nécessaires.
Les permissions de la cible rejetée n'étant pas consignées en 0.1.1, ce journal
ne permet pas de confirmer que cette condition précise explique le refus sur
cette installation. La 0.1.2 corrige cette exigence excessive et journalise les
conditions effectivement rencontrées. Ces adresses sont des observations du
journal ; elles ne sont pas codées en dur dans le plugin.

Script Hook V a également signalé un plantage pendant l'exécution
d'`InteriorsV.asi` à `GTA5.exe+0x038F316D`. Sa cause n'est pas établie par ces
journaux.

Une fermeture contrôlée est possible si le chargeur installe le plugin trop tard,
si une autre extension modifie le streaming, si les ressources ne sont pas encore
chargées ou si le plafond est dépassé. Le plugin ne force pas un chargement
synchrone depuis `GetDependencies`, opération susceptible de bloquer le streaming.
Les identités des entrées sont suivies par table, index et handle et nettoyées à
la fermeture de session. Cette gestion reste à valider dans GTA V.

## Compiler

### Windows : Visual Studio 2022, outils C++ x64, CMake

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
cmake --install build --config Release --prefix dist
```

### Linux : llvm-mingw

Télécharger un compilateur depuis les
[releases officielles llvm-mingw](https://github.com/mstorsjo/llvm-mingw/releases),
puis définir son chemin :

```sh
export LLVM_MINGW_ROOT=/chemin/vers/llvm-mingw
cmake -S . -B build-win -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=cmake/llvm-mingw.cmake \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build build-win
cmake --install build-win --prefix dist
```

Les tests indépendants du jeu fonctionnent aussi sous Linux :

```sh
cmake -S . -B build-tests -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-tests
ctest --test-dir build-tests --output-on-failure
```

La CI Windows compile le `.asi`, exécute les tests et fournit un artefact
d'installation. Aucun téléchargement de dépendance n'est requis par CMake.

## Validation à réaliser dans le jeu

Commencer avec un ensemble de vêtements connu fonctionnel, puis augmenter
progressivement les packs. Vérifier les changements répétés
homme → femme → homme, vêtements/props/talons, réapparition du personnage,
chargement d'une sauvegarde et retour au menu. Le journal doit rapporter une
extension effective lorsque le budget original est dépassé. Pour comparer,
fermer le jeu, retirer temporairement ce `.asi`, puis relancer avec les mêmes
packs et le même build.

Les tests automatisés vérifient les permissions requises, y compris les pages
modifiables et exécutables, et le refus des pages inaccessibles ou protégées par
une garde. Ils vérifient aussi plusieurs références vers une seule cible,
le refus de cibles divergentes et les déplacements d'adresses signés. Ils vérifient
le filtrage, l'absence d'écriture hors du tableau,
la conservation de l'ordre, les bornes 256/257, les dépendances non résidentes,
les cas null/zero du filtre et le parseur de signatures. Ils n'exécutent pas
le moteur GTA V, son ABI, ses hooks ou son chargement de YMT.

## Sources et crédits

- [PR FiveM #3444](https://github.com/citizenfx/fivem/pull/3444), DaniGP17 :
  première tentative d'agrandissement des tableaux, abandonnée.
- [PR FiveM #4165](https://github.com/citizenfx/fivem/pull/4165), DaniGP17 :
  approche par rétention des métadonnées ; référence de conception du portage.
  Ses tests FiveM ne sont pas des tests de cet ASI.
- [Durty Cloth Tool — Game Limits and Crashes](https://docs.gta.clothing/game-mechanics/game-limits-and-crashes) :
  contexte des différentes limites.
- [Streaming.cpp](https://github.com/citizenfx/fivem/blob/master/code/components/gta-streaming-five/src/Streaming.cpp),
  [Streaming.h](https://github.com/citizenfx/fivem/blob/master/code/components/gta-streaming-five/include/Streaming.h),
  [BlockLoadSetters.cpp](https://github.com/citizenfx/fivem/blob/master/code/components/gta-core-five/src/BlockLoadSetters.cpp) :
  signatures et conventions d'appel documentées par Cfx.re.
- [Microsoft — Memory Protection Constants](https://learn.microsoft.com/en-us/windows/win32/memory/memory-protection-constants),
  [VirtualQuery](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualquery) :
  permissions effectives des pages et inspection des régions mémoire.

Voir [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) pour les composants intégrés.
Les choix de références, de hooks et les limites de validation sont détaillés
dans [docs/IMPLEMENTATION.md](docs/IMPLEMENTATION.md).
