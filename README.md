# Video Player

A single-window C++17 / Qt Widgets video player with a dark interface, aspect-ratio-preserving video, audio, keyboard play/pause and replay, volume control, fullscreen, and a seek slider. Open a local file or drop it onto the window.

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

## GitHub builds

`.github/workflows/build.yml` runs on pushes, pull requests, and manual dispatch. It builds and tests Linux x64, Windows x64/ARM64, and macOS ARM64, then uploads an AppImage, portable Windows executables, and a macOS DMG. Pushing a `v*` tag also creates or updates a GitHub release after all builds pass.

Downloads include Qt, FFmpeg, and the compiler runtime; no separate installation of these is needed. Windows downloads are single `.exe` files: launch directly or pass a video filename. On first launch, a native launcher extracts the embedded runtime into `%LOCALAPPDATA%\VideoPlayer\Runtime\<content-hash>`; later launches reuse it. This is a self-extracting portable package, not a statically linked Qt build. The cache can be deleted when all players are closed. Linux uses an AppImage (Ubuntu 24.04 or newer compatible system); macOS ships a self-contained `.app` inside a DMG. Operating-system libraries and graphics drivers remain system requirements.

To build the single Windows executable locally, run `./build.ps1 -Portable -Test`; the result is `dist/video-player-windows-x64.exe`. CI checks playback from this executable with only Windows system directories on PATH, including repeated and concurrent launches. `VideoPlayer --runtime-check <video>` is a headless decoding diagnostic that exits with zero after receiving a video frame and nonzero on failure. The test-only FFmpeg command-line executable generates fixtures and is not shipped with the app.

CI enables `VIDEO_PLAYER_DEPLOY` to install the runtime alongside the app. For example, configure with `-DVIDEO_PLAYER_DEPLOY=ON`, build, then run `cmake --install build --prefix package`. The regular local `build.ps1` remains available.

## Resize diagnostic

Run `build/player_tests.exe resizeDiagnostics -platform windows` with `VIDEO_PLAYER_RESIZE_BENCHMARK=1` and the Qt/MinGW DLL directories on PATH. This opt-in diagnostic repeats 60 growing and shrinking window sizes with no video, paused video, playing video, background erasing disabled, and the video surface hidden. It records time in resize calls, native video-window events, and background erasing. It opens a test window and generates its own clip.

Repeated local measurements with the default Direct3D presentation: playing video took 1098–1109 ms to grow and 1847–1873 ms to shrink. Empty windows took 447–453 ms to grow. Background erasing consumed only about 4–5 ms during playing-video runs; disabling it did not materially improve resizing. Shrinking was slower than growing, so this test does not establish an enlargement-specific defect.

For comparison, run the same diagnostic with `QT_D3D_NO_FLIP=1`. This selects Qt's legacy Direct3D swap-chain model for that process. Playing-video runs dropped to 826–849 ms growing and 898–909 ms shrinking. This implicates native presentation behavior, but does not individually isolate buffer allocation from GPU waits. The setting was tested only; the app's presentation configuration remains unchanged.

Paused-only follow-up: set `VIDEO_PLAYER_RESIZE_PAUSED_ONLY=1` alongside the diagnostic flag. With zero new video frames and zero playback-position change, default presentation took 883–885 ms growing and 1385–1434 ms shrinking. Legacy presentation took 784–823 ms growing and 828–841 ms shrinking. Hiding the paused video reduced growth to 408–422 ms. This confirms the overhead persists when presenting a static frame; the legacy mode only partly helps enlargement.

## Controls

Volume is saved whenever it changes and restored on the next launch, including zero volume. The initial default is 70%.

Playback position is saved per file when you close the app (including Escape) or open another video. Reopening that file resumes from the saved position. Finished videos restart from the beginning. Positions are stored in the current user's Qt settings.

| Control | Action |
| --- | --- |
| Open video / Ctrl+O | Choose a local file |
| Space | Play or pause |
| Left-click video | After release, play or pause once the system double-click interval expires, only if no drag occurred |
| Double-click video | Toggle fullscreen without briefly pausing or resuming playback |
| Click and drag video or empty background | Move the window in normal windowed mode without changing playback |
| Timeline | Click or drag to seek immediately; playback continues from the latest selected position |
| Left / Right | Skip backward / forward 10 seconds |
| Volume slider | Adjust audio; set to zero to silence |
| Mouse wheel / Up / Down | Increase or decrease volume in 5% steps, including fullscreen |
| F / F11 | Toggle video-only fullscreen (controls hidden, cursor visible) |
| Escape | Close the app |

During dragging, each movement seeks immediately and playback continues from the latest selected position. Releasing does not seek again or change play/pause state. Seeking is disabled for media that does not support it. Format support depends on the installed FFmpeg build. Errors appear in the video area. The whole video fits inside the window without cropping or stretching, with black bars when needed, including fullscreen. The bottom contains only the seek slider, Open button, and volume slider.
