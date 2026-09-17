# YmtLimitAdjuster - GTA V Legacy
YmtLimitAdjuster is a script fully made by ChatGPT.
## How It Works

YmtLimitAdjuster allows GTA V to exceed the usual character streaming dependency budget by keeping their YMT metadata resident in memory.

When loading a character, GTA V builds a list of the resources it depends on: clothing definitions, creature metadata, component dictionaries, and other resources. Base-game and DLC clothing already occupy part of this list. Adding custom packs can exceed the capacity of the arrays used by the engine, leaving some required resources out of the dependency list.

The plugin works in four stages:

1. **Locate the engine functions.** On startup, it scans GTA V's code for signatures corresponding to the metadata, dependency, and session shutdown functions. Multiple matches are accepted when they resolve to the same verified target. If the required addresses or memory accesses cannot be validated, the patch is not enabled.
2. **Keep loaded YMTs resident.** When metadata is associated with a character, the plugin tracks its streaming entry and requests it to be loaded when necessary. Once the resource is loaded, the plugin takes its own reference using `AddRef`, keeping it resident in memory independently of the character's limited dependency list.
3. **Fit the dependencies into the original array.** The plugin intercepts `GetDependencies` and retrieves a larger dependency list into its own buffer. If the list fits in the caller's array, it is passed through unchanged. Otherwise, the plugin removes only entries that it has verified are already loaded and retained by its own references. All other entries keep their original order. Resources omitted from the list remain available in memory.
4. **Release references when the session shuts down.** The plugin waits for any active operations to finish, releases its references before the engine resets, and clears its tracking state. It verifies resource identity to avoid confusing a reused index with a previous streaming entry.

For example, if a character has **110 dependencies**, the caller's array can hold **100**, and **20** of them are already loaded and retained by the plugin, the plugin can return the **remaining 90**. The engine keeps using its original array, while the plugin ensures that the 20 omitted resources remain resident in memory.

The extension supports **up to 256 total dependencies per model**. One additional entry is reserved to detect when this limit is exceeded. This does not mean 256 additional YMT files: base-game and DLC resources also count toward the total. Limits related to drawables, props, textures, pools, and file formats remain separate. Keeping metadata resident may also increase memory usage.

The plugin never intentionally removes a dependency that has not yet been loaded just to make the list fit. If the limit is exceeded, or if too many required dependencies still need to be passed to the engine, the plugin displays a diagnostic message and then closes GTA V. Its activity and any rejected dependency lists are recorded in `YmtLimitAdjuster.log`.

## Sources and Credits

- **DaniGP17 — research on the YMT limit:** [FiveM PR #3444](https://github.com/citizenfx/fivem/pull/3444), an early attempt at increasing the dependency arrays, and [PR #4165](https://github.com/citizenfx/fivem/pull/4165), which introduced the metadata-retention approach that inspired this plugin.
- **Cfx.re / CitizenFX — technical engine references:** [Streaming.cpp](https://github.com/citizenfx/fivem/blob/master/code/components/gta-streaming-five/src/Streaming.cpp), [Streaming.h](https://github.com/citizenfx/fivem/blob/master/code/components/gta-streaming-five/include/Streaming.h), [LoadOptimizations.cpp](https://github.com/citizenfx/fivem/blob/master/code/components/gta-streaming-five/src/LoadOptimizations.cpp), and [BlockLoadSetters.cpp](https://github.com/citizenfx/fivem/blob/master/code/components/gta-core-five/src/BlockLoadSetters.cpp), used to understand signatures, streaming calls, references, and the session lifecycle.
- **DurtyFree / Durty Cloth Tool — clothing limit documentation:** [Game Limits and Crashes](https://docs.gta.clothing/game-mechanics/game-limits-and-crashes).
- **Tsuda Kageyu and the MinHook contributors — hooking library:** [MinHook 1.3.4](https://github.com/TsudaKageyu/minhook/tree/v1.3.4), bundled with the plugin to intercept game functions. The included HDE disassembler is credited to **Vyacheslav Patkov**.
- **Microsoft — Windows memory management:** [Memory Protection Constants](https://learn.microsoft.com/en-us/windows/win32/memory/memory-protection-constants) and [VirtualQuery](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualquery).
