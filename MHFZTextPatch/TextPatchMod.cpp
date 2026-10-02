#include "TextPatchMod.h"
#include "Version.h"
#include "globals.h"
#include "config/Config.h"
#include "logging/Logger.h"
#include "translation/TranslationStore.h"
#include "translation/TextApplicator.h"
#include "translation/HeadersStore.h"
#include "translation/BufferLocator.h"
#include "translation/ContentPatcher.h"
#include <Windows.h>
#include <atomic>
#include <process.h>

namespace {
	constexpr DWORD kContentSearchIntervalMs = 15000;
	constexpr DWORD kContentRefreshIntervalMs = 45000;

	bool g_attached = false;
	bool g_contentMode = false;
	std::atomic<bool> g_contentForce{false};
	std::atomic<bool> g_appliedOnce{false};
	std::atomic<unsigned> g_applyAttempts{0};
	std::atomic<DWORD> g_nextApplyMs{0};
	std::atomic<bool> g_workerStop{false};
	HANDLE g_worker = nullptr;

	std::string ModDir() {
		char path[MAX_PATH] = {};
		HMODULE self = nullptr;
		GetModuleHandleExA(
			GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCSTR>(&ModDir),
			&self);
		GetModuleFileNameA(self, path, MAX_PATH);
		std::string p(path);
		const size_t slash = p.find_last_of("\\/");
		if (slash != std::string::npos)
			p.resize(slash + 1);
		return p;
	}

	std::string GameModsDir() {
		char path[MAX_PATH] = {};
		GetModuleFileNameA(nullptr, path, MAX_PATH);
		std::string p(path);
		const size_t slash = p.find_last_of("\\/");
		if (slash != std::string::npos)
			p.resize(slash + 1);
		return p + "mods\\";
	}

	std::string ResolvePayloadPath(const std::string& name) {
		const std::string candidates[] = {
			GameModsDir() + name,
			ModDir() + name,
			GameModsDir() + "..\\data\\" + name,
			ModDir() + "..\\data\\" + name,
			GameModsDir() + "translations-en.json",
			ModDir() + "translations-en.json",
			ModDir() + "..\\data\\translations-en.json",
			GameModsDir() + "translations-en.sample.json",
			ModDir() + "..\\data\\translations-en.sample.json",
		};
		for (const auto& c : candidates) {
			if (GetFileAttributesA(c.c_str()) != INVALID_FILE_ATTRIBUTES)
				return c;
		}
		return GameModsDir() + name;
	}

	void TryApply(bool forceRescan) {
		if (!Config::Get().enabled)
			return;

		const DWORD now = GetTickCount();
		const DWORD next = g_nextApplyMs.load();
		if (!forceRescan && next != 0 && now < next)
			return;
		g_nextApplyMs = now + 2500;

		const unsigned attempt = g_applyAttempts.fetch_add(1) + 1;
		Logger::Info("Apply attempt #%u (images=%u force=%d)",
			attempt,
			static_cast<unsigned>(BufferLocator::FoundCount()),
			forceRescan ? 1 : 0);

		if (forceRescan || BufferLocator::FoundCount() == 0)
			BufferLocator::Refresh();

		TextApplicator::ApplyStats st = TextApplicator::ApplyLive();
		if (st.written == 0 && st.attempted == 0)
			st = TextApplicator::ApplyOffsetMap();

		// In-image trailing-zero reloc only (never auto-enable heap reloc).
		if (st.written == 0 && st.skippedFit > 0 && Config::Get().inPlaceOnly &&
			!Config::Get().allowHeapReloc) {
			Logger::Info(
				"0 writes with %u fit-skips — retrying with in-image reloc only (no heap)",
				st.skippedFit);
			Config::Get().inPlaceOnly = false;
			st = TextApplicator::ApplyLive();
			Config::Get().inPlaceOnly = true;
		}

		Logger::Info(
			"Apply result #%u: wrote=%u attempted=%u fitSkip=%u resolveSkip=%u "
			"reloc=%u sectionsOk=%u sectionsFail=%u images=%u",
			attempt, st.written, st.attempted, st.skippedFit, st.skippedResolve,
			st.relocated, st.sectionsOk, st.sectionsFail,
			static_cast<unsigned>(BufferLocator::FoundCount()));

		const bool healthy =
			st.sectionsOk >= 3 &&
			st.written >= 20 &&
			st.sectionsFail <= st.sectionsOk * 3;
		if (healthy) {
			g_appliedOnce = true;
			Logger::Info("Apply healthy — will idle unless reapplyEachTick=1");
		}
		// Stop hammering after repeated zero-write failures once images exist.
		if (!healthy && attempt >= 8 && st.written == 0 && BufferLocator::FoundCount() > 0) {
			g_appliedOnce = true;
			Logger::Warn("Stopping apply retries after %u failed attempts (wrote=0)", attempt);
		}
		// Also stop if we only keep failing sections — do not keep rewriting.
		if (!healthy && attempt >= 12 && st.sectionsOk == 0 && BufferLocator::FoundCount() > 0) {
			g_appliedOnce = true;
			Logger::Warn("Stopping apply retries — no sections resolved after %u attempts", attempt);
		}
	}

	std::string FindDataFile(const std::string& name) {
		const std::string candidates[] = {
			GameModsDir() + name,
			ModDir() + name,
			GameModsDir() + "..\\data\\" + name,
			ModDir() + "..\\data\\" + name,
		};
		for (const auto& c : candidates) {
			if (GetFileAttributesA(c.c_str()) != INVALID_FILE_ATTRIBUTES)
				return c;
		}
		return {};
	}

	// Content mode: verified byte-pair patching. Runs only on this worker so
	// full memory scans never stall the game or render threads.
	void ContentLoop() {
		DWORD nextMs = GetTickCount();
		while (!g_workerStop.load()) {
			const DWORD now = GetTickCount();
			if (Config::Get().enabled && (g_contentForce.exchange(false) || static_cast<LONG>(now - nextMs) >= 0)) {
				g_applyAttempts.fetch_add(1);
				ContentPatcher::Apply();
				g_appliedOnce = ContentPatcher::ImagesFound();
				nextMs = GetTickCount()
					+ (ContentPatcher::AllImagesFound() ? kContentRefreshIntervalMs : kContentSearchIntervalMs);
			}
			Sleep(200);
		}
	}

	unsigned __stdcall ApplyWorker(void*) {
		Logger::Info("Apply worker started (independent of lobby/quest ticks)");
		// Bins often finish decompressing well after Pax OnAttach.
		Sleep(3000);
		if (g_contentMode) {
			ContentLoop();
			Logger::Info("Apply worker stopped");
			return 0;
		}
		while (!g_workerStop.load()) {
			// Once marked done (healthy OR gave up), idle unless reapplyEachTick.
			if (!g_appliedOnce.load() || Config::Get().reapplyEachTick)
				TryApply(false);
			for (int i = 0; i < 25 && !g_workerStop.load(); ++i)
				Sleep(100);
		}
		Logger::Info("Apply worker stopped");
		return 0;
	}

	void StartWorker() {
		if (g_worker)
			return;
		g_workerStop = false;
		g_worker = reinterpret_cast<HANDLE>(_beginthreadex(nullptr, 0, ApplyWorker, nullptr, 0, nullptr));
		if (!g_worker)
			Logger::Warn("Failed to start apply worker — relying on Pax ticks only");
	}

	void StopWorker() {
		g_workerStop = true;
		if (g_worker) {
			WaitForSingleObject(g_worker, 8000);
			CloseHandle(g_worker);
			g_worker = nullptr;
		}
	}
}

void TextPatchMod::OnAttach() {
	Logger::Init();
	Logger::Info("Initializing %s v%s", DISPLAY_NAME.c_str(), VERSION.c_str());
	Logger::Info("%s", MHFZ_TARGET_NOTE);
	Logger::Info("Pax mhfdll_addy: 0x%08X", static_cast<unsigned>(mhfdll_addy));

	Config::Load();

	// Crash-safety defaults for TW/CN: never rewrite string pointers onto heap.
	if (Config::Get().clientProfile == "tw" || Config::Get().clientProfile == "cn") {
		if (!Config::Get().allowHeapReloc && !Config::Get().inPlaceOnly) {
			Logger::Warn("Enabling inPlaceOnly for %s (heap reloc disabled — prevents crashes)",
				Config::Get().clientProfile.c_str());
			Config::Get().inPlaceOnly = true;
		}
		Config::Get().safeIndexApply = true;
	}

	Logger::Info("safeIndexApply=%d inPlaceOnly=%d allowHeapReloc=%d clientProfile=%s reapplyEachTick=%d",
		Config::Get().safeIndexApply ? 1 : 0,
		Config::Get().inPlaceOnly ? 1 : 0,
		Config::Get().allowHeapReloc ? 1 : 0,
		Config::Get().clientProfile.c_str(),
		Config::Get().reapplyEachTick ? 1 : 0);

	HeadersStore::SetGameVersion(Config::Get().clientProfile);

	const auto& cfg = Config::Get();
	const std::string pairsPath = FindDataFile("pairs-" + cfg.clientProfile + ".json");
	g_contentMode = !pairsPath.empty() && ContentPatcher::Load(pairsPath);
	if (g_contentMode) {
		g_appliedOnce = false;
		g_applyAttempts = 0;
		g_attached = true;
		StartWorker();
		Logger::Info("Attach complete (content mode: verified byte pairs, headers not used)");
		return;
	}
	Logger::Info("No pairs-%s.json found — using header/index apply", cfg.clientProfile.c_str());
	std::string payloadName = cfg.payload;
	if (payloadName.empty()) {
		const bool twLike = (cfg.clientProfile == "tw" || cfg.clientProfile == "cn");
		const char* prefer = twLike ? "translations-tw.json" : "translations-en.json";
		const char* fallback = "translations-en.json";
		if (GetFileAttributesA((GameModsDir() + prefer).c_str()) != INVALID_FILE_ATTRIBUTES ||
			GetFileAttributesA((ModDir() + prefer).c_str()) != INVALID_FILE_ATTRIBUTES) {
			payloadName = prefer;
		} else {
			payloadName = fallback;
		}
	}
	const std::string payloadPath = ResolvePayloadPath(payloadName);
	Logger::Info("Payload path: %s", payloadPath.c_str());
	TranslationStore::LoadFromFile(payloadPath);

	std::string headersName = cfg.headersFile;
	if (headersName.empty()) {
		const std::string profileHeaders = "headers." + cfg.clientProfile + ".json";
		if (GetFileAttributesA((GameModsDir() + profileHeaders).c_str()) != INVALID_FILE_ATTRIBUTES ||
			GetFileAttributesA((ModDir() + profileHeaders).c_str()) != INVALID_FILE_ATTRIBUTES ||
			GetFileAttributesA((ModDir() + "..\\data\\" + profileHeaders).c_str()) != INVALID_FILE_ATTRIBUTES) {
			headersName = profileHeaders;
		} else {
			headersName = "headers.json";
		}
	}
	const std::string headersPath = ResolvePayloadPath(headersName);
	Logger::Info("Headers path: %s", headersPath.c_str());
	HeadersStore::LoadFromFile(headersPath);

	const std::string mapPath = ResolvePayloadPath("runtime_patch.jsonl");
	TextApplicator::LoadOffsetMap(mapPath);

	g_appliedOnce = false;
	g_applyAttempts = 0;
	g_nextApplyMs = GetTickCount() + 2000;
	g_attached = true;
	StartWorker();
	Logger::Info("Attach complete (worker will apply when text images appear)");
}

void TextPatchMod::OnDetach() {
	if (!g_attached)
		return;
	Logger::Info("Detaching...");
	StopWorker();
	Config::Save();
	TranslationStore::Clear();
	Logger::Shutdown();
	g_attached = false;
}

void TextPatchMod::OnImGUIInit() {
	Logger::Info("ImGui context ready");
}

void TextPatchMod::OnUpdateLobby() {
	if (!g_contentMode && (Config::Get().reapplyEachTick || !g_appliedOnce.load()))
		TryApply(false);
}

void TextPatchMod::OnUpdateQuest() {
	if (!g_contentMode && (Config::Get().reapplyEachTick || !g_appliedOnce.load()))
		TryApply(false);
}

void TextPatchMod::DrawModMenu() {
	ImGui::TextUnformatted("MHFZ Text Patch");
	ImGui::Text("Version: %s", VERSION.c_str());
	ImGui::Separator();
	auto& cfg = Config::Get();
	if (g_contentMode) {
		ImGui::Checkbox("Enabled", &cfg.enabled);
		const auto& st = ContentPatcher::Last();
		ImGui::Text("Profile: %s  pairs: %u", cfg.clientProfile.c_str(),
			static_cast<unsigned>(ContentPatcher::PairCount()));
		ImGui::TextWrapped("Images: %s", ContentPatcher::ImageSummary().c_str());
		ImGui::Text("Pass #%u: verified %u, in place %u, redirected %u", st.pass, st.verified, st.inPlace, st.redirected);
		ImGui::Text("Already applied %u, missing %u, unreferenced %u", st.alreadyApplied, st.missing, st.unreferenced);
		if (ImGui::Button("Re-scan + apply"))
			g_contentForce = true;
		if (ImGui::Button("Save config"))
			Config::Save();
		return;
	}
	ImGui::Checkbox("Enabled", &cfg.enabled);
	ImGui::Checkbox("Re-apply each tick", &cfg.reapplyEachTick);
	ImGui::Checkbox("Safe index apply (CN/TW)", &cfg.safeIndexApply);
	ImGui::Checkbox("In-place only (no reloc)", &cfg.inPlaceOnly);
	ImGui::Text("Profile: %s  headers ver: %s",
		cfg.clientProfile.c_str(), HeadersStore::GameVersion().c_str());
	ImGui::Text("Rows loaded: %u", static_cast<unsigned>(TranslationStore::Count()));
	ImGui::Text("Images found: %u", static_cast<unsigned>(BufferLocator::FoundCount()));
	ImGui::Text("Apply attempts: %u  healthy=%d",
		g_applyAttempts.load(), g_appliedOnce.load() ? 1 : 0);
	const auto& st = TextApplicator::LastStats();
	ImGui::Text("Last apply: %u written / %u attempted", st.written, st.attempted);
	ImGui::Text("Relocated: %u  sections ok/fail: %u/%u", st.relocated, st.sectionsOk, st.sectionsFail);
	if (ImGui::Button("Re-scan + apply")) {
		g_appliedOnce = false;
		g_applyAttempts = 0;
		TryApply(true);
	}
	if (ImGui::Button("Save config"))
		Config::Save();
	ImGui::TextWrapped(
		"v0.2.9: crash-hardened. Delete mhfz_text_patch.ini before testing. "
		"TW uses translations-tw.json + headers.tw.json.");
}

void TextPatchMod::DrawUI(bool /*show_menu*/) {
	// Backup apply path if Pax never calls lobby/quest updates.
	if (g_attached && !g_contentMode && (Config::Get().reapplyEachTick || !g_appliedOnce.load()))
		TryApply(false);
}

extern "C" {
	__declspec(dllexport) void setDllAddress(int dll_addy) {
		mhfdll_addy = dll_addy;
	}

	__declspec(dllexport) Mod* createMod() {
		return new TextPatchMod(NAME, DISPLAY_NAME, VERSION, REQUIRED_VERSION, HGE_ONLY);
	}
}
