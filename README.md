# VRPacing (`v0.8.0-beta`)

**System-wide PCVR utility for sub-millisecond Frame Pacing (`-0.10 FPS`), 2.0x Integer Spatial Reconstruction (NIS), and Fixed Foveated Rendering (D3D11 / D3D12).**

`VRPacing` is a lightweight background tray application for Windows (`VRPacingTray.exe`) designed to resolve two persistent bottlenecks in PCVR rendering and wireless streaming: frame delivery timing instability (*judder* caused by driver queue saturation) and temporal aliasing (*shimmering* and video encoder degradation) introduced by traditional fractional upscalers.

No manual `.dll` copying into game folders and no per-game `.yml` configuration files are required. `VRPacing` operates system-wide, automatically detecting and intercepting active sessions across **OpenVR (SteamVR)**, **Oculus (LibOVR)**, and **OpenXR (Native & VDXR)** under DirectX 11, DirectX 12, and Vulkan.

---

## PART 1: Quick Start Guide (Plug & Play)

### Zero-Configuration Setup
All core optimizations in `VRPacing` (**`-0.10 FPS` Frame Pacing**, **NIS `2.0x` Spatial Reconstruction**, **Fixed Foveated VRS**, and **Pre-Render Circular Mask**) are **enabled by default**. You do not need to configure settings for each individual game:

1. Run **`VRPacingTray.exe`** and leave it open in the background (or minimized to the system tray).
2. In **Virtual Desktop** (or your headset streaming software), select a base resolution preset and refresh rate (Hz) that your GPU can sustain consistently.
3. Launch your VR game. `VRPacing` automatically hooks the active runtime, applies `2.0x` spatial reconstruction, and locks frame delivery.

---

### Step 1: Choosing Your Base Resolution (and Note for Enthusiast GPUs)

Conventional upscaling guides recommend selecting an overly demanding headset resolution preset (such as *Ultra* or *Godlike*) and downscaling the game with a fractional render scale (`75%` or `77%`) combined with heavy artificial sharpening.

`VRPacing` reverses this workflow: the game engine renders natively at its `1.0x` base resolution, and **NVIDIA Image Scaling (NIS)** is applied at an exact **`2.0x` integer scale (200% per axis)** with **`0%` additional sharpening** over the **central `60%` radius** (the optical sweet spot where your eyes focus).

* **For Mid-Range and High-End GPUs:**
  Select a base resolution preset one tier lower than usual (for example, choosing **Medium** or **High** instead of *Ultra/Godlike*, while keeping SteamVR resolution at `100%`). Integer `2.0x` NIS reconstruction delivers a clean, temporally stable image without pixel crawl, freeing up the GPU headroom needed to maintain a completely flat frametime line.
* **For Enthusiast Hardware (RTX 4090 / RTX 5090):**
  The recommendation to step down one preset is intended solely to guarantee flat frametimes on GPUs with limited compute headroom. If you run an **RTX 4090 or RTX 5090** capable of sustaining target refresh rates natively at maximum presets (**Ultra** or **Godlike**), you can run those top presets with **NIS 2.0x enabled** to achieve the highest possible visual fidelity and edge reconstruction clarity.

#### Reference Resolution Table (Virtual Desktop — Meta Quest)
*(Note: While the table lists official Virtual Desktop per-eye presets for Meta Quest headsets, the exact same methodology applies to Pico 4 and other headsets by keeping SteamVR at `100%` and selecting a sustainable base preset).*

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

In virtual reality, **maintaining a completely flat frametime line is far more important than chasing a high but fluctuating average framerate**.

You can verify stability directly in the `VRPacing` window: when the diagnostic line reports **`Advisor: Target Sustained`** and the cyan frametime graph rests flat along the green target budget line, frame pacing is locked.

* **Practical Example (*Half-Life: Alyx* averaging 87 FPS):**
  If your headset is set to `90 Hz` or `120 Hz` and your PC averages **`87 FPS`**, the experience will stutter because the GPU fails to meet the headset's vertical sync deadline, causing constant phase drops. In this scenario, the optimal mode is neither 90 Hz nor 120 Hz native, but rather:
  1. Lowering the headset to **`80 Hz` Native**, locking delivery at a flat **`79.90 FPS`** with comfortable thermal and GPU headroom.
  2. Or setting the headset to **`120 Hz` with SSW Always On** (in Virtual Desktop), locking the game at a rock-solid **`59.90 FPS` (`60 / 120 Hz`)** so motion reprojection operates with consistent mathematical cadence.

#### Quick Guidelines by Game Genre
* **1. Standing & Fast-Paced Roomscale Games (Shooters / Action / Adventure):**
  * **Native Hz:** Choose the highest native refresh rate (`72`, `80`, `90`, or `120 Hz`) where your system maintains `Target Sustained`.
  * **With SSW Always On:** Use a minimum of **`60 / 120 Hz` (`59.90 FPS`)**, where 60 real frames interpolated to 120 Hz preserve responsive hand tracking and locomotion.
* **2. Fast-Paced Seated Cockpit Simulators (*Assetto Corsa*, *Star Wars: Squadrons*, Racing / Space Combat):**
  * Because you are seated within a fixed cockpit reference frame, these titles perform very well with **SSW Always On** starting at **`45 / 90 Hz` (`44.90 FPS`)**, **`50 / 100 Hz` (`49.90 FPS`)**, or **`60 / 120 Hz` (`59.90 FPS`)**.
* **3. Heavy Slow-Paced Simulators (*Microsoft Flight Simulator 2020 / 2024* in cockpit view):**
  * In slower cockpit flight with smooth head movements, **SSW Always On** is completely usable even at **`36 / 72 Hz` (`35.90 FPS`)** or **`40 / 80 Hz` (`39.90 FPS`)** provided the frametime line is completely flat—scaling up in smoothness if your hardware allows **`45 / 90 Hz` (`44.90 FPS`)**.

---

### Optional Pre-Setup Tip: FOV Tangent Reduction
If you use **Virtual Desktop**, the single most effective way to gain free performance before touching resolution is to slightly reduce the rendered Field of View (**FOV Tangent** horizontal and vertical sliders, e.g., to **`85%`** or **`80%`**, right up to the threshold where the black border is barely noticeable inside the headset).
* This discards 15%–25% of peripheral pixels that are distorted by the outer lens edges anyway, saving substantial GPU rasterization load with zero loss in central pixel density (PPD).
* In games that support `VRPacing`'s **Pre-Render Circular Visibility Mask**, the radial mask will smoothly round off the corners of that FOV crop.

---

## PART 2: Architecture & Technical Deep-Dive (Optional Reading)

```text
VRPACING SYSTEM-LEVEL PIPELINE ARCHITECTURE

 [ Game Engine (1.0x Base Render) ]
        │
        ├──> 1. Pre-Render Pass: FOV Tangent Clipping + Circular Depth Visibility Mask
        ├──> 2. Shading Pass: Fixed Foveated VRS (D3D11 NVAPI / D3D12 Tier-2)
        ▼
 [ Completed Native Framebuffer (1.0x) ] ── (Zero extra VRAM cost on G-Buffer / Shadows / MSAA)
        │
        ├──> 3. Presentation Pass: NIS 2.0x Integer Spatial Reconstruction (60% Foveal / 0% Sharpness)
        ├──> 4. Delivery Sync: High-Precision Waitable Timer at -0.10 FPS (Driver Queue Depth = 0)
        ▼
 [ Headset Compositor / Hardware Video Encoder (VDXR / SteamVR / LibOVR) ]
```

### 1. Integer Spatial Reconstruction (`NIS 2.0x` + `0% Sharpening`) vs. Fractional Scaling (`0.77x`)

Standard configurations in legacy tools rely on setting an oversized target resolution and applying fractional scaling ratios (`0.75x` or `0.77x`) paired with sharpening filters.

* **The Mathematical Flaw of Fractional Scaling:** At non-integer ratios, output pixels land at asymmetric subpixel phase offsets relative to the source texel grid. During continuous head micro-movements in VR, high-frequency geometry (foliage, power lines, fences, distant edges) shifts phase from frame to frame, producing visible temporal instability (*pixel crawl* or *shimmering*). To compensate for fractional resampling blur, users increase artificial sharpening, which introduces high-contrast edge ringing.
* **Wireless Video Encoder Degradation (HEVC / AV1):** On streaming headsets (*Virtual Desktop*, *AirLink*, *Steam Link*), high-frequency sharpening halos severely starve hardware video encoders (NVENC / AMF) of bitrate, resulting in compression noise and macroblocking.
* **The `VRPacing` Approach (`2.0x` Integer Scale with `0%` Sharpening over the central `60%`):**
  * **Deterministic 2×2 Mapping:** At exactly `200%` per axis, every native source pixel maps symmetrically to a $2 \times 2$ block of destination pixels. Phase alignment is identical across the entire frame.
  * **Why NIS Outperforms FSR 1 and CAS for Integer 2.0x Reconstruction:** Unlike FSR 1 (which relies on a modified Lanczos-2 kernel over a narrow $2 \times 2$ texel window) or CAS (a local contrast sharpening filter lacking a spatial reconstruction kernel), **NVIDIA Image Scaling (NIS)** uses a **6-tap adaptive kernel with 4 directional edge filters** (horizontal, vertical, and $\pm 45^\circ$ diagonals). Under symmetric `2.0x` integer conditions, NIS evaluates local gradients and reconstructs diagonal edges and contours at subpixel precision prior to lens barrel distortion.
  * **Why `0% Sharpening`:** Because the native `1.0x` frame was never downsampled, there is no resampling blur to compensate for. Setting `sharpness = 0.0` prevents artificial grain and ringing, feeding a clean signal to the hardware video encoder while restricting compute to the **central `60%` foveal radius**.

---

### 2. True VRAM & Compute Impact

Applying `2.0x` spatial reconstruction in `VRPacing` **does not carry the VRAM or compute cost of rendering a game at 200% native supersampling**:

* In modern game engines (Unreal Engine 4/5, Unity, RE Engine), roughly 90% of VRAM allocation and GPU frame time is spent inside internal rendering passes: multi-layer *G-Buffers*, shadow map cascades, depth buffers, volumetric lighting, MSAA targets, and temporal anti-aliasing (TAA).
* With `VRPacing`, the game engine executes **all internal rendering passes strictly at the `1.0x` base resolution**. Only a single final presentation swapchain buffer is allocated at `2.0x` for the NIS compute shader pass right before compositor submission (enforcing an automatic maximum-pixel safety ceiling to protect VRAM on extreme presets).

---

### 3. The `-0.10 FPS` Frame Pacing Law

VR headset displays operate on strict hardware VSync intervals (`72`, `80`, `90`, `100`, `120 Hz`). When a game runs uncapped or capped at the exact nominal refresh rate (e.g., `90.00 FPS`), clock drift between the GPU and the headset compositor fills the graphics driver's pre-rendered submission queue. Whenever a frame arrives a fraction of a millisecond late for the VSync latch, the compositor repeats the previous frame—causing an immediate hitch (*judder*) and increasing motion-to-photon latency.

Conversely, capping at `-1.0 FPS` (e.g., `89.0 FPS` on a `90 Hz` display) mathematically forces 1 dropped frame every single second.

`VRPacing` uses kernel-level *High-Resolution Waitable Timers* calibrated exactly **`0.10 FPS` below the effective display frequency** (`89.90`, `79.90`, `71.90`, `59.90`, `44.90 FPS`):

* Continuously drains the graphics driver submission queue (**Driver Queue Depth = 0**).
* Ensures frames are handed off just ahead of the compositor's VSync lock, achieving a flat frametime line with minimal latency.

---

### 4. Fixed Foveated Rendering (VRS D3D11 & D3D12) and Pre-Render Circular Mask

1. **Fixed Foveated Rendering (VRS — DirectX 11 NVAPI & DirectX 12 Tier-2):**
   * Reduces peripheral pixel shading rates while keeping the **central `60%` at full resolution (`1x`)**, the middle ring (`60–100%`) at `1/2x`, and the outer periphery (`>100%`) at `1/16x`.
   * While legacy tools were restricted to DirectX 11, `VRPacing` includes native support for **DirectX 12 Variable Rate Shading Tier-2**, enabling foveated shading in modern DX12 titles such as *Resident Evil 8* and Unreal Engine 5 games like *Metro Awakening*.

2. **Pre-Render Circular Visibility Mask:**
   * Clips unseen outer corner pixels in the depth buffer prior to scene shading, supporting both **OpenXR** (`xrGetVisibilityMaskKHR`) and **OpenVR** (`IVRSystem::GetHiddenAreaMesh` when the title does not provide its own mesh).

---

### 5. Compatibility Matrix by API & Runtime (Native vs. VDXR Bridge)

`VRPacing` combines an **Official Implicit OpenXR API Layer (`VRPacingLayer.dll`)** with a **Runtime Hook Module (`VRPacingHook.dll`)**. The table below details which features run natively across all headsets and which leverage **Virtual Desktop's VDXR bridge**:

| Game API | Graphics API | Frame Pacing (`-0.10 FPS`) | `NIS 2.0x` Reconstruction | Fixed Foveated (`VRS`) | Circular Mask | Runtime Architecture & Behavior |
| --- | --- | --- | --- | --- | --- | --- |
| **OpenVR** *(SteamVR)* | DirectX 11 | Yes | Yes | Yes *(NVAPI)* | Yes *(Auto HAM)* | Direct `IVRCompositor` & `IVRSystem` interception. Works across all SteamVR headsets. |
| **Oculus** *(LibOVR)* | DirectX 11 | Yes | Yes | Yes *(NVAPI)* | No | Compatible with Oculus runtime & Virtual Desktop (including legacy `< v25` titles like *Lone Echo*). |
| **OpenXR** *(Any Runtime)* | DirectX 11 | Yes | **Yes (Native)** | Yes *(NVAPI)* | Yes | Native D3D11 swapchain proxy inside `VRPacingLayer.dll`. Works on **VDXR, SteamVR OpenXR, and Meta Link** (e.g., *Dungeons of Eternity*, *Titanfall 2 VR*). |
| **OpenXR** *(VDXR Bridge)* | DirectX 12 | Yes *(Native)* | **Yes (Via VDXR)** | Yes *(D3D12 Tier-2 Native)* | Yes *(Native)* | Pacing, Mask, and D3D12 VRS run natively in OpenXR; **NIS 2.0x executes via VDXR's D3D11 presentation bridge** (e.g., *RE8*, *Metro Awakening*, *MSFS*). |
| **OpenXR** *(VDXR Bridge)* | Vulkan | Yes *(Native)* | **Yes (Via VDXR)** | No | Yes *(Native)* | **NIS 2.0x executes via VDXR's D3D11 presentation bridge** (e.g., *DOOM VFR*). |
| **OpenComposite** *(OpenVR -> OpenXR)* | DirectX 11 | Yes | Yes | **Auto-Excluded** | **Auto-Excluded** | Detects active `openvr_api.dll` translation over OpenXR and disables VRS/Mask (`Excluded (OC)`) to prevent swapchain artifacts while keeping NIS 2.0x and Pacing active (e.g., *Skyrim VR*). |

* **Known Exceptions:** Games protected by kernel-level anti-cheat systems (*Easy Anti-Cheat*) that block external module loading, and pre-DirectX 11 engines (DirectX 9 / DirectX 10, such as *Half-Life 2 VR Mod* without a D3D11 wrapper).

---

### 6. Reading Live Panel States & Operational Notes

* **Live Telemetry States (`Active` vs. `Standby`):**
  Although all three optimization checkboxes are enabled by default, actual application depends on each game engine's rendering pipeline (for instance, certain D3D11 engines using incompatible MSAA configurations cannot bind VRS, and not every game queries or renders the visibility mask in its 3D camera). The panel reports in real time which techniques are actively bound in the running process (`Active`, `D3D11 NVAPI`, `D3D12 Tier-2`, `Excluded (OC)`, or `Standby`).
* **Changing Refresh Rate (Hz) On the Fly (OpenXR vs. SteamVR):**
  * In **OpenXR (VDXR)** games, you can change headset Hz or toggle SSW in Virtual Desktop mid-game; `VRPacing` immediately detects the new display period and updates the `Active Cap` automatically.
  * In **OpenVR (SteamVR)** games, the SteamVR compositor locks display frequency at startup; changing headset Hz requires restarting SteamVR/the game for SteamVR to adopt the new refresh rate.
* **`Reset Stats` Button & `Half Refresh Rate` Mode:**
  * Click **`Reset Stats`** at any time to clear average FPS history and re-trigger the Stability Advisor after changing in-game graphics settings, or select **`Half Refresh Rate (Manual Hz / 2 lock)`** to manually force a half-rate cap without waiting for automatic SSW detection.
