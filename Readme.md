# MPC Video Renderer NR

Fork of MPC Video Renderer with **NVIDIA DLSS 5 Neural Rendering** (NGX feature 18)
integrated as a real-time post-processing pass. Runs inside MPC-BE on any
DirectShow video source.

## Key features

* Can work with DXVA2 and Direct3D 11 hardware decoder.
* DVXA2 and Direct3D11 Video Processor with hardware de-interlacing for NV12, YUY2, P010 formats.
* Shader video processor for various YUV, RGB and grayscale formats.
* Various frame resizing algorithms, including Super Resolution.
* Subtitle and OSD display.
* Rotation and flip of the video frame.
* Dithering when the final color depth is reduced from 10/16 bits to 8 bits.
* HDR video support (HDR10, HLG and partially Dolby Vision).
* Automatic HDR to SDR conversion.
* Transferring HDR10 data to the display.
* **DLSS 5 Neural Rendering** — neural denoise/enhance via NGX (see below).

## DLSS 5 Neural Rendering

The renderer spawns a separate `nr_worker.exe` process that runs the NGX NR
model (feature id 18, `nvngx_dlssnr.dll`). Cross-process shared D3D11→D3D12
textures carry the frame data; CPU-side event queries and D3D12 fences provide
frame synchronisation.

### Motion vectors (optical flow)

The worker uses the **NVIDIA Optical Flow SDK** (`NvOF`) to generate per-pixel
motion vectors between consecutive frames. A compute shader converts the
input frame to grayscale (luma), the hardware OFA engine computes flow in
S10.5 fixed-point (`R16G16_SINT`), and a second compute shader converts the
flow to `R16G16_FLOAT` pixels — the DLSS-recommended motion-vector format.
These are passed to NGX as `DLSSNR.MotionVectors` for temporal stabilisation.

The first frame initialises the previous-frame buffer without motion vectors
(`DLSSNR.Reset = 1`). From the second frame onward, the full
grayscale → NvOF → flow-to-float → NGX pipeline runs every frame.

### NGX parameters

```
DLSSNR.Width/Height           frame dimensions
DLSSNR.Style                  0=default, 1=natural, 2=cinematic
DLSSNR.Hint.Render.Preset     1–5 (quality/speed tradeoff)
DLSSNR.Intensity              NR strength
DLSSNR.LocalToneStrength      local tone mapping
DLSSNR.LocalStructureStrength local detail enhancement
DLSSNR.MotionVectors          R16G16_FLOAT texture (NvOF output)
DLSSNR.MVecScaleX/Y           1.0 (conversion done in shader)
DLSSNR.Reset                  1 on first frame, 0 thereafter
DLSSNR.Color/Output/Backbuffer shared RGBA16F textures
DLSSNR.ScalingRatio           1.0 (no upscaling)
DLSSNR.DepthInverted          1
```

### Why a separate process?

NGX `Init_Ext` returns `0xBAD00002` (PlatformError) when called in-process
inside MPC-BE, but succeeds in a standalone process. The worker
(`nr_worker.exe`) opens inherited NT-handle shared textures, initialises NGX
there, and loops on sync events: `wait(frame_ready) → NGX eval → signal(frame_done)`.

## Runtime files (NOT redistributable)

The following NVIDIA DLLs are loaded at runtime via `LoadLibrary` and are
**not** included in this repository:

| File | Source |
|------|--------|
| `_nvngx.dll` | NVIDIA driver store |
| `nvngx_dlssnr.dll` | NVIDIA driver / NGX SDK |
| `caller/nvngx.dll` | NGX shim (caller validation bypass) |

Place them in the MPC-BE install directory (`C:\Program Files\MPC-BE\`).
The `nvof/` directory contains headers from the NVIDIA Optical Flow SDK 5.0.7
(MIT-licensed); the SDK's `nvofapi64.dll` is loaded dynamically from the
driver at runtime.

## Building

Requires **Visual Studio 2022 Build Tools** (v143 toolset) and Windows SDK
10.0.26100.0.

```
MSBuild MpcVideoRenderer.sln /p:Configuration=Release /p:Platform=x64 /m /v:minimal
```

The worker is built separately (see `nr_worker.cpp` in the
[`dlss5-nr-player`](https://github.com/Zonnery/dlss5-nr-player) repo):

```
cl /nologo /EHsc /std:c++17 /O2 /I nvof /Fe:nr_worker.exe ^
   nr_worker.cpp nvof\NvOF.cpp nvof\NvOFD3DCommon.cpp nvof\NvOFD3D12.cpp ^
   /link d3d11.lib d3d12.lib dxgi.lib user32.lib ole32.lib d3dcompiler.lib
```

## Deployment

Copy the built files to the MPC-BE install directory:

```
MpcVideoRenderer64.ax → C:\Program Files\MPC-BE\Filters\
nr_worker.exe         → C:\Program Files\MPC-BE\
```

Register the filter: `regsvr32 MpcVideoRenderer64.ax`

## Minimum system requirements

* An SSE2-capable CPU
* Windows 10 or newer
* **NVIDIA RTX 50-series GPU** (RTX 5090 tested) — required for NGX NR + OFA
* DirectX 11 video card

## License

MPC Video Renderer's code is licensed under [GPL v3].

The `nvof/` headers are Copyright (c) 2018-2023 NVIDIA Corporation (MIT license).

## Links

[Original upstream](https://github.com/Aleksoid1978/VideoRenderer)

[MPC-BE](https://github.com/Aleksoid1978/MPC-BE)

[DLSS 5 NR Player](https://github.com/Zonnery/dlss5-nr-player) — standalone reference
