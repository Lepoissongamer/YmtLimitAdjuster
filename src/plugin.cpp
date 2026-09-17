#include "game.hpp"
#include "log.hpp"
#include "ymt/dependency_filter.hpp"
#include <MinHook.h>
#include <array>
#include <atomic>
#include <cstring>
#include <limits>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace {
ymt::GameBindings game;
ymt::SetMetadata originalSet{};
ymt::GetDependencies originalGet{};
void (*originalShutdown)(){};
std::atomic<unsigned> status{0};
std::atomic<bool> shuttingDown{false};
std::shared_mutex lifecycleMutex;
thread_local unsigned hookDepth{};

// A session reset must drain all plugin work before invalidating streaming
// pointers. Game calls can recurse into our hooks, so only the outermost call
// acquires the shared lock. Calls made by teardown itself use the originals:
// waiting for the exclusive lock here could deadlock the game's shutdown.
class HookLease {
public:
    HookLease() noexcept {
        if (hookDepth != 0) {
            ++hookDepth;
            active_ = true;
            return;
        }
        try {
            while (!shuttingDown.load()) {
                if (lifecycleMutex.try_lock_shared()) {
                    // Shutdown may have started between the first flag check
                    // and acquiring the lock. Do not admit new plugin work.
                    if (shuttingDown.load()) {
                        lifecycleMutex.unlock_shared();
                        return;
                    }
                    ++hookDepth;
                    active_ = true;
                    ownsLock_ = true;
                    return;
                }
                SwitchToThread();
            }
        } catch (...) {
            ymt::fail_stop("Cannot protect the streaming session lifecycle. See YmtLimitAdjuster.log.");
        }
    }
    ~HookLease() {
        if (!active_) return;
        --hookDepth;
        if (ownsLock_) lifecycleMutex.unlock_shared();
    }
    HookLease(const HookLease&) = delete;
    HookLease& operator=(const HookLease&) = delete;
    explicit operator bool() const noexcept { return active_; }

private:
    bool active_{};
    bool ownsLock_{};
};

constexpr std::uint32_t requiredMask = 0x00F10000;
constexpr int requestFlags = 7;

using RefOperation = void (*)(void*, std::uint32_t);
using RefCount = int (*)(void*, std::uint32_t);
struct Resident {
    std::uint32_t handle{};
    void* module{};
    std::uint32_t localIndex{};
    RefOperation removeRef{};
    bool requestedByUs{};
    bool ownsReference{};
    bool acquiring{};
};
std::mutex residentMutex;
std::unordered_map<std::uint32_t, Resident> residents;
std::unordered_set<std::uint32_t> loggedModels;
ymt::StreamingEntry* trackedTable{};

struct Table { ymt::StreamingEntry* entries; std::uint32_t size; };
Table table() {
    auto* const entries = game.manager->entries;
    const auto count = game.manager->count;
    if (!entries || count <= 0 || count > 16 * 1024 * 1024 ||
        !ymt::readable(entries, static_cast<std::size_t>(count) * sizeof(*entries)))
        ymt::fail_stop("The streaming table is invalid. See YmtLimitAdjuster.log.\n"
                       "This game build or another streaming mod may be incompatible.");
    return {entries, static_cast<std::uint32_t>(count)};
}
void sync_table(const Table& current) {
    // Called with residentMutex held. Indices from another table are never reused.
    if (trackedTable != current.entries) {
        if (trackedTable && !residents.empty())
            ymt::fail_stop("The streaming table changed outside session teardown.\n"
                           "This streaming modification is incompatible. See YmtLimitAdjuster.log.");
        residents.clear();
        loggedModels.clear();
        trackedTable = current.entries;
    }
}
std::uint32_t flags(const Table& current, std::uint32_t index) {
    return *reinterpret_cast<volatile const std::uint32_t*>(&current.entries[index].flags);
}

void* reference_method(void* module, std::size_t offset) {
    if (!ymt::readable(module, sizeof(void*)))
        ymt::fail_stop("The YMT reference-counting module is invalid.");
    std::uint8_t* vtable{};
    std::memcpy(&vtable, module, sizeof(vtable));
    if (!ymt::readable(vtable, offset + sizeof(void*)))
        ymt::fail_stop("The YMT reference-counting vtable is invalid.");
    void* method{};
    std::memcpy(&method, vtable + offset, sizeof(method));
    MEMORY_BASIC_INFORMATION region{};
    if (!method || !VirtualQuery(method, &region, sizeof(region)) ||
        region.State != MEM_COMMIT || region.AllocationBase != GetModuleHandleW(nullptr) ||
        (region.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
        !(region.Protect & (PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
        ymt::fail_stop("The YMT reference-counting function is not executable game code.");
    return method;
}

void retain(std::uint32_t index, const Table& current, void* module = nullptr, std::uint32_t localIndex = 0) {
    if (index >= current.size)
        ymt::fail_stop("A YMT streaming index is out of bounds. See YmtLimitAdjuster.log.");
    const auto handle = current.entries[index].handle;
    bool alreadyOwned{};
    {
        std::lock_guard<std::mutex> lock(residentMutex);
        sync_table(current);
        auto found = residents.find(index);
        if (found == residents.end() || found->second.handle != handle) {
            if (!module) return;
            if (found != residents.end() && found->second.ownsReference)
                ymt::fail_stop("A referenced YMT streaming entry was reused outside session teardown.");
            residents[index] = {handle, module, localIndex, nullptr, false, false, false};
            found = residents.find(index);
        }
        auto& record = found->second;
        if (record.acquiring) return; // A concurrent/reentrant caller cannot claim this pin yet.
        record.acquiring = true;
        module = record.module;
        localIndex = record.localIndex;
        alreadyOwned = record.ownsReference;
    }
    // Game calls can recurse into the hooks: never hold residentMutex here.
    const bool request = (flags(current, index) & requiredMask) == 0;
    if (request) game.request(game.manager, index, requestFlags);
    RefOperation removeRef{};
    bool acquired{};
    if (!alreadyOwned && (flags(current, index) & 3u) == 1u) {
        // Unlike shared request flags, a module reference is owned by this plugin.
        // The engine itself uses AddRef after loading to keep streaming assets alive.
        const auto addRef = reinterpret_cast<RefOperation>(reference_method(module, game.dependencySlot - 5 * sizeof(void*)));
        removeRef = reinterpret_cast<RefOperation>(reference_method(module, game.dependencySlot - 4 * sizeof(void*)));
        const auto countRefs = reinterpret_cast<RefCount>(reference_method(module, game.dependencySlot - 2 * sizeof(void*)));
        const auto before = countRefs(module, localIndex);
        if (before < 0 || before >= 0x7fff)
            ymt::fail_stop("Unexpected YMT reference count; refusing to overflow it.");
        addRef(module, localIndex);
        const auto after = countRefs(module, localIndex);
        if (after <= before)
            ymt::fail_stop("Could not establish an owned YMT reference. This module ABI is unsupported.");
        acquired = true;
    }
    {
        std::lock_guard<std::mutex> lock(residentMutex);
        auto found = residents.find(index);
        if (trackedTable != current.entries || found == residents.end() || found->second.handle != handle)
            ymt::fail_stop("The YMT identity changed during reference acquisition.");
        auto& record = found->second;
        record.requestedByUs |= request;
        record.ownsReference |= acquired;
        if (acquired) record.removeRef = removeRef;
        record.acquiring = false;
    }
}

void set_metadata(void* self, std::uint32_t localIndex) {
    const HookLease lease;
    originalSet(self, localIndex);
    if (!lease || localIndex == std::numeric_limits<std::uint32_t>::max()) return;
    try {
        const auto current = table();
        // Resolve each time so a session reset cannot leave a cached store pointer.
        auto* module = static_cast<std::uint8_t*>(
            game.moduleByExtension(game.manager->moduleManager, "ymt"));
        if (!ymt::readable(module, 12))
            ymt::fail_stop("The YMT store is unavailable. See YmtLimitAdjuster.log.");
        std::uint32_t base{};
        std::memcpy(&base, module + 8, sizeof(base));
        if (base >= current.size || localIndex >= current.size - base)
            ymt::fail_stop("The YMT store layout is incompatible. See YmtLimitAdjuster.log.");
        retain(base + localIndex, current, module, localIndex);
    } catch (...) {
        ymt::fail_stop("Cannot retain YMT metadata (allocation or runtime error). See YmtLimitAdjuster.log.");
    }
}

int get_dependencies(void* self, std::uint32_t model, std::uint32_t* output, int capacity) {
    const HookLease lease;
    if (!lease || capacity <= 0 || capacity > static_cast<int>(ymt::kDependencyCapacity))
        return originalGet(self, model, output, capacity);
    // The extra slot distinguishes a complete 256-entry list from truncation.
    std::array<std::uint32_t, ymt::kDependencyCapacity + 1> dependencies{};
    const auto total = originalGet(self, model, dependencies.data(), static_cast<int>(dependencies.size()));
    if (total < 0 || total > static_cast<int>(ymt::kDependencyCapacity)) {
        ymt::log("model=%u callerCapacity=%d returned=%d probeCapacity=%zu", model, capacity, total, dependencies.size());
        ymt::fail_stop("The model exceeds the supported 256 total streaming dependencies, or its dependency count is invalid.\n"
                       "Reduce the clothing/creaturemetadata packs. See YmtLimitAdjuster.log.");
    }
    if (total <= capacity) {
        if (total && !output) ymt::fail_stop("Invalid dependency output pointer.");
        if (total) std::memcpy(output, dependencies.data(), static_cast<std::size_t>(total) * sizeof(std::uint32_t));
        return total;
    }
    try {
        const auto current = table();
        std::array<std::uint32_t, ymt::kDependencyCapacity> rearm{};
        std::size_t rearmCount{};
        {
            std::lock_guard<std::mutex> lock(residentMutex);
            sync_table(current);
            for (int i = 0; i < total; ++i) {
                const auto index = dependencies[static_cast<std::size_t>(i)];
                const auto found = residents.find(index);
                if (index < current.size && found != residents.end() &&
                    found->second.handle == current.entries[index].handle &&
                    (!found->second.ownsReference || (flags(current, index) & requiredMask) == 0))
                    rearm[rearmCount++] = index;
            }
        }
        for (std::size_t i = 0; i < rearmCount; ++i) retain(rearm[i], current);

        ymt::FilterResult result{};
        bool logModel{};
        {
            std::lock_guard<std::mutex> lock(residentMutex);
            result = ymt::filter_dependencies(dependencies.data(), static_cast<std::size_t>(total),
                output, static_cast<std::size_t>(capacity), [&](std::uint32_t index) {
                    const auto found = residents.find(index);
                    if (index >= current.size || found == residents.end() ||
                        found->second.handle != current.entries[index].handle ||
                        !found->second.ownsReference || found->second.acquiring) return false;
                    const auto state = flags(current, index);
                    return (state & 3u) == 1u;
                });
            logModel = result.status != ymt::FilterStatus::ok || loggedModels.insert(model).second;
        }
        if (logModel)
            ymt::log("model=%u total=%d callerCapacity=%d returned=%zu residentOmitted=%zu status=%u",
                     model, total, capacity, result.written, result.omitted, static_cast<unsigned>(result.status));
        if (result.status != ymt::FilterStatus::ok)
            ymt::fail_stop("Too many model dependencies are still unloaded or unpinned.\n"
                           "The plugin cannot safely omit them. GTA V must close.\n"
                           "Reduce the clothing packs and send YmtLimitAdjuster.log for diagnosis.");
        return static_cast<int>(result.written);
    } catch (...) {
        ymt::fail_stop("Cannot filter model dependencies (allocation or runtime error). See YmtLimitAdjuster.log.");
    }
}

void shutdown_session() {
    if (hookDepth != 0)
        ymt::fail_stop("Session shutdown reentered an active YMT hook. See YmtLimitAdjuster.log.");
    if (shuttingDown.exchange(true))
        ymt::fail_stop("Concurrent or recursive session shutdown is unsupported. See YmtLimitAdjuster.log.");
    try {
        // Existing leases finish before the game can invalidate their tables.
        // New top-level hook calls observe shuttingDown and bypass plugin work.
        std::unique_lock<std::shared_mutex> lifecycleLock(lifecycleMutex);
        // Balance owned module references BEFORE the game can ResetAllRefs or
        // destroy the store. Request flags bridge the original teardown call.
        std::unordered_map<std::uint32_t, Resident> old;
        ymt::StreamingEntry* previous{};
        {
            std::lock_guard<std::mutex> lock(residentMutex);
            old.swap(residents);
            previous = trackedTable;
            trackedTable = nullptr;
            loggedModels.clear();
        }
        auto* entries = game.manager->entries;
        const auto size = game.manager->count;
        if (entries && entries == previous && size > 0 && size <= 16 * 1024 * 1024 &&
            ymt::readable(entries, static_cast<std::size_t>(size) * sizeof(*entries))) {
            for (auto& pair : old) {
                const auto index = pair.first;
                auto& record = pair.second;
                if (index >= static_cast<std::uint32_t>(size) || entries[index].handle != record.handle) continue;
                if (record.ownsReference) {
                    if ((entries[index].flags & requiredMask) == 0) {
                        game.request(game.manager, index, requestFlags);
                        record.requestedByUs = true;
                    }
                    record.removeRef(record.module, record.localIndex);
                }
            }
        }
        originalShutdown();
        entries = game.manager->entries;
        const auto remaining = game.manager->count;
        // Release only our requests whose entry identity survived teardown.
        if (entries && entries == previous && remaining > 0 && remaining <= 16 * 1024 * 1024 &&
            ymt::readable(entries, static_cast<std::size_t>(remaining) * sizeof(*entries))) {
            for (const auto& pair : old) {
                const auto index = pair.first;
                if (pair.second.requestedByUs && index < static_cast<std::uint32_t>(remaining) &&
                    entries[index].handle == pair.second.handle && (entries[index].flags & requiredMask))
                    game.release(game.manager, index, requestFlags);
            }
        }
        ymt::log("Session reset: cleared %zu resident records.", old.size());
        lifecycleLock.unlock();
        shuttingDown.store(false);
    } catch (...) {
        ymt::fail_stop("Cannot reset the YMT streaming session safely. See YmtLimitAdjuster.log.");
    }
}

std::wstring module_path(HMODULE module) {
    std::wstring path(32768, L'\0');
    const auto size = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (!size || size >= path.size()) throw std::runtime_error("Module path unavailable");
    path.resize(size);
    return path;
}

DWORD WINAPI initialize(void* param) {
    try {
        const auto path = module_path(static_cast<HMODULE>(param));
        const auto base = path.substr(0, path.find_last_of(L'.'));
        if (!ymt::open_log(base + L".log")) {
            const auto fileError = GetLastError();
            std::wstring temporary(32768, L'\0');
            const auto length = GetTempPathW(static_cast<DWORD>(temporary.size()), temporary.data());
            if (!length || length >= temporary.size()) {
                status.store(4);
                MessageBoxW(nullptr, L"Impossible de creer YmtLimitAdjuster.log dans le dossier du plugin ou de trouver le dossier temporaire.\nAucun correctif n'a ete installe.",
                            L"YmtLimitAdjuster - journal inaccessible", MB_OK | MB_ICONERROR);
                return 0;
            }
            temporary.resize(length);
            temporary += L"YmtLimitAdjuster.log";
            if (!ymt::open_log(temporary)) {
                status.store(4);
                MessageBoxW(nullptr, L"Impossible de creer YmtLimitAdjuster.log dans le dossier du plugin ou dans %TEMP%.\nAucun correctif n'a ete installe.",
                            L"YmtLimitAdjuster - journal inaccessible", MB_OK | MB_ICONERROR);
                return 0;
            }
            ymt::log("Could not create the log beside the ASI (Windows error %lu); using %%TEMP%%\\YmtLimitAdjuster.log.", fileError);
        }
        ymt::log("YmtLimitAdjuster 0.1.2 experimental; Windows x64 / GTA V Legacy; dependency ceiling=256; no configuration file.");
        HMODULE pinned{};
        // Hooks remain installed for the process lifetime, including during teardown.
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(param), &pinned)) {
            status.store(4);
            ymt::log("INITIALIZATION FAILED: could not pin ASI module (Windows error %lu); no hooks installed.", GetLastError());
            return 0;
        }
        if (!ymt::check_legacy_version() || !ymt::resolve_game(game)) {
            status.store(2);
            ymt::log("UNSUPPORTED: no hooks installed. Original game limits remain active.");
            return 0;
        }
        auto result = MH_Initialize();
        if (result != MH_OK) throw std::runtime_error(MH_StatusToString(result));
        result = MH_CreateHook(game.setMetadata, reinterpret_cast<void*>(&set_metadata), reinterpret_cast<void**>(&originalSet));
        if (result == MH_OK)
            result = MH_CreateHook(game.getDependencies, reinterpret_cast<void*>(&get_dependencies), reinterpret_cast<void**>(&originalGet));
        if (result == MH_OK)
            result = MH_CreateHook(game.shutdown, reinterpret_cast<void*>(&shutdown_session), reinterpret_cast<void**>(&originalShutdown));
        if (result != MH_OK) {
            MH_Uninitialize();
            throw std::runtime_error(MH_StatusToString(result));
        }
        // One MinHook transaction freezes threads while enabling the complete set.
        result = MH_EnableHook(MH_ALL_HOOKS);
        if (result != MH_OK) {
            // MinHook may have enabled an earlier target before a later failure.
            // A partial patch cannot be allowed to continue.
            ymt::log("Hook activation failed: %s", MH_StatusToString(result));
            ymt::fail_stop("Could not activate all YMT hooks. See YmtLimitAdjuster.log.");
        }
        status.store(3);
        ymt::log("ACTIVE: metadata retention, expanded dependency query and session reset hooks installed. In-game validation is required.");
    } catch (const std::exception& e) {
        status.store(4);
        ymt::log("INITIALIZATION FAILED: %s; no active dependency extension.", e.what());
    } catch (...) {
        status.store(4);
        ymt::log("INITIALIZATION FAILED: unknown exception.");
    }
    return 0;
}
}

extern "C" __declspec(dllexport) unsigned YmtLimitAdjuster_GetStatus() {
    return status.load();
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        // No pattern scans, thread suspension, waits or game calls under loader lock.
        const auto worker = CreateThread(nullptr, 0, initialize, instance, 0, nullptr);
        if (!worker) return FALSE;
        CloseHandle(worker);
    }
    return TRUE;
}
