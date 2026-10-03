# Video Player

A single-window C++17 / Qt Widgets video player with a dark interface, aspect-ratio-preserving video, audio, keyboard play/pause and replay, volume control, fullscreen, and a seek slider. Open a local file or drop it onto the window.

Choose **Register file types** in the right-click menu to add this installation to **Open With** for MP4, MKV, AVI, MOV, WebM, M4V, WMV, MPEG/MPG, TS/M2TS, and OGV. Windows also lists Video Player in **Settings > Apps > Default apps**, using per-user registration without administrator access. Choose defaults through your operating system; registration preserves existing defaults. Register again if you move the application.

Windows portable builds register the launcher executable. On Linux, registration creates a user desktop entry and icon pointing to the AppImage or executable. On macOS, run the installed `VideoPlayer.app`; videos opened from Finder or dropped onto its Dock icon open in the player. To set a macOS default, select a video in Finder, open **Get Info > Open with**, choose VideoPlayer, and click **Change All**.

Right-click for playback, seeking, audio and subtitle selection and cycling, volume steps, gamma reset, fullscreen, and **Update to latest version**. Keyboard shortcuts appear beside their commands. Updating downloads the matching asset from the latest GitHub release, verifies its size and SHA-256 checksum, replaces the app, and restarts with the current video at its saved position. Downloads can be canceled before installation. The application folder must be writable. Windows updates the portable launcher (or replaces a standalone local build with the portable release); Linux requires the AppImage; macOS requires the installed `VideoPlayer.app`. Diagnostic logs remain in a `.video-player-update-*` folder beside the app; macOS also retains the previous bundle there.

Video decoding, demuxing, seeking, and audio synchronization use FFmpeg shared libraries through **Qt Multimedia's FFmpeg backend**. The app explicitly selects this backend; it does not launch the FFmpeg command-line program or directly call libavcodec. See [Qt's backend documentation](https://doc.qt.io/qt-6/qtmultimedia-index.html).

## Build and run on this machine

Only one player runs per user. Opening another video through the executable sends its absolute path to the existing player and brings that window forward. Launching without a filename brings the existing player forward. The previous video's position is saved when switching files.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 -Test
.\dist\VideoPlayer.exe
# Or open a file immediately:
.\dist\VideoPlayer.exe "C:\Videos\movie.mp4"
```

The script uses the locally installed Qt 6.11.2 MinGW kit, MinGW 13.1, CMake, and Ninja under `C:\Qt`. Override `-QtRoot` and `-ToolsRoot` for another installation. Deployment collects Qt plugins, FFmpeg DLLs, and compiler runtime into `dist`; keep that directory together when moving the executable. CMakeLists.txt can also be opened in Qt Creator with a compatible Qt 6.5+ kit including Widgets, Multimedia, MultimediaWidgets, and Test.

Tests require `ffmpeg` on PATH solely to generate an eight-second video with audio. They exercise actual decoding, seeking and frame timestamps, pause/resume while scrubbing, replay, invalid media, and recovery using the offscreen Qt platform.

To check subtitle cycling on a local file with at least two embedded subtitle tracks, set `VIDEO_PLAYER_SUBTITLE_CYCLING_EXAMPLE` to its path and run `build/player_tests.exe subtitleCyclingExample -platform windows` with the Qt DLL directory on PATH. This checks video-frame timing and audio continuity while cycling. Set `VIDEO_PLAYER_SUBTITLE_CYCLING_GAMMA=16` to also exercise gamma correction at 1.6.

## GitHub builds

`.github/workflows/build.yml` runs on pushes, pull requests, and manual dispatch. It builds and tests Linux x64, Windows x64/ARM64, and macOS ARM64, then uploads an AppImage, portable Windows executables, and a macOS DMG. Pushing a `v*` tag also creates or updates a GitHub release after all builds pass.

Downloads include Qt, FFmpeg, and the compiler runtime; no separate installation of these is needed. Windows downloads are single `.exe` files: launch directly or pass a video filename. On first launch, a native launcher extracts the embedded runtime into `%LOCALAPPDATA%\VideoPlayer\Runtime\<content-hash>`; later launches reuse it. This is a self-extracting portable package, not a statically linked Qt build. The cache can be deleted when all players are closed. Linux uses an AppImage (Ubuntu 24.04 or newer compatible system); macOS ships a self-contained `.app` inside a DMG. Operating-system libraries and graphics drivers remain system requirements.

To build the single Windows executable locally, run `./build.ps1 -Portable -Test`; the result is `dist/vp-windows-x64.exe`. CI checks playback from this executable with only Windows system directories on PATH, including repeated and concurrent launches. `VideoPlayer --runtime-check <video>` is a headless decoding diagnostic that exits with zero after receiving a video frame and nonzero on failure. The test-only FFmpeg command-line executable generates fixtures and is not shipped with the app.

CI enables `VIDEO_PLAYER_DEPLOY` to install the runtime alongside the app. For example, configure with `-DVIDEO_PLAYER_DEPLOY=ON`, build, then run `cmake --install build --prefix package`. The regular local `build.ps1` remains available.

Release and MinSizeRel builds enable link-time optimization (LTO), optimize for size, and discard unused code and data. Windows MSVC also folds identical code; GNU-linked executables are stripped. Debug builds retain their normal settings. Configure with `-DVIDEO_PLAYER_OPTIMIZE_SIZE=OFF` to disable these application optimizations (configure the portable launcher separately with the same option if needed).

CI also builds Qt Base, Shader Tools, SVG, and Multimedia from checksum-pinned sources with Qt's LTO and size optimization options. The shared Qt libraries and plugins are optimized during their own links; LTO does not cross DLL/shared-library boundaries. FFmpeg binaries remain those supplied by the matching Qt kit. The source build checks Qt's LTCG feature and compiler flags, writes `lto-build.json`, and is cached by Qt version, architecture, compiler/SDK identity, and build scripts. Application configuration requires this verified kit in CI. The first uncached Qt build takes substantially longer.

For a local MinGW Qt build, put Python 3.12+, CMake, Ninja, and the installed MinGW toolchain on PATH, then run:

```powershell
python scripts/build-qt-lto.py --seed C:/Qt/6.11.2/mingw_64 --work build/qt-lto-work --install .qt-lto --toolchain mingw --parallel 6
powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1 -QtLto -Portable -Test
```

This creates a separate `.qt-lto` kit and `build/lto-player` build without modifying the installed Qt kit. MinGW uses targeted workarounds for GCC's Qt LTO issues: Windows platform plugins and a few accessibility/window translation units remain native code; other Qt modules use LTO. CI uses MSVC on Windows and does not need those exceptions. See [Qt's configure options](https://doc.qt.io/qt-6/configure-options.html) and [building Qt Multimedia](https://doc.qt.io/qt-6/qtmultimedia-building-from-source.html).

## Resize diagnostic

Run `build/player_tests.exe resizeDiagnostics -platform windows` with `VIDEO_PLAYER_RESIZE_BENCHMARK=1` and the Qt/MinGW DLL directories on PATH. This opt-in diagnostic repeats 60 growing and shrinking window sizes with no video, paused video, playing video, background erasing disabled, and the video surface hidden. It records time in resize calls, native video-window events, and background erasing. It opens a test window and generates its own clip.

Repeated local measurements with the default Direct3D presentation: playing video took 1098–1109 ms to grow and 1847–1873 ms to shrink. Empty windows took 447–453 ms to grow. Background erasing consumed only about 4–5 ms during playing-video runs; disabling it did not materially improve resizing. Shrinking was slower than growing, so this test does not establish an enlargement-specific defect.

For comparison, run the same diagnostic with `QT_D3D_NO_FLIP=1`. This selects Qt's legacy Direct3D swap-chain model for that process. Playing-video runs dropped to 826–849 ms growing and 898–909 ms shrinking. This implicates native presentation behavior, but does not individually isolate buffer allocation from GPU waits. The setting was tested only; the app's presentation configuration remains unchanged.

Paused-only follow-up: set `VIDEO_PLAYER_RESIZE_PAUSED_ONLY=1` alongside the diagnostic flag. With zero new video frames and zero playback-position change, default presentation took 883–885 ms growing and 1385–1434 ms shrinking. Legacy presentation took 784–823 ms growing and 828–841 ms shrinking. Hiding the paused video reduced growth to 408–422 ms. This confirms the overhead persists when presenting a static frame; the legacy mode only partly helps enlargement.

## Controls

Press `A` (Shift+A) to select embedded audio or an audio file in the video's directory whose name starts with the video's name without its extension. For example, `movie.ENG.ac3` accompanies `movie.avi`. Matching is case-insensitive; supported extensions include AC3, EAC3, DTS, AAC, M4A, MKA, MP3, FLAC, WAV, OGG, and Opus (decoding depends on FFmpeg support). External audio follows playback, seeking, and volume. Selecting an embedded track switches back; opening another video clears the external selection. Audio files are assumed to start at the same time as the video.

Volume is shared across all videos and remembered after restarting the app (initially 70%). Opening another video keeps the current volume; older per-video volume settings are ignored. Gamma is saved separately for each video's canonical file path, including neutral gamma, and restored when reopening it. New videos use gamma 1.0.

The gamma slider adjusts video midtones from 0.1 to 4.0: values above 1.0 lighten the picture and values below 1.0 darken it. Double-click the slider to reset it to 1.0 (unchanged). Changes apply immediately, including while paused, and the slider is available with the fullscreen controls. Gamma does not alter video files. At 1.0, video output bypasses the filter entirely. Non-neutral gamma converts and adjusts frames on the CPU, which can increase the cost of high-resolution playback.

Playback position is saved per file when you close the app (including Escape) or open another video. Reopening that file resumes from the saved position. Finished videos restart from the beginning. Positions are stored in the current user's Qt settings.

Press `S` (Shift+S) to choose an embedded subtitle track or any `.srt` file beside the current video, or choose Off. Embedded captions use a separate subtitle reader synchronized to playback, so changing them keeps the audio and video streams running. External subtitles support UTF-8 and BOM-marked UTF-16, multiline cues, and overlapping cues. They follow playback and seeking and can be changed while paused. Opening another video clears the subtitle selection.

In either track popup, click a checkbox (or press Space on the highlighted item) to include or skip that item when cycling. Click its name or press Enter to select it; the current track is bold. Lowercase `a` and `s` cycle through checked audio and subtitle items in menu order, wrapping at the end. Every item, including subtitles Off, starts checked when a video is opened. Checkbox choices are retained until another video is opened. With no items checked, cycling leaves playback unchanged.

| Control | Action |
| --- | --- |
| Right-click → Open video / Ctrl+O | Choose a local file |
| Space | Play or pause |
| A (Shift+A) | Choose an embedded audio track or a matching audio file beside the video |
| S (Shift+S) | Choose embedded subtitles or an `.srt` file in the video's folder; select Off to hide subtitles |
| a / s | Cycle through checked audio / subtitle items |
| Left-click video | After release, play or pause once the system double-click interval expires, only if no drag occurred |
| Double-click video | Toggle fullscreen without briefly pausing or resuming playback |
| Click and drag video or empty background | Move the window in normal windowed mode without changing playback |
| Timeline | Click or drag to seek immediately; playback continues from the latest selected position |
| Left / Right | Skip backward / forward 3 seconds |
| Shift+Left / Shift+Right | Skip backward / forward 30 seconds |
| Volume slider | Adjust audio; set to zero to silence |
| Gamma slider | Adjust midtones; double-click to reset to 1.0 |
| W / E | Decrease / increase gamma by 0.1 (range 0.1–4.0) |
| Mouse wheel / Up / Down | Increase or decrease volume in 5% steps, including fullscreen |
| Enter / F / F11 | Toggle fullscreen (move the mouse into the bottom controls area to reveal the sliders) |
| L / Folder button beside Gamma | Show/hide videos in the current folder to the right of the video; click a thumbnail or filename to play it |
| Escape | Close the app |
| Fullscreen top-right corner | Reveal the close cross; click to exit (hides when the pointer leaves) |

During dragging, each movement seeks immediately and playback continues from the latest selected position. Releasing does not seek again or change play/pause state. Seeking is disabled for media that does not support it. Format support depends on the installed FFmpeg build. Errors appear in the video area. The whole video fits inside the window without cropping or stretching, with black bars when needed, including fullscreen. The bottom contains the seek, volume, and gamma sliders with time and gamma readouts.
