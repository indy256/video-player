# Early GPU downscaling (experimental)

This Windows option resizes progressive 8-bit SDR NV12 frames on the decoder's
D3D11 device, before Qt copies them out of the decoder pool and across to its
rendering device. A 3840×2160 frame becomes 1920×1080 for a 1920-wide display;
the outgoing NV12 frame contains one quarter of the pixel data. Decoding still
runs at the video's original resolution. The video file is unchanged.

The D3D11 video processor writes into a reusable FFmpeg hardware-frame pool.
Resize commands are submitted asynchronously, without waiting on the CPU for
each frame. Qt's existing texture bridge synchronizes access between the decoder
and rendering devices. Playback can resume while a resize is still queued.
Frame timestamps, color metadata, rotation and seeking continue through the
regular Qt playback path.
Unsupported formats, HDR, interlaced video and failed GPU operations use the
original backend path. There is no CPU image conversion in the resize path.

## Build

Requires Windows, an MSVC developer shell, Python 3.12+, and the **exact Qt
6.11.2 kit** with its FFmpeg private implementation library. Both ARM64 and x64
are accepted; measurements here use ARM64. Normal builds keep this option off.

```powershell
cmake -S . -B build/early-scale-player -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=C:/Qt/6.11.2/msvc2022_arm64 -DCMAKE_MAKE_PROGRAM=C:/Qt/Tools/Ninja/ninja.exe -DVIDEO_PLAYER_EARLY_GPU_SCALE=ON -DVIDEO_PLAYER_DEPLOY=ON
cmake --build build/early-scale-player --parallel 4
cmake --install build/early-scale-player --prefix dist/early-gpu-scale
./dist/early-gpu-scale/bin/VideoPlayer.exe 'C:/Videos/movie.mp4'
```

The preparation script verifies pinned Qt source hashes, builds import libraries
for the kit's FFmpeg DLLs, and replaces one implementation object when relinking
the FFmpeg plugin. It leaves the Qt installation untouched. Installation replaces
the packaged stock FFmpeg plugin with this override after normal Qt deployment.
Copying only `VideoPlayer.exe` will not enable the feature.

This build chooses the largest physical screen width at launch to avoid reducing
detail on another connected display. Set `VIDEO_PLAYER_GPU_MAX_WIDTH=1920` to
override the limit, or `VIDEO_PLAYER_GPU_MAX_WIDTH=0` to disable resizing. The
limit preserves aspect ratio with even texture dimensions and never upscales.

## Checks and local measurements

Set `VIDEO_PLAYER_GPU_SCALE_EXAMPLE` to a 4K SDR video and
`VIDEO_PLAYER_GPU_MAX_WIDTH=1920`, then run `player_tests gpuDownscalePlayback
gpuGammaPlayback -platform windows` with the matching Qt DLL directory on PATH.
The downscale test checks actual frame dimensions and GPU-to-CPU readback, and
compares identical timestamps against an unscaled frame resized on the CPU.
When running tests from the build tree, set `QT_PLUGIN_PATH` to that build
directory so the override is selected ahead of the Qt kit's stock plugin.
The sample's RGB mean absolute difference was about 0.55 on a 0–255 scale. The gamma
test also exercises seeking, fullscreen and CPU rendering fallback.

For playback timing, use the root README's `startupDiagnostics` with
`VIDEO_PLAYER_SEEK_DIAGNOSTIC_MS=900000`. Each frame record includes dimensions
so the report confirms scaling actually happened. Alternate the width limit
between 0 and 1920 across separate runs, keeping the same plugin and video.

Two alternating pairs on Adreno X1-45, driver 31.0.137.0, native ARM64 Qt 6.11.2,
and a 4K H.264 60 fps sample produced these medians:

These measurements used the earlier `QQuickWidget` video surface and a per-frame
GPU completion wait. The current `QQuickView` renderer and asynchronous scaler
need a new comparison of scaled and original frames.

| Measurement | Original frames | Early scale to 1920 |
| --- | ---: | ---: |
| First-frame delay | 0.67 s | 0.83 s |
| Seek pause | 0.11 s | 0.29 s |
| Render updates in the first second after seeking | 14 | 43 |
| CPU usage, normalized to one core | 26.2% | 28.4% |

The extra processing improves early seek presentation in these short runs but
adds latency and CPU overhead. Startup rendering varied, and settled playback
can reach the source's 60 fps in either mode. Render updates count submitted
frames rather than physical display refreshes. These results do not establish
a general speed or power improvement, so the option remains experimental.

### Removing the per-frame wait

Two alternating pairs with `QQuickView` compared the old wait-enabled plugin
against asynchronous submission, both scaling to 1920. These nine-second runs
used the same hardware and source, gamma 1.0, an unobscured background window
with a 1100x667 video surface, and a seek to 900123 ms after three seconds.

| Median measurement | With GPU completion wait | Asynchronous |
| --- | ---: | ---: |
| First-frame delay | 0.65 s | 0.63 s |
| Seek pause | 0.52 s | 0.40 s |
| Render updates in the first second after seeking resumes | 46 | 34 |
| Settled render updates/second | 59.2 | 57.5 |
| CPU usage, normalized to one core | 31.3% | 31.0% |

Removing the wait resumes playback sooner, while some GPU work remains queued.
These short background-window measurements show a tradeoff in early seek
presentation; they do not establish a general frame-rate or CPU improvement.
All 24 native checks passed, including GPU scaling/readback, gamma, startup,
seeking, and native input. The RGB mean absolute difference remained about 0.55.
