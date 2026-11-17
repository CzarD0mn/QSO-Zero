# QSO-Zero

A lightweight FT8 / JS8 / WSPR terminal for the **M5Stack Cardputer Zero**, built for field ops with QRP radios. Plain C, no Qt, no X, no SDL: it draws straight to the Linux framebuffer and runs as a single ~200 KB binary.

* **FT8**: decode, encode and auto-sequence (CQ → report → R-report → RR73 → 73), ADIF logging
* **JS8**: native C port of the JS8Call modem and message layer. All four speeds (Normal/Fast/Turbo/Slow), heartbeats, CQ, directed commands, compound calls, Huffman and JSC-compressed free text, multi-frame messages
* **JS8 auto-replies** (off by default, a toggle in Setup): answers `SNR?`, `GRID?`, `INFO?`, `STATUS?`, `HEARING?`, `AGN?`, stores checksummed `MSG`s in an inbox file and ACKs them, and optionally ACKs heartbeats. Wording matches JS8Call's own replies
* **WSPR**: receive (a port of the WSJT-X `wsprd` decoder: sync search, Fano decoding, signal subtraction) with distance to each spotter, plus a type-1 beacon transmitter with %-of-slots scheduling
* **Rig control** through Hamlib `rigctld` (spawned and supervised by QSO-Zero), with PTT that drops on exit or crash
* **GPS time and grid** from the Cap LoRa-1262 / LoRa868 (ATGM336H). The app keeps its own disciplined clock, so it needs no root and no internet
* **Two layouts**: compact 320×170 for the built-in LCD, and a 640×360 canvas scaled to HDMI. It switches automatically when a monitor is plugged in

## Supported radios

| `radio =` | Radio | Hamlib model | Connection | PTT |
|---|---|---|---|---|
| `qmx` | QRP Labs QMX | 2057 (falls back to TS-480 2028) | one USB cable, CAT + audio | CAT |
| `qmxplus` | QRP Labs QMX+ | 2057 (→ 2028) | one USB cable | CAT |
| `g90` | Xiegu G90 | 3088 | Digirig: CAT + audio | CAT |
| `x6100` | Xiegu X6100 | 3087 | built-in USB | CAT |
| `x6200` | Xiegu X6200 | 3091 (→ 3087) | built-in USB | CAT |
| `ft891` | Yaesu FT-891 | 1036 | Digirig: CAT + DATA jack | RTS |

The model IDs come from Hamlib's `riglist.h`. QMX and X6200 need Hamlib 4.6 or newer. On older Hamlib, QSO-Zero automatically retries with the compatible backend.

## Install on the Cardputer Zero

**From a package (easiest).** Each push to `main` builds `.deb` packages for 64-bit (`arm64`) and 32-bit (`armhf`) Raspberry Pi OS. They're published on the [`packages` branch](../../tree/packages), and attached to a GitHub Release for each `vX.Y.Z` tag.

```sh
dpkg --print-architecture                      # arm64 or armhf
sudo apt install ./qso-zero_<version>_<arch>.deb   # pulls in rigctld (libhamlib-utils)
qso-zero-setup --check                         # what it finds: screen, keyboard, ports, sound cards
qso-zero-setup                                 # callsign + radio, config file, group permissions
qso-zero --nohw                                # try the interface without a radio
sudo systemctl enable --now qso-zero@$USER     # optional: start at boot instead of from the launcher
```

After installing, **QSO-Zero appears in the CardputerZero launcher** (APPLaunch) with its own icon. Open it from there like any other app. **Hold ESC for one second to exit back to the launcher.** A short ESC still means "back": it clears the input, then opens Setup. Exiting while transmitting stops TX first.

**From source.**

```sh
sudo apt install build-essential libhamlib-utils
git clone https://github.com/CzarD0mn/QSO-Zero.git && cd QSO-Zero
make -j4 && make test        # tests optional, ~3 min
make deb                     # or: sudo make install-service
```

The user running it needs to be in the `dialout`, `audio`, `video` and `input` groups; `qso-zero-setup` adds them.

**GPS:** the Cap's GPS is on a UART at 115200 8N1. Point `gps_device` at it (usually `/dev/serial0`; enable the UART and disable the serial console if your image hasn't). The Cap has no PPS line, so timing comes from NMEA burst arrival. That's typically ±50 ms after the median filter, well inside the ±1 s FT8/JS8 tolerance. The header chip shows **GPS** (green = locked) or **SYS** (orange = system clock).

## Keys

| Key | Action |
|---|---|
| type + `Enter` | send (FT8 message / JS8 text) |
| `Enter` on empty line | reply to the selected decode |
| `↑` `↓` (or `Ctrl` + `;` `.`) | select decode |
| `←` `→` (or `Ctrl` + `,` `/`) | move RX/TX offset 10 Hz (Shift: 100 Hz) |
| `Tab` | FT8 → JS8 → WSPR |
| `PgUp` / `PgDn`, `Ctrl+N` / `Ctrl+P` | band |
| `Esc` or `` ` `` | short press: back / Setup menu. **Hold 1 s: exit to the launcher** |
| `Ctrl+Q` | CQ |
| `Ctrl+E` | JS8 heartbeat |
| `Ctrl+X` | **halt TX now** |
| `Ctrl+U` | tune carrier 5 s |
| `Ctrl+W` | WSPR beacon on/off |
| `Ctrl+T` | tools (GPS/time, rig, audio) |
| `Ctrl+L` | force compact / large layout |
| `F1` | help |

Commands: `/band 40`, `/mode js8`, `/sub turbo`, `/freq 7.078`, `/call K1ABC`, `/grid FN42`, `/level 40`, `/pwr 23`, `/tune 5`, `/auto on|off`, `/hback on|off`, `/info TEXT`, `/status TEXT`, `/save`, `/quit`.

## wsprnet upload

With `wsprnet_upload = 1` (the default), a background checker runs **every 5 minutes**. It resolves `wsprnet.org` and opens a connection to it. If that works, every WSPR spot decoded since the last check is uploaded. If it fails, those spots are **discarded**: there's no offline backlog, so a day in the field without signal never turns into a flood of stale reports. Your callsign must be set (spots are never sent as N0CALL). `/net` forces an immediate check.

Uploads go over **HTTPS first**, with wsprnet's certificate checked against the system CA store and its hostname verified. OpenSSL (`libssl.so.3`, or `.so.1.1`) is loaded at runtime, so there's nothing extra to install. If HTTPS fails for any reason (no TLS library, port 443 blocked, handshake or certificate error, server error), the same spot is sent over **plain HTTP** instead. After one HTTPS failure in a cycle, the remaining spots go straight to HTTP. Tools shows how many went each way and the last error. The Tools screen and the large layout show online/offline status and sent/dropped counts.

## JS8 auto-replies

Turn on with **Setup → JS8 auto-reply** (or `/auto on`). When it's on, an **AUTO** chip shows in the header. QSO-Zero then answers, the same way JS8Call does:

| Query to your call | Reply |
|---|---|
| `SNR?` | `CALL SNR -08` |
| `GRID?` | `CALL GRID EM73` |
| `INFO?` / `STATUS?` | your INFO / STATUS text (no reply if the text is blank) |
| `HEARING?` | up to 4 stations heard in the last hour |
| `AGN?` | repeats your last transmission |
| `MSG …` | checks the checksum, appends to `inbox_path`, replies `ACK` |
| `QUERY MSGS` | `NO` (no store-and-forward for others) |
| `@HB` heartbeat | `CALL HEARTBEAT SNR -12`, only if **JS8 heartbeat ACK** is also on, max once per station every 15 min |

It only answers messages addressed to your callsign. It never answers while you have something queued or are transmitting, or after `Ctrl+X`. Every reply is logged on screen as `AUTO-REPLY queued: …`. Relays (`>`) and `MSG TO:` store-and-forward are deliberately not automated.

## CardputerZero launcher and App Store

QSO-Zero follows M5Stack's [AppBuilder](https://github.com/m5stack/CardputerZero-AppBuilder) conventions:

* `/usr/share/APPLaunch/applications/qso-zero.desktop` and icons in `/usr/share/APPLaunch/share/images/` put it in the launcher
* it draws on the framebuffer the launcher passes in `APPLAUNCH_LINUX_FBDEV_DEVICE`. An HDMI monitor still takes priority and gets the large layout
* hold ESC to exit, like M5's own apps
* it runs as your user, never as root; the optional service is per-user (`qso-zero@<user>`)
* [`app-builder.json`](app-builder.json) carries the store listing: summary, `Radio & Comms` category, four 320×170 screenshots, icon, GPL-3.0-or-later, share code `QSOZ`, permissions (network, additional hardware, external display) and Chinese/Japanese titles

Locally, the arm64 package passes czdev's own preflight checks (`.desktop` present, no root services, valid name), the store's install-path policy, and its metadata policy.

**Publishing** (from this repository, with the arm64 `.deb` of a tagged release):

```sh
git clone https://github.com/m5stack/CardputerZero-AppBuilder ../CardputerZero-AppBuilder
../CardputerZero-AppBuilder/czdev login
../CardputerZero-AppBuilder/czdev publish --deb qso-zero_0.1.0_arm64.deb
```

For a first release, the store wants a short video of the app running on a real device posted in the publish PR. The listing points at this repository as its source, and GPL code shared through the store should have public source, so make the repository public before publishing.

## How it was verified

All of this runs with `make test`:

* **JS8 modem is bit-exact with JS8Call:** 20,000 random frames across all submodes and frame types give identical tones from JS8Call's own `JS8::encode()` and from QSO-Zero. CRC-12 matches Boost `augmented_crc` on 100,000 frames. The LDPC tables are extracted mechanically and checked (every generator codeword satisfies every parity check).
* **JS8 decode:** 4 simultaneous signals per submode with no false decodes. Single-signal 10/10 copy down to −18 dB (Normal) and −20 dB (Slow).
* **JS8 messages:** callsign, grid and CRC-16 primitives, every frame type, and free text in all submodes round-trip identically.
* **FT8:** 4 simultaneous signals, 20/20 down to −18 dB, about 11 ms per slot on a desktop CPU.
* **WSPR:** TX symbols identical to Etherkit JTEncode. Receive: 5 stations in one cycle, all decoded with frequency to 0.1 Hz, 5/5 copy down to −28 dB. A full 2-minute cycle also decodes through the live audio ring and decode thread.
* **Keyboard:** real evdev key events fed to the input code: short ESC gives "back", holding ESC 1 s exits, the Cardputer's `` ` `` key acts as ESC, Shift+`` ` `` types `~`, and Ctrl+X still halts TX.
* **wsprnet upload:** tested against local servers in four cases: a valid HTTPS certificate (sent over HTTPS), a certificate for the wrong host (rejected, sent over HTTP, nothing leaked to the bad server), port 443 closed (sent over HTTP) and no network (spots dropped).
* **JS8 auto-replies:** 16 checks, covering every reply's wording, off/on, only-to-me, checksum rejection, inbox writes, heartbeat rate limiting and hold-off while TX is queued or halted.
* **Engine:** real-time decode through the live audio ring and UTC tagging (DT 0.00), a full auto-sequenced QSO including the ADIF entry, and a 3-frame JS8 message reassembled after passing through the modem.

**Not yet tested on real hardware.** The Cardputer Zero framebuffer and keyboard, the radios, the sound cards and the GPS UART all need a first on-air shakedown. Check the panel orientation, the device names and `tx_level` (aim for no ALC action) before a real outing.

## Known limits / next steps

* PSKReporter upload (for FT8/JS8 spots) isn't implemented; WSPR spots go to wsprnet
* JS8 relays (`>`), `MSG TO:` store-and-forward and `QUERY MSG` retrieval are not automated
* The WSPR decode takes a few seconds on the CM0 every 2 minutes; nothing else is affected
* The FT8 decoder (ft8_lib) is about 2–3 dB less sensitive than WSJT-X; adding OSD/a-priori decoding would close most of that gap
* FT4 is supported in the DSP wrapper but not yet exposed in the UI

## Licence

GPL-3.0 (the JS8 modem and message layer are ports of JS8Call). Bundled: ft8_lib (MIT, Karlis Goba YL3JG), kissfft (BSD) and wsprd (GPLv3, K1JT/K9AN, C version by VA2GKA, adapted to kissfft). `data/js8.jsc` is generated from JS8Call's dictionary by `tools/gen_jsc.py`.
