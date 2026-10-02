#include "ContentPatcher.h"
#include "../logging/Logger.h"
#include "../memory/MemoryUtils.h"
#include "../include/json.hpp"
#include <Windows.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using json = nlohmann::json;

namespace {
	constexpr size_t kChunk = 1u << 20;
	constexpr size_t kTail = 1024;
	constexpr size_t kMaxSrcLen = kTail - 2;
	constexpr size_t kMinContentLen = 4;
	constexpr size_t kMaxAnchorsPerFile = 96;
	constexpr size_t kMaxBasesPerFile = 4;
	constexpr size_t kMinRedirectAnchors = 8;
	constexpr uint32_t kNeighbourReach = 64;
	constexpr size_t kMinTableRun = 8;
	constexpr unsigned kMinDumpHits = 32;
	constexpr size_t kMaxDumpBytes = 96u << 20;
	// Our own copies of game data are kept masked so no scan can ever match
	// them: string addresses XOR kMask, source bytes XOR kSrcMask, index keys
	// XOR kMask / kMask64.
	constexpr uint32_t kMask = 0x5A3C96E1u;
	constexpr uint64_t kMask64 = 0x93C467E37DB0C7A4ull;
	constexpr uint8_t kSrcMask = 0xA7;

	struct Pair {
		uint8_t file = 0;
		std::string src; // masked with kSrcMask
		std::string tgt;
		std::vector<uint32_t> offs;
		std::vector<uint32_t> slots;
		uint32_t pooledMasked = 0; // pool address of tgt, XOR kMask
		// False when another string starts inside src (tail-merged text).
		bool inPlaceSafe = true;

		bool WritesInPlace() const { return inPlaceSafe && tgt.size() <= src.size(); }
	};

	struct FileImage {
		std::string key;
		uint32_t size = 0;
		std::vector<uint32_t> anchors; // pair indices
		std::vector<uint32_t> bases;
		bool dumped = false;
	};

	struct Region {
		uintptr_t base = 0;
		size_t size = 0;
		uintptr_t alloc = 0;
		bool writable = false;
	};

	struct Candidate {
		uintptr_t slot = 0;
		uint32_t maskedTarget = 0;
		uint32_t pair = 0;
		bool known = false;
	};

	std::vector<Pair> g_pairs;
	std::vector<FileImage> g_files;
	// First four source bytes (XOR kMask) -> pair indices, for content search.
	std::unordered_multimap<uint32_t, uint32_t> g_contentIndex;
	uint8_t* g_pool = nullptr;
	size_t g_poolSize = 0;
	uint8_t* g_scratch = nullptr;
	uint8_t* g_probe = nullptr;
	size_t g_probeSize = kTail;
	std::string g_dumpDir;
	ContentPatcher::Stats g_last;
	unsigned g_passes = 0;

	bool HexDecode(const std::string& hex, std::string& out) {
		if (hex.size() % 2)
			return false;
		out.resize(hex.size() / 2);
		for (size_t i = 0; i < out.size(); ++i) {
			auto nib = [](char c) -> int {
				if (c >= '0' && c <= '9') return c - '0';
				if (c >= 'a' && c <= 'f') return c - 'a' + 10;
				if (c >= 'A' && c <= 'F') return c - 'A' + 10;
				return -1;
			};
			const int hi = nib(hex[2 * i]);
			const int lo = nib(hex[2 * i + 1]);
			if (hi < 0 || lo < 0)
				return false;
			out[i] = static_cast<char>((hi << 4) | lo);
		}
		return true;
	}

	uint32_t Pooled(const Pair& p) {
		return p.pooledMasked ^ kMask;
	}

	uint8_t SrcByte(const Pair& p, size_t i) {
		return static_cast<uint8_t>(p.src[i]) ^ kSrcMask;
	}

	bool SrcMatches(const Pair& p, const uint8_t* mem) {
		for (size_t i = 0; i < p.src.size(); ++i)
			if ((mem[i] ^ kSrcMask) != static_cast<uint8_t>(p.src[i]))
				return false;
		return mem[p.src.size()] == 0;
	}

	uint32_t SrcKey32(const Pair& p) {
		uint32_t k = 0;
		for (size_t i = 0; i < 4; ++i)
			k |= static_cast<uint32_t>(SrcByte(p, i)) << (8 * i);
		return k ^ kMask;
	}

	uint64_t SrcKey64(const Pair& p) {
		uint64_t k = 0;
		for (size_t i = 0; i < 8; ++i)
			k |= static_cast<uint64_t>(SrcByte(p, i)) << (8 * i);
		return k ^ kMask64;
	}

	bool ReadableProtect(DWORD protect) {
		switch (protect & 0xFF) {
		case PAGE_READONLY:
		case PAGE_READWRITE:
		case PAGE_WRITECOPY:
		case PAGE_EXECUTE_READ:
		case PAGE_EXECUTE_READWRITE:
		case PAGE_EXECUTE_WRITECOPY:
			return true;
		default:
			return false;
		}
	}

	bool WritableProtect(DWORD protect) {
		switch (protect & 0xFF) {
		case PAGE_READWRITE:
		case PAGE_WRITECOPY:
		case PAGE_EXECUTE_READWRITE:
		case PAGE_EXECUTE_WRITECOPY:
			return true;
		default:
			return false;
		}
	}

	// Private committed memory, excluding our own buffers and every thread
	// stack (allocations carrying a guard page): swapping a pointer a running
	// function is iterating over could send it walking off into our pool.
	std::vector<Region> EnumRegions() {
		std::vector<MEMORY_BASIC_INFORMATION> all;
		std::unordered_set<const void*> guarded;

		SYSTEM_INFO si{};
		GetSystemInfo(&si);
		uintptr_t addr = reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress);
		const uintptr_t end = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);
		MEMORY_BASIC_INFORMATION mbi{};
		while (addr < end && VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi))) {
			const uintptr_t next = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
			if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE) {
				if (mbi.Protect & PAGE_GUARD)
					guarded.insert(mbi.AllocationBase);
				all.push_back(mbi);
			}
			if (next <= addr)
				break;
			addr = next;
		}

		MEMORY_BASIC_INFORMATION self{};
		VirtualQuery(&self, &self, sizeof(self));
		std::vector<Region> out;
		for (const auto& m : all) {
			if ((m.Protect & (PAGE_GUARD | PAGE_NOACCESS)) || !ReadableProtect(m.Protect))
				continue;
			if (guarded.count(m.AllocationBase) || m.AllocationBase == self.AllocationBase
				|| m.AllocationBase == g_pool || m.AllocationBase == g_scratch || m.AllocationBase == g_probe)
				continue;
			out.push_back({ reinterpret_cast<uintptr_t>(m.BaseAddress), m.RegionSize,
				reinterpret_cast<uintptr_t>(m.AllocationBase), WritableProtect(m.Protect) });
		}
		return out;
	}

	// Reads [addr, addr + size) into out; pages that fail (heap pages the game
	// decommitted since EnumRegions) read as zeros so the rest is still scanned.
	bool ReadChunk(uintptr_t addr, uint8_t* out, size_t size) {
		if (MemoryUtils::SafeReadBytes(addr, out, size))
			return true;
		constexpr size_t kPage = 0x1000;
		bool any = false;
		for (size_t done = 0; done < size;) {
			const size_t n = (std::min)(size - done, kPage - ((addr + done) & (kPage - 1)));
			if (MemoryUtils::SafeReadBytes(addr + done, out + done, n))
				any = true;
			else
				memset(out + done, 0, n);
			done += n;
		}
		return any;
	}

	// Calls fn(buf, mainLen, totalLen, chunkAddr, prevByte) per chunk; the
	// buffer carries `tail` extra bytes so matches can straddle chunk edges.
	template <typename Fn>
	void ScanRegion(const Region& r, size_t tail, Fn&& fn) {
		uint8_t prev = 0xFF;
		for (size_t off = 0; off < r.size; off += kChunk) {
			const size_t mainLen = (std::min)(kChunk, r.size - off);
			const size_t total = (std::min)(kChunk + tail, r.size - off);
			if (!ReadChunk(r.base + off, g_scratch, total)) {
				prev = 0xFF;
				continue;
			}
			fn(g_scratch, mainLen, total, r.base + off, prev);
			prev = g_scratch[mainLen - 1];
		}
	}

	bool SrcAt(uintptr_t addr, const Pair& p) {
		return MemoryUtils::SafeReadBytes(addr, g_probe, p.src.size() + 1) && SrcMatches(p, g_probe);
	}

	bool TgtAt(uintptr_t addr, const Pair& p) {
		return MemoryUtils::SafeReadBytes(addr, g_probe, p.tgt.size() + 1)
			&& g_probe[p.tgt.size()] == 0 && memcmp(g_probe, p.tgt.data(), p.tgt.size()) == 0;
	}

	// Re-verifies right before writing: content hits are applied after a full
	// memory scan, by which time a freed heap block holding a stale copy may
	// already belong to someone else.
	bool WriteInPlace(uintptr_t addr, const Pair& p) {
		if (!SrcAt(addr, p))
			return false;
		std::string padded = p.tgt;
		padded.resize(p.src.size(), '\0');
		return MemoryUtils::IsWritable(addr, padded.size())
			&& MemoryUtils::SafeWriteBytes(addr, padded.data(), padded.size());
	}

	void PickAnchors() {
		for (size_t f = 0; f < g_files.size(); ++f) {
			std::vector<uint32_t> pool, fitting;
			for (uint32_t i = 0; i < g_pairs.size(); ++i) {
				const Pair& p = g_pairs[i];
				if (p.file != f || p.offs.size() != 1 || p.src.size() < 8 || SrcByte(p, 0) < 0x80)
					continue;
				// Prefer redirect-only pairs: they are never rewritten in place,
				// so their source bytes stay findable across passes.
				(p.WritesInPlace() ? fitting : pool).push_back(i);
			}
			if (pool.size() < kMinRedirectAnchors)
				pool.insert(pool.end(), fitting.begin(), fitting.end());
			auto& anchors = g_files[f].anchors;
			const size_t step = (std::max<size_t>)(1, pool.size() / kMaxAnchorsPerFile);
			for (size_t i = 0; i < pool.size() && anchors.size() < kMaxAnchorsPerFile; i += step)
				anchors.push_back(pool[i]);
		}
	}

	void BuildContentIndex() {
		std::unordered_set<std::string> seen;
		for (uint32_t i = 0; i < g_pairs.size(); ++i) {
			const Pair& p = g_pairs[i];
			if (p.src.size() < kMinContentLen || SrcByte(p, 0) < 0x80)
				continue;
			// One pair per distinct source text; the first occurrence wins.
			if (!seen.insert(p.src).second)
				continue;
			g_contentIndex.emplace(SrcKey32(p), i);
		}
	}

	bool InLocatedImage(uintptr_t addr) {
		for (const auto& f : g_files)
			for (uint32_t base : f.bases)
				if (addr >= base && addr < static_cast<uintptr_t>(base) + f.size)
					return true;
		return false;
	}

	enum class SlotKind { Outside, Known, Unknown };

	// Inside a located image only the slot offsets recorded offline are real
	// pointers; any other hit is text or struct data that merely equals a
	// string address.
	SlotKind ClassifySlot(uintptr_t slot, const Pair& p) {
		for (size_t f = 0; f < g_files.size(); ++f) {
			for (uint32_t base : g_files[f].bases) {
				if (slot < base || slot >= static_cast<uintptr_t>(base) + g_files[f].size)
					continue;
				const uint32_t off = static_cast<uint32_t>(slot - base);
				const bool known = p.file == f && std::find(p.slots.begin(), p.slots.end(), off) != p.slots.end();
				return known ? SlotKind::Known : SlotKind::Unknown;
			}
		}
		return SlotKind::Outside;
	}

	// Points the slots recorded offline for this string at the pool, whatever
	// their alignment. File-relative slots (base + offset) get a wrapping
	// offset: base + (pooled - base) == pooled in 32-bit math; absolute slots
	// get the pool address. Returns true if any slot is (now) redirected.
	// `verified` holds the sorted offsets whose source text was just checked.
	// `imageWritable`: the whole image is one writable region, so per-slot
	// VirtualQuery checks can be skipped.
	bool RedirectKnownSlots(const Pair& p, uint32_t base, const std::vector<uint32_t>& verified, bool imageWritable,
		ContentPatcher::Stats& st)
	{
		const uint32_t pooled = Pooled(p);
		const uint32_t rel = pooled - base;
		auto isVerified = [&](uint32_t off) { return std::binary_search(verified.begin(), verified.end(), off); };
		bool any = false;
		for (uint32_t s : p.slots) {
			const uintptr_t slot = static_cast<uintptr_t>(base) + s;
			uint32_t v = 0;
			if (!MemoryUtils::SafeReadU32(slot, v))
				continue;
			if (v == rel || v == pooled) {
				++st.alreadyApplied;
				any = true;
				continue;
			}
			const uint32_t want = isVerified(v) ? rel : isVerified(v - base) ? pooled : 0;
			if (want && (imageWritable || MemoryUtils::IsWritable(slot, 4))
				&& MemoryUtils::SafeCompareExchangeU32(slot, v, want)) {
				++st.redirected;
				any = true;
			}
		}
		return any;
	}

	unsigned MinVotes(const FileImage& f) {
		return static_cast<unsigned>((std::clamp<size_t>)(f.anchors.size() / 2, 2, 6));
	}

	// Drops bases whose anchors no longer verify; returns true if any changed.
	bool PruneBases() {
		bool changed = false;
		for (auto& f : g_files) {
			std::vector<uint32_t> keep;
			for (uint32_t base : f.bases) {
				unsigned hits = 0, tried = 0;
				for (uint32_t a : f.anchors) {
					if (tried >= 8)
						break;
					++tried;
					const Pair& p = g_pairs[a];
					const uintptr_t addr = base + p.offs[0];
					if (SrcAt(addr, p) || (p.WritesInPlace() && TgtAt(addr, p)))
						++hits;
				}
				if (tried && hits * 2 >= tried)
					keep.push_back(base);
			}
			changed = changed || keep.size() != f.bases.size();
			f.bases = keep;
		}
		return changed;
	}

	bool AnyFileMissing() {
		for (const auto& f : g_files)
			if (f.bases.empty())
				return true;
		return false;
	}

	// How the game stores the original slots inside each located image:
	// file-relative offsets, absolute pointers, our pooled text, or moved.
	void LogLayout(unsigned pass) {
		for (const auto& f : g_files) {
			for (uint32_t base : f.bases) {
				unsigned rel = 0, abs = 0, ours = 0, other = 0, sampled = 0;
				for (const Pair& p : g_pairs) {
					if (&g_files[p.file] != &f || p.slots.empty())
						continue;
					if (++sampled > 512)
						break;
					uint32_t v = 0;
					if (!MemoryUtils::SafeReadU32(base + p.slots[0], v))
						++other;
					else if (std::find(p.offs.begin(), p.offs.end(), v) != p.offs.end())
						++rel;
					else if (v == Pooled(p) || v == Pooled(p) - base)
						++ours;
					else if (v >= base && std::find(p.offs.begin(), p.offs.end(), v - base) != p.offs.end())
						++abs;
					else
						++other;
				}
				Logger::Info("Content pass #%u: %s@0x%08X slot layout: relative=%u absolute=%u patched=%u moved=%u",
					pass, f.key.c_str(), base, rel, abs, ours, other);
			}
		}
	}

	// Searches only for files that currently have no located image; files are
	// decompressed at different times (dat/pac load well after gao/jmp/rcc).
	// Returns true if any new image was found.
	bool FindMissingBases(const std::vector<Region>& regions) {
		std::unordered_multimap<uint64_t, std::pair<uint8_t, uint32_t>> index;
		for (uint8_t f = 0; f < g_files.size(); ++f) {
			if (!g_files[f].bases.empty())
				continue;
			for (uint32_t a : g_files[f].anchors)
				index.emplace(SrcKey64(g_pairs[a]), std::make_pair(f, a));
		}
		std::vector<std::unordered_map<uint32_t, unsigned>> votes(g_files.size());
		if (index.empty())
			return false;

		for (const Region& r : regions) {
			ScanRegion(r, kTail, [&](const uint8_t* buf, size_t mainLen, size_t total, uintptr_t addr, uint8_t prev) {
				for (size_t i = 0; i < mainLen; ++i) {
					if (buf[i] < 0x80 || (i ? buf[i - 1] : prev) != 0 || i + 8 > total)
						continue;
					uint64_t key = 0;
					memcpy(&key, buf + i, 8);
					auto range = index.equal_range(key ^ kMask64);
					for (auto it = range.first; it != range.second; ++it) {
						const Pair& p = g_pairs[it->second.second];
						if (i + p.src.size() + 1 > total || !SrcMatches(p, buf + i))
							continue;
						const uint32_t base = static_cast<uint32_t>(addr + i) - p.offs[0];
						++votes[it->second.first][base];
					}
				}
			});
		}

		bool found = false;
		for (size_t f = 0; f < g_files.size(); ++f) {
			if (!g_files[f].bases.empty())
				continue;
			std::vector<std::pair<unsigned, uint32_t>> ranked;
			for (const auto& [base, n] : votes[f])
				ranked.emplace_back(n, base);
			std::sort(ranked.rbegin(), ranked.rend());
			if (!ranked.empty() && ranked[0].first < MinVotes(g_files[f]))
				Logger::Info("Content: %s anchors seen but not aligned (best base 0x%08X has %u of %zu votes)",
					g_files[f].key.c_str(), ranked[0].second, ranked[0].first, g_files[f].anchors.size());
			for (const auto& [n, base] : ranked) {
				if (g_files[f].bases.size() >= kMaxBasesPerFile || n < MinVotes(g_files[f]) || n * 2 < ranked[0].first)
					break;
				g_files[f].bases.push_back(base);
				found = true;
			}
		}
		return found;
	}

	struct ContentHit {
		uint32_t maskedAddr;
		uint32_t pair;
	};

	// Finds every known source string anywhere outside the located images,
	// regardless of file offsets (the live file may be a different build).
	std::vector<ContentHit> ContentSearch(const std::vector<Region>& regions,
		std::map<uintptr_t, std::vector<unsigned>>& hitsByAlloc)
	{
		std::vector<ContentHit> hits;
		for (const Region& r : regions) {
			ScanRegion(r, kTail, [&](const uint8_t* buf, size_t mainLen, size_t total, uintptr_t addr, uint8_t prev) {
				for (size_t i = 0; i < mainLen; ++i) {
					if (buf[i] < 0x80 || (i ? buf[i - 1] : prev) != 0 || i + kMinContentLen > total)
						continue;
					uint32_t key = 0;
					memcpy(&key, buf + i, 4);
					auto range = g_contentIndex.equal_range(key ^ kMask);
					for (auto it = range.first; it != range.second; ++it) {
						const Pair& p = g_pairs[it->second];
						if (i + p.src.size() + 1 > total || !SrcMatches(p, buf + i))
							continue;
						if (InLocatedImage(addr + i))
							break;
						hits.push_back({ static_cast<uint32_t>(addr + i) ^ kMask, it->second });
						auto& counts = hitsByAlloc[r.alloc];
						counts.resize(g_files.size());
						++counts[p.file];
						i += p.src.size();
						break;
					}
				}
			});
		}
		return hits;
	}

	// Writes the whole allocation holding the most hits for a still-missing
	// file to disk once, so pairs can be rebuilt from the exact live bytes.
	void DumpMissing(const std::vector<Region>& regions, const std::map<uintptr_t, std::vector<unsigned>>& hitsByAlloc) {
		if (g_dumpDir.empty())
			return;
		for (size_t f = 0; f < g_files.size(); ++f) {
			FileImage& file = g_files[f];
			if (!file.bases.empty() || file.dumped)
				continue;
			uintptr_t best = 0;
			unsigned bestHits = 0;
			for (const auto& [alloc, counts] : hitsByAlloc) {
				if (counts.size() > f && counts[f] > bestHits) {
					bestHits = counts[f];
					best = alloc;
				}
			}
			if (bestHits < kMinDumpHits)
				continue;
			uintptr_t lo = UINTPTR_MAX, hi = 0;
			for (const Region& r : regions) {
				if (r.alloc != best)
					continue;
				lo = (std::min)(lo, r.base);
				hi = (std::max)(hi, r.base + r.size);
			}
			if (lo >= hi || hi - lo > kMaxDumpBytes)
				continue;
			CreateDirectoryA(g_dumpDir.c_str(), nullptr);
			char name[MAX_PATH];
			snprintf(name, sizeof(name), "%s%s_%08X_%X.bin", g_dumpDir.c_str(), file.key.c_str(),
				static_cast<unsigned>(lo), static_cast<unsigned>(hi - lo));
			std::ofstream out(name, std::ios::binary);
			std::vector<uint8_t> chunk(kChunk);
			for (uintptr_t a = lo; a < hi && out; a += kChunk) {
				const size_t n = (std::min)(kChunk, static_cast<size_t>(hi - a));
				if (!MemoryUtils::SafeReadBytes(a, chunk.data(), n))
					std::fill(chunk.begin(), chunk.begin() + n, 0);
				out.write(reinterpret_cast<const char*>(chunk.data()), n);
			}
			file.dumped = true;
			Logger::Info("Content: dumped live %s candidate (%u hits) to %s", file.key.c_str(), bestHits, name);
		}
	}

	void LogContentSummary(unsigned pass, size_t hitCount, unsigned inPlace,
		const std::map<uintptr_t, std::vector<unsigned>>& hitsByAlloc)
	{
		std::vector<unsigned> perFile(g_files.size(), 0);
		for (const auto& [alloc, counts] : hitsByAlloc)
			for (size_t f = 0; f < counts.size(); ++f)
				perFile[f] += counts[f];
		std::string per;
		char buf[96];
		for (size_t f = 0; f < g_files.size(); ++f) {
			snprintf(buf, sizeof(buf), "%s%s=%u", per.empty() ? "" : " ", g_files[f].key.c_str(), perFile[f]);
			per += buf;
		}
		Logger::Info("Content pass #%u: content search hits=%zu (%s) inPlace=%u", pass, hitCount, per.c_str(), inPlace);
		for (size_t f = 0; f < g_files.size(); ++f) {
			uintptr_t best = 0;
			unsigned bestHits = 0;
			for (const auto& [alloc, counts] : hitsByAlloc) {
				if (counts.size() > f && counts[f] > bestHits) {
					bestHits = counts[f];
					best = alloc;
				}
			}
			if (bestHits)
				Logger::Info("Content pass #%u:   %s densest allocation 0x%08X (%u hits)",
					pass, g_files[f].key.c_str(), static_cast<unsigned>(best), bestHits);
		}
	}

	// Sorted, merged [lo, hi) ranges with binary-search membership.
	class RangeSet {
	public:
		void Add(uint32_t lo, uint32_t hi) { m_ranges.emplace_back(lo, hi); }
		void Finish() {
			std::sort(m_ranges.begin(), m_ranges.end());
			std::vector<std::pair<uint32_t, uint32_t>> merged;
			for (const auto& r : m_ranges) {
				if (!merged.empty() && r.first <= merged.back().second)
					merged.back().second = (std::max)(merged.back().second, r.second);
				else
					merged.push_back(r);
			}
			m_ranges = std::move(merged);
		}
		bool Contains(uint32_t v) const {
			auto it = std::upper_bound(m_ranges.begin(), m_ranges.end(), std::make_pair(v, UINT32_MAX));
			return it != m_ranges.begin() && v < (it - 1)->second;
		}
		bool Empty() const { return m_ranges.empty(); }

	private:
		std::vector<std::pair<uint32_t, uint32_t>> m_ranges;
	};
}

namespace ContentPatcher {
	bool Load(const std::string& path) {
		std::ifstream in(path, std::ios::binary);
		if (!in)
			return false;
		json doc;
		try {
			in >> doc;
		} catch (const std::exception& ex) {
			Logger::Warn("Pairs file %s is not valid JSON: %s", path.c_str(), ex.what());
			return false;
		}

		const json files = doc.value("files", json::object());
		const json rows = doc.value("pairs", json::array());
		std::unordered_map<std::string, uint8_t> fileIndex;
		for (const auto& [key, size] : files.items()) {
			fileIndex[key] = static_cast<uint8_t>(g_files.size());
			g_files.push_back({ key, size.get<uint32_t>() });
		}

		size_t rejected = 0, poolBytes = 0;
		for (const auto& row : rows) {
			Pair p;
			const auto fit = fileIndex.find(row.value("k", ""));
			if (fit == fileIndex.end() || !HexDecode(row.value("s", ""), p.src) || !HexDecode(row.value("t", ""), p.tgt)
				|| p.src.empty() || p.tgt.empty() || p.src.size() > kMaxSrcLen) {
				++rejected;
				continue;
			}
			for (char& c : p.src)
				c = static_cast<char>(static_cast<uint8_t>(c) ^ kSrcMask);
			p.file = fit->second;
			p.inPlaceSafe = row.value("r", 0) == 0;
			const uint32_t fileSize = g_files[p.file].size;
			for (const auto& o : row.value("o", json::array())) {
				const uint32_t off = o.get<uint32_t>();
				if (off + p.src.size() < fileSize)
					p.offs.push_back(off);
			}
			for (const auto& s : row.value("p", json::array()))
				p.slots.push_back(s.get<uint32_t>());
			if (p.offs.empty()) {
				++rejected;
				continue;
			}
			poolBytes += p.tgt.size() + 1;
			g_probeSize = (std::max)(g_probeSize, (std::max)(p.src.size(), p.tgt.size()) + 16);
			g_pairs.push_back(std::move(p));
		}

		g_poolSize = poolBytes + 16;
		g_pool = static_cast<uint8_t*>(VirtualAlloc(nullptr, g_poolSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
		g_scratch = static_cast<uint8_t*>(VirtualAlloc(nullptr, kChunk + kTail, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
		g_probe = static_cast<uint8_t*>(VirtualAlloc(nullptr, g_probeSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
		if (!g_pool || !g_scratch || !g_probe) {
			Logger::Error("Content patcher: VirtualAlloc failed (pool %zu bytes)", poolBytes);
			g_pairs.clear();
			return false;
		}
		uint8_t* cursor = g_pool;
		for (auto& p : g_pairs) {
			memcpy(cursor, p.tgt.data(), p.tgt.size());
			cursor[p.tgt.size()] = 0;
			p.pooledMasked = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(cursor)) ^ kMask;
			cursor += p.tgt.size() + 1;
		}
		PickAnchors();
		BuildContentIndex();

		const size_t slash = path.find_last_of("\\/");
		g_dumpDir = (slash == std::string::npos ? std::string() : path.substr(0, slash + 1)) + "dump\\";

		for (const auto& f : g_files)
			Logger::Info("Content patcher: file %s size=0x%X anchors=%zu", f.key.c_str(), f.size, f.anchors.size());
		Logger::Info("Content patcher: loaded %zu pairs from %s (rejected %zu, pool %zu bytes, content keys %zu)",
			g_pairs.size(), path.c_str(), rejected, poolBytes, g_contentIndex.size());
		return !g_pairs.empty();
	}

	bool Loaded() {
		return !g_pairs.empty();
	}

	size_t PairCount() {
		return g_pairs.size();
	}

	bool ImagesFound() {
		for (const auto& f : g_files)
			if (!f.bases.empty())
				return true;
		return false;
	}

	bool AllImagesFound() {
		return !g_files.empty() && !AnyFileMissing();
	}

	const Stats& Last() {
		return g_last;
	}

	std::string ImageSummary() {
		std::string out;
		char buf[64];
		for (const auto& f : g_files) {
			for (uint32_t base : f.bases) {
				snprintf(buf, sizeof(buf), "%s%s@0x%08X", out.empty() ? "" : " ", f.key.c_str(), base);
				out += buf;
			}
		}
		return out.empty() ? "none" : out;
	}

	Stats Apply() {
		Stats st;
		st.pass = ++g_passes;
		if (g_pairs.empty())
			return g_last = st;
		const auto t0 = std::chrono::steady_clock::now();

		std::vector<Region> regions = EnumRegions();
		bool fresh = PruneBases();
		if (AnyFileMissing())
			fresh = FindMissingBases(regions) || fresh;
		if (fresh || st.pass == 1)
			Logger::Info("Content pass #%u: located images: %s", st.pass, ImageSummary().c_str());

		std::unordered_map<uint32_t, uint32_t> redirectTargets;
		std::unordered_set<uint32_t> relReferenced;
		RangeSet ranges;
		for (const auto& f : g_files) {
			for (uint32_t base : f.bases) {
				++st.imagesFound;
				ranges.Add(base, base + f.size);
			}
		}

		std::unordered_map<uint32_t, bool> writableImage;
		std::unordered_set<uint32_t> readOnlyImage;
		for (const auto& f : g_files) {
			for (uint32_t base : f.bases) {
				if (MemoryUtils::IsWritable(base, f.size))
					writableImage[base] = true;
				else if (!MemoryUtils::IsWritable(base, 1) && !MemoryUtils::IsWritable(base + f.size - 1, 1))
					readOnlyImage.insert(base);
			}
		}

		std::vector<uint32_t> verified;
		for (uint32_t pi = 0; pi < g_pairs.size(); ++pi) {
			const Pair& p = g_pairs[pi];
			for (uint32_t base : g_files[p.file].bases) {
				verified.clear();
				for (uint32_t off : p.offs) {
					const uintptr_t addr = base + off;
					if (SrcAt(addr, p)) {
						++st.verified;
						if (p.WritesInPlace()) {
							if (!readOnlyImage.count(base) && WriteInPlace(addr, p))
								++st.inPlace;
						} else {
							redirectTargets.emplace(static_cast<uint32_t>(addr) ^ kMask, pi);
							verified.push_back(off);
						}
					} else if (p.WritesInPlace() && TgtAt(addr, p)) {
						++st.alreadyApplied;
					} else {
						++st.missing;
					}
				}
				if (!verified.empty() && !readOnlyImage.count(base)
					&& RedirectKnownSlots(p, base, verified, writableImage.count(base) != 0, st))
					for (uint32_t off : verified)
						relReferenced.insert((base + off) ^ kMask);
			}
		}

		auto msSince = [](std::chrono::steady_clock::time_point t) {
			return static_cast<unsigned>(std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now() - t).count());
		};
		const unsigned imageMs = msSince(t0);
		const auto tContent = std::chrono::steady_clock::now();
		std::map<uintptr_t, std::vector<unsigned>> hitsByAlloc;
		const std::vector<ContentHit> hits = ContentSearch(regions, hitsByAlloc);
		const unsigned contentMs = msSince(tContent);
		const auto tPointers = std::chrono::steady_clock::now();
		st.contentHits = static_cast<unsigned>(hits.size());
		for (const ContentHit& h : hits) {
			const Pair& p = g_pairs[h.pair];
			if (p.WritesInPlace()) {
				const uintptr_t addr = h.maskedAddr ^ kMask;
				uint8_t prev = 1;
				if (MemoryUtils::SafeReadBytes(addr - 1, &prev, 1) && prev == 0 && WriteInPlace(addr, p))
					++st.contentInPlace;
			} else {
				redirectTargets.emplace(h.maskedAddr, h.pair);
			}
		}
		for (const Region& r : regions)
			if (hitsByAlloc.count(r.alloc))
				ranges.Add(static_cast<uint32_t>(r.base), static_cast<uint32_t>(r.base + r.size));
		ranges.Finish();
		if (AnyFileMissing() || fresh || st.pass == 1)
			LogContentSummary(st.pass, hits.size(), st.contentInPlace, hitsByAlloc);
		DumpMissing(regions, hitsByAlloc);

		const uint32_t poolLo = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g_pool));
		const uint32_t poolHi = poolLo + static_cast<uint32_t>(g_poolSize);
		std::vector<Candidate> cands;
		if (!redirectTargets.empty()) {
			for (const Region& r : regions) {
				if (!r.writable)
					continue;
				ScanRegion(r, 0, [&](const uint8_t* buf, size_t mainLen, size_t, uintptr_t addr, uint8_t) {
					for (size_t i = 0; i + 4 <= mainLen; i += 4) {
						uint32_t v = 0;
						memcpy(&v, buf + i, 4);
						if (v >= poolLo && v < poolHi) {
							++st.liveRedirects;
							continue;
						}
						if (!ranges.Contains(v))
							continue;
						const auto it = redirectTargets.find(v ^ kMask);
						if (it == redirectTargets.end())
							continue;
						const SlotKind kind = ClassifySlot(addr + i, g_pairs[it->second]);
						if (kind == SlotKind::Unknown)
							++st.slotsRejected;
						else
							cands.push_back({ addr + i, v ^ kMask, it->second, kind == SlotKind::Known });
					}
				});
			}
		}

		std::sort(cands.begin(), cands.end(), [](const Candidate& a, const Candidate& b) { return a.slot < b.slot; });
		{
			std::unordered_set<uint32_t> referenced(relReferenced);
			for (const auto& c : cands)
				referenced.insert(c.maskedTarget);
			st.unreferenced = static_cast<unsigned>(redirectTargets.size() - referenced.size());
		}
		if (fresh && ImagesFound())
			LogLayout(st.pass);
		// Outside the images a slot must sit in a run of nearby slots aimed at
		// several distinct strings: real tables list many strings, while
		// cursor pairs (begin/current of one buffer) and stray struct fields
		// only hold one or two.
		std::vector<bool> inTable(cands.size(), false);
		for (size_t lo = 0; lo < cands.size();) {
			size_t hi = lo + 1;
			while (hi < cands.size() && cands[hi].slot - cands[hi - 1].slot <= kNeighbourReach)
				++hi;
			std::unordered_set<uint32_t> distinct;
			for (size_t k = lo; k < hi; ++k)
				distinct.insert(cands[k].maskedTarget);
			if (distinct.size() >= kMinTableRun)
				std::fill(inTable.begin() + lo, inTable.begin() + hi, true);
			lo = hi;
		}
		std::map<uintptr_t, unsigned> outside;
		for (size_t k = 0; k < cands.size(); ++k) {
			if (!cands[k].known && !inTable[k]) {
				++st.slotsRejected;
				continue;
			}
			const Pair& p = g_pairs[cands[k].pair];
			const uint32_t target = cands[k].maskedTarget ^ kMask;
			if (!SrcAt(target, p))
				continue;
			if (!MemoryUtils::SafeCompareExchangeU32(cands[k].slot, target, Pooled(p)))
				continue;
			++st.redirected;
			if (!cands[k].known) {
				MEMORY_BASIC_INFORMATION mbi{};
				VirtualQuery(reinterpret_cast<void*>(cands[k].slot), &mbi, sizeof(mbi));
				++outside[reinterpret_cast<uintptr_t>(mbi.AllocationBase)];
			}
		}
		for (const auto& [alloc, n] : outside)
			Logger::Info("Content pass #%u: redirected %u slot(s) outside the images in allocation 0x%08X",
				st.pass, n, static_cast<unsigned>(alloc));

		const unsigned pointerMs = msSince(tPointers);
		st.scanMs = msSince(t0);
		Logger::Info("Content pass #%u: images=%u verified=%u inPlace=%u redirected=%u alreadyApplied=%u "
			"liveRedirects=%u missing=%u unreferenced=%u slotsRejected=%u contentHits=%u contentInPlace=%u "
			"(%u ms: images %u, content %u, pointers %u)",
			st.pass, st.imagesFound, st.verified, st.inPlace, st.redirected, st.alreadyApplied, st.liveRedirects,
			st.missing, st.unreferenced, st.slotsRejected, st.contentHits, st.contentInPlace, st.scanMs,
			imageMs, contentMs, pointerMs);
		return g_last = st;
	}
}
