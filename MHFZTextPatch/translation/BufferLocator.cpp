#include "BufferLocator.h"
#include "../logging/Logger.h"
#include "../memory/MemoryUtils.h"
#include <Windows.h>
#include <cstring>
#include <vector>

namespace {
	std::unordered_map<std::string, BufferLocator::Image> g_images;

	bool ReadU32(uintptr_t addr, uint32_t& out) {
		return MemoryUtils::SafeReadU32(addr, out);
	}

	bool LooksLikeSjisCString(uintptr_t addr, size_t minLen = 2, size_t maxLen = 256) {
		size_t n = 0;
		for (; n < maxLen; ++n) {
			uint8_t b = 0;
			if (!MemoryUtils::SafeReadU8(addr + n, b))
				return false;
			if (b == 0)
				return n >= minLen;
			if (b < 0x20 && b != '\t' && b != '\n' && b != '\r')
				return false;
		}
		return false;
	}

	uintptr_t ResolveInImage(uintptr_t base, size_t size, uint32_t stored, int& absVotes, int& relVotes) {
		if (!stored)
			return 0;
		// File-relative offset (typical for still-packed/decompressed images).
		if (stored < size) {
			++relVotes;
			return base + stored;
		}
		// Absolute VA into this image (some loaders relocate pointers).
		if (stored >= base && stored < base + size) {
			++absVotes;
			return stored;
		}
		return 0;
	}

	struct ScoreResult {
		bool ok = false;
		int score = 0;
		bool absolute = false;
		int denseHits = 0;
	};

	struct SizeHint {
		size_t expected = 0;
		size_t minSize = 0;
		size_t maxSize = 0;
	};

	SizeHint HintFor(const char* fileKey) {
		// Prefer *decompressed* in-memory sizes (ECD expands ~4x on TW).
		// Keep max tight so we do not claim a huge shared region and walk past the real bin.
		if (strcmp(fileKey, "dat") == 0)
			return {11632120u, 8u * 1024u * 1024u, 14u * 1024u * 1024u};
		if (strcmp(fileKey, "pac") == 0)
			return {1070304u, 512u * 1024u, 2u * 1024u * 1024u};
		if (strcmp(fileKey, "inf") == 0)
			return {1558336u, 512u * 1024u, 3u * 1024u * 1024u};
		if (strcmp(fileKey, "gao") == 0)
			return {210176u, 128u * 1024u, 512u * 1024u};
		if (strcmp(fileKey, "jmp") == 0)
			return {3296u, 2u * 1024u, 64u * 1024u};
		if (strcmp(fileKey, "rcc") == 0)
			return {2304u, 2u * 1024u, 64u * 1024u};
		return {0, 4u * 1024u, 256u * 1024u * 1024u};
	}

	size_t ClampClaimedSize(size_t avail, const SizeHint& hint) {
		if (!hint.expected || avail <= hint.expected)
			return avail;
		// Region often oversized; never walk past ~125% of known decompressed size.
		const size_t cap = hint.expected + (hint.expected / 4);
		if (avail > cap)
			return cap;
		return avail;
	}

	int SizeBonus(size_t avail, const SizeHint& hint) {
		if (!hint.expected)
			return 0;
		if (avail < hint.minSize || avail > hint.maxSize)
			return -1000;
		const int64_t diff = static_cast<int64_t>(avail) - static_cast<int64_t>(hint.expected);
		const int64_t ad = diff < 0 ? -diff : diff;
		const int64_t tol = static_cast<int64_t>(hint.expected / 4); // 25%
		if (ad <= tol)
			return 100;
		if (ad <= tol * 2)
			return 50;
		return 10; // still in band
	}

	ScoreResult ScoreCandidate(uintptr_t base, size_t size, const char* fileKey) {
		ScoreResult r;
		struct Probe {
			uint32_t beginPtrOff;
		};
		std::vector<Probe> probes;
		uint32_t densityBegin = 0;
		int densityNeed = 0;
		size_t minSize = 0;
		size_t maxSize = 0;

		const SizeHint hint = HintFor(fileKey);
		if (strcmp(fileKey, "dat") == 0) {
			// TW: items/name @0x100 is broken; armor slots @0x64+ are reliable.
			probes = { {0x64}, {0x68}, {0x6C}, {0x70}, {0x74}, {0x84}, {0x88} };
			densityBegin = 0x64;
			densityNeed = 12;
		} else if (strcmp(fileKey, "pac") == 0) {
			probes = { {0x18}, {0x1C}, {0x28}, {0x30}, {0x40} };
			densityBegin = 0x28;
			densityNeed = 4;
		} else if (strcmp(fileKey, "inf") == 0) {
			probes = { {0x14} };
		} else if (strcmp(fileKey, "gao") == 0) {
			probes = { {0x20}, {0x28} };
		} else if (strcmp(fileKey, "jmp") == 0) {
			probes = { {0x0C} };
		} else if (strcmp(fileKey, "rcc") == 0) {
			probes = { {0x08} };
		} else {
			return r;
		}
		minSize = hint.minSize;
		maxSize = hint.maxSize;

		if (size < minSize)
			return r;
		const size_t effectiveSize = ClampClaimedSize(size, hint);
		if (effectiveSize < minSize || (maxSize && effectiveSize > maxSize))
			return r;

		int absVotes = 0;
		int relVotes = 0;
		int score = 0;
		uint32_t prevTable = 0;
		int ordered = 0;

		// Score only within the clamped claim window so we don't credit
		// pointers that live past the size we will actually publish.
		for (const auto& p : probes) {
			if (static_cast<size_t>(p.beginPtrOff) + 4 > effectiveSize)
				continue;
			uint32_t tableStored = 0;
			if (!ReadU32(base + p.beginPtrOff, tableStored) || tableStored == 0)
				continue;

			uintptr_t tableAddr = ResolveInImage(base, effectiveSize, tableStored, absVotes, relVotes);
			if (!tableAddr)
				continue;

			// Soft check: header table pointers often increase for adjacent armor slots.
			if (prevTable && tableStored > prevTable)
				++ordered;
			prevTable = tableStored;

			uint32_t strStored = 0;
			if (!ReadU32(tableAddr, strStored) || strStored == 0)
				continue;
			uintptr_t strAddr = ResolveInImage(base, effectiveSize, strStored, absVotes, relVotes);
			if (!strAddr)
				continue;
			if (LooksLikeSjisCString(strAddr))
				++score;
		}

		int denseHits = 0;
		if (densityBegin && densityNeed > 0 && densityBegin + 4 <= effectiveSize) {
			uint32_t tableStored = 0;
			if (ReadU32(base + densityBegin, tableStored) && tableStored) {
				uintptr_t tableAddr = ResolveInImage(base, effectiveSize, tableStored, absVotes, relVotes);
				if (tableAddr) {
					for (int i = 0; i < densityNeed * 2 && denseHits < densityNeed; ++i) {
						uint32_t strStored = 0;
						if (!ReadU32(tableAddr + static_cast<uintptr_t>(i * 4), strStored) || !strStored)
							continue;
						uintptr_t strAddr = ResolveInImage(base, effectiveSize, strStored, absVotes, relVotes);
						if (strAddr && LooksLikeSjisCString(strAddr, 1, 128))
							++denseHits;
					}
				}
			}
		}

		const int need = (strcmp(fileKey, "dat") == 0 || strcmp(fileKey, "pac") == 0) ? 2 : 1;
		if (score < need)
			return r;
		if (densityNeed > 0 && denseHits < (densityNeed / 2))
			return r;

		const int sizeBonus = SizeBonus(effectiveSize, hint);
		if (sizeBonus <= -1000)
			return r;

		r.ok = true;
		r.score = score * 10 + denseHits + ordered + sizeBonus;
		r.absolute = absVotes > relVotes;
		r.denseHits = denseHits;
		return r;
	}

	void ScanForKey(const char* fileKey, size_t minSizeHint) {
		SYSTEM_INFO si{};
		GetSystemInfo(&si);
		uintptr_t addr = reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress);
		const uintptr_t maxAddr = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);

		BufferLocator::Image best{};
		int bestScore = -1;

		while (addr < maxAddr) {
			MEMORY_BASIC_INFORMATION mbi{};
			if (!VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi)))
				break;
			const uintptr_t region = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
			const size_t regionSize = mbi.RegionSize;
			uintptr_t next = region + regionSize;
			if (next <= addr)
				break;

			const bool committed = mbi.State == MEM_COMMIT;
			const DWORD prot = mbi.Protect & 0xFF;
			const bool readable =
				prot == PAGE_READONLY ||
				prot == PAGE_READWRITE ||
				prot == PAGE_EXECUTE_READ ||
				prot == PAGE_EXECUTE_READWRITE ||
				prot == PAGE_WRITECOPY ||
				prot == PAGE_EXECUTE_WRITECOPY;

			if (committed && readable && !(mbi.Protect & PAGE_GUARD) && regionSize >= minSizeHint) {
				const uintptr_t candidates[] = {
					region,
					region + 0x1000,
					region + 0x10000,
				};
				for (uintptr_t cand : candidates) {
					if (cand >= region + regionSize)
						continue;
					const size_t avail = region + regionSize - cand;
					if (avail < minSizeHint)
						continue;
					// Never reuse a region already claimed by another file key.
					bool claimed = false;
					for (const auto& kv : g_images) {
						const auto& img = kv.second;
						const size_t claimAvail = ClampClaimedSize(avail, HintFor(fileKey));
						if (cand < img.base + img.size && img.base < cand + claimAvail) {
							claimed = true;
							break;
						}
					}
					if (claimed)
						continue;
					ScoreResult sc = ScoreCandidate(cand, avail, fileKey);
					if (!sc.ok)
						continue;
					const SizeHint hint = HintFor(fileKey);
					const size_t claim = ClampClaimedSize(avail, hint);
					// Prefer tighter size matches so a 27MB shared region loses to ~11MB.
					int score = sc.score;
					if (hint.expected) {
						const int64_t diff = static_cast<int64_t>(claim) - static_cast<int64_t>(hint.expected);
						const int64_t ad = diff < 0 ? -diff : diff;
						if (ad <= static_cast<int64_t>(hint.expected / 20))
							score += 40;
					}
					if (score > bestScore) {
						bestScore = score;
						best.base = cand;
						best.size = claim;
						best.absolutePointers = sc.absolute;
					}
				}
			}
			addr = next;
		}

		if (best.base) {
			g_images[fileKey] = best;
			Logger::Info("Located %s image at 0x%08X size=0x%X abs=%d score=%d",
				fileKey,
				static_cast<unsigned>(best.base),
				static_cast<unsigned>(best.size),
				best.absolutePointers ? 1 : 0,
				bestScore);
		} else {
			Logger::Warn("No %s image located", fileKey);
		}
	}
}

namespace BufferLocator {
	bool Refresh() {
		g_images.clear();
		ScanForKey("dat", 8 * 1024 * 1024);
		ScanForKey("pac", 512 * 1024);
		ScanForKey("inf", 512 * 1024);
		ScanForKey("gao", 128 * 1024);
		ScanForKey("jmp", 2 * 1024);
		ScanForKey("rcc", 2 * 1024);
		Logger::Info("Buffer locate: %u images found", static_cast<unsigned>(g_images.size()));
		return !g_images.empty();
	}

	const Image* Get(const std::string& fileKey) {
		const auto it = g_images.find(fileKey);
		if (it == g_images.end())
			return nullptr;
		return &it->second;
	}

	size_t FoundCount() { return g_images.size(); }

	void InjectImage(const std::string& fileKey, uintptr_t base, size_t size, bool absolutePointers) {
		Image img;
		img.base = base;
		img.size = size;
		img.absolutePointers = absolutePointers;
		g_images[fileKey] = img;
		Logger::Info("Injected %s image at 0x%08X size=0x%X abs=%d",
			fileKey.c_str(),
			static_cast<unsigned>(base),
			static_cast<unsigned>(size),
			absolutePointers ? 1 : 0);
	}

	void Clear() { g_images.clear(); }
}
