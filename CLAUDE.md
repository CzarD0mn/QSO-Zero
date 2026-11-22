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

## Working with the owner
- Changes are built and tested locally first. Make an arm64 test package
  (`0.x.y~testN`) for the device, and **do not push to GitHub until the
  owner has tested it and says to push.**
- Pushing a new `version` in `app-builder.json` makes CI publish a GitHub
  Release automatically.
