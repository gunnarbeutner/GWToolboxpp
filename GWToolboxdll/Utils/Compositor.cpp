#include "stdafx.h"

#include "Compositor.h"

#include <algorithm>

#include <GWCA/GameContainers/Array.h>
#include <GWCA/Managers/MapMgr.h>
#include <GWCA/Managers/UIMgr.h>
#include <GWCA/Utilities/Hooker.h>
#include <GWCA/Utilities/Scanner.h>

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_impl_dx9.h>
#include <D3DContainers.h>
#include <Utils/GuiUtils.h>

namespace Compositor {

    // -----------------------------------------------------------------------
    // GW internal types
    // -----------------------------------------------------------------------

    enum FrCacheEntryType : uint32_t {
        FRCACHE_GPU_RENDER      = 0,
        FRCACHE_FRAME_CALLBACK  = 1,
        FRCACHE_CLIENT_VIEWPORT = 2,
        FRCACHE_FRAME_VIEWPORT  = 3,
    };

    struct FrCacheBufferEntry {
        FrCacheEntryType type;
        uint32_t index;  // into FrameArray (types 1/2/3) or SecondaryArray start (type 0)
        uint32_t param;  // model count (type 0) or other
    };

    // -----------------------------------------------------------------------
    // Scanned globals — resolved at hook install time via GW::Scanner
    // -----------------------------------------------------------------------

    // Pointers to GW's Array<T> globals (buffer, capacity, size, param — 16 bytes each)
    static GW::Array<FrCacheBufferEntry>* s_RenderBuffer = nullptr;
    static GW::Array<GW::UI::Frame*>* s_FrameArray = nullptr;
    static GW::Array<int*>* s_SecondaryArray = nullptr;
    static GW::Array<GW::UI::Frame*>* s_ZSortList = nullptr; // z-sorted popup container(s)
    static GW::Array<GW::UI::Frame*>* s_OverlayList = nullptr; // overlay/popup root frames
    static void** s_GrDev_Default = nullptr;

    static IDirect3DDevice9* s_device = nullptr;

    static GW::Array<FrCacheBufferEntry>& RenderBuffer() { return *s_RenderBuffer; }
    static GW::Array<GW::UI::Frame*>& FrameArray() { return *s_FrameArray; }
    static GW::Array<int*>& SecondaryArray() { return *s_SecondaryArray; }
    static GW::Array<GW::UI::Frame*>& ZSortList() { return *s_ZSortList; }

    // -----------------------------------------------------------------------
    // Hook types and state
    // -----------------------------------------------------------------------

    using FrCacheRenderFn = void(__cdecl*)(uint32_t, uint32_t);

    // FrCache_RenderAll — hook on the buffer processing function
    static FrCacheRenderFn FrCacheRenderAll_Original = nullptr;
    static FrCacheRenderFn FrCacheRenderAll_Trampoline = nullptr;

    // Internal functions called by FrCache_RenderAll — we call these directly
    // to re-implement the buffer processing with TB injection points.
    using FrCacheRenderEntryFn = void(__cdecl*)(int*, uint32_t, int*, uint32_t);
    using FrCacheSetViewportFn = void(__cdecl*)(int*, float*);
    using FramePosGetRectFn = float*(__thiscall*)(void*, float*);
    using ViewportTransformRectFn = void(__cdecl*)(float*, float*);
    using FrMsgDispatchFn = void(__fastcall*)(void*, void*, int, void*, uint32_t);
    using FrameStateTestFlagFn = uint32_t(__thiscall*)(void*, uint32_t);

    using GrDevFlushQueuesFn = bool(__fastcall*)(void*);
    static GrDevFlushQueuesFn GrDev_FlushQueues_Fn = nullptr;

    static FrCacheRenderEntryFn FrCacheRenderEntry_Fn = nullptr;
    static FrCacheSetViewportFn FrCacheSetViewport_Fn = nullptr;
    static FramePosGetRectFn FramePosGetClientRect_Fn = nullptr;
    static FramePosGetRectFn FramePosGetFrameRect_Fn = nullptr;
    static ViewportTransformRectFn ViewportTransformRect_Fn = nullptr;
    static FrMsgDispatchFn FrMsgDispatch_Fn = nullptr;
    static FrameStateTestFlagFn FrameStateTestFlag_Fn = nullptr;

    // -----------------------------------------------------------------------
    // UnifiedZOrder
    // -----------------------------------------------------------------------

    UnifiedZOrder& GetUnifiedZOrder()
    {
        static UnifiedZOrder instance;
        return instance;
    }

    void UnifiedZOrder::Update()
    {
        auto* root = GW::UI::GetRootFrame();
        if (!root) return;

        // Discover floating windows from the overlay list (z > 0 = floating).
        gw_windows_.clear();
        current_frames_.clear();
        if (s_OverlayList && s_OverlayList->m_buffer) {
            auto& overlays = *s_OverlayList;
            for (uint32_t k = 0; k < overlays.size(); k++) {
                auto* frame = overlays[k];
                if (!frame || !frame->field1_0x0) continue; // z == 0 → not floating
                if (!frame->IsVisible()) continue;

                current_frames_.insert(frame);
                GWWindowInfo info{};
                info.frame = frame;
                const auto tl = frame->position.GetTopLeftOnScreen(root);
                const auto br = frame->position.GetBottomRightOnScreen(root);
                info.bounds = {tl.x, tl.y, br.x, br.y};
                // Persist z across frames — only assign once when first visible
                auto it = gw_z_.find(frame);
                if (it != gw_z_.end()) {
                    info.unified_z = it->second;
                } else {
                    info.unified_z = next_z_++;
                    gw_z_[frame] = info.unified_z;
                }
                gw_windows_.push_back(info);
            }
        }

        // Remove stale entries (closed windows or frames from a previous map)
        for (auto it = gw_z_.begin(); it != gw_z_.end(); ) {
            if (!current_frames_.count(it->first))
                it = gw_z_.erase(it);
            else
                ++it;
        }

        // Build composite order: merge GW windows + TB windows, sort by z
        composite_order_.clear();
        for (const auto& w : gw_windows_) {
            composite_order_.push_back({w.unified_z, false, nullptr, w.frame, w.bounds});
        }
        for (const auto& [name, z] : tb_z_) {
            composite_order_.push_back({z, true, name.c_str(), nullptr, {}});
        }
        std::sort(composite_order_.begin(), composite_order_.end(),
                  [](const CompositeEntry& a, const CompositeEntry& b) { return a.z < b.z; });
    }

    void UnifiedZOrder::OnWindowFocused(const char* tb_name)
    {
        tb_z_[tb_name] = next_z_++;
    }

    uint64_t UnifiedZOrder::GetZ(const char* tb_name) const
    {
        auto it = tb_z_.find(tb_name);
        return it != tb_z_.end() ? it->second : 0;
    }

    void UnifiedZOrder::OnGWWindowClicked(float x, float y)
    {
        // Find the topmost GW window (highest GW z) under the cursor.
        // The overlay list can contain parent containers that overlap their
        // children — only bump the frontmost one.
        GWWindowInfo* topmost = nullptr;
        for (auto& w : gw_windows_) {
            if (!w.bounds.ContainsPoint(x, y)) continue;
            if (!topmost || w.frame->field1_0x0 > topmost->frame->field1_0x0)
                topmost = &w;
        }
        if (topmost) {
            topmost->unified_z = next_z_++;
            gw_z_[topmost->frame] = topmost->unified_z;
        }
    }

    // -----------------------------------------------------------------------
    // Frame draw callback — invoked from FrCacheRenderAll_Hook before compositing
    // -----------------------------------------------------------------------

    static FrameDrawCallback s_frame_draw_callback = nullptr;
    static bool s_hook_failed = false;

    // -----------------------------------------------------------------------
    // D3D rendering helpers
    // -----------------------------------------------------------------------

    struct D3DImGuiVertex { float pos[3]; D3DCOLOR col; float uv[2]; };
    #define D3DFVF_IMGUI (D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1)
    #ifndef IMGUI_USE_BGRA_PACKED_COLOR
    #define IMGUI_COL_TO_DX9_ARGB(_COL) (((_COL) & 0xFF00FF00) | (((_COL) & 0xFF0000) >> 16) | (((_COL) & 0xFF) << 16))
    #else
    #define IMGUI_COL_TO_DX9_ARGB(_COL) (_COL)
    #endif

    // Set up D3D state for ImGui-style rendering (no state block management).
    static void SetupImGuiD3DState(IDirect3DDevice9* device)
    {
        device->SetPixelShader(nullptr);
        device->SetVertexShader(nullptr);
        device->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
        device->SetRenderState(D3DRS_SHADEMODE, D3DSHADE_GOURAUD);
        device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        device->SetRenderState(D3DRS_ZENABLE, FALSE);
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        device->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
        device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        device->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
        device->SetRenderState(D3DRS_FOGENABLE, FALSE);
        device->SetRenderState(D3DRS_LIGHTING, FALSE);
        device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
        device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
        device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        device->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
        device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        device->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
        device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        device->SetFVF(D3DFVF_IMGUI);

        const auto& io = ImGui::GetIO();
        float L = 0.5f, R = io.DisplaySize.x + 0.5f;
        float T = 0.5f, B = io.DisplaySize.y + 0.5f;
        D3DMATRIX proj = { { {
            2.0f/(R-L), 0, 0, 0,  0, 2.0f/(T-B), 0, 0,
            0, 0, 0.5f, 0,  (L+R)/(L-R), (T+B)/(B-T), 0.5f, 1.0f,
        } } };
        D3DMATRIX identity = { { { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 } } };
        device->SetTransform(D3DTS_WORLD, &identity);
        device->SetTransform(D3DTS_VIEW, &identity);
        device->SetTransform(D3DTS_PROJECTION, &proj);

        D3DVIEWPORT9 vp = {0, 0, (DWORD)io.DisplaySize.x, (DWORD)io.DisplaySize.y, 0, 1};
        device->SetViewport(&vp);
    }

    // Render an ImDrawList directly. Assumes SetupImGuiD3DState was already called.
    static void RenderImDrawListRaw(IDirect3DDevice9* device, ImDrawList* draw_list)
    {
        if (!draw_list || draw_list->CmdBuffer.Size == 0 || draw_list->VtxBuffer.Size == 0) return;

        const int vtx_count = draw_list->VtxBuffer.Size;
        static std::vector<D3DImGuiVertex> vertices;
        vertices.resize(vtx_count);
        for (int i = 0; i < vtx_count; i++) {
            const auto& src = draw_list->VtxBuffer[i];
            auto& dst = vertices[i];
            dst.pos[0] = src.pos.x; dst.pos[1] = src.pos.y; dst.pos[2] = 0;
            dst.col = IMGUI_COL_TO_DX9_ARGB(src.col);
            dst.uv[0] = src.uv.x; dst.uv[1] = src.uv.y;
        }

        for (int cmd_i = 0; cmd_i < draw_list->CmdBuffer.Size; cmd_i++) {
            const auto& cmd = draw_list->CmdBuffer[cmd_i];
            if (cmd.ElemCount == 0) continue;
            const RECT scissor = {
                (LONG)cmd.ClipRect.x, (LONG)cmd.ClipRect.y,
                (LONG)cmd.ClipRect.z, (LONG)cmd.ClipRect.w,
            };
            device->SetScissorRect(&scissor);
            ImTextureID tex_id = cmd.TexRef._TexData ? cmd.TexRef._TexData->TexID : cmd.TexRef._TexID;
            device->SetTexture(0, tex_id != ImTextureID_Invalid
                ? static_cast<IDirect3DTexture9*>((void*)(intptr_t)tex_id) : nullptr);
            device->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, vtx_count,
                cmd.ElemCount / 3,
                draw_list->IdxBuffer.Data + cmd.IdxOffset,
                sizeof(ImDrawIdx) == 2 ? D3DFMT_INDEX16 : D3DFMT_INDEX32,
                vertices.data(), sizeof(D3DImGuiVertex));
        }
    }

    // Render a TB window and all its children directly to the backbuffer.
    // Assumes SetupImGuiD3DState was already called. Clears draw lists after rendering.
    static void RenderTBWindowDirect(IDirect3DDevice9* device, const char* name)
    {
        auto* win = ImGui::FindWindowByName(name);
        if (!win || !win->Active || win->Hidden) return;
        auto& ctx = *ImGui::GetCurrentContext();
        for (int j = 0; j < ctx.Windows.Size; j++) {
            auto* child = ctx.Windows[j];
            if (!child->Active || child->Hidden) continue;
            if (child->Flags & (ImGuiWindowFlags_Tooltip | ImGuiWindowFlags_Popup)) continue;
            auto* p = child;
            while (p && p != win) p = p->ParentWindow;
            if (p != win) continue;
            RenderImDrawListRaw(device, child->DrawList);
            child->DrawList->CmdBuffer.resize(0);
        }
    }

    void RenderImGuiWindow(IDirect3DDevice9* device, const char* window_name)
    {
        if (!device || !window_name) return;
        SetupImGuiD3DState(device);
        RenderTBWindowDirect(device, window_name);
    }

    // -----------------------------------------------------------------------
    // Overlay callbacks — modules register these, composited at the right z
    // -----------------------------------------------------------------------

    struct OverlayCallbackEntry {
        const wchar_t* gw_frame_label = nullptr;
        OverlayRenderCallback callback;
        std::function<void()> prepare_fn; // called during ImGui frame (before EndFrame)
        int priority = 0;              // lower renders first within same frame
        GW::UI::Frame* resolved_frame = nullptr; // cached per frame
        bool rendered = false;
    };

    static std::vector<OverlayCallbackEntry> s_overlay_callbacks;

    void RegisterOverlayCallback(const wchar_t* gw_frame_label, OverlayRenderCallback callback, int priority)
    {
        s_overlay_callbacks.push_back({gw_frame_label, std::move(callback), {}, priority});
        std::stable_sort(s_overlay_callbacks.begin(), s_overlay_callbacks.end(),
            [](const auto& a, const auto& b) { return a.priority < b.priority; });
    }

    void RegisterImGuiOverlay(const wchar_t* gw_frame_label, ImGuiOverlayCallback draw_fn, int priority)
    {
        // The draw_fn may call ImGui functions (e.g. SetTooltip) that require an
        // active frame scope.  We split into two phases:
        //   prepare_fn — runs during the ImGui frame (before EndFrame), builds the
        //                ImDrawList and allows ImGui calls.
        //   callback   — runs during compositing, renders the cached draw list.
        auto cached_dl = std::make_shared<ImDrawList>(nullptr);

        auto prepare = [gw_frame_label, draw_fn, cached_dl]() {
            cached_dl->_Data = ImGui::GetDrawListSharedData();
            cached_dl->_ResetForNewFrame();
            cached_dl->PushTexture(ImGui::GetIO().Fonts->TexRef);
            if (gw_frame_label) {
                auto* root = GW::UI::GetRootFrame();
                auto* frame = GW::UI::GetFrameByLabel(gw_frame_label);
                if (frame && root) {
                    auto tl = frame->position.GetContentTopLeft(root);
                    auto br = frame->position.GetContentBottomRight(root);
                    cached_dl->PushClipRect({tl.x, tl.y}, {br.x, br.y});
                } else {
                    cached_dl->PushClipRectFullScreen();
                }
            } else {
                cached_dl->PushClipRectFullScreen();
            }
            draw_fn(*cached_dl);
        };

        auto render = [cached_dl](IDirect3DDevice9* device) {
            if (cached_dl->CmdBuffer.Size == 0 || cached_dl->VtxBuffer.Size == 0) return;
            SetupImGuiD3DState(device);
            RenderImDrawListRaw(device, cached_dl.get());
        };

        OverlayCallbackEntry entry;
        entry.gw_frame_label = gw_frame_label;
        entry.callback = std::move(render);
        entry.prepare_fn = std::move(prepare);
        entry.priority = priority;
        s_overlay_callbacks.push_back(std::move(entry));
        std::stable_sort(s_overlay_callbacks.begin(), s_overlay_callbacks.end(),
            [](const auto& a, const auto& b) { return a.priority < b.priority; });
    }

    void PrepareOverlays()
    {
        for (auto& cb : s_overlay_callbacks) {
            if (cb.prepare_fn) cb.prepare_fn();
        }
    }


    static void ResetOverlayCallbacks()
    {
        for (auto& cb : s_overlay_callbacks) {
            cb.resolved_frame = cb.gw_frame_label
                ? GW::UI::GetFrameByLabel(cb.gw_frame_label) : nullptr;
            cb.rendered = false;
        }
    }

    bool IsPointOccludedByGW(float x, float y, uint64_t tb_z)
    {
        if (tb_z == 0) return false;
        // TB widgets render on top of all GW content when the world map is showing.
        if (GW::UI::GetIsWorldMapShowing()) return false;
        auto& zo = GetUnifiedZOrder();
        for (const auto& w : zo.GetGWWindows()) {
            if (w.unified_z > tb_z && w.bounds.ContainsPoint(x, y))
                return true;
        }
        return false;
    }

    uint64_t GetHoveredTBWindowZ()
    {
        auto& zo = GetUnifiedZOrder();
        // Check hovered window
        auto* hovered = ImGui::GetCurrentContext()->HoveredWindow;
        if (hovered) {
            while (hovered->ParentWindow) hovered = hovered->ParentWindow;
            uint64_t z = zo.GetZ(hovered->Name);
            if (z > 0) return z;
        }
        // Check active window (for in-progress drags)
        auto* active = ImGui::GetCurrentContext()->ActiveIdWindow;
        if (active) {
            while (active->ParentWindow) active = active->ParentWindow;
            uint64_t z = zo.GetZ(active->Name);
            if (z > 0) return z;
        }
        return 0;
    }

    // -----------------------------------------------------------------------
    // FrCacheRenderAll hook — re-implements buffer processing with TB injection
    // -----------------------------------------------------------------------

    // Flush GW's GPU command queue so enqueued commands become D3D calls.
    // Only safe when device->m_queueFlushing == 3 (GR_QUEUES).
    static void FlushGWCommandQueue()
    {
        if (!GrDev_FlushQueues_Fn || !s_GrDev_Default) return;
        auto* device = *s_GrDev_Default;
        if (!device) return;
        // Check m_queueFlushing at device+0x1CC — must be 3 to flush safely
        int queue_state = *reinterpret_cast<int*>(reinterpret_cast<uintptr_t>(device) + 0x1CC);
        if (queue_state == 3) {
            GrDev_FlushQueues_Fn(device);
        }
    }

    // Manages a single D3D state block for GW<->TB transitions during compositing.
    // EnterTBState saves GW state (once) and sets up ImGui rendering state.
    // EnterGWState restores the saved GW state.
    struct StateTransition {
        IDirect3DDevice9* device = nullptr;
        IDirect3DStateBlock9* sb = nullptr;
        bool in_tb_state = false;

        void Init(IDirect3DDevice9* dev) { device = dev; }

        void EnterTBState() {
            if (in_tb_state) return;
            FlushGWCommandQueue();
            if (!sb)
                device->CreateStateBlock(D3DSBT_ALL, &sb);
            if (sb) sb->Capture(); // re-capture current GW state each time
            SetupImGuiD3DState(device);
            in_tb_state = true;
        }

        void EnterGWState() {
            if (!in_tb_state) return;
            if (sb) sb->Apply();
            in_tb_state = false;
        }

        void Release() {
            if (sb) { if (in_tb_state) sb->Apply(); sb->Release(); sb = nullptr; }
            in_tb_state = false;
        }
    };

    // Run all matching overlay callbacks. Caller manages state transitions.
    static void RunOverlayCallbacks(StateTransition& st,
        std::function<bool(OverlayCallbackEntry&)> predicate)
    {
        for (auto& cb : s_overlay_callbacks) {
            if (cb.rendered) continue;
            if (!predicate(cb)) continue;
            st.EnterTBState();
            cb.callback(st.device);
            cb.rendered = true;
        }
    }

    void __cdecl FrCacheRenderAll_Hook(uint32_t param_1, uint32_t param_2)
    {
        GW::Hook::EnterHook();

        // Run the TB draw callback so ImGui draw lists are ready for
        // compositing. BuildRenderList has already finished at this point
        // (called by the original FrCache_Render before RenderAll).
        if (s_frame_draw_callback && s_device) {
            s_frame_draw_callback(s_device);
        }

        auto& zo = GetUnifiedZOrder();
        const auto& composite = zo.GetCompositeOrder();

        // Skip z-interleaving when the world map is showing. The world map
        // renders via the ZSort layer which sits above all overlays in the
        // buffer. TB widgets that ShowOnWorldMap() render after all GW content.
        if (GW::UI::GetIsWorldMapShowing() || composite.empty() || !s_device || !FrCacheRenderEntry_Fn) {
            FrCacheRenderAll_Trampoline(param_1, param_2);
            // Still run base-UI overlay callbacks (e.g. world map annotations)
            if (s_device) {
                StateTransition st;
                st.Init(s_device);
                RunOverlayCallbacks(st, [](OverlayCallbackEntry& cb) {
                    return !cb.gw_frame_label;
                });
                st.Release();
            }
            ResetOverlayCallbacks();
            GW::Hook::LeaveHook();
            return;
        }

        // Process the render buffer entry by entry using proper GW::BaseArray types
        auto& buffer = RenderBuffer();
        auto& frames = FrameArray();
        auto& secondary = SecondaryArray();

        float viewport[4] = {0, 0, 1, 1};
        GW::UI::Frame* current_floating = nullptr;
        static std::vector<bool> tb_drawn;
        tb_drawn.assign(composite.size(), false);
        bool base_overlays_rendered = false;
        StateTransition st;
        st.Init(s_device);

        for (uint32_t i = 0; i < buffer.size(); i++) {
            const auto& entry = buffer[i];

            // Detect popup/overlay boundaries via the POPUP flag (0x20) on
            // type 1/2/3 buffer entries.  This flag is stable (doesn't change
            // between buffer build and render), unlike z-values which the game
            // may update after the buffer was built.
            if (entry.type != FRCACHE_GPU_RENDER && entry.index < frames.size()) {
                auto* frame = frames[entry.index];
                if (frame && (frame->field92_0x190 & 0x20) != 0
                    && frame != current_floating) {
                    // Find this popup frame in the composite order
                    GW::UI::Frame* popup_frame = nullptr;
                    uint64_t popup_z = UINT64_MAX;
                    for (const auto& ce : composite) {
                        if (!ce.is_tb && ce.gw_frame == frame) {
                            popup_frame = frame;
                            popup_z = ce.z;
                            break;
                        }
                    }

                if (popup_frame) {

                    // Before the first popup: render base UI overlays AND any
                    // frame-specific overlays for non-popup frames.
                    if (!base_overlays_rendered && s_device) {
                        base_overlays_rendered = true;
                        RunOverlayCallbacks(st, [](OverlayCallbackEntry& cb) {
                            if (!cb.gw_frame_label) return true; // base UI
                            bool is_popup = cb.resolved_frame &&
                                (cb.resolved_frame->field92_0x190 & 0x20);
                            return !is_popup; // non-popup overlays render with base UI
                        });
                    }

                    // Render callbacks for the PREVIOUS popup (its content just finished)
                    if (current_floating && s_device) {
                        RunOverlayCallbacks(st, [current_floating](OverlayCallbackEntry& cb) {
                            return cb.resolved_frame == current_floating;
                        });
                    }

                    // Render TB windows with z < popup_z directly to the backbuffer
                    if (s_device) {
                        for (size_t ci = 0; ci < composite.size(); ci++) {
                            if (tb_drawn[ci] || !composite[ci].is_tb || composite[ci].z >= popup_z)
                                continue;
                            st.EnterTBState();
                            RenderTBWindowDirect(s_device, composite[ci].tb_name);
                            tb_drawn[ci] = true;
                        }
                    }

                    // Restore GW state before processing more buffer entries
                    st.EnterGWState();

                    current_floating = popup_frame;
                }
                }
            }

            // Process the entry using GW's internal functions
            switch (entry.type) {
            case FRCACHE_GPU_RENDER: {
                auto* models = reinterpret_cast<int*>(&secondary[entry.index]);
                FrCacheRenderEntry_Fn(nullptr, entry.param, models, 1);
                break;
            }
            case FRCACHE_FRAME_CALLBACK: {
                if (entry.index < frames.size()) {
                    auto* frame = frames[entry.index];
                    if (frame) {
                        auto* state_ptr = reinterpret_cast<void*>(
                            reinterpret_cast<uintptr_t>(frame) + 0x18C);
                        if (FrameStateTestFlag_Fn(state_ptr, 0x1000) == 0) {
                            uint32_t params[6];
                            params[0] = param_2;
                            memcpy(&params[1], viewport, sizeof(viewport));
                            params[5] = param_1;
                            auto* callbacks = reinterpret_cast<void*>(
                                reinterpret_cast<uintptr_t>(frame) + 0xA8);
                            FrMsgDispatch_Fn(callbacks, nullptr, 0x35, params, 0);
                        }
                    }
                }
                break;
            }
            case FRCACHE_CLIENT_VIEWPORT: {
                if (entry.index < frames.size()) {
                    auto* frame = frames[entry.index];
                    if (frame) {
                        float rect[4], transformed[4];
                        auto* pos = reinterpret_cast<void*>(
                            reinterpret_cast<uintptr_t>(frame) + 0xD0);
                        FramePosGetClientRect_Fn(pos, rect);
                        ViewportTransformRect_Fn(transformed, rect);
                        // Validate: skip if right < left or bottom <= top (matches original)
                        if (transformed[2] >= transformed[0] && transformed[3] > transformed[1]) {
                            memcpy(viewport, transformed, sizeof(viewport));
                            FrCacheSetViewport_Fn(nullptr, viewport);
                        }
                    }
                }
                break;
            }
            case FRCACHE_FRAME_VIEWPORT: {
                if (entry.index < frames.size()) {
                    auto* frame = frames[entry.index];
                    if (frame) {
                        float rect[4], transformed[4];
                        auto* pos = reinterpret_cast<void*>(
                            reinterpret_cast<uintptr_t>(frame) + 0xD0);
                        FramePosGetFrameRect_Fn(pos, rect);
                        ViewportTransformRect_Fn(transformed, rect);
                        // Validate: skip if left > right or top >= bottom (matches original)
                        if (transformed[0] <= transformed[2] && transformed[1] < transformed[3]) {
                            memcpy(viewport, transformed, sizeof(viewport));
                            FrCacheSetViewport_Fn(nullptr, viewport);
                        }
                    }
                }
                break;
            }
            }
        }

        // Render any callbacks/overlays that weren't rendered during popup processing.
        // Only render if the target GW frame is visible — at character select or when
        // the target window is closed, skip rather than rendering orphaned overlays.
        if (s_device) {
            RunOverlayCallbacks(st, [](OverlayCallbackEntry& cb) {
                // Skip if the target frame doesn't exist or isn't visible
                if (cb.gw_frame_label && (!cb.resolved_frame || !cb.resolved_frame->IsVisible()))
                    return false;
                return true;
            });
        }

        // Render any TB windows not yet drawn
        if (s_device) {
            for (size_t ci = 0; ci < composite.size(); ci++) {
                if (tb_drawn[ci] || !composite[ci].is_tb) continue;
                st.EnterTBState();
                RenderTBWindowDirect(s_device, composite[ci].tb_name);
                tb_drawn[ci] = true;
            }
        }

        // Restore GW state and release the state block
        st.Release();

        // Reset viewport to full screen (matches original FrCache_RenderAll epilogue)
        float full_viewport[4] = {0, 0, 1, 1};
        FrCacheSetViewport_Fn(nullptr, full_viewport);

        ResetOverlayCallbacks();
        GW::Hook::LeaveHook();
    }

    // -----------------------------------------------------------------------
    // Device management
    // -----------------------------------------------------------------------

    void SetDevice(IDirect3DDevice9* device)
    {
        if (s_device != device) {
            ReleaseDeviceResources();
        }
        s_device = device;
    }

    void ReleaseDeviceResources()
    {
        if (FrCacheRenderAll_Original) {
            GW::Hook::DisableHooks(FrCacheRenderAll_Original);
            GW::Hook::RemoveHook(FrCacheRenderAll_Original);
            FrCacheRenderAll_Original = nullptr;
            FrCacheRenderAll_Trampoline = nullptr;
        }
        s_frame_draw_callback = nullptr;
        s_hook_failed = false;
        s_RenderBuffer = nullptr;
        s_FrameArray = nullptr;
        s_SecondaryArray = nullptr;
        s_ZSortList = nullptr;
        s_OverlayList = nullptr;
        s_GrDev_Default = nullptr;
    }

    // -----------------------------------------------------------------------
    // Hook installation
    // -----------------------------------------------------------------------

    bool IsHooked()
    {
        return FrCacheRenderAll_Original != nullptr;
    }

    bool HookFrCacheRender()
    {
        if (FrCacheRenderAll_Original) return true;
        if (s_hook_failed) return false;

        // ---- Scan for anchor functions via assertions and string references ----

        // FrCache_RenderAll: asserts "frame" in FrCache.cpp at line 0x9e (case 2).
        // Assertion is ~0x120 bytes into the function, so extend scan range.
        auto render_all_addr = GW::Scanner::ToFunctionStart(
            GW::Scanner::FindAssertion("FrCache.cpp", "frame", 0x9e, 0), 0x200);
        if (!render_all_addr) {
            Log::Warning("Compositor: failed to find FrCache_RenderAll, z-interleaving disabled");
            s_hook_failed = true;
            return false;
        }

        // FrCache_Render: only caller of FrCache_RenderAll — scan backward for CALL to it
        uintptr_t render_addr = 0;
        for (auto a = render_all_addr - 5; a > render_all_addr - 0x100; a--) {
            if (*reinterpret_cast<uint8_t*>(a) == 0xE8) {
                if (GW::Scanner::FunctionFromNearCall(a) == render_all_addr) {
                    render_addr = GW::Scanner::ToFunctionStart(a);
                    break;
                }
            }
        }
        if (!render_addr) {
            Log::Warning("Compositor: failed to find FrCache_Render, z-interleaving disabled");
            s_hook_failed = true;
            return false;
        }

        // FrCache_SetViewport: asserts "viewport.x0 >= 0" in GrDev.cpp
        FrCacheSetViewport_Fn = reinterpret_cast<FrCacheSetViewportFn>(
            GW::Scanner::ToFunctionStart(
                GW::Scanner::FindAssertion("GrDev.cpp", "viewport.x0 >= 0", 0, 0)));

        // GrDev_FlushQueues: asserts "m_queueFlushing == GR_QUEUES" in GrDev.cpp
        GrDev_FlushQueues_Fn = reinterpret_cast<GrDevFlushQueuesFn>(
            GW::Scanner::ToFunctionStart(
                GW::Scanner::FindAssertion("GrDev.cpp", "m_queueFlushing == GR_QUEUES", 0, 0)));

        // FrMsg_Dispatch: 2nd function asserting "msgId != FRAME_MSG_CREATE" in FrMsg.cpp
        FrMsgDispatch_Fn = reinterpret_cast<FrMsgDispatchFn>(
            GW::Scanner::ToFunctionStart(
                GW::Scanner::FindNthUseOfString("msgId != FRAME_MSG_CREATE", 1)));

        // FrCache_RenderEntry: asserts "!array[GR_SORT_DEFAULT].Count()" in GrDev.cpp.
        // Assertion is ~0x65E bytes into the function, so extend scan range.
        FrCacheRenderEntry_Fn = reinterpret_cast<FrCacheRenderEntryFn>(
            GW::Scanner::ToFunctionStart(
                GW::Scanner::FindAssertion("GrDev.cpp", "!array[GR_SORT_DEFAULT].Count()", 0, 0), 0x800));

        // ---- Extract utility functions from FrCache_RenderAll body ----

        auto render_all_end = render_all_addr + 0x300; // function is ~0x280 bytes

        // FrameState_TestFlag: called right after PUSH 0x1000 in FrCache_RenderAll
        auto push_1000 = GW::Scanner::FindInRange(
            "\x68\x00\x10\x00\x00", "xxxxx", 0, render_all_addr, render_all_end);
        if (push_1000) {
            // Scan forward for the next E8 (CALL) within a few bytes
            for (auto a = push_1000 + 5; a < push_1000 + 20; a++) {
                if (*reinterpret_cast<uint8_t*>(a) == 0xE8) {
                    FrameStateTestFlag_Fn = reinterpret_cast<FrameStateTestFlagFn>(
                        GW::Scanner::FunctionFromNearCall(a));
                    break;
                }
            }
        }

        // FramePos_GetClientRect: the function containing TEST [ECX+0xC0],0x420 is
        // GetFrameRect; GetClientRect is the other FramePos_ call (LEA EAX,[ECX+0x2C]).
        // Find GetFrameRect via its unique TEST pattern:
        FramePosGetFrameRect_Fn = reinterpret_cast<FramePosGetRectFn>(
            GW::Scanner::ToFunctionStart(
                GW::Scanner::Find("\xF7\x81\xC0\x00\x00\x00\x20\x04\x00\x00", "xxxxxxxxxx")));

        // FramePos_GetClientRect: LEA EAX,[ECX+0x2C]; PUSH EAX; PUSH [EBP+8]; CALL
        FramePosGetClientRect_Fn = reinterpret_cast<FramePosGetRectFn>(
            GW::Scanner::ToFunctionStart(
                GW::Scanner::Find("\x8D\x41\x2C\x50\xFF\x75\x08\xE8", "xxxxxxxx")));

        // Viewport_TransformRect: called right after FramePos_GetClientRect in RenderAll.
        // Find the CALL to GetClientRect in RenderAll, then the next CALL is TransformRect.
        if (FramePosGetClientRect_Fn) {
            auto get_client_addr = reinterpret_cast<uintptr_t>(FramePosGetClientRect_Fn);
            for (auto a = render_all_addr; a < render_all_end - 5; a++) {
                if (*reinterpret_cast<uint8_t*>(a) == 0xE8 &&
                    GW::Scanner::FunctionFromNearCall(a) == get_client_addr) {
                    // Found call to GetClientRect — next CALL is TransformRect
                    for (auto b = a + 5; b < a + 30; b++) {
                        if (*reinterpret_cast<uint8_t*>(b) == 0xE8) {
                            ViewportTransformRect_Fn = reinterpret_cast<ViewportTransformRectFn>(
                                GW::Scanner::FunctionFromNearCall(b));
                            break;
                        }
                    }
                    break;
                }
            }
        }

        // ---- Extract globals from FrCache_Render ----

        // FrCache_Render starts with: CMP dword ptr [RenderFrameList.m_size], 0
        // Bytes at offset +3: 83 3D [addr32] 00
        // The 4-byte address at render_addr+5 is RenderFrameList.m_size.
        // The 4 Array<T> structs are consecutive (16 bytes each):
        //   RenderFrameList, FrameArray, SecondaryArray, RenderBuffer
        auto frame_list_size_addr = *reinterpret_cast<uintptr_t*>(render_addr + 5);
        auto frame_list_base = frame_list_size_addr - 8; // m_size is at +8 in Array<T>
        s_FrameArray = reinterpret_cast<GW::Array<GW::UI::Frame*>*>(frame_list_base + 0x10);
        s_SecondaryArray = reinterpret_cast<GW::Array<int*>*>(frame_list_base + 0x20);
        s_RenderBuffer = reinterpret_cast<GW::Array<FrCacheBufferEntry>*>(frame_list_base + 0x30);
        s_ZSortList = reinterpret_cast<GW::Array<GW::UI::Frame*>*>(frame_list_base + 0x40);
        s_OverlayList = reinterpret_cast<GW::Array<GW::UI::Frame*>*>(frame_list_base + 0x50);

        // GrDev_Default: loaded via MOV ESI,[addr] in FrCache_SetViewport
        // (at ~offset 0x6A, in the null render_target path)
        if (FrCacheSetViewport_Fn) {
            auto svp = reinterpret_cast<uintptr_t>(FrCacheSetViewport_Fn);
            for (auto a = svp; a < svp + 0x80; a++) {
                auto b = *reinterpret_cast<uint8_t*>(a);
                if (b == 0xA1) { // MOV EAX, [addr32]
                    auto addr = *reinterpret_cast<uintptr_t*>(a + 1);
                    if (GW::Scanner::IsValidPtr(addr)) {
                        s_GrDev_Default = reinterpret_cast<void**>(addr);
                        break;
                    }
                }
                if (b == 0x8B && (*reinterpret_cast<uint8_t*>(a + 1) & 0xC7) == 0x05) {
                    // MOV reg, [addr32] (8B /r with mod=00, r/m=101)
                    auto addr = *reinterpret_cast<uintptr_t*>(a + 2);
                    if (GW::Scanner::IsValidPtr(addr)) {
                        s_GrDev_Default = reinterpret_cast<void**>(addr);
                        break;
                    }
                }
            }
        }

        // ---- Verify all pointers resolved ----
#ifdef _DEBUG
        ASSERT(FrCacheSetViewport_Fn);
        ASSERT(FrCacheRenderEntry_Fn);
        ASSERT(FramePosGetClientRect_Fn);
        ASSERT(FramePosGetFrameRect_Fn);
        ASSERT(ViewportTransformRect_Fn);
        ASSERT(FrMsgDispatch_Fn);
        ASSERT(FrameStateTestFlag_Fn);
        ASSERT(GrDev_FlushQueues_Fn);
        ASSERT(s_RenderBuffer);
        ASSERT(s_FrameArray);
        ASSERT(s_SecondaryArray);
        ASSERT(s_ZSortList);
        ASSERT(s_GrDev_Default);
#endif
        if (!FrCacheSetViewport_Fn || !FrCacheRenderEntry_Fn ||
            !FramePosGetClientRect_Fn || !FramePosGetFrameRect_Fn ||
            !ViewportTransformRect_Fn || !FrMsgDispatch_Fn ||
            !FrameStateTestFlag_Fn || !GrDev_FlushQueues_Fn ||
            !s_RenderBuffer || !s_FrameArray || !s_SecondaryArray ||
            !s_ZSortList || !s_GrDev_Default) {
            Log::Warning("Compositor: one or more function pointers failed to resolve, z-interleaving disabled");
            s_hook_failed = true;
            return false;
        }

        // ---- Install hooks ----

        FrCacheRenderAll_Original = reinterpret_cast<FrCacheRenderFn>(render_all_addr);
        GW::Hook::CreateHook(reinterpret_cast<void**>(&FrCacheRenderAll_Original),
                              FrCacheRenderAll_Hook,
                              reinterpret_cast<void**>(&FrCacheRenderAll_Trampoline));
        GW::Hook::EnableHooks(FrCacheRenderAll_Original);

        return true;
    }

    // -----------------------------------------------------------------------
    // Frame draw callback management
    // -----------------------------------------------------------------------

    void SetFrameDrawCallback(FrameDrawCallback callback)
    {
        s_frame_draw_callback = callback;
    }

}
