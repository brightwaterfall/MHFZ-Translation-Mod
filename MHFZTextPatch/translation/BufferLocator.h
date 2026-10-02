#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>

namespace BufferLocator {
	struct Image {
		uintptr_t base = 0;
		size_t size = 0;
		bool absolutePointers = false; // true if slot values are VAs
	};

	// Scan process memory for decompressed game bins using headers fingerprints.
	bool Refresh();
	const Image* Get(const std::string& fileKey); // dat, pac, inf, ...
	size_t FoundCount();

	// Test/helpers: inject a known image (skips scanning for that key).
	void InjectImage(const std::string& fileKey, uintptr_t base, size_t size, bool absolutePointers);
	void Clear();
}
