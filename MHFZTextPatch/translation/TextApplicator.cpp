#include "TextApplicator.h"
#include "TranslationStore.h"
#include "HeadersStore.h"
#include "BufferLocator.h"
#include "GameCodec.h"
#include "../config/Config.h"
#include "../logging/Logger.h"
#include "../memory/MemoryUtils.h"
#include "../include/json.hpp"
#include <Windows.h>
#include <algorithm>
#include <fstream>
#include <unordered_map>
#include <vector>

using json = nlohmann::json;

namespace {
	struct OffsetPatch {
		uintptr_t address = 0;
		std::string bytes;
		size_t maxLen = 0;
	};

	struct LiveSlot {
		uint32_t pointerSlotFileOff = 0; // offset within image (0 if using VA)
		uintptr_t pointerSlotVa = 0;     // absolute slot address when non-zero
	};

	struct LiveEntry {
		std::vector<LiveSlot> slots; // one per {j} part (non-null pointers only)
	};

	struct BumpPool {
		uintptr_t base = 0;
		size_t size = 0;
		size_t used = 0;
		bool absolute = false;
	};

	std::vector<OffsetPatch> g_offsetPatches;
	TextApplicator::ApplyStats g_last;
	std::unordered_map<uintptr_t, BumpPool> g_pools;

	bool ReadU32Img(const BufferLocator::Image& img, uint32_t fileOff, uint32_t& out) {
		if (static_cast<size_t>(fileOff) + 4 > img.size)
			return false;
		return MemoryUtils::SafeReadU32(img.base + fileOff, out);
	}

	bool WriteU32Img(const BufferLocator::Image& img, uint32_t fileOff, uint32_t value) {
		if (static_cast<size_t>(fileOff) + 4 > img.size)
			return false;
		return MemoryUtils::SafeWriteU32(img.base + fileOff, value);
	}

	uintptr_t ResolvePtr(const BufferLocator::Image& img, uint32_t stored) {
		if (!stored)
			return 0;
		// Absolute-mode vote: prefer in-image VAs first, then still try relative.
		if (img.absolutePointers) {
			if (stored >= img.base && stored < img.base + img.size)
				return static_cast<uintptr_t>(stored);
		}
		// File-relative offset (common for decompressed bins).
		if (stored < img.size) {
			const uintptr_t rel = img.base + stored;
			uint8_t probe = 0;
			if (MemoryUtils::SafeReadU8(rel, probe))
				return rel;
		}
		// Absolute VA into this image (even when absolutePointers=false).
		if (stored >= img.base && stored < img.base + img.size)
			return static_cast<uintptr_t>(stored);
		// Never chase arbitrary readable addresses outside the image.
		return 0;
	}

	bool StoredLooksRelative(const BufferLocator::Image& img, uint32_t stored) {
		return stored != 0 && stored < img.size;
	}

	bool LooksLikeAsciiWord(uint32_t v) {
		int printable = 0;
		for (int i = 0; i < 4; ++i) {
			const uint8_t b = static_cast<uint8_t>((v >> (8 * i)) & 0xFF);
			if (b >= 0x20 && b <= 0x7E)
				++printable;
		}
		return printable >= 3;
	}

	bool MapBeginToFileOff(const BufferLocator::Image& img, uint32_t beginStored, uint32_t& fileOffOut) {
		if (!beginStored)
			return false;
		if (LooksLikeAsciiWord(beginStored))
			return false;
		if (beginStored < img.size) {
			fileOffOut = beginStored;
			return true;
		}
		if (beginStored >= img.base && beginStored < img.base + img.size) {
			fileOffOut = static_cast<uint32_t>(beginStored - img.base);
			return true;
		}
		return false;
	}

	uint32_t EncodePtr(const BufferLocator::Image& img, uintptr_t addr, bool preferRelative) {
		if (!addr)
			return 0;
		if (preferRelative && addr >= img.base && addr < img.base + img.size)
			return static_cast<uint32_t>(addr - img.base);
		return static_cast<uint32_t>(addr);
	}

	size_t MeasureCString(uintptr_t addr) {
		size_t cap = 0;
		for (; cap < 4096; ++cap) {
			uint8_t b = 0;
			if (!MemoryUtils::SafeReadU8(addr + cap, b))
				return 0;
			if (b == 0)
				break;
		}
		return cap;
	}

	bool WriteBytes(uintptr_t addr, const std::string& bytes, size_t cap) {
		if (!addr)
			return false;
		// Allow empty write as a single NUL when capacity exists.
		if (bytes.size() > cap)
			return false;
		for (size_t i = 0; i < bytes.size(); ++i) {
			if (!MemoryUtils::SafeWriteU8(addr + i, static_cast<uint8_t>(bytes[i])))
				return false;
		}
		if (bytes.size() <= cap) {
			if (!MemoryUtils::SafeWriteU8(addr + bytes.size(), 0))
				return false;
		}
		return true;
	}

	BumpPool& PoolFor(BufferLocator::Image& img) {
		auto it = g_pools.find(img.base);
		if (it != g_pools.end())
			return it->second;

		BumpPool pool;
		pool.absolute = false;
		// Always try trailing zeros inside the image first (keeps file-relative pointers).
		{
			const size_t want = 1024 * 1024;
			size_t zeroRun = 0;
			for (size_t back = 0; back < img.size && back < want * 2; ++back) {
				uint8_t b = 0xFF;
				if (!MemoryUtils::SafeReadU8(img.base + img.size - 1 - back, b))
					break;
				if (b != 0)
					break;
				++zeroRun;
			}
			if (zeroRun > 64 * 1024) {
				pool.base = img.base + (img.size - zeroRun);
				pool.size = zeroRun;
				pool.absolute = false;
			}
		}
		// Heap reloc is opt-in only — rewriting game pointers to VirtualAlloc crashes TW.
		if (!pool.base && Config::Get().allowHeapReloc) {
			void* mem = VirtualAlloc(nullptr, 2 * 1024 * 1024, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
			if (mem) {
				pool.base = reinterpret_cast<uintptr_t>(mem);
				pool.size = 2 * 1024 * 1024;
				pool.absolute = true;
			}
		}
		pool.used = 0;
		g_pools[img.base] = pool;
		return g_pools[img.base];
	}

	bool ReadU32Slot(const BufferLocator::Image& img, const LiveSlot& slot, uint32_t& out) {
		if (slot.pointerSlotVa)
			return MemoryUtils::SafeReadU32(slot.pointerSlotVa, out);
		return ReadU32Img(img, slot.pointerSlotFileOff, out);
	}

	bool WriteU32Slot(const BufferLocator::Image& img, const LiveSlot& slot, uint32_t value) {
		if (slot.pointerSlotVa)
			return MemoryUtils::SafeWriteU32(slot.pointerSlotVa, value);
		return WriteU32Img(img, slot.pointerSlotFileOff, value);
	}

	bool WriteOrRelocate(
		BufferLocator::Image& img,
		const LiveSlot& slot,
		const std::string& sjis,
		TextApplicator::ApplyStats& st)
	{
		uint32_t stored = 0;
		if (!ReadU32Slot(img, slot, stored) || !stored) {
			++st.skippedResolve;
			return false;
		}
		const bool origRelative = StoredLooksRelative(img, stored);
		uintptr_t strAddr = ResolvePtr(img, stored);
		if (!strAddr) {
			++st.skippedResolve;
			return false;
		}
		const size_t cap = MeasureCString(strAddr);
		if (cap > 0 && WriteBytes(strAddr, sjis, cap)) {
			++st.written;
			return true;
		}

		BumpPool& pool = PoolFor(img);
		if (Config::Get().inPlaceOnly || !pool.base || pool.used + sjis.size() + 1 > pool.size) {
			++st.skippedFit;
			return false;
		}
		// Never rewrite slots to heap VAs unless explicitly allowed.
		if (pool.absolute && !Config::Get().allowHeapReloc) {
			++st.skippedFit;
			return false;
		}
		const uintptr_t dest = pool.base + pool.used;
		for (size_t i = 0; i < sjis.size(); ++i) {
			if (!MemoryUtils::SafeWriteU8(dest + i, static_cast<uint8_t>(sjis[i]))) {
				++st.skippedFit;
				return false;
			}
		}
		MemoryUtils::SafeWriteU8(dest + sjis.size(), 0);
		pool.used += sjis.size() + 1;
		pool.used = (pool.used + 3u) & ~3u;

		// Prefer relative encoding when original was relative; always keep dest in-image
		// unless heap reloc is allowed.
		const bool destInImage = dest >= img.base && dest < img.base + img.size;
		if (!destInImage && !Config::Get().allowHeapReloc) {
			++st.skippedFit;
			return false;
		}
		const bool preferRel = (origRelative || !img.absolutePointers) && !pool.absolute && destInImage;
		const uint32_t newStored = EncodePtr(img, dest, preferRel);
		if (!newStored || !WriteU32Slot(img, slot, newStored)) {
			++st.skippedFit;
			return false;
		}
		++st.written;
		++st.relocated;
		return true;
	}

	std::string EncodeTarget(const std::string& utf8Target) {
		const std::string braced = GameCodec::ColorCodesFromCsv(utf8Target);
		return GameCodec::Utf8ToSjis(braced);
	}

	bool ReadStringAtPtr(const BufferLocator::Image& img, uint32_t stored, std::string& outText) {
		const uintptr_t addr = ResolvePtr(img, stored);
		if (!addr)
			return false;
		std::string raw;
		for (size_t i = 0; i < 4096; ++i) {
			uint8_t b = 0;
			if (!MemoryUtils::SafeReadU8(addr + i, b))
				return false;
			if (b == 0)
				break;
			raw.push_back(static_cast<char>(b));
		}
		outText = raw; // raw SJIS bytes kept only for length; text content unused for apply
		(void)outText;
		return true;
	}

	bool CollectFlatEntries(
		const BufferLocator::Image& img,
		const HeadersStore::SectionConfig& cfg,
		std::vector<LiveEntry>& out,
		std::string* failReason)
	{
		out.clear();
		uint32_t startRaw = 0;
		if (!ReadU32Img(img, cfg.beginPointer, startRaw) || !startRaw) {
			if (failReason)
				*failReason = "begin_pointer unreadable/zero";
			return false;
		}

		uint32_t startFile = 0;
		uintptr_t tableVa = 0;
		if (LooksLikeAsciiWord(startRaw)) {
			if (failReason) {
				char buf[128];
				_snprintf_s(buf, _TRUNCATE, "begin=0x%X looks like ASCII garbage", startRaw);
				*failReason = buf;
			}
			return false;
		}
		const bool inImage = MapBeginToFileOff(img, startRaw, startFile);
		if (inImage) {
			tableVa = img.base + startFile;
		} else {
			// Never treat out-of-image absolute addresses as tables — crash vector.
			if (failReason) {
				char buf[128];
				_snprintf_s(buf, _TRUNCATE,
					"begin=0x%X not readable (base=0x%X size=0x%X)",
					startRaw, static_cast<unsigned>(img.base), static_cast<unsigned>(img.size));
				*failReason = buf;
			}
			return false;
		}

		const int ppe = cfg.pointersPerEntry > 0 ? cfg.pointersPerEntry : 1;
		auto makeSlot = [&](uint32_t indexBytes) -> LiveSlot {
			LiveSlot s;
			const uintptr_t va = tableVa + indexBytes;
			if (inImage)
				s.pointerSlotFileOff = startFile + indexBytes;
			else
				s.pointerSlotVa = va;
			return s;
		};

		if (cfg.groupedEntries && ppe > 1) {
			const int count = cfg.entryCount > 0 ? cfg.entryCount : 0;
			if (count <= 0) {
				if (failReason) *failReason = "grouped entry_count<=0";
				return false;
			}
			for (int e = 0; e < count; ++e) {
				LiveEntry entry;
				for (int i = 0; i < ppe; ++i) {
					const uint32_t indexBytes = static_cast<uint32_t>((e * ppe + i) * 4);
					uint32_t ptr = 0;
					if (!MemoryUtils::SafeReadU32(tableVa + indexBytes, ptr) || !ptr)
						continue;
					std::string tmp;
					if (!ReadStringAtPtr(img, ptr, tmp))
						continue;
					entry.slots.push_back(makeSlot(indexBytes));
				}
				if (!entry.slots.empty())
					out.push_back(std::move(entry));
			}
			if (out.empty() && failReason)
				*failReason = "grouped: no readable string slots";
			return !out.empty();
		}

		int totalPtrs = cfg.entryCount > 0 ? cfg.entryCount * ppe : 0;
		if (totalPtrs <= 0 && cfg.nullTerminated) {
			int scanned = 0;
			while (scanned < 4 * 1024 * 1024) {
				uint32_t first = 0;
				if (!MemoryUtils::SafeReadU32(tableVa + static_cast<uintptr_t>(scanned * 4), first))
					break;
				if (first == 0)
					break;
				scanned += ppe;
			}
			totalPtrs = scanned;
		}
		if (totalPtrs <= 0) {
			if (failReason) *failReason = "totalPtrs<=0";
			return false;
		}
		if (inImage) {
			const int maxPtrsByImage = static_cast<int>((img.size - startFile) / 4);
			if (totalPtrs > maxPtrsByImage)
				totalPtrs = maxPtrsByImage;
		}
		// Hard safety cap for CN / bad counts.
		if (totalPtrs > 300000)
			totalPtrs = 300000;
		if (totalPtrs <= 0) {
			if (failReason) *failReason = "table past image end";
			return false;
		}

		std::vector<uint32_t> ptrs(static_cast<size_t>(totalPtrs));
		bool anyNull = false;
		int readableStrings = 0;
		for (int i = 0; i < totalPtrs; ++i) {
			uint32_t p = 0;
			MemoryUtils::SafeReadU32(tableVa + static_cast<uintptr_t>(i * 4), p);
			ptrs[static_cast<size_t>(i)] = p;
			if (p == 0)
				anyNull = true;
		}
		const bool joinLines = !cfg.nullPadding && anyNull;

		LiveEntry cur;
		for (int i = 0; i < totalPtrs; ++i) {
			const uint32_t indexBytes = static_cast<uint32_t>(i * 4);
			const uint32_t p = ptrs[static_cast<size_t>(i)];
			if (p == 0) {
				if (joinLines) {
					if (!cur.slots.empty()) {
						out.push_back(std::move(cur));
						cur = {};
					}
				}
				continue;
			}
			std::string tmp;
			if (!ReadStringAtPtr(img, p, tmp))
				continue;
			++readableStrings;
			if (joinLines) {
				cur.slots.push_back(makeSlot(indexBytes));
			} else {
				LiveEntry e;
				e.slots.push_back(makeSlot(indexBytes));
				out.push_back(std::move(e));
			}
		}
		if (!cur.slots.empty())
			out.push_back(std::move(cur));
		if (out.empty() && failReason) {
			char buf[160];
			_snprintf_s(buf, _TRUNCATE,
				"flat: begin=0x%X tableVa=0x%X ptrs=%d readableStrings=%d inImage=%d",
				startRaw, static_cast<unsigned>(tableVa), totalPtrs, readableStrings, inImage ? 1 : 0);
			*failReason = buf;
		}
		return !out.empty();
	}

	bool CollectStructEntries(
		const BufferLocator::Image& img,
		const HeadersStore::SectionConfig& cfg,
		std::vector<LiveEntry>& out,
		std::string* failReason)
	{
		out.clear();
		uint32_t baseOff = cfg.beginPointer;
		uintptr_t arrayVa = 0;
		bool inImage = true;

		if (!cfg.literalBase) {
			uint32_t stored = 0;
			if (!ReadU32Img(img, cfg.beginPointer, stored) || !stored) {
				if (failReason) *failReason = "struct begin unreadable/zero";
				return false;
			}
			if (MapBeginToFileOff(img, stored, baseOff)) {
				arrayVa = img.base + baseOff;
				inImage = true;
			} else {
				if (failReason) {
					char buf[128];
					_snprintf_s(buf, _TRUNCATE, "struct begin=0x%X not readable", stored);
					*failReason = buf;
				}
				return false;
			}
		} else {
			arrayVa = img.base + baseOff;
		}

		if (cfg.entryCount <= 0 || cfg.entrySize <= 0) {
			if (failReason) *failReason = "struct entry_count/size invalid";
			return false;
		}
		std::vector<int> fields = cfg.fieldOffsets;
		if (fields.empty())
			fields.push_back(cfg.fieldOffset);

		int count = cfg.entryCount;
		if (inImage) {
			const size_t bytesNeeded = static_cast<size_t>(count) * static_cast<size_t>(cfg.entrySize);
			if (baseOff + bytesNeeded > img.size)
				count = static_cast<int>((img.size - baseOff) / cfg.entrySize);
		}
		if (count > 300000)
			count = 300000;
		if (count <= 0) {
			if (failReason) *failReason = "struct table past image end";
			return false;
		}

		for (int i = 0; i < count; ++i) {
			const uintptr_t entryVa = arrayVa + static_cast<uintptr_t>(i * cfg.entrySize);
			for (int fo : fields) {
				const uintptr_t slotVa = entryVa + static_cast<uintptr_t>(fo);
				uint32_t ptr = 0;
				if (!MemoryUtils::SafeReadU32(slotVa, ptr) || !ptr)
					continue;
				std::string tmp;
				if (!ReadStringAtPtr(img, ptr, tmp))
					continue;
				LiveEntry e;
				LiveSlot slot;
				if (inImage)
					slot.pointerSlotFileOff = static_cast<uint32_t>(slotVa - img.base);
				else
					slot.pointerSlotVa = slotVa;
				e.slots.push_back(slot);
				out.push_back(std::move(e));
			}
		}
		if (out.empty() && failReason)
			*failReason = "struct: no readable string fields";
		return !out.empty();
	}

	bool CollectQuestEntries(
		const BufferLocator::Image& img,
		const HeadersStore::SectionConfig& cfg,
		std::vector<LiveEntry>& out,
		std::string* failReason)
	{
		out.clear();
		uint32_t baseAddrStored = 0;
		if (!ReadU32Img(img, cfg.countBasePointer, baseAddrStored) || !baseAddrStored) {
			if (failReason) *failReason = "quest count_base unreadable";
			return false;
		}
		uintptr_t countAddr = ResolvePtr(img, baseAddrStored);
		if (!countAddr) {
			if (failReason) *failReason = "quest count base resolve failed";
			return false;
		}
		countAddr += cfg.countOffset;
		uint32_t count = 0;
		if (cfg.countType == "u16") {
			uint16_t c16 = 0;
			if (!MemoryUtils::SafeReadU16(countAddr, c16)) {
				if (failReason) *failReason = "quest count u16 read failed";
				return false;
			}
			count = c16;
		} else {
			if (!MemoryUtils::SafeReadU32(countAddr, count)) {
				if (failReason) *failReason = "quest count u32 read failed";
				return false;
			}
		}
		count = static_cast<uint32_t>(static_cast<int>(count) + cfg.countAdjust);
		if (count == 0 || count > 512) {
			if (failReason) *failReason = "quest category count out of range (wrong inf?)";
			return false;
		}
		if (img.size < 400u * 1024u) {
			if (failReason) *failReason = "inf image too small for quests";
			return false;
		}

		uint32_t catStored = 0;
		if (!ReadU32Img(img, cfg.beginPointer, catStored) || !catStored) {
			if (failReason) *failReason = "quest begin unreadable";
			return false;
		}
		uint32_t catFile = 0;
		if (!MapBeginToFileOff(img, catStored, catFile)) {
			if (failReason) *failReason = "quest begin not in image";
			return false;
		}

		for (uint32_t cat = 0; cat < count; ++cat) {
			const uint32_t catAddr = catFile + cat * 8;
			uint16_t qcount = 0;
			uint32_t questArrayPtr = 0;
			if (!MemoryUtils::SafeReadU16(img.base + catAddr + 2, qcount))
				continue;
			if (!MemoryUtils::SafeReadU32(img.base + catAddr + 4, questArrayPtr))
				continue;
			if (!questArrayPtr || !qcount)
				continue;
			uintptr_t arr = ResolvePtr(img, questArrayPtr);
			if (!arr)
				continue;
			for (uint16_t qi = 0; qi < qcount; ++qi) {
				uint32_t questPtr = 0;
				if (!MemoryUtils::SafeReadU32(arr + qi * 4u, questPtr) || !questPtr)
					continue;
				uintptr_t quest = ResolvePtr(img, questPtr);
				if (!quest)
					continue;
				uint32_t textBlock = 0;
				if (!MemoryUtils::SafeReadU32(quest + cfg.questTextOffset, textBlock) || !textBlock)
					continue;
				uintptr_t block = ResolvePtr(img, textBlock);
				if (!block)
					continue;
				LiveEntry entry;
				for (int ti = 0; ti < cfg.textPointersCount; ++ti) {
					uint32_t sp = 0;
					const uintptr_t slotVa = block + static_cast<uintptr_t>(ti * 4);
					if (!MemoryUtils::SafeReadU32(slotVa, sp) || !sp)
						continue;
					std::string tmp;
					if (!ReadStringAtPtr(img, sp, tmp))
						continue;
					if (slotVa < img.base || slotVa >= img.base + img.size)
						continue;
					entry.slots.push_back({ static_cast<uint32_t>(slotVa - img.base) });
				}
				if (!entry.slots.empty())
					out.push_back(std::move(entry));
				if (out.size() > 8000) {
					if (failReason) *failReason = "quest entry flood (wrong inf image)";
					out.clear();
					return false;
				}
			}
		}
		if (out.empty() && failReason)
			*failReason = "quest: no readable text blocks";
		return !out.empty();
	}

	void ApplySection(
		BufferLocator::Image imgCopy,
		const HeadersStore::SectionConfig& cfg,
		const std::unordered_map<int, std::string>& rows,
		TextApplicator::ApplyStats& st)
	{
		std::vector<LiveEntry> entries;
		std::string reason;
		bool ok = false;
		if (cfg.questTable)
			ok = CollectQuestEntries(imgCopy, cfg, entries, &reason);
		else if (cfg.entrySize > 0 && cfg.entryCount > 0)
			ok = CollectStructEntries(imgCopy, cfg, entries, &reason);
		else
			ok = CollectFlatEntries(imgCopy, cfg, entries, &reason);

		if (!ok) {
			++st.sectionsFail;
			Logger::Warn("Section resolve failed: %s (%s)", cfg.xpath.c_str(),
				reason.empty() ? "unknown" : reason.c_str());
			return;
		}

		// Reject obvious wrong-image tables; mild drift still applies to live slots.
		if (cfg.questTable && entries.size() > 8000) {
			++st.sectionsFail;
			Logger::Warn("Quest section rejected: %s live=%u (wrong inf image?)",
				cfg.xpath.c_str(), static_cast<unsigned>(entries.size()));
			return;
		}
		if (Config::Get().safeIndexApply && cfg.entryCount > 0 && !cfg.questTable) {
			const int live = static_cast<int>(entries.size());
			const int expected = cfg.entryCount;
			if (expected > 0 && live > 0) {
				// Large tables must match closely; loose 50%-200% was writing
				// English into the wrong slots and crashing the client.
				int lo = expected - (expected / 10); // -10%
				int hi = expected + (expected / 10); // +10%
				if (expected < 64) {
					lo = expected / 2;
					hi = expected * 2;
				}
				if (lo < 1)
					lo = 1;
				if (live < lo || live > hi) {
					++st.sectionsFail;
					Logger::Warn(
						"Section skipped (wrong image?): %s live=%d expected=%d",
						cfg.xpath.c_str(), live, expected);
					return;
				}
				if (live != expected) {
					Logger::Warn(
						"Section drift: %s live=%d expected=%d — applying to live indices",
						cfg.xpath.c_str(), live, expected);
				}
			}
		}

		++st.sectionsOk;
		Logger::Info("Section ok: %s entries=%u", cfg.xpath.c_str(),
			static_cast<unsigned>(entries.size()));

		for (const auto& kv : rows) {
			const int index = kv.first;
			if (index < 0 || index >= static_cast<int>(entries.size())) {
				++st.skippedResolve;
				continue;
			}
			const auto parts = GameCodec::SplitJoin(kv.second);
			const LiveEntry& entry = entries[static_cast<size_t>(index)];
			if (parts.size() != entry.slots.size()) {
				if (!(parts.size() == 1 && entry.slots.size() == 1)) {
					++st.skippedResolve;
					continue;
				}
			}
			const size_t n = (std::min)(parts.size(), entry.slots.size());
			for (size_t i = 0; i < n; ++i) {
				++st.attempted;
				const std::string sjis = EncodeTarget(parts[i]);
				if (sjis.empty() && !parts[i].empty()) {
					++st.skippedFit;
					continue;
				}
				WriteOrRelocate(imgCopy, entry.slots[i], sjis, st);
			}
		}
	}
}

namespace TextApplicator {
	bool LoadOffsetMap(const std::string& path) {
		g_offsetPatches.clear();
		std::ifstream in(path);
		if (!in) {
			Logger::Warn("Offset map not found (optional): %s", path.c_str());
			return false;
		}
		std::string line;
		while (std::getline(in, line)) {
			if (line.empty() || line[0] == '#')
				continue;
			try {
				json row = json::parse(line);
				OffsetPatch p;
				if (row.contains("address"))
					p.address = static_cast<uintptr_t>(std::stoull(row["address"].get<std::string>(), nullptr, 0));
				else if (row.contains("addr"))
					p.address = static_cast<uintptr_t>(std::stoull(row["addr"].get<std::string>(), nullptr, 0));
				if (row.contains("max"))
					p.maxLen = row["max"].get<size_t>();
				std::string target;
				if (row.contains("target"))
					target = row["target"].get<std::string>();
				p.bytes = EncodeTarget(target);
				if (p.address && !p.bytes.empty())
					g_offsetPatches.push_back(std::move(p));
			} catch (...) {
				continue;
			}
		}
		Logger::Info("Offset map loaded: %u patches from %s",
			static_cast<unsigned>(g_offsetPatches.size()), path.c_str());
		return !g_offsetPatches.empty();
	}

	ApplyStats ApplyOffsetMap() {
		g_last = {};
		for (const auto& p : g_offsetPatches) {
			++g_last.attempted;
			const size_t cap = p.maxLen ? p.maxLen : MeasureCString(p.address);
			if (WriteBytes(p.address, p.bytes, cap ? cap : p.bytes.size()))
				++g_last.written;
			else
				++g_last.skippedFit;
		}
		Logger::Info("Offset-map apply: wrote %u / %u (fit-skip %u)",
			g_last.written, g_last.attempted, g_last.skippedFit);
		return g_last;
	}

	ApplyStats ApplyLive() {
		g_last = {};
		if (!HeadersStore::Loaded()) {
			Logger::Warn("ApplyLive: headers.json not loaded");
			return g_last;
		}
		if (BufferLocator::FoundCount() == 0)
			BufferLocator::Refresh();
		if (BufferLocator::FoundCount() == 0) {
			Logger::Warn("ApplyLive: no game text images located yet");
			return g_last;
		}

		// Fresh bump pools per apply pass (bases may change after a forced rescan).
		g_pools.clear();

		for (const auto& xp : TranslationStore::ByXpath()) {
			auto cfg = HeadersStore::Get(xp.first);
			if (!cfg.valid) {
				++g_last.sectionsFail;
				Logger::Warn("No headers config for xpath %s", xp.first.c_str());
				continue;
			}
			const auto* img = BufferLocator::Get(cfg.fileKey);
			if (!img) {
				++g_last.sectionsFail;
				continue;
			}
			ApplySection(*img, cfg, xp.second, g_last);
		}

		Logger::Info(
			"Live apply: wrote %u / %u (reloc %u, fit-skip %u, resolve-skip %u, sections %u ok / %u fail)",
			g_last.written, g_last.attempted, g_last.relocated, g_last.skippedFit,
			g_last.skippedResolve, g_last.sectionsOk, g_last.sectionsFail);
		return g_last;
	}

	const ApplyStats& LastStats() { return g_last; }
}
