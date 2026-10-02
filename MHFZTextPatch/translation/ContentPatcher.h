#pragma once
#include <cstdint>
#include <string>

// Header-free live patcher driven by pairs-<profile>.json:
// exact client source bytes -> replacement bytes, with the file offsets where
// each source string lives. Every write is preceded by a byte-for-byte check
// of the string it replaces, so a wrong table or a different client build can
// only make the patcher do nothing, never write to the wrong place.
namespace ContentPatcher {
	struct Stats {
		unsigned pass = 0;
		unsigned imagesFound = 0;
		unsigned verified = 0;       // source strings confirmed at base+offset
		unsigned alreadyApplied = 0; // strings already showing our text
		unsigned missing = 0;        // offsets that matched neither source nor target
		unsigned inPlace = 0;        // strings overwritten in place this pass
		unsigned redirected = 0;     // pointer slots swapped to pooled text this pass
		unsigned slotsRejected = 0;  // candidate slots with no neighbouring hit
		unsigned unreferenced = 0;   // verified long strings no pointer slot was found for
		unsigned liveRedirects = 0;  // slots currently pointing into our pool
		unsigned contentHits = 0;    // source strings found outside located images
		unsigned contentInPlace = 0; // of those, overwritten in place this pass
		unsigned scanMs = 0;
	};

	bool Load(const std::string& path);
	bool Loaded();
	size_t PairCount();
	bool ImagesFound();
	bool AllImagesFound();
	Stats Apply();
	const Stats& Last();
	std::string ImageSummary();
}
