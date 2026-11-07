# VRPacing (`v0.8.1-beta`)

**System-wide PCVR utility for tight Frame Pacing (`-0.10 FPS` cap), 2.0x Integer Spatial Reconstruction (NVIDIA Image Scaling, NIS), and Fixed Foveated Rendering (D3D11 / D3D12).**

`VRPacing` is a lightweight background tray application for Windows (`VRPacingTray.exe`) designed to address two persistent problems in PCVR rendering and wireless streaming: frame delivery timing instability (*judder* caused by driver queue saturation) and temporal aliasing (*shimmering* and video encoder degradation) introduced by traditional fractional upscalers.

No per-game `.dll` copying and no per-game configuration files are required. `VRPacing` works system-wide, detecting and intercepting active sessions across **OpenVR (SteamVR)**, **Oculus (LibOVR)**, and **OpenXR (Native & VDXR)** under DirectX 11, DirectX 12, and Vulkan (see the compatibility matrix for what is supported on each combination).

> **Beta software.** Results below come from my own testing on a Meta Quest + Virtual Desktop setup. Your mileage may vary. **Do not use it in online games protected by anti-cheat** (Easy Anti-Cheat, BattlEye, etc.): injecting modules into such games can cause crashes or account bans. Use at your own risk.

## Credits & Acknowledgements

VRPacing is a spiritual successor to two excellent projects, and its core design follows the approach pioneered by them. Many thanks to their authors:

* **[vrperfkit](https://github.com/fholger/vrperfkit)** by Holger Frydrych (fholger): the original open-source VR performance toolkit (injection-based upscaling, foveated rendering and hidden-area mask). VRPacing's core is much closer to vrperfkit than to any other project.
* **[OpenXR-Toolkit](https://github.com/mbucchia/OpenXR-Toolkit)** by Matthieu Bucchianeri (mbucchia): the OpenXR API-layer approach to upscaling, sharpening and foveated rendering.

Third-party components bundled with VRPacing (NVIDIA Image Scaling, NVAPI, MinHook, nlohmann/json, OpenVR and OpenXR headers) keep their own licenses; see [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md). VRPacing itself is released under the MIT License (see `LICENSE`).

## Requirements

* Windows 10/11 x64.
* An OpenVR, Oculus or OpenXR PCVR game using DirectX 11 or DirectX 12 (Vulkan: see the matrix).
* **Fixed Foveated Rendering on D3D11 uses NVAPI and therefore requires an NVIDIA GPU. On D3D12 it requires a GPU with Variable Rate Shading Tier 2 (e.g., NVIDIA Turing or newer, AMD RDNA2 or newer).** Frame pacing, the circular mask and NIS (a compute shader) do not depend on a specific GPU vendor.

---

## PART 1: Quick Start Guide (Plug & Play)

### Installation / Uninstallation

1. Download `VRPacing-windows-x64.zip` from the **Releases** page and extract it anywhere.
2. Run `VRPacingTray.exe`.

### Zero-Configuration Setup
All core optimizations in `VRPacing` (**`-0.10 FPS` Frame Pacing**, **NIS `2.0x` Spatial Reconstruction**, **Fixed Foveated VRS**, and **Pre-Render Circular Mask**) are **enabled by default**. You do not need to configure settings for each individual game:

1. Run **`VRPacingTray.exe`** and leave it open in the background (or minimized to the system tray).
2. In **Virtual Desktop** (or your headset streaming software), select a base resolution preset and refresh rate (Hz) that your GPU can sustain consistently.
3. Launch your VR game. `VRPacing` automatically hooks the active runtime, applies `2.0x` spatial reconstruction, and caps frame delivery.

---

### Step 1: Choosing Your Base Resolution (and Note for Enthusiast GPUs)

Conventional upscaling guides recommend selecting an overly demanding headset resolution preset (such as *Ultra* or *Godlike*) and downscaling the game with a fractional render scale (`75%` or `77%`) combined with artificial sharpening.

`VRPacing` reverses this workflow: the game engine renders at its `1.0x` base resolution, and **NVIDIA Image Scaling (NIS)** is applied at an exact **`2.0x` integer scale (200% per axis)** with **`0%` additional sharpening** over the **central `60%` radius** (the area where your eyes focus most of the time).

* **For Mid-Range and High-End GPUs:**
  Select a base resolution preset one tier lower than usual (for example, **Medium** or **High** instead of *Ultra/Godlike*, while keeping SteamVR resolution at `100%`). In my testing, integer `2.0x` NIS reconstruction gives a clean, temporally stable image with less pixel crawl, and frees up the GPU headroom needed to keep a flat frametime line.
* **For Enthusiast Hardware (RTX 4090 / RTX 5090):**
  The recommendation to step down one preset is meant only to protect frametimes on GPUs with limited headroom. If your GPU can sustain the target refresh rate at maximum presets (**Ultra** or **Godlike**), you can use those presets with **NIS 2.0x enabled** for the best possible image quality.

#### Reference Resolution Table (Virtual Desktop — Meta Quest)
*(Note: the table lists Virtual Desktop per-eye presets for Meta Quest headsets. The same methodology applies to Pico 4 and other headsets by keeping SteamVR at `100%` and selecting a sustainable base preset. The reference GPUs are approximate.)*

| Virtual Desktop Preset | Per-Eye Render (Width × Height) | Combined Base Render | Reference GPU (Base) | Recommended Usage with `VRPacing` (NIS 2.0x Active) |
| :--- | :--- | :--- | :--- | :--- |
| **Potato** | `1344 × 1440` | `2688 × 1440` | GTX 970 / RX 580 | Entry-level hardware |
| **Low** | `1728 × 1824` | `3456 × 1824` | GTX 1070 / RX 5500 XT | Entry-level and budget laptop GPUs |
| **Medium** | `2112 × 2304` | `4224 × 2304` | RTX 2070 / RX 5700 XT | Ideal balance when prioritizing rock-solid framerate stability |
| **High** | `2496 × 2688` | `4992 × 2688` | RTX 3070 / RX 6800 XT | **Recommended sweet spot** over *Godlike* + fractional downscaling |
| **Ultra** | `2688 × 2880` | `5376 × 2880` | RTX 3090 / RX 6950 XT | High-end systems with ample performance headroom |
| **Godlike** | `3072 × 3264` | `6144 × 3264` | RTX 4090 / RTX 5090 / RX 7900 XTX | Maximum fidelity for enthusiast GPUs able to sustain target Hz |

---

### Step 2: Choosing Headset Refresh Rate (Native Hz vs. SSW Always On)

In virtual reality, **a flat frametime line matters more than a high but fluctuating average framerate**.

You can verify stability in the `VRPacing` window: when the diagnostic line reports **`Advisor: Target Sustained`** and the cyan frametime graph rests flat along the green target budget line, frame pacing is locked.

* **Practical Example (*Half-Life: Alyx* averaging 87 FPS):**
  If your headset is set to `90 Hz` or `120 Hz` and your PC averages **`87 FPS`**, the experience will stutter because the GPU misses the headset's vsync deadline. In this scenario the better options are:
  1. Lowering the headset to **`80 Hz` Native**, capping delivery at a flat **`79.90 FPS`** with GPU headroom to spare.
  2. Or setting the headset to **`120 Hz` with SSW Always On** (in Virtual Desktop), capping the game at **`59.90 FPS` (`60 / 120 Hz`)** so motion reprojection works with a consistent cadence.

#### Quick Guidelines by Game Genre
* **1. Standing & Fast-Paced Roomscale Games (Shooters / Action / Adventure):**
  * **Native Hz:** Choose the highest native refresh rate (`72`, `80`, `90`, or `120 Hz`) where your system maintains `Target Sustained`.
  * **With SSW Always On:** Use a minimum of **`60 / 120 Hz` (`59.90 FPS`)**, where 60 real frames interpolated to 120 Hz keep hand tracking and locomotion responsive.
* **2. Fast-Paced Seated Cockpit Simulators (*Assetto Corsa*, *Star Wars: Squadrons*, Racing / Space Combat):**
  * Because you are seated in a fixed cockpit reference frame, these titles work well with **SSW Always On** starting at **`45 / 90 Hz` (`44.90 FPS`)**, **`50 / 100 Hz` (`49.90 FPS`)**, or **`60 / 120 Hz` (`59.90 FPS`)**.
* **3. Heavy Slow-Paced Simulators (*Microsoft Flight Simulator 2020 / 2024* in cockpit view):**
  * With slow cockpit flight and smooth head movements, **SSW Always On** is usable even at **`36 / 72 Hz` (`35.90 FPS`)** or **`40 / 80 Hz` (`39.90 FPS`)** provided the frametime line is flat. Move up to **`45 / 90 Hz` (`44.90 FPS`)** if your hardware allows.

---

### Optional Pre-Setup Tip: FOV Tangent Reduction
If you use **Virtual Desktop**, one of the most effective ways to gain free performance before touching resolution is to slightly reduce the rendered Field of View (**FOV Tangent** horizontal and vertical sliders, e.g., to **`85%`** or **`80%`**, up to the point where the black border is barely noticeable inside the headset).
* This discards a share of the peripheral pixels that are distorted by the outer lens edges anyway, saving GPU rasterization load with no loss in central pixel density (PPD).
* In games that support `VRPacing`'s **Pre-Render Circular Visibility Mask**, the radial mask smoothly rounds off the corners of that FOV crop.

---

## PART 2: Architecture & Technical Deep-Dive (Optional Reading)

```text
VRPACING SYSTEM-LEVEL PIPELINE ARCHITECTURE

 [ Game Engine (1.0x Base Render) ]
        │
        ├──> 1. Pre-Render Pass: Circular Depth Visibility Mask (hidden-area mesh)
        ├──> 2. Shading Pass: Fixed Foveated VRS (D3D11 NVAPI / D3D12 Tier-2)
        ▼
 [ Completed Native Framebuffer (1.0x) ] ── (no extra VRAM cost on G-Buffer / Shadows / MSAA)
        │
        ├──> 3. Presentation Pass: NIS 2.0x Integer Spatial Reconstruction (60% radius / 0% sharpness)
        ├──> 4. Delivery Sync: High-Resolution Waitable Timer at -0.10 FPS below the display rate
        ▼
 [ Headset Compositor / Hardware Video Encoder (VDXR / SteamVR / LibOVR) ]
```

### 1. Integer Spatial Reconstruction (`NIS 2.0x` + `0% Sharpening`) vs. Fractional Scaling (`0.77x`)

Common configurations in similar tools rely on setting an oversized target resolution and applying fractional scaling ratios (`0.75x` or `0.77x`) paired with sharpening filters.

* **The problem with fractional scaling:** At non-integer ratios, output pixels land at different subpixel phase offsets relative to the source texel grid. During continuous head micro-movements in VR, high-frequency geometry (foliage, power lines, fences, distant edges) shifts phase from frame to frame, which can produce visible temporal instability (*pixel crawl* or *shimmering*). To compensate for fractional resampling blur, users often increase sharpening, which introduces edge ringing.
* **Wireless video encoder impact (HEVC / AV1):** On streaming headsets (*Virtual Desktop*, *AirLink*, *Steam Link*), strong sharpening halos consume encoder bitrate and can contribute to compression noise and macroblocking.
* **The `VRPacing` approach (`2.0x` integer scale with `0%` sharpening over the central `60%`):**
  * **Deterministic 2×2 mapping:** At exactly `200%` per axis, every source pixel maps to a 2×2 block of destination pixels, so phase alignment is identical across the frame.
  * **Why NIS:** NVIDIA Image Scaling is a directional, edge-adaptive scaler with its own sharpening stage and a vendor-neutral compute-shader implementation. At symmetric `2.0x` integer conditions it reconstructs diagonal edges and contours more cleanly than a plain bilinear/Lanczos upscale, in my testing. Comparison screenshots against vrperfkit and OpenXR Toolkit will be added.
  * **Why `0% sharpening`:** Because the `1.0x` frame was never downsampled, there is no resampling blur to compensate for. Setting `sharpness = 0.0` avoids artificial grain and ringing and feeds a cleaner signal to the video encoder. NIS runs only inside the **central `60%` radius**.

> **Honest note on what NIS does:** upscaling a `1.0x` render cannot add detail that was not rendered. The benefit is better reconstruction and temporal stability compared to fractional-scale pipelines at the same GPU cost, not "true" higher resolution. Higher base presets and NIS can be combined if your GPU allows.

---

### 2. VRAM & Compute Impact

Applying `2.0x` spatial reconstruction in `VRPacing` **does not carry the VRAM or compute cost of rendering a game at 200% supersampling**:

* In modern game engines, the bulk of VRAM and GPU frame time is spent inside internal rendering passes: multi-layer *G-Buffers*, shadow map cascades, depth buffers, volumetric lighting, MSAA targets, and temporal anti-aliasing (TAA).
* With `VRPacing`, the game executes **all internal rendering passes at the `1.0x` base resolution**. Only a final presentation swapchain buffer is allocated at `2.0x` for the NIS compute pass right before compositor submission (with an automatic maximum-pixel safety ceiling to protect VRAM on extreme presets).

---

### 3. The `-0.10 FPS` Frame Pacing Cap

VR headset displays operate on fixed hardware VSync intervals (`72`, `80`, `90`, `100`, `120 Hz`). When a game runs uncapped, or capped at exactly the nominal refresh rate (e.g., `90.00 FPS`), clock drift between the GPU and the headset compositor can fill the graphics driver's pre-rendered submission queue. When a frame then arrives late for the VSync latch, the compositor repeats the previous frame, causing a hitch (*judder*) and extra motion-to-photon latency.

A cap that is too low (e.g., `-1.0 FPS`, `89.0 FPS` on a `90 Hz` display) causes one repeated frame every second.

`VRPacing` waits with kernel *High-Resolution Waitable Timers* and caps delivery **`0.10 FPS` below the effective display frequency** (`89.90`, `79.90`, `71.90`, `59.90`, `44.90 FPS`):

* The small margin keeps the driver submission queue drained instead of letting it fill (the goal is a queue depth of 0).
* Frames are handed off just ahead of the compositor's VSync, targeting a flat frametime line with minimal latency. At `-0.10 FPS` a repeated frame still occurs very rarely (about once every 10 seconds at 90 Hz), instead of once per second with a `-1 FPS` cap.
* The limiter waits **before** the runtime call (`xrWaitFrame`, `WaitGetPoses`, `ovr_WaitToBeginFrame`), so it does not block the later submission stage.

Do not run RTSS or another frame limiter at the same time.

---

### 4. Fixed Foveated Rendering (VRS D3D11 & D3D12) and Pre-Render Circular Mask

1. **Fixed Foveated Rendering (VRS — DirectX 11 NVAPI & DirectX 12 Tier-2):**
   * Reduces peripheral shading rate while keeping the **central `60%` at full rate (`1x1`)**, the middle ring (`60–100%`) at `2x1` shading, and the outer periphery (`>100%`) at `4x4` (`1/16` of the pixels shaded).
   * Includes native support for **DirectX 12 Variable Rate Shading Tier-2**, intended for DX12 titles such as *Resident Evil 8* and Unreal Engine 5 games such as *Metro Awakening*.

2. **Pre-Render Circular Visibility Mask:**
   * Hides unseen outer corner pixels before scene shading, using **OpenXR** (`xrGetVisibilityMaskKHR`) and **OpenVR** (`IVRSystem::GetHiddenAreaMesh`, when the title does not provide its own mesh).

---

### 5. Compatibility Matrix by API & Runtime (Native vs. VDXR Bridge)

`VRPacing` combines an **OpenXR Implicit API Layer (`VRPacingLayer.dll`)** with a **Runtime Hook Module (`VRPacingHook.dll`)**. The table below shows which features run natively and which go through **Virtual Desktop's VDXR bridge**:

| Game API | Graphics API | Frame Pacing (`-0.10 FPS`) | `NIS 2.0x` Reconstruction | Fixed Foveated (`VRS`) | Circular Mask | Runtime Architecture & Behavior |
| --- | --- | --- | --- | --- | --- | --- |
| **OpenVR** *(SteamVR)* | DirectX 11 | Yes | Yes | Yes *(NVAPI)* | Yes *(Auto HAM)* | Direct `IVRCompositor` & `IVRSystem` interception. Works across SteamVR headsets. |
| **Oculus** *(LibOVR)* | DirectX 11 | Yes | Yes | Yes *(NVAPI)* | No | Compatible with Oculus runtime & Virtual Desktop (including legacy `< v25` titles like *Lone Echo*). |
| **OpenXR** *(Any Runtime)* | DirectX 11 | Yes | **Yes (Native)** | Yes *(NVAPI)* | Yes | Native D3D11 swapchain proxy inside `VRPacingLayer.dll`. Works on **VDXR, SteamVR OpenXR, and Meta Link** (e.g., *Dungeons of Eternity*, *Titanfall 2 VR*). |
| **OpenXR** *(VDXR Bridge)* | DirectX 12 | Yes *(Native)* | **Yes (Via VDXR)** | Yes *(D3D12 Tier-2 Native)* | Yes *(Native)* | Pacing, Mask, and D3D12 VRS run in the OpenXR layer; **NIS 2.0x executes via VDXR's D3D11 presentation bridge** (e.g., *RE8*, *Metro Awakening*, *MSFS*). |
| **OpenXR** *(VDXR Bridge)* | Vulkan | Yes *(Native)* | **Yes (Via VDXR)** | No | Yes *(Native)* | **NIS 2.0x executes via VDXR's D3D11 presentation bridge** (e.g., *DOOM VFR*). |
| **OpenComposite** *(OpenVR -> OpenXR)* | DirectX 11 | Yes | Yes | **Auto-Excluded** | **Auto-Excluded** | Detects active `openvr_api.dll` translation over OpenXR and disables VRS/Mask (`Excluded (OC)`) to prevent swapchain artifacts while keeping NIS 2.0x and Pacing active (e.g., *Skyrim VR*). |

* **Known exceptions:** games protected by kernel-level anti-cheat (*Easy Anti-Cheat*) that block external module loading, and pre-DirectX 11 engines (DirectX 9 / DirectX 10, such as *Half-Life 2 VR Mod* without a D3D11 wrapper).
* **Non-VDXR OpenXR runtimes with DirectX 12 or Vulkan games:** NIS is not applied (pacing, mask and D3D12 VRS still work where listed).

---

### 6. Reading Live Panel States & Operational Notes

* **Live Telemetry States (`Active` vs. `Standby`):**
  Although all optimization checkboxes are enabled by default, what actually applies depends on each game's rendering pipeline (for instance, some D3D11 engines with incompatible MSAA configurations cannot bind VRS, and not every game queries the visibility mask). The panel reports in real time which techniques are active in the running process (`Active`, `D3D11 NVAPI`, `D3D12 Tier-2`, `Excluded (OC)`, or `Standby`).
* **Changing Refresh Rate (Hz) On the Fly (OpenXR vs. SteamVR):**
  * In **OpenXR (VDXR)** games, you can change headset Hz or toggle SSW in Virtual Desktop mid-game; `VRPacing` detects the new display period and updates the `Active Cap` automatically.
  * In **OpenVR (SteamVR)** games, the SteamVR compositor locks display frequency at startup; changing headset Hz requires restarting SteamVR/the game.
* **`Reset Stats` Button & `Half Refresh Rate` Mode:**
  * Click **`Reset Stats`** at any time to clear average FPS history and re-trigger the Stability Advisor after changing in-game graphics settings, or select **`Half Refresh Rate (Manual Hz / 2 lock)`** to force a half-rate cap without waiting for automatic SSW detection.

---

## License

VRPacing is released under the MIT License (see `LICENSE`). Third-party components keep their own licenses; see `THIRD_PARTY_NOTICES.md`.

