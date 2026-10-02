#pragma once
#include <Windows.h>

inline int mhfdll_addy = 0;

inline void OffsetByDll(DWORD& addy) {
	addy += static_cast<DWORD>(mhfdll_addy);
}

extern "C" __declspec(dllexport) void setDllAddress(int dll_addy);
