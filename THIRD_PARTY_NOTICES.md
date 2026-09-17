# Third-party components and research references

## MinHook 1.3.4 (vendored)

Project: https://github.com/TsudaKageyu/minhook

Commit: `c3fcafdc10146beb5919319d0683e44e3c30d537` (tag `v1.3.4`).
Release source archive SHA-256:
`1aebeae4ca898330c507860acc2fca2eb335fe446a3a2b8444c3bf8b2660a14e`.

The unmodified `src/` and `include/` files are included under
`third_party/minhook/`, together with `AUTHORS.txt` and `LICENSE.txt`.
MinHook and the bundled HDE disassembler carry BSD-style licenses reproduced
in that license file. Binary installation packages include it as
`licenses/MinHook.txt`.

## Research references (not vendored libraries)

The YMT residency technique and engine signatures were researched in DaniGP17's
FiveM PRs #3444 and #4165 and CitizenFX's streaming/core ABI declarations.
The plugin, scanner, filtering implementation, lifecycle integration and tests
in this repository were written separately; no FiveM runtime or source file is
linked or bundled. This project is not a Cfx.re/Rockstar release.

FiveM's current repository license is not a blanket LGPL license. In particular,
the `gta-streaming-five` code is outside its listed LGPL exception. Do not treat
the references as permission to redistribute those source files under MinHook's
license or an assumed FiveM-wide LGPL license.

References:

- https://github.com/citizenfx/fivem/pull/3444
- https://github.com/citizenfx/fivem/pull/4165
- https://github.com/citizenfx/fivem/blob/master/LICENSE
- https://docs.gta.clothing/game-mechanics/game-limits-and-crashes

Script Hook V and its ASI loader are separately distributed by Alexander Blade
at https://www.dev-c.com/gtav/scripthookv/ and are not included here.
