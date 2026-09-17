#pragma once
#include <windows.h>
#include <cstdint>
#include <cstddef>
#include <vector>

namespace ymt {
struct StreamingEntry { std::uint32_t handle; std::uint32_t flags; };
struct StreamingManager {
    StreamingEntry* entries;
    std::uint8_t reserved0[16];
    std::int32_t count;
    std::uint8_t reserved1[412];
    std::uint8_t moduleManager[40];
};
static_assert(sizeof(StreamingEntry) == 8);
static_assert(offsetof(StreamingManager, count) == 24);
static_assert(offsetof(StreamingManager, moduleManager) == 0x1B8);

using GetDependencies = int (*)(void*, std::uint32_t, std::uint32_t*, int);
using SetMetadata = void (*)(void*, std::uint32_t);
using RequestObject = void (*)(void*, std::uint32_t, int);
using ReleaseObject = bool (*)(void*, std::uint32_t, int);
using ModuleByExtension = void* (*)(void*, const char*);

struct GameBindings {
    StreamingManager* manager{};
    RequestObject request{};
    ReleaseObject release{};
    ModuleByExtension moduleByExtension{};
    void* setMetadata{};
    void* getDependencies{};
    void* shutdown{};
    std::uint32_t dependencySlot{}; // Byte offset in strStreamingModule's vtable.
};

bool readable(const void* address, std::size_t bytes);
bool resolve_game(GameBindings& result);
bool check_legacy_version();
}
