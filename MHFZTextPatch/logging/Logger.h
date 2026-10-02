#pragma once
#include <string>

namespace Logger {
	void Init();
	void Info(const char* fmt, ...);
	void Warn(const char* fmt, ...);
	void Error(const char* fmt, ...);
	void Shutdown();
}
