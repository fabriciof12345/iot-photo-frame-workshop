#!/usr/bin/env python3
"""Prompt for Wi-Fi credentials and store them in the ignored ESP-IDF sdkconfig."""

from __future__ import annotations

import getpass
from pathlib import Path

SDKCONFIG = Path(__file__).resolve().parents[1] / "sdkconfig"


def kconfig_string(value: str) -> str:
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'


def set_config(text: str, key: str, value: str) -> str:
    line = f"{key}={kconfig_string(value)}"
    lines = text.splitlines()
    for index, existing in enumerate(lines):
        if existing.startswith(f"{key}="):
            lines[index] = line
            break
    else:
        lines.append(line)
    return "\n".join(lines) + "\n"


def main() -> None:
    if not SDKCONFIG.exists():
        raise SystemExit("sdkconfig is missing; run 'idf.py set-target esp32s3' first")

    ssid = input("2.4 GHz Wi-Fi SSID: ").strip()
    password = getpass.getpass("Wi-Fi password (input hidden): ")

    if not 1 <= len(ssid.encode("utf-8")) <= 32:
        raise SystemExit("SSID must be between 1 and 32 bytes")
    if password and not 8 <= len(password.encode("utf-8")) <= 63:
        raise SystemExit("WPA/WPA2 password must be between 8 and 63 bytes")

    text = SDKCONFIG.read_text(encoding="utf-8")
    text = set_config(text, "CONFIG_PHOTO_FRAME_WIFI_SSID", ssid)
    text = set_config(text, "CONFIG_PHOTO_FRAME_WIFI_PASSWORD", password)
    SDKCONFIG.write_text(text, encoding="utf-8")
    SDKCONFIG.chmod(0o600)
    print("Wi-Fi credentials saved locally in ignored sdkconfig (mode 0600).")


if __name__ == "__main__":
    main()
