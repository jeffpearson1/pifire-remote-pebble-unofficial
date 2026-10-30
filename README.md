# PiFire Remote for Pebble

Watch app that shows a PiFire grill's status and controls it.

## Watch controls

| Screen | UP | SELECT | DOWN |
| --- | --- | --- | --- |
| Status | Refresh now | Action menu | Set Hold temp |
| Action menu | — | Run action | — |
| Hold temp | +5° (hold to repeat) | Start Hold | −5° (hold to repeat) |

Action menu: Startup, Smoke, Hold…, Smoke Plus (toggle), Shutdown, Monitor, Reignite, Emergency Stop.

Ending a cook:
- **Shutdown** is the normal way to finish. PiFire runs its shutdown cycle, keeping the fan
  going to burn off the pellets in the firepot. Press SELECT on the confirm screen.
- **Emergency Stop** sends PiFire's Stop mode, which cuts everything and skips the shutdown
  cycle. Leftover pellets can smolder and cause problems on the next startup, so it's only for
  emergencies. It's last in the menu, and its red confirm screen needs SELECT *held* for 1 s.
  A short press does nothing, and it vibrates when it fires.

The status screen polls every 10 s and shows the mode (color-coded), grill temp,
the Hold setpoint or Smoke+, up to 3 food probes with their notification targets,
and lid-open or critical-error warnings.

## Setup

1. Requires PiFire v1.7+ (uses `/api/current` and the `/api/set/...` commands).
2. Build in WSL (Pebble SDK 4.33.1 via `uv tool install pebble-tool`):
   ```
   wsl bash build-wsl.sh .
   ```
   This copies the project to `~/pifire-remote` in WSL, builds it, and puts
   `pifire-remote.pbw` back in this folder. Install it with
   `pebble install --phone <phone-ip>` from `~/pifire-remote`, or sideload the `.pbw`.
3. In the Pebble phone app, open the app's settings and enter your PiFire address
   (e.g. `http://pifire.local` or `http://192.168.1.50`).

The phone makes the HTTP calls, so it must be able to reach the PiFire (same Wi-Fi or a VPN).

Targets:

| Platform | Watches | Screen | Layout |
| --- | --- | --- | --- |
| aplite | Pebble, Pebble Steel | 144×168 B/W | small |
| basalt | Pebble Time, Time Steel | 144×168 color | small |
| chalk | Pebble Time Round | 180×180 round | small |
| diorite | Pebble 2 | 144×168 B/W | small |
| flint | Pebble 2 Duo | 144×168 B/W | small |
| emery | Pebble Time 2 | 200×228 color | large |
| gabbro | Pebble Round 2 | 260×260 round | large |

Screens 200px and wider use the large layout: bigger header, Hold line, status and confirm
text, and a 49px Roboto grill temp. Round screens scale their side and bottom margins
to the display size.
Settings use `@rebble/clay`; the original `pebble-clay` doesn't support flint or gabbro.

## Testing without a grill

```
wsl python3 tools/mock_pifire.py 8080      # fake PiFire, leave running
wsl bash tools/emu-test.sh basalt          # point emulator at it, build, install
```

`emu-test.sh` writes the PiFire address straight into the emulator's saved settings,
so the settings page doesn't need to be opened. Pass a second argument to use a real
PiFire instead: `tools/emu-test.sh chalk http://192.168.1.50`.

## Layout

- `src/c/main.c`: watch UI and AppMessage handling
- `src/pkjs/index.js`: phone-side bridge to the PiFire REST API
- `src/pkjs/config.js`: settings page (Clay)
- `tools/mock_pifire.py`: fake PiFire API for emulator testing
- `tools/emu-test.sh`: seed emulator settings, build, install
- `tools/screens.sh`: screenshot every screen on given platforms into `screens/` (mock only:
  `wsl bash tools/screens.sh emery gabbro`)
