// Offline harness for ContentPatcher against the bins embedded in HY5.exe.
//
// Layouts simulated (mirrors what the live logs showed):
//   dat: pair slots relocated in place to absolute pointers
//   pac: slots left file-relative, plus a heap "moved table" of absolute
//        pointers (the live pac tables point outside the image)
//   gao/jmp/rcc: file-relative only (in-place writes are the only option)
// A lone decoy pointer with no neighbours must be left untouched.
// Afterwards every image byte is diffed against the original file: the only
// allowed changes are in-place target writes and the redirected slots.

#include "translation/ContentPatcher.h"
#include "logging/Logger.h"
#include "include/json.hpp"
#include <Windows.h>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

using json = nlohmann::json;

namespace {
	// `orig` lives in read-only pages: the patcher may locate it but must never write it.
	struct Image {
		std::string key;
		const uint8_t* orig = nullptr;
		size_t size = 0;
		uint8_t* mem = nullptr;
	};

	std::vector<uint8_t> ReadFile(const std::string& path) {
		std::ifstream in(path, std::ios::binary);
		return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	}

	std::string Hex(const std::string& hex, uint8_t mask = 0) {
		std::string out(hex.size() / 2, '\0');
		for (size_t i = 0; i < out.size(); ++i)
			out[i] = static_cast<char>(std::stoi(hex.substr(2 * i, 2), nullptr, 16) ^ mask);
		return out;
	}

	// The harness's own copies of source text stay masked so the patcher's
	// content search cannot find (and rewrite) them.
	constexpr uint8_t kHarnessMask = 0x3C;

	std::string Unmask(const std::string& s) {
		std::string out = s;
		for (char& c : out)
			c = static_cast<char>(static_cast<uint8_t>(c) ^ kHarnessMask);
		return out;
	}

	uint32_t U32(const uint8_t* p) {
		uint32_t v;
		memcpy(&v, p, 4);
		return v;
	}

	int g_failures = 0;
	void Check(bool ok, const char* what) {
		if (!ok) {
			++g_failures;
			printf("FAIL: %s\n", what);
		}
	}
}

int main(int argc, char** argv) {
	setvbuf(stdout, nullptr, _IONBF, 0);
	const std::string root = argc > 1 ? std::string(argv[1]) + "\\" : "..\\";
	CreateDirectoryA("mods", nullptr);
	Logger::Init();

	const char* keys[] = { "dat", "pac", "gao", "jmp", "rcc" };
	std::map<std::string, Image> images;
	for (const char* k : keys) {
		Image img;
		img.key = k;
		{
			std::vector<uint8_t> file = ReadFile(root + "client\\live_decompressed\\mhf" + k + ".bin");
			if (file.empty()) {
				printf("missing decompressed %s\n", k);
				return 2;
			}
			img.size = file.size();
			auto* ro = static_cast<uint8_t*>(VirtualAlloc(nullptr, img.size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
			memcpy(ro, file.data(), img.size);
			DWORD old = 0;
			VirtualProtect(ro, img.size, PAGE_READONLY, &old);
			img.orig = ro;
			img.mem = static_cast<uint8_t*>(HeapAlloc(GetProcessHeap(), 0, img.size + 64));
			memcpy(img.mem, file.data(), img.size);
			SecureZeroMemory(file.data(), file.size());
		}
		images[k] = std::move(img);
	}

	json doc;
	try {
		std::ifstream(root + "data\\pairs-tw.json") >> doc;
	} catch (const std::exception& ex) {
		printf("pairs json: %s\n", ex.what());
		return 2;
	}
	printf("loaded %zu pair rows\n", doc["pairs"].size());

	struct Expect {
		std::string key, src, tgt;
		std::vector<uint32_t> offs, slots;
		bool redirectOnly = false;
		bool Fits() const { return !redirectOnly && tgt.size() <= src.size(); }
	};
	std::vector<Expect> pairs;
	for (const auto& row : doc["pairs"]) {
		Expect e{ row["k"], Hex(row["s"], kHarnessMask), Hex(row["t"]) };
		e.redirectOnly = row.value("r", 0) != 0;
		for (auto& o : row["o"]) e.offs.push_back(o);
		for (auto& s : row["p"]) e.slots.push_back(s);
		pairs.push_back(std::move(e));
	}

	// dat: relocate pair slots to absolute pointers in place.
	std::set<uint32_t> datSlots;
	for (const auto& e : pairs) {
		if (e.key != "dat") continue;
		Image& img = images["dat"];
		for (uint32_t s : e.slots) {
			const uint32_t rel = U32(img.mem + s);
			const uint32_t abs = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(img.mem)) + rel;
			memcpy(img.mem + s, &abs, 4);
			datSlots.insert(s);
		}
	}

	// pac: moved table of absolute pointers on the heap, original slots stay relative.
	std::vector<std::pair<uint32_t, const Expect*>> pacMoved;
	for (const auto& e : pairs)
		if (e.key == "pac")
			pacMoved.emplace_back(e.offs[0], &e);
	auto* moved = static_cast<uint32_t*>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, pacMoved.size() * 4 + 16));
	for (size_t i = 0; i < pacMoved.size(); ++i)
		moved[i] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(images["pac"].mem)) + pacMoved[i].first;

	// Decoy: one isolated absolute pointer to a long dat string, padded by junk.
	const Expect* decoyPair = nullptr;
	for (const auto& e : pairs)
		if (e.key == "dat" && !e.Fits()) { decoyPair = &e; break; }
	auto* decoyBlock = static_cast<uint32_t*>(HeapAlloc(GetProcessHeap(), 0, 4096));
	for (int i = 0; i < 1024; ++i) decoyBlock[i] = 0x11111111u * (i % 7);
	const uint32_t decoyValue = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(images["dat"].mem)) + decoyPair->offs[0];
	decoyBlock[512] = decoyValue;

	// Foreign layout: copies of dat strings outside any file image with a
	// contiguous pointer table, as a different client build would hold them.
	std::map<std::string, int> srcCount;
	for (const auto& e : pairs)
		srcCount[e.src]++;
	std::vector<const Expect*> foreignLong;
	const Expect* foreignFit = nullptr;
	for (const auto& e : pairs) {
		if (e.key != "dat" || srcCount[e.src] != 1 || e.src.size() < 4
			|| static_cast<uint8_t>(e.src[0] ^ kHarnessMask) < 0x80)
			continue;
		if (!e.Fits() && foreignLong.size() < 64)
			foreignLong.push_back(&e);
		else if (e.Fits() && !foreignFit)
			foreignFit = &e;
	}
	auto* foreign = static_cast<uint8_t*>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 64 * 1024));
	auto* foreignTable = reinterpret_cast<uint32_t*>(foreign);
	size_t cursor = 1024;
	for (size_t i = 0; i < foreignLong.size(); ++i) {
		const std::string raw = Unmask(foreignLong[i]->src);
		memcpy(foreign + cursor, raw.data(), raw.size());
		foreignTable[i] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(foreign + cursor));
		cursor += raw.size() + 1;
	}
	const size_t foreignFitOff = ++cursor;
	if (foreignFit) {
		const std::string raw = Unmask(foreignFit->src);
		memcpy(foreign + foreignFitOff, raw.data(), raw.size());
	}

	auto checksum = [](const uint8_t* p, size_t n) {
		uint64_t h = 1469598103934665603ull;
		for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
		return h;
	};
	std::map<std::string, uint64_t> origSums;
	for (auto& [key, img] : images)
		origSums[key] = checksum(img.orig, img.size);

	auto allocOf = [](const void* p) {
		MEMORY_BASIC_INFORMATION mbi{};
		VirtualQuery(p, &mbi, sizeof(mbi));
		return static_cast<unsigned>(reinterpret_cast<uintptr_t>(mbi.AllocationBase));
	};
	printf("layouts prepared (moved table alloc 0x%08X, foreign alloc 0x%08X, decoy alloc 0x%08X), loading patcher\n",
		allocOf(moved), allocOf(foreign), allocOf(decoyBlock));
	if (!ContentPatcher::Load(root + "data\\pairs-tw.json")) {
		printf("Load failed\n");
		return 2;
	}
	printf("patcher loaded %zu pairs, applying\n", ContentPatcher::PairCount());
	const auto s1 = ContentPatcher::Apply();
	printf("pass 1 done\n");
	const auto s2 = ContentPatcher::Apply();
	printf("pass1: images=%u verified=%u inPlace=%u redirected=%u missing=%u unreferenced=%u rejected=%u %ums\n",
		s1.imagesFound, s1.verified, s1.inPlace, s1.redirected, s1.missing, s1.unreferenced, s1.slotsRejected, s1.scanMs);
	printf("pass2: images=%u verified=%u inPlace=%u redirected=%u already=%u missing=%u %ums\n",
		s2.imagesFound, s2.verified, s2.inPlace, s2.redirected, s2.alreadyApplied, s2.missing, s2.scanMs);
	printf("images: %s\n", ContentPatcher::ImageSummary().c_str());

	for (auto& [key, img] : images) {
		char tag[64];
		snprintf(tag, sizeof(tag), "%s@0x%08X", key.c_str(), static_cast<unsigned>(reinterpret_cast<uintptr_t>(img.mem)));
		const bool found = ContentPatcher::ImageSummary().find(tag) != std::string::npos;
		printf("%s working copy located: %s\n", key.c_str(), found ? "yes" : "NO");
		Check(found, "working copy located");
		Check(checksum(img.orig, img.size) == origSums[key], "read-only copy untouched");
	}
	Check(s1.missing == 0, "no missing strings on pass 1");
	Check(s2.redirected == 0 && s2.inPlace == 0, "pass 2 is idempotent");

	auto stringAt = [](uint32_t addr) { return std::string(reinterpret_cast<const char*>(static_cast<uintptr_t>(addr))); };

	// Every pair's text must now read as the target via every slot / offset.
	size_t datOk = 0, datBad = 0, relOk = 0, relBad = 0, relSkipped = 0;
	for (const auto& e : pairs) {
		Image& img = images[e.key];
		const bool fits = e.Fits();
		for (uint32_t s : e.slots) {
			if (e.key == "dat") {
				const std::string now = stringAt(U32(img.mem + s));
				(now == e.tgt ? datOk : datBad)++;
				if (now != e.tgt)
					printf("  bad dat slot=0x%X offs0=0x%X srcLen=%zu tgtLen=%zu nowLen=%zu ptr=0x%08X\n", s, e.offs[0],
						e.src.size(), e.tgt.size(), now.size(), U32(img.mem + s));
			} else {
				const uint32_t rel = U32(img.mem + s);
				const uint32_t base = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(img.mem));
				const std::string now = stringAt(base + rel);
				if (!fits) relSkipped++;
				if (now == e.tgt) {
					relOk++;
				} else {
					relBad++;
					printf("  bad %s slot=0x%X rel=0x%X offs0=0x%X srcLen=%zu tgtLen=%zu nowLen=%zu\n", e.key.c_str(), s, rel,
						e.offs[0], e.src.size(), e.tgt.size(), now.size());
				}
			}
		}
	}
	printf("dat slots: ok=%zu bad=%zu | relative slots: ok=%zu bad=%zu (of which redirected long=%zu)\n",
		datOk, datBad, relOk, relBad, relSkipped);
	Check(datBad == 0, "every dat slot reads the target");
	Check(relBad == 0, "every relative slot reads the target");

	size_t movedOk = 0;
	for (size_t i = 0; i < pacMoved.size(); ++i) {
		if (stringAt(moved[i]) == pacMoved[i].second->tgt) {
			movedOk++;
		} else if (i - movedOk < 8) {
			const Expect& e = *pacMoved[i].second;
			printf("  bad pac moved[%zu] off=0x%X srcLen=%zu tgtLen=%zu redirectOnly=%d ptr=0x%08X nowLen=%zu\n", i,
				pacMoved[i].first, e.src.size(), e.tgt.size(), e.redirectOnly, moved[i], stringAt(moved[i]).size());
		}
	}
	printf("pac moved table: %zu/%zu read target\n", movedOk, pacMoved.size());
	Check(movedOk == pacMoved.size(), "moved pac table fully redirected");

	Check(decoyBlock[512] == decoyValue, "isolated decoy pointer untouched");

	size_t foreignOk = 0;
	for (size_t i = 0; i < foreignLong.size(); ++i)
		if (stringAt(foreignTable[i]) == foreignLong[i]->tgt) foreignOk++;
	printf("foreign layout: %zu/%zu table slots read target, fitting copy %s\n", foreignOk, foreignLong.size(),
		foreignFit && std::string(reinterpret_cast<const char*>(foreign + foreignFitOff)) == foreignFit->tgt ? "patched" : "NOT patched");
	Check(foreignOk == foreignLong.size() && !foreignLong.empty(), "foreign pointer table redirected by content search");
	Check(foreignFit && std::string(reinterpret_cast<const char*>(foreign + foreignFitOff)) == foreignFit->tgt,
		"foreign fitting copy written in place");

	// Byte diff: only in-place target regions and our own dat slots may change.
	for (auto& [key, img] : images) {
		std::vector<uint8_t> allowed(img.size, 0);
		for (const auto& e : pairs) {
			if (e.key != key) continue;
			if (e.Fits())
				for (uint32_t o : e.offs)
					for (size_t b = 0; b < e.src.size(); ++b) allowed[o + b] = 1;
			if (key == "dat" || !e.Fits())
				for (uint32_t s : e.slots)
					for (int b = 0; b < 4; ++b) allowed[s + b] = 1;
		}
		size_t illegal = 0;
		for (size_t i = 0; i < img.size; ++i)
			if (img.mem[i] != img.orig[i] && !allowed[i]) {
				if (illegal < 24)
					printf("  illegal %s +0x%zX orig=%02X now=%02X\n", key.c_str(), i, img.orig[i], img.mem[i]);
				illegal++;
			}
		printf("%s: illegal byte changes = %zu\n", key.c_str(), illegal);
		Check(illegal == 0, "no writes outside verified strings/slots");
	}

	printf(g_failures ? "RESULT: %d FAILURE(S)\n" : "RESULT: ALL CHECKS PASSED\n", g_failures);
	return g_failures ? 1 : 0;
}
