# Implementation notes

## Scope

The implementation is a standalone Windows x64 ASI loaded at process startup.
It does not import ScriptHookV.dll, use script natives, modify game files,
resize engine stack frames, or patch immediate dependency-count instructions.
Version 0.1.2 has no configuration file and always attempts activation. The
first dependency-budget overflow per model per session is logged automatically.
It is experimental and has no successful GTA V runtime validation yet.

The reported 0.1.0 test on Legacy 1.0.3586.0 failed at initialization. The plugin
log records 10 `StreamingManager` matches, 2 `RequestObject` matches and 2
`ShutdownSession` matches; each of the other five signatures matched once.
The old unique-occurrence check rejected these results with `UNSUPPORTED`,
before any hooks were installed. Script Hook V also reports a crash while
executing `InteriorsV.asi` at `GTA5.exe+0x038F316D`; its cause is not established
by these logs.

The subsequent 0.1.1 log on the same build confirms that all ten manager
references converge at RVA `0x2F86270`, but target memory validation rejects
them. Both request references converge at RVA `0x168A0F4` and pass validation;
the refined shutdown signature matches once, resolves to RVA `0x27138` and
passes validation. No hooks were installed. The log does not include section
characteristics or page protections, so the exact failing memory predicate
cannot be established from that log. Version 0.1.2 corrects unnecessarily
exclusive permission requirements and adds that diagnostic information.
Successful activation and behavior still need an in-game retest. Observed
RVAs are diagnostic evidence, not hardcoded addresses.

The log is written beside the plugin, with `%TEMP%\YmtLimitAdjuster.log` as a
fallback when the primary file cannot be opened. Failure to open both locations
produces a warning. An old INI is ignored and is not distributed.

## Address resolution

`src/game.cpp` resolves engine bindings from eight signatures, validates a PE32+ AMD64 image,
scans only readable executable code, resolves signed RIP-relative operands,
checks E8 call opcodes, and validates the target section and current protection.
The manager must permit reading and writing; executable permissions do not
invalidate those capabilities. The model vtable must be readable; writable or
executable permissions do not by themselves invalidate a table that is only
read by the resolver. Earlier versions incorrectly required non-executable
manager data and read-only, non-executable vtable data.

Image bounds, complete section extents, alignment, committed memory, executable
image ownership and actual access permissions remain checked. Guarded and
inaccessible pages are rejected. Resolved function destinations must still be
valid readable executable game code. Bindings are published only if all checks
pass. The dependency vtable byte offset must be 0xA8 or 0xD8. Independent binding
checks continue after another binding fails, so one log can show multiple
incompatibilities without publishing a partially valid binding set.

Target diagnostics include PE section characteristics and `VirtualQuery` page
state, protection and type. These describe different aspects of a target:
section membership constrains its location, while current page protection
governs permitted memory operations. The capability policy follows Microsoft's
[memory protection constants](https://learn.microsoft.com/en-us/windows/win32/memory/memory-protection-constants)
and [VirtualQuery documentation](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualquery).
This corrects a policy error without assuming that a rejected target is valid
merely because all references agree.

For `StreamingManager`, `RequestObject` and `ShutdownSession`, uniqueness applies
to the resolved data/function target rather than the raw pattern location.
The resolver examines every matching candidate and requires every target to be
valid and identical. An invalid target, no match or multiple different targets
reject activation; candidates are logged for diagnosis. The
remaining signatures retain their unique-occurrence checks. This permits
repeated references to one binding without choosing an arbitrary match.

The shutdown signature now includes the E8 call twelve bytes before the old
tail. This excludes a matching tail without that call instead of decoding
arbitrary preceding bytes. The request signature also includes its full rel32
operand. Signed displacement arithmetic rejects overflow and underflow.

The request, release, metadata setter, dependency method, module lookup and
session-shutdown signatures describe Legacy engine routines. These byte strings
are research-derived identifiers, not evidence of verification on every build.
Version detection rejects executables other than GTA5.exe and product versions
outside Legacy 1.0, or below build 1604.

MinHook 1.3.4 detours the metadata setter, the resolved dependency method and the
session-shutdown function. This differs from the upstream PR's vtable-slot write:
all three hooks are prepared before one activation operation. A partial
activation failure stops the process instead of continuing with inconsistent
hooks. An ASI worker performs initialization outside the loader lock; startup
timing still needs validation with actual ASI loaders. Runtime injection is not
supported. The module stays pinned while its detours remain installed.

## Dependency retention

The metadata setter records each YMT's global streaming index, local store index,
module and entry handle after the original call. An absent request flag causes
a RequestObject(index, 7) call outside the map mutex. Requests are serialized per
record so concurrent/reentrant calls cannot claim the same pending acquisition.

Request flags are shared engine state. They alone are insufficient proof of
ownership: when a YMT is loaded, the plugin takes one **module AddRef** reference.
The AddRef, RemoveRef and GetNumRefs offsets are respectively 40, 32 and 16 bytes
before the discovered GetDependencies slot. The code validates those function
pointers and observes a reference-count increase before marking a pin owned.
This is based on the engine-facing sequence used in Cfx's
[LoadOptimizations.cpp](https://github.com/citizenfx/fivem/blob/master/code/components/gta-streaming-five/src/LoadOptimizations.cpp):
load a resource, AddRef it, then release its streaming request.

The dependency hook queries the original method into 257 entries. More than 256
is rejected, including a saturated 257-entry result. A fitting result passes
through unchanged. An overflowing result removes only entries with the same
table/index/handle identity, a completed owned reference and loaded state.
If too many required entries remain, the plugin logs and closes the process;
it does not silently drop unloaded overflow or call LoadObjectsNow recursively.

This strict policy differs from PR #4165's asynchronous overflow requests. It may
refuse a cold-load scenario that the experimental FiveM patch happens to accept.
That refusal needs real-game investigation before claiming equivalent coverage.

## Lifetime and concurrency

A recursive-per-thread shared lease protects ordinary hook work. Session shutdown
marks a teardown boundary and drains in-flight leases before changing the tracked
table. New teardown callbacks use the original engine methods without waiting for
the exclusive lease, avoiding a lock cycle with engine worker threads. Map mutexes
are never held while calling game request/reference functions.

Owned module references are removed before the original shutdown can reset
reference counts or destroy stores; streaming request flags bridge that call.
After shutdown, only plugin-issued requests with surviving table/index/handle
identities are released. No engine calls or unhooking occur from DllMain on exit.
Unexpected table replacement or reuse of a referenced entry outside this boundary
is treated as an incompatible runtime state.

The engine can have lifecycle paths beyond the intercepted boundary. In
particular, externally forced ResetAllRefs, streaming mods, teardown dependency
traversals and mod-loader timing are not covered by the portable tests. Shared
request flags do not form a private ownership namespace during teardown. These
are explicit reasons this build is experimental, and session/save reload testing
is required before describing it as stable.

## Tests and reproducibility

`dependency_filter_tests` covers exact fit, overflow, pending/unowned dependencies,
order, duplicates, overlapping buffers, canaries, callback exceptions, null/zero
cases, exhaustive small masks and the 256/257 boundary. `pattern_tests` checks
wildcards, case, whitespace, malformed tokens and truncated buffers.
`reference_targets_tests` scans synthetic instruction buffers, including ten
references to one manager, forward/backward calls, a competing target despite
a 9:1 majority, accidental shutdown-tail matches and address-arithmetic bounds.
`memory_access_tests` covers readable, writable and executable capabilities,
including additional permissions, and rejection of inaccessible or guarded
memory. These portable cases exercise permission policy without needing a game
installation.

These tests do **not** execute the hooks, the Windows resolver or the game ABI.
The Windows CI builds with MSVC and runs the portable tests; it is provided as
configuration, and was not remotely executed during initial development.

The initial local .asi was cross-compiled with llvm-mingw 20260908 UCRT x86_64,
whose downloaded archive SHA-256 was checked against the official GitHub release
digest: `2258c745e3155870c80793f3e8c80b28fbde11b9ff73c4c78783635b3440b092`.
The PE headers/imports are checked separately; Windows loading and GTA behavior
must still be validated on the target machine.
