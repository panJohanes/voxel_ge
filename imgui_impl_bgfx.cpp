#include "imgui_impl_bgfx.h"
#include <imgui.h>
#include <bgfx/bgfx.h>
#include <bx/math.h>
#include <cstring>
#include <cstdio>
#include <iostream>
static bgfx::ViewId s_viewId = 255;
static bgfx::ProgramHandle s_program = BGFX_INVALID_HANDLE;
static bgfx::VertexLayout s_layout;
static bgfx::TextureHandle s_fontTexture = BGFX_INVALID_HANDLE;
static bgfx::UniformHandle s_fontSampler = BGFX_INVALID_HANDLE;


bgfx::ShaderHandle loadImGuiShader(const char* path){
    FILE* file = fopen(path, "rb");
    std::cout << "1\n";
    if (!file){
        std::cout << "failed to load file " << path << "\n";
        return BGFX_INVALID_HANDLE;
    }
    std::cout << "2\n";
    fseek(file, 0, SEEK_END);
    std::cout << "3\n";
    uint32_t size = static_cast<uint32_t>(ftell(file));
    std::cout << "4\n";
    fseek(file, 0, SEEK_SET);
    std::cout << "5\n";
    const bgfx::Memory* mem = bgfx::alloc(size);
    std::cout << "6\n";
    fread(mem->data, 1, size, file);
    std::cout << "7\n";
    fclose(file);
    std::cout << "8\n";
    //mem->data[mem->size-1]='\0';
    std::cout << "9\n";
    return bgfx::createShader(mem);
}
bool ImGui_Implbgfx_Init(int viewId)
{
    s_viewId = (bgfx::ViewId)viewId;

    bgfx::ShaderHandle vsh = loadImGuiShader("shaders/vs_ocornut_imgui.bin");
    bgfx::ShaderHandle fsh = loadImGuiShader("shaders/fs_ocornut_imgui.bin");

    if (!bgfx::isValid(vsh) || !bgfx::isValid(fsh))
    {
        std::cout << "Failed to load ImGui shaders\n";
        std::cin.get();
        return false;
    }

    s_program = bgfx::createProgram(vsh, fsh, true);
    if (!bgfx::isValid(s_program))
    {
        std::cout << "Failed to create ImGui program\n";
        return false;
    }

    s_layout.begin()
        .add(bgfx::Attrib::Position, 2, bgfx::AttribType::Float)
        .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
        .add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Uint8, true)
        .end();

    s_fontSampler = bgfx::createUniform("s_tex", bgfx::UniformType::Sampler);

    ImGuiIO& io = ImGui::GetIO();
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    io.Fonts->Build();

    return true;
}

void ImGui_Implbgfx_Shutdown() {
    if (bgfx::isValid(s_fontTexture)) {
        bgfx::destroy(s_fontTexture);
        s_fontTexture = BGFX_INVALID_HANDLE;
    }
    if (bgfx::isValid(s_program)) {
        bgfx::destroy(s_program);
        s_program = BGFX_INVALID_HANDLE;
    }
}

void ImGui_Implbgfx_NewFrame() {
}
void ImGui_Implbgfx_RenderDrawLists(ImDrawData* draw_data) {
    if (draw_data->Textures != nullptr) {
        for (ImTextureData* tex : *draw_data->Textures) {
            if (tex->Status == ImTextureStatus_WantCreate) {
                if (!bgfx::isValid(s_fontTexture) && tex->Pixels) {

                    s_fontTexture = bgfx::createTexture2D(
                        (uint16_t)tex->Width, (uint16_t)tex->Height, false, 1,
                        bgfx::TextureFormat::RGBA8, 
                        BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP | BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT,
                        bgfx::copy(tex->Pixels, tex->Width * tex->Height * 4)
                    );
                }
                tex->SetTexID((ImTextureID)(intptr_t)s_fontTexture.idx);
                tex->SetStatus(ImTextureStatus_OK);
            } else if (tex->Status == ImTextureStatus_WantUpdates) {
                tex->SetStatus(ImTextureStatus_OK);
            } else if (tex->Status == ImTextureStatus_WantDestroy) {
                if (bgfx::isValid(s_fontTexture)) {
                    bgfx::destroy(s_fontTexture);
                    s_fontTexture = BGFX_INVALID_HANDLE;
                }
                tex->SetStatus(ImTextureStatus_Destroyed);
            } else {
                tex->SetStatus(ImTextureStatus_OK);
            }
        }
    }

    if (draw_data->TotalVtxCount == 0) return;

    int fb_width = (int)(draw_data->DisplaySize.x * draw_data->FramebufferScale.x);
    int fb_height = (int)(draw_data->DisplaySize.y * draw_data->FramebufferScale.y);
    if (fb_width <= 0 || fb_height <= 0) return;

    bgfx::setViewRect(s_viewId, 0, 0, (uint16_t)fb_width, (uint16_t)fb_height);

    float ortho[16];
    float L = draw_data->DisplayPos.x;
    float R = draw_data->DisplayPos.x + draw_data->DisplaySize.x;
    float T = draw_data->DisplayPos.y;
    float B = draw_data->DisplayPos.y + draw_data->DisplaySize.y;
    bx::mtxOrtho(ortho, L, R, B, T, -1.0f, 1.0f, 0.0f, bgfx::getCaps()->homogeneousDepth);

    bgfx::setViewTransform(s_viewId, nullptr, ortho);

    for (int n = 0; n < draw_data->CmdListsCount; n++) {
        const ImDrawList* cmd_list = draw_data->CmdLists[n];
        uint32_t numVertices = (uint32_t)cmd_list->VtxBuffer.Size;
        uint32_t numIndices = (uint32_t)cmd_list->IdxBuffer.Size;

        if (numVertices == 0 || numIndices == 0) continue;

        bgfx::TransientVertexBuffer tvb;
        bgfx::TransientIndexBuffer tib;

        bgfx::allocTransientVertexBuffer(&tvb, numVertices, s_layout);
        bgfx::allocTransientIndexBuffer(&tib, numIndices, sizeof(ImDrawIdx) == 4);

        std::memcpy(tvb.data, cmd_list->VtxBuffer.Data, numVertices * sizeof(ImDrawVert));
        std::memcpy(tib.data, cmd_list->IdxBuffer.Data, numIndices * sizeof(ImDrawIdx));

        for (int cmd_i = 0; cmd_i < cmd_list->CmdBuffer.Size; cmd_i++) {
            const ImDrawCmd* pcmd = &cmd_list->CmdBuffer[cmd_i];
            if (pcmd->UserCallback) {
                pcmd->UserCallback(cmd_list, pcmd);
            } else {
                if (pcmd->ElemCount == 0) continue;

                uint16_t xx = (uint16_t)pcmd->ClipRect.x;
                uint16_t yy = (uint16_t)pcmd->ClipRect.y;
                uint16_t ww = (uint16_t)(pcmd->ClipRect.z - pcmd->ClipRect.x);
                uint16_t hh = (uint16_t)(pcmd->ClipRect.w - pcmd->ClipRect.y);

                bgfx::setState(0
                    | BGFX_STATE_WRITE_RGB
                    | BGFX_STATE_WRITE_A
                    | BGFX_STATE_MSAA
                    | BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_SRC_ALPHA, BGFX_STATE_BLEND_INV_SRC_ALPHA)
                );

                bgfx::TextureHandle texture = { (uint16_t)((intptr_t)pcmd->GetTexID() & 0xffff) };
                bgfx::setScissor(xx, yy, ww, hh);
                
                bgfx::setTexture(0, s_fontSampler, texture);
                
                bgfx::setVertexBuffer(0, &tvb, 0, numVertices);
                bgfx::setIndexBuffer(&tib, pcmd->IdxOffset, pcmd->ElemCount);
                bgfx::submit(s_viewId, s_program);
            }
        }
    }
}