#include "GameCodec.h"
#include <Windows.h>
#include <regex>
#include <sstream>

namespace GameCodec {
	std::string ColorCodesFromCsv(const std::string& text) {
		static const std::regex re(R"(\{/c\}|\{c(\d{2})\})");
		std::string out;
		out.reserve(text.size());
		std::sregex_iterator it(text.begin(), text.end(), re), end;
		size_t last = 0;
		for (; it != end; ++it) {
			const auto& m = *it;
			out.append(text, last, static_cast<size_t>(m.position()) - last);
			if (m.str() == "{/c}")
				out += "~C00";
			else
				out += "~C" + m[1].str();
			last = static_cast<size_t>(m.position() + m.length());
		}
		out.append(text, last, std::string::npos);
		return out;
	}

	std::string Utf8ToSjis(const std::string& utf8) {
		if (utf8.empty())
			return {};
		const int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), nullptr, 0);
		if (wlen <= 0)
			return {};
		std::wstring wide(wlen, L'\0');
		MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), wide.data(), wlen);
		const int slen = WideCharToMultiByte(932, 0, wide.c_str(), wlen, nullptr, 0, nullptr, nullptr);
		if (slen <= 0)
			return {};
		std::string sjis(slen, '\0');
		WideCharToMultiByte(932, 0, wide.c_str(), wlen, sjis.data(), slen, nullptr, nullptr);
		return sjis;
	}

	std::vector<std::string> SplitJoin(const std::string& text) {
		static const std::regex re(R"(\{j\}|<join at=\"-?\d+\">)");
		std::vector<std::string> parts;
		std::sregex_token_iterator it(text.begin(), text.end(), re, -1), end;
		for (; it != end; ++it)
			parts.push_back(it->str());
		if (parts.empty())
			parts.push_back(text);
		return parts;
	}
}
