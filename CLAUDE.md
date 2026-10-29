# claude-usage

ESP32 CYD (ESP32-2432S028R, ILI9341 240x320, landscape) firmware, PlatformIO + Arduino. It polls the Claude OAuth usage endpoint over Wi-Fi and draws session (5h) and weekly bars with reset times.

## Commands

- `pio run` build, `pio run -t upload` flash, `pio run -t erase` wipe flash (including stored tokens)
- `pio device monitor` needs an interactive terminal; from scripts read `/dev/ttyUSB0` with pyserial instead
- `python3 tools/device_login.py` OAuth login for the board; writes `src/secrets.h`

## Rules

- `src/secrets.h` holds the Wi-Fi password and OAuth tokens. It is git-ignored: never commit, print, or paste its contents. Only `src/secrets.h.example` is tracked.
- Never reuse the PC's `~/.claude/.credentials.json` for the board. Refresh tokens rotate, so sharing one chain logs out the other device. The board gets its own login via `tools/device_login.py`.
- All code, comments, identifiers and commit messages are in English. No AI co-author trailers on commits.
- Use `cyd-dev` MCP (`cyd_pinout`, `cyd_gotchas`) before touching pins or TFT flags. Do not hand-edit TFT_eSPI setup outside `platformio.ini` build flags.

## Gotchas

- Read HTTP bodies with `http.getString()` before parsing. Parsing `getStream()` directly breaks on chunked responses and silently yielded 0% values.
- Cloudflare returns `403 error code: 1010` for the default Python User-Agent; send `User-Agent: claude-code/2.0` (script and firmware both do).
- The access token is seeded from `SEED_*` only when NVS is empty. After changing the seed, run `pio run -t erase` before flashing.
- The OAuth endpoints and client id were taken from the installed Claude Code binary and are undocumented; they may change.
- Layout is hard-coded for 320x240 (`setRotation(1)`); the status line sits at y=215.
