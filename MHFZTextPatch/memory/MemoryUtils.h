#pragma once
#include <Windows.h>
#include <cstdint>
#include <string>
#include <vector>

struct ModuleSection {
	uintptr_t start = 0;
	size_t size = 0;
	std::string name;
};

namespace MemoryUtils {
	HMODULE GetModuleByName(const char* moduleName);
	uintptr_t GetModuleBase(HMODULE module);
	size_t GetModuleSize(HMODULE module);
	bool GetSection(HMODULE module, const char* sectionName, ModuleSection& out);

	bool IsReadable(uintptr_t address, size_t size);
	bool IsWritable(uintptr_t address, size_t size);
	bool IsExecutable(uintptr_t address);

	// Structured-exception guarded accessors.
	// Game data pages can be unmapped between quests, so every gameplay
	// read/write goes through these instead of raw pointer dereferences.
	bool SafeReadU8(uintptr_t address, uint8_t& out);
	bool SafeReadU16(uintptr_t address, uint16_t& out);
	bool SafeReadU32(uintptr_t address, uint32_t& out);
	bool SafeReadBytes(uintptr_t address, void* out, size_t size);
	bool SafeWriteU8(uintptr_t address, uint8_t value);
	bool SafeWriteU16(uintptr_t address, uint16_t value);
	bool SafeWriteU32(uintptr_t address, uint32_t value);
	bool SafeWriteBytes(uintptr_t address, const void* data, size_t size);
	// Swap that fails if the slot no longer holds `expected`; atomic unless the
	// 4 bytes straddle a cache line (relocated tables are unaligned).
	bool SafeCompareExchangeU32(uintptr_t address, uint32_t expected, uint32_t desired);
}
