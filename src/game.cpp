#include "game.hpp"
#include "log.hpp"
#include "ymt/pattern.hpp"
#include "ymt/reference_targets.hpp"
#include "ymt/memory_access.hpp"

#include <algorithm>
#include <cstring>
#include <cwchar>
#include <exception>
#include <limits>
#include <string>

namespace ymt {
namespace {
static_assert(PAGE_READONLY == 0x02 && PAGE_READWRITE == 0x04 && PAGE_WRITECOPY == 0x08);
static_assert(PAGE_EXECUTE == 0x10 && PAGE_EXECUTE_READ == 0x20 && PAGE_EXECUTE_READWRITE == 0x40);
static_assert(PAGE_EXECUTE_WRITECOPY == 0x80 && PAGE_GUARD == 0x100 && PAGE_NOACCESS == 0x01);

bool accessible(std::uintptr_t begin, std::size_t bytes, Access access, std::uintptr_t imageOwner = 0) {
    if (!begin || !bytes || bytes > std::numeric_limits<std::uintptr_t>::max() - begin)
        return false;
    const auto end = begin + bytes;
    while (begin < end) {
        MEMORY_BASIC_INFORMATION region{};
        if (VirtualQuery(reinterpret_cast<const void*>(begin), &region, sizeof(region)) != sizeof(region) ||
            region.State != MEM_COMMIT || !permits(region.Protect, access)) return false;
        if (imageOwner && (reinterpret_cast<std::uintptr_t>(region.AllocationBase) != imageOwner ||
                          region.Type != MEM_IMAGE)) return false;
        const auto base = reinterpret_cast<std::uintptr_t>(region.BaseAddress);
        if (region.RegionSize > std::numeric_limits<std::uintptr_t>::max() - base) return false;
        const auto regionEnd = base + region.RegionSize;
        if (regionEnd <= begin) return false;
        begin = std::min(end, regionEnd);
    }
    return true;
}

struct Section {
    std::uintptr_t begin;
    std::uintptr_t end;
    DWORD characteristics;
    char name[9]{};
};

class Image {
public:
    bool initialize() {
        base_ = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        if (!accessible(base_, sizeof(IMAGE_DOS_HEADER), Access::Read))
            return error("Unreadable executable DOS header");
        IMAGE_DOS_HEADER dos{};
        std::memcpy(&dos, reinterpret_cast<void*>(base_), sizeof(dos));
        if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < static_cast<LONG>(sizeof(dos)) ||
            dos.e_lfanew > 0x100000)
            return error("Invalid executable DOS header");
        const auto ntAddress = base_ + static_cast<std::uintptr_t>(dos.e_lfanew);
        if (ntAddress < base_ || !accessible(ntAddress, sizeof(IMAGE_NT_HEADERS64), Access::Read))
            return error("Unreadable executable PE header");
        IMAGE_NT_HEADERS64 nt{};
        std::memcpy(&nt, reinterpret_cast<void*>(ntAddress), sizeof(nt));
        if (nt.Signature != IMAGE_NT_SIGNATURE || nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
            nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
            nt.FileHeader.SizeOfOptionalHeader != sizeof(IMAGE_OPTIONAL_HEADER64) ||
            !nt.FileHeader.NumberOfSections || nt.FileHeader.NumberOfSections > 96)
            return error("Unsupported executable PE layout");
        size_ = nt.OptionalHeader.SizeOfImage;
        if (!size_ || size_ > std::numeric_limits<std::uintptr_t>::max() - base_ ||
            nt.OptionalHeader.SizeOfHeaders > size_)
            return error("Invalid executable image size");
        const auto table = ntAddress + sizeof(IMAGE_NT_HEADERS64);
        const auto tableBytes = static_cast<std::size_t>(nt.FileHeader.NumberOfSections) * sizeof(IMAGE_SECTION_HEADER);
        if (!contains(table, tableBytes) || table - base_ + tableBytes > nt.OptionalHeader.SizeOfHeaders ||
            !accessible(table, tableBytes, Access::Read))
            return error("Unreadable executable section table");
        for (unsigned i = 0; i < nt.FileHeader.NumberOfSections; ++i) {
            IMAGE_SECTION_HEADER section{};
            std::memcpy(&section, reinterpret_cast<void*>(table + i * sizeof(section)), sizeof(section));
            const auto length = static_cast<std::size_t>(section.Misc.VirtualSize);
            if (!length) continue;
            if (section.VirtualAddress >= size_ || length > size_ - section.VirtualAddress)
                return error("Executable section extends beyond image");
            Section loaded{base_ + section.VirtualAddress, base_ + section.VirtualAddress + length,
                section.Characteristics};
            std::memcpy(loaded.name, section.Name, sizeof(section.Name));
            sections_.push_back(loaded);
        }
        std::sort(sections_.begin(), sections_.end(), [](const Section& a, const Section& b) {
            return a.begin < b.begin;
        });
        for (std::size_t i = 1; i < sections_.size(); ++i)
            if (sections_[i].begin < sections_[i - 1].end) return error("Overlapping executable sections");
        log("Executable image: base=%p size=0x%zx sections=%zu", reinterpret_cast<void*>(base_), size_, sections_.size());
        return true;
    }

    bool contains(std::uintptr_t address, std::size_t bytes) const {
        return address >= base_ && address - base_ < size_ && bytes <= size_ - (address - base_);
    }

    const char* rejection(std::uintptr_t address, std::size_t bytes, Access access) const {
        if (!contains(address, bytes)) return "outside-image-range";
        for (const auto& section : sections_) {
            if (address < section.begin || address >= section.end || bytes > section.end - address) continue;
            const auto flags = section.characteristics;
            const bool read = (flags & IMAGE_SCN_MEM_READ) != 0;
            const bool execute = (flags & IMAGE_SCN_MEM_EXECUTE) != 0;
            if (!read) return "section-not-readable";
            if (access == Access::Execute && !execute) return "section-not-executable";
            // Data access follows the loaded pages' actual capabilities. Neither
            // an executable data page nor a writable vtable is inherently invalid.
            // PE headers describe the image on disk; loaders/mods may change its
            // page protections. Preserve bounds, ownership and required access.
            if (!accessible(address, bytes, access, base_)) return "page-access-state-or-image-owner";
            return nullptr;
        }
        return "range-not-within-one-section";
    }

    bool is(std::uintptr_t address, std::size_t bytes, Access access) const {
        return rejection(address, bytes, access) == nullptr;
    }

    void describe(const char* name, std::uintptr_t address, std::size_t bytes, Access access) const {
        const auto reason = rejection(address, bytes, access);
        const char* required = access == Access::Read ? "R" : access == Access::Write ? "RW" : "RX";
        log("Memory %-23s address=%p bytes=0x%zx required=%s result=%s", name,
            reinterpret_cast<void*>(address), bytes, required, reason ? reason : "accepted");
        for (const auto& section : sections_) {
            if (address >= section.begin && address < section.end) {
                log("  section=%s startRVA=0x%zx size=0x%zx flags=0x%08lx", section.name,
                    static_cast<std::size_t>(section.begin - base_),
                    static_cast<std::size_t>(section.end - section.begin),
                    static_cast<unsigned long>(section.characteristics));
                break;
            }
        }
        if (!contains(address, bytes)) return;
        const auto end = address + bytes;
        while (address < end) {
            MEMORY_BASIC_INFORMATION region{};
            if (VirtualQuery(reinterpret_cast<void*>(address), &region, sizeof(region)) != sizeof(region)) {
                log("  VirtualQuery failed: error=%lu", GetLastError());
                return;
            }
            log("  region=%p size=0x%zx state=0x%lx protect=0x%lx type=0x%lx allocationBase=%p", region.BaseAddress,
                static_cast<std::size_t>(region.RegionSize), static_cast<unsigned long>(region.State),
                static_cast<unsigned long>(region.Protect), static_cast<unsigned long>(region.Type), region.AllocationBase);
            const auto regionBase = reinterpret_cast<std::uintptr_t>(region.BaseAddress);
            if (region.RegionSize > std::numeric_limits<std::uintptr_t>::max() - regionBase ||
                regionBase + region.RegionSize <= address) return;
            address = std::min(end, regionBase + region.RegionSize);
        }
    }

    std::vector<std::uintptr_t> matches(const char* name, const char* signature) const {
        Pattern pattern(signature);
        std::vector<std::uintptr_t> found;
        for (const auto& section : sections_) {
            if ((section.characteristics & (IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ | IMAGE_SCN_CNT_CODE)) !=
                (IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ | IMAGE_SCN_CNT_CODE)) continue;
            // Merge adjacent readable executable regions, so a signature may straddle pages.
            std::uintptr_t cursor = section.begin;
            std::uintptr_t runBegin = 0;
            auto scan = [&](std::uintptr_t end) {
                if (!runBegin || end - runBegin < pattern.size()) { runBegin = 0; return; }
                const auto last = end - pattern.size();
                for (auto at = runBegin; at <= last; ++at) {
                    if (pattern.matches(reinterpret_cast<const std::uint8_t*>(at), pattern.size())) {
                        found.push_back(at);
                    }
                }
                runBegin = 0;
            };
            while (cursor < section.end) {
                MEMORY_BASIC_INFORMATION region{};
                if (VirtualQuery(reinterpret_cast<void*>(cursor), &region, sizeof(region)) != sizeof(region)) {
                    scan(cursor);
                    log("Signature %s: VirtualQuery failed at %p", name, reinterpret_cast<void*>(cursor));
                    return {};
                }
                const auto regionBase = reinterpret_cast<std::uintptr_t>(region.BaseAddress);
                if (region.RegionSize > std::numeric_limits<std::uintptr_t>::max() - regionBase ||
                    regionBase + region.RegionSize <= cursor) {
                    log("Signature %s: invalid memory region", name);
                    return {};
                }
                const auto next = std::min(section.end, regionBase + region.RegionSize);
                if (region.State == MEM_COMMIT && permits(region.Protect, Access::Execute)) {
                    if (!runBegin) runBegin = cursor;
                } else scan(cursor);
                cursor = next;
            }
            scan(section.end);
        }
        log("Signature %-20s matches=%zu firstRVA=0x%zx", name, found.size(),
            found.empty() ? 0 : static_cast<std::size_t>(found.front() - base_));
        return found;
    }

    std::uintptr_t unique(const char* name, const char* signature) const {
        const auto found = matches(name, signature);
        return found.size() == 1 ? found.front() : 0;
    }

    // Some signatures identify instructions that REFER to a singleton/function.
    // Repeated call/LEA sites are fine only if EVERY site resolves to the same
    // validated target. Never select the first match or a majority target.
    std::uintptr_t referenced(const char* name, const char* signature, std::size_t offset,
                              bool isCall, Access access, std::size_t bytes,
                              std::size_t alignment = 1) const {
        const auto sites = matches(name, signature);
        ReferenceTargets targets;
        bool valid = !sites.empty();
        for (const auto site : sites) {
            if (offset > std::numeric_limits<std::uintptr_t>::max() - site) {
                valid = false;
                continue;
            }
            const auto target = isCall ? call(site + offset, name) : relative(site + offset, name);
            const bool accepted = target && target % alignment == 0 && is(target, bytes, access);
            log("Reference %-20s siteRVA=0x%zx targetRVA=0x%zx valid=%u", name,
                static_cast<std::size_t>(site - base_),
                target ? static_cast<std::size_t>(target - base_) : 0, accepted ? 1u : 0u);
            const auto previousCount = targets.distinct_count();
            targets.add(target); // Count decoded destinations even when access validation fails.
            if (targets.distinct_count() != previousCount) describe(name, target, bytes, access);
            if (target && target % alignment != 0) log("  %s: target alignment must be %zu bytes", name, alignment);
            if (!accepted) valid = false;
        }
        const auto target = targets.unique();
        log("Resolved %-21s sites=%zu distinctTargets=%zu allValid=%u", name,
            sites.size(), targets.distinct_count(), valid ? 1u : 0u);
        if (!valid || !target) {
            log("Resolver: %s references do not identify one validated target", name);
            return 0;
        }
        return *target;
    }

    std::uintptr_t relative(std::uintptr_t displacement, const char* name) const {
        if (!is(displacement, sizeof(std::int32_t), Access::Execute)) {
            log("%s: displacement is outside readable executable code", name);
            return 0;
        }
        std::int32_t delta{};
        std::memcpy(&delta, reinterpret_cast<void*>(displacement), sizeof(delta));
        const auto target = relative_target(displacement, delta);
        if (!target || !contains(*target, 1)) {
            log("%s: relative target lies outside executable image", name);
            return 0;
        }
        return *target;
    }

    std::uintptr_t call(std::uintptr_t address, const char* name) const {
        if (!is(address, 5, Access::Execute) || *reinterpret_cast<const std::uint8_t*>(address) != 0xe8) {
            log("%s: expected an executable E8 rel32 call", name);
            return 0;
        }
        const auto target = relative(address + 1, name);
        if (!target || !is(target, 1, Access::Execute)) {
            log("%s: call target is not executable game code", name);
            return 0;
        }
        return target;
    }

private:
    static bool error(const char* message) { log("Resolver: %s", message); return false; }
    std::uintptr_t base_{};
    std::size_t size_{};
    std::vector<Section> sections_;
};
}

bool readable(const void* address, std::size_t bytes) {
    return accessible(reinterpret_cast<std::uintptr_t>(address), bytes, Access::Read);
}

bool check_legacy_version() {
    std::vector<wchar_t> path(32768);
    const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length >= path.size()) {
        log("Version check: cannot obtain complete executable path (error %lu)", GetLastError());
        return false;
    }
    const wchar_t* filename = std::wcsrchr(path.data(), L'\\');
    filename = filename ? filename + 1 : path.data();
    if (_wcsicmp(filename, L"GTA5.exe") != 0) {
        log("Version check: only GTA5.exe (Legacy) is supported");
        return false;
    }
    DWORD ignored{};
    const DWORD size = GetFileVersionInfoSizeW(path.data(), &ignored);
    if (!size) {
        log("Version check: missing executable version resource (error %lu)", GetLastError());
        return false;
    }
    std::vector<std::uint8_t> resource(size);
    if (!GetFileVersionInfoW(path.data(), 0, size, resource.data())) {
        log("Version check: cannot read executable version (error %lu)", GetLastError());
        return false;
    }
    VS_FIXEDFILEINFO* info{};
    UINT infoSize{};
    if (!VerQueryValueW(resource.data(), L"\\", reinterpret_cast<void**>(&info), &infoSize) ||
        !info || infoSize < sizeof(VS_FIXEDFILEINFO) || info->dwSignature != 0xfeef04bd) {
        log("Version check: invalid executable fixed version information");
        return false;
    }
    const unsigned major = HIWORD(info->dwProductVersionMS);
    const unsigned minor = LOWORD(info->dwProductVersionMS);
    const unsigned build = HIWORD(info->dwProductVersionLS);
    const unsigned revision = LOWORD(info->dwProductVersionLS);
    log("GTA5.exe product=%u.%u.%u.%u file=%u.%u.%u.%u", major, minor, build, revision,
        static_cast<unsigned>(HIWORD(info->dwFileVersionMS)), static_cast<unsigned>(LOWORD(info->dwFileVersionMS)),
        static_cast<unsigned>(HIWORD(info->dwFileVersionLS)), static_cast<unsigned>(LOWORD(info->dwFileVersionLS)));
    if (major != 1 || minor != 0 || build < 1604) {
        log("Version check: requires GTA V Legacy 1.0.1604 or later");
        return false;
    }
    return true;
}

bool resolve_game(GameBindings& result) {
    result = {};
    try {
        Image image;
        if (!image.initialize()) return false;
        const auto set = image.unique("SetPedMetaDataFile", "83 FA FF 74 ? 53 48 83 EC 20 8B DA 48 81 C1 E0 00 00 00 BA 01 00 00 00 E8");
        const auto vtableSignature = image.unique("ModelInfoVtable", "48 8D 05 ? ? ? ? 48 8D 0D ? ? ? ? 48 89 05 ? ? ? ? E8 ? ? ? ? 48 8D 15 ? ? ? ? 48 8D 0D ? ? ? ? E8");
        const auto slotSignature = image.unique("DependenciesSlot", "FF 90 ? ? ? ? 33 D2 4C 63 C0 85 C0");
        const auto manager = image.referenced("StreamingManager", "74 1A 8B 15 ? ? ? ? 48 8D 0D ? ? ? ? 41",
            11, false, Access::Write, sizeof(StreamingManager), alignof(void*));
        const auto request = image.referenced("RequestObject", "41 B8 14 00 00 00 03 D3 E8 ? ? ? ?",
            8, true, Access::Execute, 1);
        const auto moduleSignature = image.unique("ModuleByExtension", "74 15 48 8D 50 01 48 8D");
        const auto releaseSignature = image.unique("ReleaseObject", "8B CA 4D 8B 11 45 0F B7 5C CA 06 45");
        // Cfx's shutdown call is twelve bytes before the old tail signature.
        // Include its E8 opcode in the match rather than interpreting arbitrary
        // bytes before every matching tail as a call.
        const auto shutdown = image.referenced("ShutdownSession",
            "E8 ? ? ? ? ? ? ? ? ? ? ? 75 0F E8 ? ? ? ? 8B 0D ? ? ? ? 3B C8",
            0, true, Access::Execute, 1);
        bool complete = set && vtableSignature && slotSignature && manager && request &&
            moduleSignature && releaseSignature && shutdown;
        // Keep checking independent bindings after an earlier failure. Otherwise
        // a single rejected manager hides vtable/code issues until another run.
        std::uint32_t slot{};
        if (slotSignature) std::memcpy(&slot, reinterpret_cast<void*>(slotSignature + 2), sizeof(slot));
        const bool slotValid = slot == 0xa8 || slot == 0xd8;
        if (!slotValid) {
            log("Resolver: unexpected GetDependencies vtable offset 0x%x", slot);
            complete = false;
        }
        const auto vtable = vtableSignature ? image.relative(vtableSignature + 3, "ModelInfoVtable") : 0;
        const auto module = moduleSignature ? image.call(moduleSignature + 13, "ModuleByExtension") : 0;
        const auto release = releaseSignature >= 11 ? releaseSignature - 11 : 0;
        const auto vtableBytes = slotValid ? slot + sizeof(void*) : sizeof(void*);
        if (vtable) image.describe("ModelInfoVtable", vtable, vtableBytes, Access::Read);
        const bool vtableValid = vtable && vtable % alignof(void*) == 0 &&
            slotValid && image.is(vtable, vtableBytes, Access::Read);
        if (!vtableValid) {
            log("Resolver: model vtable is not aligned readable game data, or its slot is invalid");
            complete = false;
        }
        if (!manager || manager % alignof(void*) != 0 || !image.is(manager, sizeof(StreamingManager), Access::Write)) {
            log("Resolver: streaming manager is not aligned readable/writable game data");
            complete = false;
        }
        std::uintptr_t dependencies{};
        if (vtableValid) std::memcpy(&dependencies, reinterpret_cast<void*>(vtable + slot), sizeof(dependencies));
        if (dependencies) image.describe("GetDependencies", dependencies, 1, Access::Execute);
        if (set) image.describe("SetPedMetaDataFile", set, 1, Access::Execute);
        if (release) image.describe("ReleaseObject", release, 1, Access::Execute);
        if (module) image.describe("ModuleByExtension", module, 1, Access::Execute);
        if (!image.is(dependencies, 1, Access::Execute) || !image.is(set, 1, Access::Execute) ||
            !image.is(release, 1, Access::Execute) || !request || !module || !shutdown) {
            log("Resolver: at least one required function is not valid executable game code");
            complete = false;
        }
        if (!complete) {
            log("Resolver: one or more binding checks failed; all available independent checks completed; no hooks will be installed");
            return false;
        }
        GameBindings verified{};
        verified.manager = reinterpret_cast<StreamingManager*>(manager);
        verified.request = reinterpret_cast<RequestObject>(request);
        verified.release = reinterpret_cast<ReleaseObject>(release);
        verified.moduleByExtension = reinterpret_cast<ModuleByExtension>(module);
        verified.setMetadata = reinterpret_cast<void*>(set);
        verified.getDependencies = reinterpret_cast<void*>(dependencies);
        verified.shutdown = reinterpret_cast<void*>(shutdown);
        verified.dependencySlot = slot;
        result = verified;
        log("Resolved game ABI: manager=%p vtable=%p GetDependencies=%p slot=0x%x", static_cast<void*>(verified.manager),
            reinterpret_cast<void*>(vtable), verified.getDependencies, slot);
        return true;
    } catch (const std::exception& exception) {
        log("Resolver failed: %s", exception.what());
        return false;
    }
}
}
