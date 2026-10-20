"""Build an SDK and a pinned Qt backend override without changing the Qt kit."""
import argparse
import hashlib
import importlib.util
from pathlib import Path
from types import SimpleNamespace
import urllib.request

QT_VERSION = "6.11.2"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qt-root", required=True, type=Path)
    parser.add_argument("--work", required=True, type=Path)
    parser.add_argument("--arch", required=True, choices=("arm64", "x64"))
    args = parser.parse_args()
    spec = importlib.util.spec_from_file_location(
        "qt_builder", Path(__file__).resolve().parents[2] / "scripts/build-qt-lto.py")
    builder = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(builder)
    builder.ffmpeg_sdk(SimpleNamespace(version=QT_VERSION, work=args.work,
        seed=args.qt_root, toolchain="msvc", arch=args.arch))
    sources = {
        "qffmpeghwaccel_d3d11.cpp": "2a2de8a7df485a1ff26f5d68345ce9d0baa4ea1a1b57ff2400851e815a81d1ed",
        "qffmpegplugin.cpp": "0ec3f8bbde614c2a4f4202042f77fbbcda51065a7c7996bf47f6b2678a224827",
        "ffmpeg.json": "b1bef03e0c3aeac3a5f0dac7d85fed68fa390317cd6f15e6f715108269634f02",
    }
    upstream = args.work / "upstream"
    upstream.mkdir(exist_ok=True)
    for name, checksum in sources.items():
        path = upstream / name
        if not path.exists():
            urllib.request.urlretrieve(
                f"https://raw.githubusercontent.com/qt/qtmultimedia/v{QT_VERSION}/"
                "src/plugins/multimedia/ffmpeg/" + name, path)
        if hashlib.sha256(path.read_bytes()).hexdigest() != checksum:
            raise RuntimeError(f"Qt source checksum mismatch: {path}")
        content = path.read_text(encoding="utf-8")
        if name == "qffmpeghwaccel_d3d11.cpp":
            anchor = "AVFrameUPtr copyFromHwPoolD3D11(AVFrameUPtr src)"
            allocation = "    AVFrameUPtr dest = makeAVFrame();"
            if content.count(anchor) != 1 or content.count(allocation) != 1:
                raise RuntimeError("Unrecognized Qt D3D11 frame copy implementation")
            content = content.replace(anchor, '#include "earlyscale.h"\n\n' + anchor)
            content = content.replace(allocation,
                '    thread_local EarlyGpuScaler scaler;\n'
                '    if (auto scaled = scaler.scale(src.get(),\n'
                '            qEnvironmentVariableIntValue("VIDEO_PLAYER_GPU_MAX_WIDTH")))\n'
                '        return scaled;\n\n' + allocation)
        target = args.work / name
        if not target.exists() or target.read_text(encoding="utf-8") != content:
            target.write_text(content, encoding="utf-8")


if __name__ == "__main__":
    main()
