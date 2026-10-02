#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

struct TranslationEntry {
	std::string xpath;
	int index = -1;
	std::string target; // UTF-8 from JSON
};

namespace TranslationStore {
	bool LoadFromFile(const std::string& path);
	const std::vector<TranslationEntry>& Entries();
	// xpath -> (index -> target UTF-8)
	const std::unordered_map<std::string, std::unordered_map<int, std::string>>& ByXpath();
	size_t Count();
	void Clear();
}
