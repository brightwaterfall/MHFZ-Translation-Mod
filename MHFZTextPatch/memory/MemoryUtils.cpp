#include "MemoryUtils.h"
#include <cstring>

namespace {
	// Kept free of C++ objects so SEH (__try/__except) is legal here.
	bool GuardedCopy(void* dest, const void* src, size_t size) {
		__try {
			memcpy(dest, src, size);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	bool GuardedCompareExchange(volatile LONG* target, LONG expected, LONG desired) {
		__try {
			return InterlockedCompareExchange(target, desired, expected) == expected;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	template <typename T>
	bool ProtectedWrite(uintptr_t address, T value) {
		DWORD oldProtect = 0;
		if (!VirtualProtect(reinterpret_cast<void*>(address), sizeof(T),
			PAGE_EXECUTE_READWRITE, &oldProtect)) {
			return false;
		}
		const bool ok = GuardedCopy(reinterpret_cast<void*>(address), &value, sizeof(value));
		DWORD restored = 0;
		VirtualProtect(reinterpret_cast<void*>(address), sizeof(T), oldProtect, &restored);
		return ok;
	}
}

namespace MemoryUtils {
	HMODULE GetModuleByName(const char* moduleName) {
		return GetModuleHandleA(moduleName);
	}

	uintptr_t GetModuleBase(HMODULE module) {
		return reinterpret_cast<uintptr_t>(module);
	}

	size_t GetModuleSize(HMODULE module) {
		if (!module)
			return 0;
		auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(module);
		if (dos->e_magic != IMAGE_DOS_SIGNATURE)
			return 0;
		auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(
			reinterpret_cast<uint8_t*>(module) + dos->e_lfanew);
		if (nt->Signature != IMAGE_NT_SIGNATURE)
			return 0;
		return nt->OptionalHeader.SizeOfImage;
	}

	bool GetSection(HMODULE module, const char* sectionName, ModuleSection& out) {
		out = {};
		if (!module || !sectionName)
			return false;

		auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(module);
		if (dos->e_magic != IMAGE_DOS_SIGNATURE)
			return false;
		auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(
			reinterpret_cast<uint8_t*>(module) + dos->e_lfanew);
		if (nt->Signature != IMAGE_NT_SIGNATURE)
			return false;

		auto* section = IMAGE_FIRST_SECTION(nt);
		for (UINT i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
			char name[9] = {};
			memcpy(name, section->Name, 8);
			if (_stricmp(name, sectionName) == 0) {
				out.name = name;
				out.start = reinterpret_cast<uintptr_t>(module) + section->VirtualAddress;
				out.size = section->Misc.VirtualSize
					? section->Misc.VirtualSize
					: section->SizeOfRawData;
				return out.start != 0 && out.size != 0;
			}
		}
		return false;
	}

	bool IsReadable(uintptr_t address, size_t size) {
		if (!address || !size)
			return false;
		MEMORY_BASIC_INFORMATION mbi{};
		if (!VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof(mbi)))
			return false;
		if (mbi.State != MEM_COMMIT)
			return false;
		if (mbi.Protect & PAGE_GUARD)
			return false;
		const DWORD protect = mbi.Protect & 0xFF;
		switch (protect) {
		case PAGE_READONLY:
		case PAGE_READWRITE:
		case PAGE_WRITECOPY:
		case PAGE_EXECUTE_READ:
		case PAGE_EXECUTE_READWRITE:
		case PAGE_EXECUTE_WRITECOPY:
			break;
		default:
			return false;
		}
		const uintptr_t regionEnd =
			reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
		return address + size <= regionEnd;
	}

	bool IsWritable(uintptr_t address, size_t size) {
		if (!address || !size)
			return false;
		MEMORY_BASIC_INFORMATION mbi{};
		if (!VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof(mbi)))
			return false;
		if (mbi.State != MEM_COMMIT)
			return false;
		if (mbi.Protect & PAGE_GUARD)
			return false;
		const DWORD protect = mbi.Protect & 0xFF;
		switch (protect) {
		case PAGE_READWRITE:
		case PAGE_WRITECOPY:
		case PAGE_EXECUTE_READWRITE:
		case PAGE_EXECUTE_WRITECOPY:
			break;
		default:
			return false;
		}
		const uintptr_t regionEnd =
			reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
		return address + size <= regionEnd;
	}

	bool IsExecutable(uintptr_t address) {
		if (!address)
			return false;
		MEMORY_BASIC_INFORMATION mbi{};
		if (!VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof(mbi)))
			return false;
		if (mbi.State != MEM_COMMIT)
			return false;
		const DWORD protect = mbi.Protect & 0xFF;
		return protect == PAGE_EXECUTE
			|| protect == PAGE_EXECUTE_READ
			|| protect == PAGE_EXECUTE_READWRITE
			|| protect == PAGE_EXECUTE_WRITECOPY;
	}

	bool SafeReadU8(uintptr_t address, uint8_t& out) {
		if (!address)
			return false;
		uint8_t tmp = 0;
		if (!GuardedCopy(&tmp, reinterpret_cast<const void*>(address), sizeof(tmp)))
			return false;
		out = tmp;
		return true;
	}

	bool SafeReadU16(uintptr_t address, uint16_t& out) {
		if (!address)
			return false;
		uint16_t tmp = 0;
		if (!GuardedCopy(&tmp, reinterpret_cast<const void*>(address), sizeof(tmp)))
			return false;
		out = tmp;
		return true;
	}

	bool SafeReadU32(uintptr_t address, uint32_t& out) {
		if (!address)
			return false;
		uint32_t tmp = 0;
		if (!GuardedCopy(&tmp, reinterpret_cast<const void*>(address), sizeof(tmp)))
			return false;
		out = tmp;
		return true;
	}

	bool SafeReadBytes(uintptr_t address, void* out, size_t size) {
		if (!address || !out || !size)
			return false;
		return GuardedCopy(out, reinterpret_cast<const void*>(address), size);
	}

	bool SafeWriteU8(uintptr_t address, uint8_t value) {
		if (!address)
			return false;
		return ProtectedWrite(address, value);
	}

	bool SafeWriteU16(uintptr_t address, uint16_t value) {
		if (!address)
			return false;
		return ProtectedWrite(address, value);
	}

	bool SafeWriteU32(uintptr_t address, uint32_t value) {
		if (!address)
			return false;
		return ProtectedWrite(address, value);
	}

	bool SafeWriteBytes(uintptr_t address, const void* data, size_t size) {
		if (!address || !data || !size)
			return false;
		if (IsWritable(address, size))
			return GuardedCopy(reinterpret_cast<void*>(address), data, size);
		DWORD oldProtect = 0;
		if (!VirtualProtect(reinterpret_cast<void*>(address), size, PAGE_READWRITE, &oldProtect))
			return false;
		const bool ok = GuardedCopy(reinterpret_cast<void*>(address), data, size);
		DWORD restored = 0;
		VirtualProtect(reinterpret_cast<void*>(address), size, oldProtect, &restored);
		return ok;
	}

	bool SafeCompareExchangeU32(uintptr_t address, uint32_t expected, uint32_t desired) {
		if (!address)
			return false;
		// A locked cmpxchg spanning two cache lines is a split lock, which
		// some CPUs/OS configurations fault on; fall back to a checked write.
		if ((address & 63) <= 60)
			return GuardedCompareExchange(reinterpret_cast<volatile LONG*>(address),
				static_cast<LONG>(expected), static_cast<LONG>(desired));
		uint32_t current = 0;
		return GuardedCopy(&current, reinterpret_cast<const void*>(address), 4) && current == expected
			&& GuardedCopy(reinterpret_cast<void*>(address), &desired, 4);
	}
}
