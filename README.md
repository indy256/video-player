# Video Player

A single-window C++17 / Qt Widgets video player with a dark interface, aspect-ratio-preserving video, audio, play/pause, replay, volume/mute, fullscreen, and a seek slider. Open a local file or drop it onto the window.

Video decoding, demuxing, seeking, and audio synchronization use FFmpeg shared libraries through **Qt Multimedia's FFmpeg backend**. The app explicitly selects this backend; it does not launch the FFmpeg command-line program or directly call libavcodec. See [Qt's backend documentation](https://doc.qt.io/qt-6/qtmultimedia-index.html).

## Build and run on this machine

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 -Test
.\dist\VideoPlayer.exe
# Or open a file immediately:
.\dist\VideoPlayer.exe "C:\Videos\movie.mp4"
```

The script uses the locally installed Qt 6.11.2 MinGW kit, MinGW 13.1, CMake, and Ninja under `C:\Qt`. Override `-QtRoot` and `-ToolsRoot` for another installation. Deployment collects Qt plugins, FFmpeg DLLs, and compiler runtime into `dist`; keep that directory together when moving the executable. CMakeLists.txt can also be opened in Qt Creator with a compatible Qt 6.5+ kit including Widgets, Multimedia, MultimediaWidgets, and Test.

Tests require `ffmpeg` on PATH solely to generate an eight-second video with audio. They exercise actual decoding, seeking and frame timestamps, pause/resume while scrubbing, replay, invalid media, and recovery using the offscreen Qt platform.

## Controls

| Control | Action |
| --- | --- |
| Open video / Ctrl+O | Choose a local file |
| Space | Play or pause |
| Timeline | Click anywhere or drag to a position; release to seek |
| Left / Right | Skip backward / forward 10 seconds |
| Volume / Mute | Adjust or mute audio |
| F11 / Escape | Toggle fullscreen / leave fullscreen |

During dragging, playback pauses and the time display previews the target. Releasing resumes only if the video was playing before dragging. Seeking is disabled for media that does not support it. Format support depends on the installed FFmpeg build. Errors appear inside the same window.
