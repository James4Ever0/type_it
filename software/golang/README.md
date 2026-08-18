# Type It Go — Desktop Demo (Ebiten)

A Go/[Ebiten](https://ebitengine.org/) simulation of the macro keyboard: it
renders the LED display and knob, reads the same `./profiles` text files as
the firmware, and copies the selected candidate to the system clipboard.

## Requirements

- Go 1.22+
- Linux: X11 development headers (Ebiten dependency):

  ```bash
  sudo apt-get install -y libxrandr-dev libxcursor-dev libxinerama-dev libxi-dev libgl1-mesa-dev libxxf86vm-dev
  ```

## Build & run

```bash
./compile.sh     # checks X11 headers, go mod tidy, builds ./type_it
./type_it        # or: go run .
```

`./test.sh` runs `go vet`, builds, and — when `xvfb-run` is available —
smoke-runs the app headless for 2 seconds.

## Controls

| Input | Action |
|-------|--------|
| ↑ / W | Scroll up |
| ↓ / S | Scroll down |
| → / Enter / Space | Open profile / copy candidate |
| ← / Esc / Backspace | Go back |
| Q | Save & quit |
| Mouse wheel | Scroll |
| Drag on the knob | Scroll |
| Click on the knob | Press (long press = back) |

## Profiles

One profile = one `.txt` file in `./profiles/`. The file name is the profile
name; each non-empty, trimmed line is a candidate; duplicate lines are
collapsed.

```text
# profiles/work.txt
知道了
ok
这个做不了
```

Profiles and candidates are sorted most-recently-used first; the ordering is
persisted to `./usage.json` and restored on start.

## Files

- `main.go` — the entire demo (single file)
- `compile.sh`, `test.sh` — build / smoke-test helpers
- `profiles/` — user profile content
- `usage.json` — runtime state, regenerated automatically
