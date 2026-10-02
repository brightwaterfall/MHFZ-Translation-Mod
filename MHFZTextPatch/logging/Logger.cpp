#include "Logger.h"
#include <Windows.h>
#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace {
	std::mutex g_mutex;
	FILE* g_file = nullptr;
	bool g_consoleReady = false;

	void EnsureConsole() {
		if (g_consoleReady)
			return;
		// Pax Loader usually already owns a console; AttachConsole is best-effort.
		if (!AttachConsole(ATTACH_PARENT_PROCESS)) {
			// Do not allocate a new console unless stdout is already available.
		}
		g_consoleReady = true;
	}

	void WriteLine(const char* level, const char* message) {
		std::lock_guard<std::mutex> lock(g_mutex);
		EnsureConsole();

		SYSTEMTIME st{};
		GetLocalTime(&st);

		char line[2048];
		_snprintf_s(line, _TRUNCATE,
			"[%02d:%02d:%02d] [TEXTPATCH] [%s] %s\n",
			st.wHour, st.wMinute, st.wSecond, level, message);

		fputs(line, stdout);
		fflush(stdout);

		if (g_file) {
			fputs(line, g_file);
			fflush(g_file);
		}

		OutputDebugStringA(line);
	}

	void FormatAndWrite(const char* level, const char* fmt, va_list args) {
		char message[1800];
		_vsnprintf_s(message, _TRUNCATE, fmt, args);
		WriteLine(level, message);
	}
}

namespace Logger {
	void Init() {
		std::lock_guard<std::mutex> lock(g_mutex);
		if (!g_file) {
			fopen_s(&g_file, "./mods/mhfz_text_patch.log", "a");
		}
	}

	void Info(const char* fmt, ...) {
		va_list args;
		va_start(args, fmt);
		FormatAndWrite("INFO", fmt, args);
		va_end(args);
	}

	void Warn(const char* fmt, ...) {
		va_list args;
		va_start(args, fmt);
		FormatAndWrite("WARN", fmt, args);
		va_end(args);
	}

	void Error(const char* fmt, ...) {
		va_list args;
		va_start(args, fmt);
		FormatAndWrite("ERROR", fmt, args);
		va_end(args);
	}

	void Shutdown() {
		std::lock_guard<std::mutex> lock(g_mutex);
		if (g_file) {
			fclose(g_file);
			g_file = nullptr;
		}
	}
}
