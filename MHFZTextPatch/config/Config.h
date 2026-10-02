#pragma once
#include <string>

namespace Config {
	struct Settings {
		bool enabled = true;
		std::string lang = "en";
		// Empty = auto: translations-tw.json for tw/cn when present, else translations-en.json
		std::string payload;
		bool reapplyEachTick = false;
		// Skip sections whose live entry count diverges from headers (CN/TW drift).
		bool safeIndexApply = true;
		// Never rewrite pointer slots — in-place fit only.
		// Default true for crash safety: heap reloc of game pointers is dangerous.
		bool inPlaceOnly = true;
		// Allow VirtualAlloc heap relocation (off by default — can crash the client).
		bool allowHeapReloc = false;
		// Client structure profile: tw (Taiwan/CN private), zz (JP), ko, cn
		std::string clientProfile = "tw";
		// Optional override; empty = headers.<profile>.json then headers.json
		std::string headersFile;
	};

	Settings& Get();
	void Load();
	void Save();
}
