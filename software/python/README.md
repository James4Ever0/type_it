# Type It Python — Desktop Demo (pygame)

A [pygame](https://www.pygame.org/) simulation of the macro keyboard: it
renders the LED display and knob, reads the same `./profiles` text files as
the firmware, and copies the selected candidate to the clipboard.

## Requirements

- Python 3.10+
- Linux/X11: `wmctrl` (optional — keeps the window always-on-top)

## Install & run

```bash
pip install -r requirements.txt
python main.py
```

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

Same format as the Go demo and the firmware: one `.txt` file per profile in
`./profiles/`, each non-empty line is a candidate. Recency ordering **and**
the last-viewed UI state are persisted to `./usage.json` and restored on
start.

## Notes

- A CJK-capable system font is picked automatically for headers and list
  items, with a DejaVu fallback.
- On Linux the window is raised above others via `wmctrl` when available.

## Files

- `main.py` — the entire demo (single file)
- `requirements.txt` — pygame, pyperclip
- `profiles/` — user profile content
- `usage.json` — runtime state, regenerated automatically
