# YmtLimitAdjuster — GTA V Legacy

## Fonctionnement

YmtLimitAdjuster est un plugin `.asi` pour le mode Histoire de GTA V Legacy. Il permet de dépasser le budget habituel de dépendances de streaming des personnages en maintenant leurs métadonnées YMT en mémoire. Il fonctionne automatiquement, sans fichier INI, et modifie uniquement le jeu en mémoire.

Pour charger un personnage, GTA V établit une liste des ressources dont il dépend : définitions de vêtements, métadonnées de créatures, dictionnaires de composants et autres ressources. Les vêtements du jeu et des DLC occupent déjà une partie de cette liste. Ajouter des packs peut dépasser la capacité des tableaux utilisés par le moteur et laisser certaines ressources nécessaires hors de la liste.

Le plugin intervient en quatre étapes :

1. **Repérer les fonctions du moteur.** Au démarrage, il recherche des signatures dans le code de GTA V pour retrouver les fonctions de gestion des métadonnées, des dépendances et de fermeture de session. Plusieurs occurrences sont acceptées lorsqu'elles pointent vers la même cible vérifiée. Si les adresses ou les accès mémoire nécessaires ne peuvent pas être validés, le correctif ne s'active pas.
2. **Conserver les YMT chargés.** Lorsqu'une métadonnée est associée à un personnage, le plugin suit son entrée de streaming et demande son chargement si nécessaire. Une fois la ressource chargée, il prend sa propre référence avec `AddRef` pour la maintenir en mémoire indépendamment de la petite liste de dépendances du personnage.
3. **Faire tenir les dépendances dans le tableau original.** Le plugin intercepte `GetDependencies` et récupère une liste plus grande dans son propre tableau. Si elle tient dans le tableau du demandeur, elle est transmise telle quelle. Sinon, le plugin retire uniquement les entrées dont il a vérifié qu'elles sont déjà chargées et conservées par ses propres références. Les autres entrées gardent leur ordre. Les ressources retirées de la liste restent disponibles en mémoire.
4. **Libérer les références à la fermeture de session.** Le plugin attend la fin de ses opérations en cours, libère ses références avant la remise à zéro du moteur et nettoie son suivi. Il vérifie l'identité des ressources pour éviter de confondre un ancien index avec une nouvelle entrée.

Par exemple, si un personnage possède **110 dépendances**, que le tableau du demandeur peut en recevoir **100** et que **20** sont déjà chargées et maintenues par le plugin, celui-ci peut renvoyer les **90 restantes**. Le moteur conserve son tableau d'origine, tandis que le plugin assure la présence des 20 ressources omises.

L'extension prend en charge **jusqu'à 256 dépendances totales par modèle**. Une entrée supplémentaire sert à détecter le dépassement de ce plafond. Il ne s'agit pas de 256 fichiers YMT additionnels : les ressources du jeu et des DLC comptent aussi. Les limites propres aux drawables, props, textures, pools et formats de fichiers restent distinctes. Le maintien des métadonnées peut également augmenter l'utilisation mémoire.

Le plugin ne supprime jamais volontairement une dépendance encore non chargée pour faire tenir la liste. Si le plafond est dépassé ou si trop de dépendances indispensables restent à transmettre, il affiche un diagnostic puis ferme GTA V. Son activité et les éventuels refus sont consignés dans `YmtLimitAdjuster.log`.

## Sources et crédits

- **DaniGP17 — recherches sur la limite YMT :** [PR FiveM #3444](https://github.com/citizenfx/fivem/pull/3444), première tentative d'agrandissement des tableaux, et [PR #4165](https://github.com/citizenfx/fivem/pull/4165), approche par maintien des métadonnées en mémoire qui a inspiré ce plugin.
- **Cfx.re / CitizenFX — références techniques du moteur :** [Streaming.cpp](https://github.com/citizenfx/fivem/blob/master/code/components/gta-streaming-five/src/Streaming.cpp), [Streaming.h](https://github.com/citizenfx/fivem/blob/master/code/components/gta-streaming-five/include/Streaming.h), [LoadOptimizations.cpp](https://github.com/citizenfx/fivem/blob/master/code/components/gta-streaming-five/src/LoadOptimizations.cpp) et [BlockLoadSetters.cpp](https://github.com/citizenfx/fivem/blob/master/code/components/gta-core-five/src/BlockLoadSetters.cpp), utilisés pour comprendre les signatures, les appels de streaming, les références et le cycle des sessions.
- **DurtyFree / Durty Cloth Tool — documentation des limites de vêtements :** [Game Limits and Crashes](https://docs.gta.clothing/game-mechanics/game-limits-and-crashes).
- **Tsuda Kageyu et les contributeurs de MinHook — bibliothèque de hooks :** [MinHook 1.3.4](https://github.com/TsudaKageyu/minhook/tree/v1.3.4), intégrée au plugin pour intercepter les fonctions du jeu. Le désassembleur HDE inclus est crédité à **Vyacheslav Patkov**.
- **Alexander Blade — environnement de chargement ASI :** [Script Hook V et son ASI Loader](https://www.dev-c.com/gtav/scripthookv/). Le plugin est compatible avec ce chargeur, sans appeler les natives Script Hook V.
- **Microsoft — gestion de la mémoire Windows :** [Memory Protection Constants](https://learn.microsoft.com/en-us/windows/win32/memory/memory-protection-constants) et [VirtualQuery](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualquery).

Le plugin est une implémentation indépendante ; aucun runtime FiveM n'est intégré. Les attributions et licences des composants embarqués sont détaillées dans [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
