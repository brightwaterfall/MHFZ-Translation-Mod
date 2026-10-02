#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace TextApplicator {
	struct ApplyStats {
		unsigned attempted = 0;
		unsigned written = 0;
		unsigned skippedFit = 0;
		unsigned skippedResolve = 0;
		unsigned relocated = 0;
		unsigned sectionsOk = 0;
		unsigned sectionsFail = 0;
	};

	// Optional absolute-address map (CN / debug).
	bool LoadOffsetMap(const std::string& path);
	ApplyStats ApplyOffsetMap();

	// TextHandler-equivalent: locate live bins, walk headers sections, apply EN rows.
	ApplyStats ApplyLive();

	const ApplyStats& LastStats();
}
