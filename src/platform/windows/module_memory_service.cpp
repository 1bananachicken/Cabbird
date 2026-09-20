#include "cabbird/module_memory_service.hpp"

namespace cabbird {

std::vector<cabbird::mem::ModuleInfo> ModuleMemoryService::EnumerateModules() const {
    return cabbird::mem::EnumerateModules();
}

std::optional<cabbird::mem::ModuleInfo> ModuleMemoryService::FindModule(
    std::wstring_view name) const {
    return cabbird::mem::FindModule(name);
}

std::vector<cabbird::mem::SectionInfo> ModuleMemoryService::EnumerateSections(
    const cabbird::mem::ModuleInfo& module) const {
    return cabbird::mem::EnumerateSections(module);
}

std::vector<cabbird::mem::RegionInfo> ModuleMemoryService::EnumerateRegions(
    const cabbird::mem::ModuleInfo& module) const {
    return cabbird::mem::EnumerateRegions(module);
}

std::vector<cabbird::mem::RegionInfo> ModuleMemoryService::EnumerateProcessRegions() const {
    return cabbird::mem::EnumerateProcessRegions();
}

std::vector<std::uintptr_t> ModuleMemoryService::ScanSection(
    const cabbird::mem::ModuleInfo& module,
    std::string_view section_name,
    const cabbird::mem::Pattern& pattern,
    std::size_t limit) const {
    return cabbird::mem::ScanSection(module, section_name, pattern, limit);
}

std::optional<std::uintptr_t> ModuleMemoryService::ResolveRipRelative(
    std::uintptr_t instruction,
    std::size_t displacement_offset,
    std::size_t instruction_size,
    std::ptrdiff_t addend) const {
    return cabbird::mem::ResolveRipRelative(
        instruction, displacement_offset, instruction_size, addend);
}

std::optional<std::vector<std::uint8_t>> ModuleMemoryService::ReadMemory(
    std::uintptr_t address, std::size_t size) const {
    return cabbird::mem::ReadMemory(address, size);
}

bool ModuleMemoryService::ReadMemoryInto(
    std::uintptr_t address, void* destination, std::size_t size) const {
    return cabbird::mem::ReadMemoryInto(address, destination, size);
}

bool ModuleMemoryService::WriteMemory(
    std::uintptr_t address, const void* source, std::size_t size) const {
    return cabbird::mem::WriteMemory(address, source, size);
}

bool ModuleMemoryService::PatchMemory(
    std::uintptr_t address, const void* source, std::size_t size) const {
    return cabbird::mem::PatchMemory(address, source, size);
}

bool ModuleMemoryService::ProtectMemory(
    std::uintptr_t address,
    std::size_t size,
    DWORD protection,
    DWORD& previous) const {
    return cabbird::mem::ProtectMemory(address, size, protection, previous);
}

void* ModuleMemoryService::AllocateMemory(std::size_t size, DWORD protection) const {
    return cabbird::mem::AllocateMemory(size, protection);
}

bool ModuleMemoryService::FreeMemory(void* address) const {
    return cabbird::mem::FreeMemory(address);
}

std::optional<std::uintptr_t> ModuleMemoryService::ResolvePointerChain(
    std::uintptr_t base,
    const std::ptrdiff_t* offsets,
    std::size_t count) const {
    return cabbird::mem::ResolvePointerChain(base, offsets, count);
}

std::string ModuleMemoryService::ProtectionName(DWORD protection) const {
    return cabbird::mem::ProtectionName(protection);
}

std::string ModuleMemoryService::StateName(DWORD state) const {
    return cabbird::mem::StateName(state);
}

std::string ModuleMemoryService::TypeName(DWORD type) const {
    return cabbird::mem::TypeName(type);
}

}  // namespace cabbird
