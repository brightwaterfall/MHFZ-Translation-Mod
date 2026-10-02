#pragma once
#include "mod.h"

class TextPatchMod : public Mod {
public:
	using Mod::Mod;

	void OnAttach() override;
	void OnDetach() override;
	void OnImGUIInit() override;
	void OnUpdateLobby() override;
	void OnUpdateQuest() override;
	void DrawModMenu() override;
	void DrawUI(bool show_menu) override;
	void InitImGUIContext(ImGuiContext* ctx) override {
		ImGui::SetCurrentContext(ctx);
	}
};
