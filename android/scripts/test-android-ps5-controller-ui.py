#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Test AndroidPs5ControllerUi with the real bridge and both hotplug handlers.

Compiles an optimized host executable, not an APK. JNI uses the installed JDK
headers and a bounded function-table double; SDL/device lifecycle and unrelated
services are stubs. This does not qualify physical hardware or Java overlay UI.
"""
import argparse
import os
from pathlib import Path
import platform
import shutil
import subprocess
import tempfile


def java_home():
    if os.environ.get("JAVA_HOME"):
        return Path(os.environ["JAVA_HOME"])
    if platform.system() == "Darwin":
        return Path(subprocess.check_output(["/usr/libexec/java_home"], text=True).strip())
    javac = shutil.which("javac")
    if javac:
        return Path(javac).resolve().parents[1]
    raise RuntimeError("A JDK is required: set JAVA_HOME or pass --java-home")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", default="clang++")
    parser.add_argument("--java-home", type=Path)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[2]
    jdk = args.java_home or java_home()
    jni_platform = {"Darwin": "darwin", "Linux": "linux"}.get(platform.system())
    if jni_platform is None or not (jdk / "include/jni.h").is_file():
        raise RuntimeError("This host fixture requires macOS/Linux and a JDK with jni.h")
    headers = {
        "common/common.h": "#pragma once\n#include <cstddef>\n#include <cstdint>\n",
        "SDL3/SDL_system.h": "#pragma once\nvoid* SDL_GetAndroidJNIEnv();\n",
        "android/log.h": "#pragma once\n#define ANDROID_LOG_ERROR 6\n"
                         "int __android_log_print(int, const char*, const char*, ...);\n"
                         "int __android_log_write(int, const char*, const char*);\n",
        "graphics/presentation/presenter.h": "#pragma once\n#include <cstdint>\n"
                         "namespace Libs::Graphics { uint64_t GetPresentedGuestFrameCount(); }\n",
        "graphics/presentation/systemOverlay.h": "#pragma once\n#include <cstdint>\n#include <string_view>\n"
                         "namespace Libs::Graphics {\n"
                         "bool SystemImeHostReplaceText(uint64_t, std::u16string_view, uint32_t);\n"
                         "bool SystemImeHostAccept(uint64_t);\nbool SystemImeHostCancel(uint64_t);\n}\n",
    }
    with tempfile.TemporaryDirectory(prefix="lumen-AndroidPs5ControllerUi-") as directory:
        work = Path(directory)
        for name, content in headers.items():
            target = work / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(content)
        for variant in ("engine", "compatibility"):
            source = repo / f"source/gen4/gen5/{variant}/src"
            window = (source / "graphics/presentation/window/window.cpp").read_text()
            start = window.index("static void GameEventController(")
            end = window.index("\nstatic void GameEventLowMemory(", start)
            # Compile the complete, unchanged production function, not a copy of
            # its boolean expression. Unexpected structural changes fail closed.
            handler = window[start:end]
            if not handler.rstrip().endswith("}"):
                raise RuntimeError("Unable to isolate production GameEventController")
            (work / "AndroidPs5ControllerUiWindow.inc").write_text(handler)
            command = [args.compiler, "-std=c++20", "-O2", "-DNDEBUG", "-Wall", "-Wextra", "-Werror", "-pthread",
                       "-DLUMEN_PS5_ANDROID_ENGINE", "-I" + str(work), "-I" + str(source),
                       "-I" + str(repo / "source/gen4/gen5/engine/src"),
                       "-I" + str(jdk / "include"), "-I" + str(jdk / "include" / jni_platform),
                       "-I" + str(repo / "android/app/src/main/cpp"),
                       str(repo / "android/app/src/main/cpp/AndroidPs5RuntimeUi.cpp"),
                       str(repo / "android/app/src/test/cpp/AndroidPs5ControllerUiTest.cpp"),
                       "-o", str(work / "AndroidPs5ControllerUiTest")]
            subprocess.run(command, check=True)
            print(variant + ":", flush=True)
            subprocess.run([str(work / "AndroidPs5ControllerUiTest")], check=True, timeout=15)


if __name__ == "__main__":
    main()
