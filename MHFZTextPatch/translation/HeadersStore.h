#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include "../include/json.hpp"

namespace HeadersStore {
	struct SectionConfig {
		std::string xpath;
		std::string fileKey; // dat, pac, inf, ...
		uint32_t beginPointer = 0;
		int entryCount = 0;
		int entrySize = 0;
		int fieldOffset = 0;
		std::vector<int> fieldOffsets;
		int pointersPerEntry = 1;
		bool nullPadding = false;
		bool nullTerminated = false;
		bool groupedEntries = false;
		bool literalBase = false;
		bool questTable = false;
		uint32_t nextFieldPointer = 0;
		uint32_t countBasePointer = 0;
		uint32_t countOffset = 0;
		std::string countType = "u16";
		int countAdjust = 0;
		uint32_t questTextOffset = 0x28;
		int textPointersCount = 8;
		int maxSubCount = 0;
		bool valid = false;
	};

	// zz (JP), ko, tw (Taiwan / many CN private servers), cn
	void SetGameVersion(const std::string& version);
	const std::string& GameVersion();

	bool LoadFromFile(const std::string& path);
	bool Loaded();
	SectionConfig Get(const std::string& xpath);
	std::string FileKeyForXpath(const std::string& xpath);
}
