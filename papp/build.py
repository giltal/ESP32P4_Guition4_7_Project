#!/usr/bin/env python3
"""Guition 4.3 Demo — the teaching demo as a launcher app (.papp).

    python papp/build.py        -> build/guition43/guition43.papp

Run it with the ESP-IDF Python so the riscv32-esp-elf toolchain under ~/.espressif is found:
    C:\\Users\\<you>\\.espressif\\python_env\\idf5.5_py3.12_env\\Scripts\\python.exe papp\\build.py

The PAPP SDK lives in the launcher checkout: PAPP_LAUNCHER, or the default sibling path below.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
LAUNCHER = os.environ.get("PAPP_LAUNCHER", r"C:\ESPIDFprojects\ESP32_P4_Launcher")

sdk = os.path.join(LAUNCHER, "papp-sdk", "tools")
if not os.path.isdir(sdk):
    sys.exit(f"PAPP SDK not found at {sdk}\nSet PAPP_LAUNCHER to your ESP32_P4_Launcher checkout.")
sys.path.insert(0, sdk)
import papp_build  # noqa: E402

NAME = "guition43"

papp_build.build(
    NAME,
    [os.path.join(HERE, "main.c")],
    libc=True,                                   # printf, fopen, snprintf, math
    out_dir=os.path.join(ROOT, "build", NAME),
    icon_dir=HERE,
    version_dir=HERE,
)
