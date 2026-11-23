# QSO-Zero — notes for anyone (human or AI) working on this repo

## Radios
- **Every radio, existing and future, must be put in its USB-Digital / DATA
  mode for all digital modes (FT8, FT4, JS8, WSPR).** In
  `src/rig/radios.c` set `data_mode = "PKTUSB"` (Hamlib's name for USB-D,
  U-DIG, DATA-U, PKT-U).
- If a radio has no data mode, it must run **USB at the widest passband it
  accepts** (typically 3 kHz). `set_data_mode()` in `src/core/app.c` does
  this automatically: PKTUSB at 3 kHz, read back, else USB trying 3000 →
  2900 → 2800 → 2700 → 2400 Hz → radio default.
- Only use plain `"USB"` as `data_mode` for a radio known to misbehave when
  asked for PKTUSB, and say why in a comment.
- New radios also need: CAT serial hint (`@ifNN` if CAT isn't the first USB
  interface), sound-card hint, band coverage, and a test case in
  `tests/test_devices.c` using the real `/dev/serial/by-id` and
  `/proc/asound/{cards,pcm}` from the device.

## Keyboard (Cardputer Zero)
- The built-in keypad (TCA8418) sends each **Sym** symbol as its own key
  code (e.g. 43 = `^`, 53 = `(`, 92 = `/`), defined by the stock keymap
  `/usr/share/keymaps/tca8418_keypad_m5stack_keymap.map`. `src/ui/input.c`
  reads that file (fallback: the same 32 entries built in) and applies it
  **only to the built-in keypad** - USB keyboards keep the US layout.
- Never bind shortcuts to `;` `,` `.` `/` `` ` `` or other symbol key codes:
  on the Zero they are Sym symbols. Arrows are **fn + F / X / Z / C**
  (real KEY_UP/DOWN/LEFT/RIGHT), PgUp/PgDn are **fn + L / M**, F1 is
  **fn + 1**, and there is a real **esc** key and a **ctrl** key.
- Test keymap: `tests/data/tca8418_keypad_m5stack_keymap.map` (from a device).

## Working with the owner
- Changes are built and tested locally first. Make an arm64 test package
  (`0.x.y~testN`) for the device, and **do not push to GitHub until the
  owner has tested it and says to push.**
- Pushing a new `version` in `app-builder.json` makes CI publish a GitHub
  Release automatically.
- **Never put a chat or session link in commits, PRs, release notes or files**
  (no `Claude-Session:` / `claude.ai/code/session_…` lines). The owner wants
  the conversation kept private. A plain `Co-Authored-By:` line is fine.
