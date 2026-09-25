#pragma once

#include <functional>
#include <d3d9.h>

namespace GameWorldCompositor {
    using DrawCallback = std::function<void(IDirect3DDevice9* device)>;

    inline constexpr float kZNear = 46.875f;
    inline constexpr float kZFar = 48000.f;

    int RegisterDraw(DrawCallback callback);
    void UnregisterDraw(int token);

    // Renders a contiguous [start, start+count) slice of GW's frame render buffer through the
    // original render pass. Handed to the HUD compositor so it can drive GW's HUD rendering in
    // segments (interleaving its own draws between GW's HUD frames) without owning the hook.
    using RenderRange = std::function<void(uint32_t start, uint32_t count)>;

    // Optional single delegate that renders the HUD portion of the frame buffer. When set, the
    // hook hands it the HUD start index, the full buffer size and a range renderer instead of
    // drawing the HUD itself, so the Toolbox z-order compositor can interleave TB windows between
    // GW's HUD frames. hud_start == buffer_size means "GW already drew everything; just draw on
    // top" (the second-dispatch / no-split fallback). Setting a compositor installs the hook the
    // same way RegisterDraw does. Pass nullptr to clear. Only one may be set at a time.
    using HudCompositor = std::function<void(uint32_t hud_start, uint32_t buffer_size, const RenderRange& render_range)>;
    void SetHudCompositor(HudCompositor callback);

    // True once the hook is installed and operational; false if scanning/installing it failed.
    // A module that can also draw on top of the UI should fall back to that when this is false.
    [[nodiscard]] bool IsActive();
    [[nodiscard]] bool HasFailed();

    // Reset the once-per-frame draw guard. Must be called exactly once per rendered frame.
    void BeginFrame();

    bool SetupPipeline(IDirect3DDevice9* device, bool occlude, float max_distance, float fog_factor);

    bool SetWorldViewProj(IDirect3DDevice9* device);
    void SetWorldRenderStates(IDirect3DDevice9* device, bool occlude);
    void SetDistanceFog(IDirect3DDevice9* device, float max_distance, float fog_factor);

    // The shared pipeline objects, valid after a successful SetupPipeline - e.g. to restore the
    // programmable pipeline after a fixed-function pass (such as a stencil punch-out).
    [[nodiscard]] IDirect3DVertexShader9* VertexShader();
    [[nodiscard]] IDirect3DPixelShader9* PixelShader();
    [[nodiscard]] IDirect3DVertexDeclaration9* VertexDeclaration();

#ifdef _DEBUG
    // Log every FrCacheRenderAll invocation's buffer layout for the next few calls (harness diagnostics).
    void RequestBufferDump();
#endif

    // Remove the hook and release shared GPU resources (shutdown).
    void Terminate();
} // namespace GameWorldCompositor
