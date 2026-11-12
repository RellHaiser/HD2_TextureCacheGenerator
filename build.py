"""Builds hd2tcg.exe (x86, MSVC; the same compiler settings as the LS3DF port the encoder comes from).

    python build.py            release build into build\\hd2tcg.exe
    python build.py --debug

Needs Visual Studio with the C++ x86 tools (found with vswhere).
"""
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(ROOT, "src")
VSWHERE = r"C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
SOURCES = ["main.cpp", "texture.cpp", "image.cpp", "datafs.cpp", "model4ds.cpp", r"ls3df\igraph\s3tc.cpp"]


def msvc_env():
    path = subprocess.run([VSWHERE, "-latest", "-products", "*", "-requires",
                           "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"],
                          capture_output=True, text=True).stdout.strip()
    if not path:
        sys.exit("build: Visual Studio with the C++ x86 tools not found")
    bat = os.path.join(path, r"VC\Auxiliary\Build\vcvars32.bat")
    out = subprocess.run('"%s" >nul && set' % bat, capture_output=True, text=True, shell=True).stdout
    env = {}
    for line in out.splitlines():
        k, sep, v = line.partition("=")
        if sep:
            env[k] = v
    return env


def main():
    debug = "--debug" in sys.argv
    work = os.path.join(ROOT, "build", "debug" if debug else "release")
    os.makedirs(work, exist_ok=True)
    exe = os.path.join(ROOT, "build", "hd2tcg_d.exe" if debug else "hd2tcg.exe")
    cmd = ["cl", "/nologo", "/EHsc", "/std:c++17", "/W4", "/permissive-", "/utf-8", "/MT" + ("d" if debug else ""),
           "/D_CRT_SECURE_NO_WARNINGS", "/DWIN32_LEAN_AND_MEAN", "/DNOMINMAX",
           "/Od" if debug else "/O2", "/DNDEBUG" if not debug else "/D_DEBUG", "/Zi",
           "/I" + SRC, "/I" + os.path.join(SRC, "ls3df"),
           "/Fo" + work + "\\", "/Fd" + os.path.join(work, "hd2tcg.pdb"), "/Fe" + exe]
    cmd += [os.path.join(SRC, s) for s in SOURCES]
    cmd += ["/link", "/INCREMENTAL:NO", "/DEBUG"]
    env = msvc_env()
    path = next((v for k, v in env.items() if k.upper() == "PATH"), "")
    cmd[0] = shutil.which("cl", path=path) or sys.exit("build: cl.exe not found")
    r = subprocess.run(cmd, env=env, capture_output=True, text=True)
    text = "\n".join(l for l in (r.stdout + r.stderr).splitlines()
                     if l.strip() and l.strip() not in [os.path.basename(s) for s in SOURCES])
    if text:
        print(text)
    if r.returncode:
        sys.exit("build failed")
    print("built", exe)


if __name__ == "__main__":
    main()
