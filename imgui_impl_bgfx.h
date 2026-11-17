#pragma once
#include <imgui.h>

bool ImGui_Implbgfx_Init(int viewId);
void ImGui_Implbgfx_Shutdown();
void ImGui_Implbgfx_NewFrame();
void ImGui_Implbgfx_RenderDrawLists(ImDrawData* draw_data);
