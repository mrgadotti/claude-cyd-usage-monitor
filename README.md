# claude-usage

Standalone Claude Code usage monitor for the ESP32 Cheap Yellow Display
(ESP32-2432S028R, ILI9341 240x320). It joins Wi-Fi and shows two bars, the
5-hour **session** and the **weekly** usage of your Claude plan, each with the
local time it resets. No PC is needed once it is flashed.

## How it works

- Connects to Wi-Fi and syncs the clock over NTP (timezone `-03`, see `TZ_BRAZIL` in `src/main.cpp`).
- Every 60 s calls `GET https://api.anthropic.com/api/oauth/usage` with an OAuth token and draws the bars.
- Refreshes its own access token shortly before it expires and stores the rotated tokens in NVS.
- Bars turn orange at 70% and red at 90%. The footer shows `updated HH:MM:SS` or the last error.

> The usage endpoint is not officially documented and may change. TLS certificates are not verified (`setInsecure()`).

## Requirements

- A CYD (ESP32-2432S028R) with an ILI9341 panel. For ST7789 boards (CYD2USB) change the TFT flags in `platformio.ini`.
- [PlatformIO](https://platformio.org/) (`pio` CLI)
- Python 3 (standard library only)
- A Claude subscription account (the usage limits come from the plan)

## Build and flash

1. Create the credentials file (see below). The build fails without `src/secrets.h`.
2. Build and flash:

```bash
pio run                # compile
pio run -t erase       # optional: wipe stored tokens when you change accounts
pio run -t upload      # flash
pio device monitor     # serial log at 115200
```

## Getting the token

The board needs its **own** OAuth login. Do not copy the PC's `~/.claude/.credentials.json`:
OAuth refresh tokens rotate, so two devices sharing one would invalidate each other.

```bash
python3 tools/device_login.py
```

The script:

1. asks for your Wi-Fi SSID and password,
2. prints an authorization URL, which is the same flow as `claude /login`,
3. asks you to open it, authorize, and paste the code shown (`code#state`),
4. exchanges the code for tokens and writes everything to `src/secrets.h`.

Then flash with `pio run -t erase && pio run -t upload`.

To revoke the board's access, remove its session in your Claude account settings.

## Secrets and git

`src/secrets.h` holds the Wi-Fi password and OAuth tokens and is listed in `.gitignore`.
Never commit it. `src/secrets.h.example` shows the expected shape.

## Troubleshooting

| Footer / symptom | Meaning |
|---|---|
| `usage error 401` | Token rejected; run `tools/device_login.py` again and reflash with `-t erase` |
| `refresh error 400` | Refresh token is no longer valid (e.g. shared with another device); log in again. Run `pio device monitor` to see the server's reason |
| `connecting wifi` stuck | Wrong SSID/password, or a 5 GHz-only network (ESP32 needs 2.4 GHz) |
| `syncing time` stuck | NTP unreachable |
| Blank or negative screen | Wrong panel driver; see `platformio.ini` (`ILI9341_2_DRIVER`, `TFT_INVERSION_ON`) |
| Upside-down image | Change `setRotation(1)` to `3` |
| `403 error code: 1010` from the script | Cloudflare blocked the default Python User-Agent; the script sets one |

## Layout

```
platformio.ini            board, TFT_eSPI flags, libraries
src/main.cpp              firmware
src/secrets.h.example     credentials template
tools/device_login.py     one-time OAuth login, generates src/secrets.h
```
