"""Build the player's shared Qt modules with LTO, preserving the kit's FFmpeg ABI.

Requires Python 3.12+, CMake, Ninja, and a native C/C++ toolchain on PATH.
The seed kit supplies only FFmpeg binaries, not Qt libraries for the new build.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile

SCRIPTS = Path(__file__).resolve().parent
FFMPEG = {
    "6.8.3": ("7.1", "7ddad2d992bd250a6c56053c26029f7e728bebf0f37f80cf3f8a0e6ec706431a"),
    "6.10.3": ("7.1.3", "e0b04c4b43d7e6d67cb6710334fb513adf13ac860532f30e1d0ac4c231f232fb"),
    "6.11.2": ("7.1.5", "e3963a50831c985933e1a625ed566ec4c7adb5c012c34fa9f84438e1d61bdacc"),
}


def run(*args, cwd=None, capture=False):
    print("+", " ".join(map(str, args)), flush=True)
    return subprocess.run(list(map(str, args)), cwd=cwd, check=True,
                          text=True, stdout=subprocess.PIPE if capture else None).stdout


def source(work, name, url, checksum, dirname):
    archive = work / name
    if not archive.exists():
        run("curl", "-fL", "--retry", "3", url, "-o", archive)
    with archive.open("rb") as stream:
        if hashlib.file_digest(stream, "sha256").hexdigest() != checksum:
            raise RuntimeError(f"Source checksum mismatch: {archive}")
    dest = work / "src"
    result = dest / dirname
    if not (result / ".extracted").exists():
        dest.mkdir(parents=True, exist_ok=True)
        with tarfile.open(archive) as tar:
            tar.extractall(dest, filter="data")
        (result / ".extracted").touch()
    return result


def ffmpeg_sdk(args):
    """Pair Qt's FFmpeg binaries with the matching upstream public headers.

    Windows kits omit import libraries; recreate those from the DLL exports.
    All supported targets (x64/ARM64) are little-endian with unaligned access.
    No FFmpeg implementation code is compiled or changed here.
    """
    version, checksum = FFMPEG[args.version]
    src = source(args.work, f"ffmpeg-{version}.tar.gz",
                 f"https://codeload.github.com/FFmpeg/FFmpeg/tar.gz/refs/tags/n{version}",
                 checksum, f"FFmpeg-n{version}")
    sdk = args.work / "ffmpeg-sdk"
    for part in ("include", "lib", "bin"):
        (sdk / part).mkdir(parents=True, exist_ok=True)
    components = {"avcodec": 61, "avformat": 61, "avutil": 59, "swresample": 5, "swscale": 8}
    for component, major in components.items():
        headers = sdk / "include" / f"lib{component}"
        headers.mkdir(exist_ok=True)
        for header in (src / f"lib{component}").glob("*.h"):
            shutil.copy2(header, headers / header.name)
        if sys.platform == "win32":
            dll = args.seed / "bin" / f"{component}-{major}.dll"
            shutil.copy2(dll, sdk / "bin" / dll.name)
            definition = sdk / "lib" / f"{component}.def"
            if args.toolchain == "msvc":
                exports = run("dumpbin", "/EXPORTS", dll, capture=True)
                symbols = re.findall(r"^\s*\d+\s+[0-9A-F]+\s+[0-9A-F]+\s+(\w+)\s*$", exports, re.M)
                if not symbols:
                    raise RuntimeError(f"No exports found in {dll}")
                definition.write_text(f"LIBRARY {dll.name}\nEXPORTS\n" + "\n".join(symbols) + "\n")
                run("lib", "/NOLOGO", f"/DEF:{definition}", f"/MACHINE:{args.arch}",
                    f"/OUT:{sdk / 'lib' / (component + '.lib')}")
            else:
                exports = run("gendef", "-", dll, capture=True)
                definition.write_text(exports)
                run("dlltool", "-d", definition, "-l", sdk / "lib" / f"lib{component}.dll.a")
        else:
            pattern = f"lib{component}*.dylib" if sys.platform == "darwin" else f"lib{component}.so*"
            libraries = list((args.seed / "lib").glob(pattern))
            if not libraries:
                raise RuntimeError(f"Missing FFmpeg library: {pattern}")
            for library in libraries:
                target = sdk / "lib" / library.name
                if target.is_symlink():
                    target.unlink()
                shutil.copy2(library, target, follow_symlinks=False)
            suffix = ".dylib" if sys.platform == "darwin" else ".so"
            link = sdk / "lib" / f"lib{component}{suffix}"
            if not link.exists():
                target = sorted(p.name for p in libraries if not p.is_symlink())[0]
                link.symlink_to(target)
            if sys.platform.startswith("linux"):
                # Qt's FFmpeg binaries depend on Qt-built resolver stubs for
                # VAAPI/OpenSSL. FindFFmpeg discovers them through Libs in .pc
                # files, which the binary kit omits. Recover the dependencies
                # from ELF so Qt rebuilds these resolver libraries with LTO too.
                dynamic = run("readelf", "-d", link, capture=True)
                stubs = sorted(set(re.findall(r"\[lib(Qt6FFmpegStub-[\w-]+)\.so\.", dynamic)))
                pkgconfig = sdk / "lib/pkgconfig"
                pkgconfig.mkdir(exist_ok=True)
                (pkgconfig / f"lib{component}.pc").write_text(
                    f"Name: lib{component}\nDescription: Qt FFmpeg runtime\nVersion: {version}\n"
                    f"Libs: -l{component} " + " ".join(f"-l{stub}" for stub in stubs) + "\nLibs.private: \n")
    (sdk / "include/libavutil/avconfig.h").write_text(
        "#ifndef AVUTIL_AVCONFIG_H\n#define AVUTIL_AVCONFIG_H\n"
        "#define AV_HAVE_BIGENDIAN 0\n#define AV_HAVE_FAST_UNALIGNED 1\n#endif\n")
    return sdk


def patch_mingw(src):
    # GCC's LTO reader cannot consume MinGW bigobj COFF. Slim LTO objects
    # don't need bigobj. Guard Qt's inline-assembler macros across merged TUs.
    targets = src / "cmake/QtInternalTargets.cmake"
    targets.write_text(targets.read_text().replace(
        "target_compile_options(PlatformCommonInternal INTERFACE -Wa,-mbig-obj)",
        "# bigobj disabled for slim MinGW LTO objects"))
    simd = src / "src/corelib/global/qsimd_p.h"
    content = simd.read_text()
    if "qt_mingw_avx_macros" not in content:
        content = content.replace('".macro vmovapd args:vararg\\n"',
            '".ifndef qt_mingw_avx_macros\\n" " .set qt_mingw_avx_macros, 1\\n" ".macro vmovapd args:vararg\\n"')
        content = re.sub(r'("    vmovdqu64 \\\\args\\n"\s*"\.endm\\n")', r'\1 ".endif\\n"', content)
        simd.write_text(content)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version", choices=FFMPEG, default="6.11.2")
    parser.add_argument("--seed", type=Path, required=True)
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--install", type=Path, required=True)
    parser.add_argument("--parallel", type=int, default=2)
    parser.add_argument("--toolchain", choices=("msvc", "mingw", "native"), default="native")
    parser.add_argument("--arch", choices=("x64", "arm64"), default="x64")
    args = parser.parse_args()
    args.seed, args.work, args.install = (p.resolve() for p in (args.seed, args.work, args.install))
    if args.install == args.seed:
        parser.error("The LTO installation must be separate from the seed kit")
    (args.install / "lto-build.json").unlink(missing_ok=True)
    args.work.mkdir(parents=True, exist_ok=True)
    sdk = ffmpeg_sdk(args)
    # Avoid picking up prebuilt Qt libraries or plugins while building modules.
    for name in ("CMAKE_PREFIX_PATH", "Qt6_DIR", "QTDIR", "QT_ROOT_DIR", "QT_PLUGIN_PATH", "QT_QPA_PLATFORM_PLUGIN_PATH"):
        os.environ.pop(name, None)
    os.environ["PATH"] = str(args.install / "bin") + os.pathsep + os.pathsep.join(
        p for p in os.environ["PATH"].split(os.pathsep) if Path(p).resolve() != args.seed / "bin")
    manifest = json.loads((SCRIPTS / "qt-source-hashes.json").read_text())[args.version]
    common = ["-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release", "-DBUILD_SHARED_LIBS=ON",
              "-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON", "-DFEATURE_optimize_size=ON",
              "-DQT_BUILD_TESTS=OFF", "-DQT_BUILD_EXAMPLES=OFF", "-DQT_BUILD_BENCHMARKS=OFF",
              f"-DCMAKE_INSTALL_PREFIX={args.install.as_posix()}", f"-DCMAKE_PREFIX_PATH={args.install.as_posix()}"]
    if sys.platform == "darwin":
        # Match Qt's supported deployment baseline instead of inheriting the
        # runner's OS version, where legacy capture APIs are already unavailable.
        # CMake's IPO flags cover C/C++; include Qt's Objective-C sources too.
        common += ["-DCMAKE_OSX_DEPLOYMENT_TARGET=13.0", "-DCMAKE_OBJC_FLAGS=-flto=thin",
                   "-DCMAKE_OBJCXX_FLAGS=-flto=thin"]
    if args.toolchain == "msvc":
        common += ["-DCMAKE_C_COMPILER=cl", "-DCMAKE_CXX_COMPILER=cl"]
    elif args.toolchain == "mingw":
        avx_assembler = f'-Wa,"{(SCRIPTS / "qt-mingw-avx.s").as_posix()}"'
        common += ["-DCMAKE_C_COMPILER=gcc", "-DCMAKE_CXX_COMPILER=g++", "-DCMAKE_CXX_FLAGS=-fno-declone-ctor-dtor",
                   # Qt's top-level assembler workaround for GCC's Windows AVX
                   # stack alignment must be present in every generated function's
                   # assembler unit, including each separate LTO partition.
                   f"-DCMAKE_EXE_LINKER_FLAGS={avx_assembler}",
                   f"-DCMAKE_SHARED_LINKER_FLAGS={avx_assembler}",
                   f"-DCMAKE_MODULE_LINKER_FLAGS={avx_assembler}"]
    records = {}
    for module, checksum in manifest.items():
        archive = f"{module}-everywhere-src-{args.version}.tar.xz"
        series = ".".join(args.version.split(".")[:2])
        src = source(args.work, archive,
                     f"https://download.qt.io/official_releases/qt/{series}/{args.version}/submodules/{archive}",
                     checksum, archive.removesuffix(".tar.xz"))
        build = args.work / f"{module}-build"
        options = common.copy()
        if module == "qtbase":
            options += ["-DFEATURE_sql=OFF", "-DFEATURE_printsupport=OFF"]
            if sys.platform.startswith("linux"):
                # Desktop X11 playback does not need Qt Quick's embedded EGLFS
                # screen-capture integration (an implicit dependency in Qt 6.8).
                options += ["-DFEATURE_eglfs=OFF"]
            if args.toolchain == "mingw":
                patch_mingw(src)
                options += [f"-DCMAKE_PROJECT_QtBase_INCLUDE={(SCRIPTS / 'qt-mingw-lto.cmake').as_posix()}"]
        if module == "qtmultimedia":
            options += [f"-DFFMPEG_DIR={sdk.as_posix()}", "-DQT_DEPLOY_FFMPEG=ON", "-DFEATURE_ffmpeg=ON",
                        "-DFEATURE_spatialaudio=OFF",
                        "-DCMAKE_DISABLE_FIND_PACKAGE_Qt6Qml=ON", "-DCMAKE_DISABLE_FIND_PACKAGE_Qt6Quick=ON"]
        run("cmake", "-S", src, "-B", build, *options)
        cache = (build / "CMakeCache.txt").read_text()
        if not re.search(r"^CMAKE_INTERPROCEDURAL_OPTIMIZATION:[^=]+=(ON|TRUE|1)$", cache, re.M):
            raise RuntimeError(f"LTO was not enabled for {module}")
        if module == "qtbase" and not re.search(r"^QT_FEATURE_ltcg:INTERNAL=(ON|TRUE|1)$", cache, re.M):
            raise RuntimeError("Qt did not enable its LTCG feature")
        ninja = (build / "build.ninja").read_text()
        if not re.search(r"(-flto|[-/]GL)([= ;\n]|$)", ninja):
            raise RuntimeError(f"Missing compiler LTO flags for {module}")
        run("cmake", "--build", build, "--parallel", args.parallel)
        run("cmake", "--install", build, "--strip")
        records[module] = {"sha256": checksum, "lto": True, "optimize_size": True}
    # Cached installs retain evidence of the checked source build configuration.
    (args.install / "lto-build.json").write_text(json.dumps({"qt": args.version, "modules": records}, indent=2))


if __name__ == "__main__":
    main()
