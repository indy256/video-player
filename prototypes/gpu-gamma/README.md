# GPU gamma experiment

An optional, standalone player benchmarks the application's `VideoSurface` in
GPU and CPU fallback modes. It shares the regular player's renderer and shader. Both paths use Qt Multimedia's FFmpeg backend and its default hardware
decoder selection. Audio is muted, and this tool does not read or save the
application's settings.

The GPU path applies gamma to a texture after Qt renders/scales the video. It
avoids the CPU filter's `toImage()`, per-pixel loop, and upload of a replacement
frame. Gamma 1.0 disables the extra effect layer. The shader preserves
premultiplied alpha. HDR display/tonemapping equivalence remains unvalidated.
The main player retains its existing subtitle and notification overlays outside
the gamma layer.

## Build and try

Use a normal Qt kit containing **Quick Widgets, QML, Qt Multimedia's QML plugin,
and Shader Tools**. The project's LTO build now includes Qt Declarative and the
Multimedia QML plugin. The optional benchmark executable is not part of release
packages.

With the existing build configured and the matching Qt DLL directory on PATH:

```powershell
cmake -S . -B build -DVIDEO_PLAYER_GPU_GAMMA_PROTOTYPE=ON
cmake --build build --target gpu_gamma_prototype --parallel 4
./build/prototypes/gpu-gamma/gpu_gamma_prototype.exe "C:/Videos/movie.mp4" --mode gpu --gamma 1.6
```

The window has a timeline and gamma slider. Space toggles playback, F toggles
fullscreen, and Escape closes it. Use `--mode cpu` for the original filter.
Use `--fullscreen` with `--seconds` to measure at the display's full resolution.
The benchmark runner also accepts `--fullscreen`. Launching without a filename
opens a file picker.

For a deployment beside its own DLLs and QML imports:

```powershell
New-Item -ItemType Directory dist/gpu-gamma -Force
Copy-Item build/prototypes/gpu-gamma/gpu_gamma_prototype.exe dist/gpu-gamma/
windeployqt --release --compiler-runtime --qmldir src/video dist/gpu-gamma/gpu_gamma_prototype.exe
```

## Reproduce the comparison

Generate a video longer than the two-second warmup plus the measurement interval:

```powershell
ffmpeg -v error -f lavfi -i "testsrc2=size=3840x2160:rate=30" -t 14 -c:v libx264 -preset ultrafast -crf 25 -threads 4 -pix_fmt yuv420p -y build/gamma-4k-h264.mp4
python prototypes/gpu-gamma/benchmark.py --executable build/prototypes/gpu-gamma/gpu_gamma_prototype.exe --video build/gamma-4k-h264.mp4 --qt-bin C:/Qt/6.11.2/msvc2022_arm64/bin --repeats 2 --seconds 6 --output build/gamma-benchmark.json
```

The runner opens visible windows sequentially and alternates the order of the
four configurations. Avoid covering/minimizing them. Reports and decoder logs
are retained beside the aggregate report, in a `.runs` directory. An early media
end or nonzero process exit fails the run.

CPU percentage is normalized to **one core** (100% means one fully occupied
core), using process kernel+user CPU time on Windows. Frame rate and p95 timing
measure frames delivered to the renderer, not physical display refreshes. The
requested measurement duration can overrun when the CPU filter blocks the UI;
reports use the actual elapsed time. The source is 4K, displayed in a 1280×800
window; GPU gamma runs at display resolution, while the existing CPU filter
processes full-resolution video frames.

On Snapdragon X Plus X1P42100 / Adreno X1-45, driver 31.0.137.0, native ARM64 Qt
6.11.2 and FFmpeg 7.1.5, two runs produced these medians (six-second measurement,
two-second warmup, desktop scale 125%):

| Path | Gamma | CPU (% of one core) | Frame delivery (fps) |
| --- | --- | ---: | ---: |
| Existing native surface | 1.0 | 11.8 | 30.0 |
| Qt Quick surface | 1.0 | 8.8 | 30.0 |
| Existing CPU gamma | 1.6 | 108.0 | 13.4 |
| Qt Quick GPU gamma | 1.6 | 20.1 | 29.9 |

All four configurations selected D3D11 hardware decoding. These are short local
measurements with desktop-load variation, not power measurements or broad
hardware benchmarks.

## Visual and interaction check

The regular player's `gpuGammaPlayback` test checks paused gamma pixels, reset,
seeking, fullscreen, input forwarding, and recovery to the CPU renderer. Run it
on a GPU-backed desktop; headless/offscreen tests exercise the CPU fallback.

```powershell
$env:VIDEO_PLAYER_GPU_GAMMA_EXAMPLE = "build/gamma-4k-h264.mp4"
./build/player_tests.exe gpuGammaPlayback -platform windows -o build/gpu-gamma-tests.txt,txt
```
