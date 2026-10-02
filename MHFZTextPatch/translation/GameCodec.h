#pragma once
#include <string>
#include <vector>

namespace GameCodec {
	// Mogapedia brace form → game bytes lexical form (~CNN / ~C00), still UTF-8 text.
	std::string ColorCodesFromCsv(const std::string& text);

	// UTF-8 → CP932 (Shift_JIS). Empty on failure.
	std::string Utf8ToSjis(const std::string& utf8);

	// Split on {j} or legacy <join at="N">.
	std::vector<std::string> SplitJoin(const std::string& text);
}
