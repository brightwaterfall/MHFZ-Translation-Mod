#include "logging/Logger.h"
#include "config/Config.h"
#include "translation/TranslationStore.h"
#include "translation/HeadersStore.h"
#include "translation/BufferLocator.h"
#include "translation/TextApplicator.h"
#include "translation/GameCodec.h"
#include <Windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {
	void WriteU32(uint8_t* base, uint32_t off, uint32_t value) {
		std::memcpy(base + off, &value, 4);
	}

	uint32_t ReadU32(const uint8_t* base, uint32_t off) {
		uint32_t v = 0;
		std::memcpy(&v, base + off, 4);
		return v;
	}

	std::string ReadCString(const uint8_t* base, uint32_t off) {
		std::string s;
		for (size_t i = off; base[i] != 0 && i < off + 4096; ++i)
			s.push_back(static_cast<char>(base[i]));
		return s;
	}

	int Fail(const char* msg) {
		std::printf("FAIL: %s\n", msg);
		return 1;
	}
}

int main() {
	CreateDirectoryA("mods", nullptr);
	Logger::Init();
	Config::Get().inPlaceOnly = false; // smoke exercises relocation path
	Config::Get().safeIndexApply = true;
	std::printf("=== MHFZTextPatch offline smoke ===\n");

	// --- Codec unit checks ---
	{
		const std::string in = "Hello {c10}World{/c}";
		const std::string mid = GameCodec::ColorCodesFromCsv(in);
		if (mid.find("~C10") == std::string::npos || mid.find("~C00") == std::string::npos)
			return Fail("color codec");
		const std::string sjis = GameCodec::Utf8ToSjis(mid);
		if (sjis.empty())
			return Fail("utf8->sjis");
		const auto parts = GameCodec::SplitJoin("A{j}B{j}C");
		if (parts.size() != 3 || parts[1] != "B")
			return Fail("join split");
		std::printf("OK codec\n");
	}

	// --- Export check via LoadLibrary on sibling Release DLL ---
	{
		char self[MAX_PATH] = {};
		GetModuleFileNameA(nullptr, self, MAX_PATH);
		std::string dir(self);
		const size_t slash = dir.find_last_of("\\/");
		if (slash != std::string::npos)
			dir.resize(slash + 1);
		const std::string dllPath = dir + "MHFZTextPatch.dll";
		HMODULE mod = LoadLibraryA(dllPath.c_str());
		if (!mod) {
			// Try solution Release folder relative to tools output
			mod = LoadLibraryA("..\\Release\\MHFZTextPatch.dll");
		}
		if (!mod)
			mod = LoadLibraryA("MHFZTextPatch.dll");
		if (!mod) {
			std::printf("WARN: could not LoadLibrary MHFZTextPatch.dll (export check skipped)\n");
		} else {
			FARPROC createMod = GetProcAddress(mod, "createMod");
			FARPROC setDll = GetProcAddress(mod, "setDllAddress");
			if (!createMod || !setDll) {
				FreeLibrary(mod);
				return Fail("GetProcAddress createMod/setDllAddress");
			}
			std::printf("OK exports createMod=%p setDllAddress=%p\n", createMod, setDll);
			FreeLibrary(mod);
		}
	}

	// --- Synthetic dat image + live apply ---
	constexpr size_t kImageSize = 8u * 1024u * 1024u;
	auto* image = static_cast<uint8_t*>(VirtualAlloc(nullptr, kImageSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
	if (!image)
		return Fail("VirtualAlloc image");
	std::memset(image, 0, kImageSize);

	// Fingerprints for BufferLocator::ScoreCandidate (dat): 0x64, 0x68, 0x100
	const uint32_t tableArmorHead = 0x20000;
	const uint32_t tableArmorBody = 0x21000;
	const uint32_t tableItems = 0x22000;
	const uint32_t strPool = 0x30000;

	WriteU32(image, 0x64, tableArmorHead);
	WriteU32(image, 0x68, tableArmorBody);
	WriteU32(image, 0x100, tableItems); // dat/items/name begin_pointer

	auto putSjis = [&](uint32_t off, const char* utf8) {
		const std::string sj = GameCodec::Utf8ToSjis(utf8);
		std::memcpy(image + off, sj.data(), sj.size());
		image[off + sj.size()] = 0;
		return static_cast<uint32_t>(sj.size());
	};

	// Armor head fingerprint strings
	WriteU32(image, tableArmorHead, strPool);
	putSjis(strPool, "头甲");
	WriteU32(image, tableArmorBody, strPool + 0x40);
	putSjis(strPool + 0x40, "胴甲");

	// Item name table: entry_count in headers is huge; we only need a few slots.
	// Index 0 unused, index 1..3 used (matches Mogapedia min index 1).
	uint32_t cursor = strPool + 0x100;
	for (int i = 0; i < 8; ++i) {
		WriteU32(image, tableItems + static_cast<uint32_t>(i * 4), cursor);
		char buf[64];
		_snprintf_s(buf, _TRUNCATE, "道具%02d", i);
		const uint32_t len = putSjis(cursor, buf);
		cursor += len + 8; // keep slack for longer EN
	}

	// Write a tiny translations JSON for items 1..3
	CreateDirectoryA("data", nullptr);
	const char* miniJson =
		"{\"en\":{\"dat/items/name\":["
		"{\"index\":\"1\",\"target\":\"Potion\"},"
		"{\"index\":\"2\",\"target\":\"Mega Potion\"},"
		"{\"index\":\"3\",\"target\":\"{c10}Max Potion{/c}\"}"
		"]}}";
	FILE* jf = nullptr;
	fopen_s(&jf, "data\\smoke-translations.json", "wb");
	if (!jf)
		return Fail("write smoke-translations.json");
	fputs(miniJson, jf);
	fclose(jf);

	// Mini headers: keep entry_count tiny so the smoke image stays dense/non-null.
	const char* miniHeaders =
		"{\"dat\":{\"items\":{\"name\":{"
		"\"begin_pointer\":\"0x100\","
		"\"entry_count\":8,"
		"\"max_sub_count\":1,"
		"\"null_padding\":true"
		"}}}}";
	FILE* hf = nullptr;
	fopen_s(&hf, "data\\smoke-headers.json", "wb");
	if (!hf)
		return Fail("write smoke-headers.json");
	fputs(miniHeaders, hf);
	fclose(hf);

	if (!HeadersStore::LoadFromFile("data\\smoke-headers.json"))
		return Fail("HeadersStore::LoadFromFile");
	if (!TranslationStore::LoadFromFile("data\\smoke-translations.json"))
		return Fail("TranslationStore::LoadFromFile");

	BufferLocator::Clear();
	BufferLocator::InjectImage("dat", reinterpret_cast<uintptr_t>(image), kImageSize, false);

	auto st = TextApplicator::ApplyLive();
	std::printf("ApplyLive written=%u attempted=%u sectionsOk=%u sectionsFail=%u fitSkip=%u resolveSkip=%u reloc=%u\n",
		st.written, st.attempted, st.sectionsOk, st.sectionsFail, st.skippedFit, st.skippedResolve, st.relocated);

	if (st.written < 3)
		return Fail("expected >=3 writes for item names 1..3");

	const uint32_t p1 = ReadU32(image, tableItems + 4);
	const uint32_t p2 = ReadU32(image, tableItems + 8);
	const uint32_t p3 = ReadU32(image, tableItems + 12);
	const std::string s1 = ReadCString(image, p1);
	const std::string s2 = ReadCString(image, p2);
	const std::string s3 = ReadCString(image, p3);
	const std::string e1 = GameCodec::Utf8ToSjis("Potion");
	const std::string e2 = GameCodec::Utf8ToSjis("Mega Potion");
	const std::string e3 = GameCodec::Utf8ToSjis(GameCodec::ColorCodesFromCsv("{c10}Max Potion{/c}"));

	std::printf("slot1 bytes=%zu expected=%zu, slot2=%zu/%zu, slot3=%zu/%zu\n",
		s1.size(), e1.size(), s2.size(), e2.size(), s3.size(), e3.size());
	if (s1 != e1)
		return Fail("item index 1 content mismatch");
	if (s2 != e2)
		return Fail("item index 2 content mismatch");
	if (s3 != e3)
		return Fail("item index 3 color content mismatch");

	VirtualFree(image, 0, MEM_RELEASE);
	std::printf("OK live apply on synthetic dat image\n");
	std::printf("=== ALL SMOKE CHECKS PASSED ===\n");
	Logger::Shutdown();
	return 0;
}
