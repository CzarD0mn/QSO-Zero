# QSO-Zero

A lightweight FT8 / FT4 / JS8 / WSPR terminal for the **M5Stack Cardputer Zero**, built for field ops with QRP radios. Plain C, no Qt, no X, no SDL: it draws straight to the Linux framebuffer and runs as a single ~200 KB binary.

* **FT8**: decode, encode and auto-sequence (CQ → report → R-report → RR73 → 73), ADIF logging to `~/qso-zero/qso-zero.adi` (folder created automatically)
* **FT4**: the same messages and auto-sequencer on 7.5-second cycles, at the WSJT-X FT4 frequencies, logged as MFSK/FT4 in ADIF
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

### Adding a radio

Radio profiles live in `src/rig/radios.c`. Each new radio needs:
* **Hamlib model:** with a fallback model for older Hamlib if needed.
* **CAT serial hint, default port and baud.**
* **Sound card hint.**
* **Band coverage.**
* **Data mode (required):** digital modes must run in the radio's **USB-Digital / DATA mode**, so set `data_mode` to `"PKTUSB"` (Hamlib's name for USB-D, U-DIG, DATA-U or PKT-U). If the radio has no data mode, QSO-Zero automatically falls back to **USB at the widest passband the radio accepts** (it tries 3 kHz first, usually the maximum). Use plain `"USB"` only for a radio known to misbehave when asked for `PKTUSB`.

Then add the radio's serial ports and `/proc/asound` listing to `tests/test_devices.c`, and check `qso-zero-setup --log` on the real radio for the `[rig] mode …` line.

## Install on the Cardputer Zero

**From a package (easiest).** Each push to `main` builds `.deb` packages for 64-bit (`arm64`) and 32-bit (`armhf`) Raspberry Pi OS. They're published on the [`packages` branch](../../tree/packages), and each new version also gets a GitHub [Release](../../releases/latest) (`vX.Y.Z`) with plain `X.Y.Z` packages to download.

```sh
dpkg --print-architecture                      # arm64 or armhf
sudo apt install ./qso-zero_<version>_<arch>.deb   # pulls in rigctld (libhamlib-utils)
qso-zero-setup --check                         # what it finds: screen, keyboard, ports, sound cards
qso-zero-setup                                 # callsign + radio, config file, group permissions
qso-zero --nohw                                # try the interface without a radio
sudo systemctl enable --now qso-zero@$USER     # optional: start at boot instead of from the launcher
```

**From your computer over SSH** (the current czdev has no deploy command):

```sh
scp qso-zero_<version>_arm64.deb pi@<device-ip>:/tmp/
ssh pi@<device-ip> 'sudo apt install -y /tmp/qso-zero_<version>_arm64.deb && sudo systemctl restart APPLaunch.service'
```

Restarting `APPLaunch.service` makes the launcher show the new app straight away. M5's `scripts/deploy.sh` in the AppBuilder repository does the same copy-and-restart, but installs with `dpkg -i`, which doesn't fetch dependencies. Run `sudo apt install libhamlib-utils` on the device once first if you use it.

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

These are for the **Cardputer Zero's own keyboard**, following M5Stack's Quick Start. **fn**, **sym** and **shift** can be held, tapped once (next key only) or tapped twice (locked). The arrows are on the **fn** layer: **F** up, **X** down, **Z** left, **C** right. Symbols in blue are on the **sym** layer, exactly as the legends show (QSO-Zero reads the Zero's own keymap, `/usr/share/keymaps/tca8418_keypad_m5stack_keymap.map`, so the sym layer types what's printed). A USB keyboard plugged into the Zero works with its normal layout.

| Key | Action |
|---|---|
| type + `Enter` | send (FT8/FT4 message, JS8 text) |
| `Enter` on empty line | reply to the selected decode |
| `fn`+`F` / `fn`+`X` (↑ ↓) | select decode; move in Setup |
| `fn`+`Z` / `fn`+`C` (← →) | move RX/TX offset 10 Hz (Shift: 100 Hz); change a Setup value |
| `Tab` | FT8 → FT4 → JS8 → WSPR (the decode list starts fresh in the new mode) |
| `fn`+`L` / `fn`+`M` (PgUp/PgDn), or `ctrl`+`N` / `ctrl`+`P` | next / previous band (skips bands the radio can't do) |
| `esc` | short press: back / Setup menu. **Hold 1 s: exit to the launcher** |
| `ctrl`+`Q` | CQ |
| `ctrl`+`E` | JS8 heartbeat |
| `ctrl`+`X` | **halt TX now** |
| `ctrl`+`U` | tune carrier 5 s |
| `ctrl`+`W` | WSPR beacon on/off |
| `ctrl`+`T` | tools (GPS/time, rig, audio, battery) |
| `fn`+`1` (F1) | help |

Commands (type `/` on the sym layer): `/band 40`, `/mode js8` (ft8, ft4, js8, wspr), `/sub turbo`, `/freq 7.078`, `/call K1ABC`, `/grid FN42`, `/level 40`, `/pwr 23`, `/tune 5`, `/auto on|off`, `/qsy on|off`, `/ghost 80|40|20|off`, `/cqmod POTA`, `/aprs CALL msg`, `/sms NUMBER msg`, `/email ADDRESS msg`, `/aprspos`, `/port /dev/ttyUSB0`, `/ain plughw:CARD=Device`, `/aout …`, `/hback on|off`, `/info TEXT`, `/status TEXT`, `/save`, `/quit`.

### Battery

QSO-Zero reads the Cardputer Zero's battery every 30 seconds from Linux's power-supply interface (`/sys/class/power_supply`, the supply with type "Battery"), the same source M5Stack's own apps use.

* **Header:** the percentage and a small battery icon, next to the GPS/SYS clock chip. The icon is green above 50%, yellow from 21–50% and red at 20% and below. A notch in the icon means it's charging.
* **Tools (Ctrl+T):** voltage and current (when the battery reports them), the drain rate in %/hour, and the time left (or time to full while charging). Time left comes from the battery's current reading when it has one. Otherwise it's estimated from how fast the percentage drops, which needs about 5 minutes of data ("estimating" until then).
* **Warnings:** a status message appears at 15% and again at 5%, with the estimated minutes left.
* **No battery:** nothing is shown when no battery is reported (on a bench supply, for example).

### Setup menu

ESC opens Setup. Options for the current mode come right under **Mode**, and options for the other modes are hidden:
* **JS8:** JS8 speed, auto-reply, heartbeat ACK, INFO and STATUS text, and the GHOSTNET and APRS sections
* **FT8:** CQ modifier
* **WSPR:** WSPR receive, power and TX %

Change the Mode line (←/→) and the list updates. Everything else (band, auto frequency, callsign, radio, ports, audio and so on) is always shown.

### FT8 CQ modifier (CQ POTA, SOTA, QRP, DX)

Setup › **CQ modifier** (FT8 only). Use ←/→ to step through none, DX, POTA, SOTA and QRP, or press ENTER to type your own (`/cqmod WWFF`). Ctrl+Q then calls `CQ POTA KN4CRD EM73`, and the auto-sequencer runs the QSO as usual.

FT8 allows a modifier of **1 to 4 letters (A–Z) or exactly 3 digits** (`CQ 290`, used for calling on a frequency). The limit isn't about the 15-second timing. FT8 always sends exactly 77 bits, and the modifier rides inside the 28-bit field that normally holds the first callsign, which only has room for those forms. Anything else (5 letters, letters mixed with digits, `/`) is refused with a message, because it can't go out as a standard CQ.

### APRS over JS8 (Setup › APRS, JS8 only)

JS8 can get messages and position reports into the APRS network from places with no APRS coverage. You send to the **@APRSIS** group. Any JS8Call station that hears you and has APRS reporting turned on relays it to APRS-IS (as `YOURCALL>APJ8CL,qAS,RELAYCALL:…`).

| Setup item | Command | Sends |
|---|---|---|
| Send position | `/aprspos` | `@APRSIS GRID EM73TU`, your grid (use Grid from GPS for 6 characters) |
| Message a station | `/aprs W1AW-7 QRV 7.078` | `@APRSIS CMD :W1AW-7   :QRV 7.078{12` |
| Text a phone (SMS) | `/sms 5551234567 Arrived OK` | `@APRSIS CMD :SMS      :@5551234567 ARRIVED OK{13` |
| Email | `/email me@example.com On summit` | `@APRSIS CMD :EMAIL-2  :ME@EXAMPLE.COM ON SUMMIT{14` |

QSO-Zero formats these the way APRS needs them:
* the addressee is padded to exactly 9 characters
* each message gets a `{number}`
* the message is limited to 67 characters, with no `|` `~` `{`
* JS8's 16-bit checksum is added, so a relay checks the message before passing it on. Corrupted copies are dropped.

Things to know:
* **Someone has to relay it.** It only reaches APRS if a JS8Call station with APRS spotting on decodes it. There's no acknowledgement back over JS8, so check aprs.fi or ask for a reply.
* **SMS** goes through the NA7Q **SMS** gateway (the old SMSGTE is gone). The phone number must be opted in at aprs.wiki first. You can set up an alias there so your number isn't sent over the air (`/sms MIKE …`).
* **Email** goes through **EMAIL-2**. It's meant for interactive use, not unattended or automatic messages.
* **Upper case:** JS8 sends upper case only, so texts and emails arrive in capitals.

### GHOSTNET

Setup has a **GHOSTNET** section for running JS8 on the GHOSTNET frequencies (dial frequency, USB-D):

| Band | GHOSTNET JS8 |
|---|---|
| 80 m | 3.575 MHz |
| 40 m | 7.107 MHz |
| 20 m | 14.107 MHz |

* Press ENTER on one of the three frequencies to switch to JS8 there and tune the radio. This works even with Auto frequency off.
* The **GHOSTNET** line switches the mode on or off. Turning it on keeps you on 80, 40 or 20 m if you're already there, otherwise it goes to 40 m. Turning it off returns to the normal JS8 frequency on the band.
* **GHOSTNET isn't locked like the other modes.** Choosing it tunes to the net frequency, then the VFO is yours. Nets sometimes move off frequency to avoid interference, so you can tune anywhere and QSO-Zero won't pull you back. Turning the VFO onto another band also doesn't retune.
* QSO-Zero remembers where you tuned on each band. If you switch to FT8 and back, or step bands, you return to where the net was. Setup shows "now 7.110" when you're off the default. Choosing a GHOSTNET frequency in Setup again (or `/ghost 40`) resets to the default.
* While GHOSTNET is on and you're in JS8, the band keys cycle only 80 → 40 → 20 m. A **GN** chip (or **GHOSTNET** on an external display) shows in the header.
* FT8 and WSPR keep their usual frequencies. Switching back to JS8 returns to GHOSTNET.
* `/ghost 40` (or `80`, `20`, `off`) does the same from the keyboard, and `ghostnet = 1` in the config makes it stick. "USB-D" is the radio's data mode (see below).

### Choosing the radio, CAT port and sound cards

Setup (ESC) has four pickers: **Radio**, **CAT port**, **Audio in** and **Audio out**. Press ENTER on one to see what's plugged in right now. Use ↑/↓ to choose and ENTER to use it. The **<< Rescan >>** row at the top (or R) looks again after you plug something in. Then use **Save settings** to keep the choice. Changes take effect straight away without restarting. The radio reconnects in the background, and you can't make changes while transmitting.

* **CAT port on auto** finds the radio by its USB ID. If the radio doesn't answer on the first likely port, QSO-Zero tries its other serial interfaces. The X6100/X6200 have two (a CH342 "USB Dual Serial"), and CAT is on the second one (`if02`, usually `/dev/ttyACM1`).
* **CAT port** lists every `/dev/ttyUSB#` and `/dev/ttyACM#` with what it is (for example, *Silicon Labs CP2102N…* for a Digirig, *QRP Labs QMX* for a QMX). QSO-Zero saves your choice by its USB ID (`/dev/serial/by-id/…`), so it keeps finding the right radio even if the `ttyUSB` numbers change between boots. **auto** uses the radio's usual port.
* **Audio in** lists only devices that can **record** (radio receive audio comes in here). **Audio out** lists only devices that can **play** (transmit audio goes out here). Each entry is one ALSA device, `plughw:CARD=<name>,DEV=<n>`, with USB devices first, then the built-in codec (ES8389) and HDMI (output only).
* **auto** (the default for both) picks the radio's **USB** sound card: the QMX's own codec, or the C-Media codec in the Digirig for the G90 and FT-891. It never picks the Cardputer Zero's built-in codec or HDMI. With no USB sound card plugged in, it says so instead of falling back to the built-in audio.
* A radio's USB sound chip shows up in both lists, like the separate microphone and speaker entries other computers show: **Device (input)** under Audio in and **Device (output)** under Audio out (the X6200 and X6100 report a C-Media "USB Audio Device"). Linux sees one USB card with a record side and a play side, and auto pairs them up.
* If a saved device isn't plugged in at startup, QSO-Zero uses auto and tells you.
* **In level / Out level** (under Audio in and Audio out) set the sound card's own recording and playback levels, the same controls `alsamixer` shows (on the X6100/X6200 and Digirig, "Mic" and "Speaker"). Use ←/→ to change them in 5% steps. The first press on a muted control turns it on. They're saved with **Save settings** and reapplied every time the audio connects, so there's no need for `alsactl store` or root. **TX audio level** is QSO-Zero's own transmit drive, applied on top of Out level. Commands: `/inlevel 60`, `/outlevel 45`.
* **Permissions:** sound devices and their levels need the `audio` group, and CAT needs `dialout`. Installing with `sudo apt install` adds the user who ran it to `dialout`, `audio`, `video` and `input` (reboot to apply). `qso-zero-setup` does the same for any other user. A level that can't be changed shows **n/a**, and the hint line says why.
* **Unreliable USB audio:** some radios' USB sound chips (the X6200's C-Media chip on the Cardputer Zero, for example) sometimes refuse to start with USB error -71. QSO-Zero retries the open, and restarts the input if samples stop arriving for 3 seconds. Some chips also won't play while recording ("Device or resource busy"), so QSO-Zero pauses the input for the length of each transmission. Tools shows the input rate, errors and restart count.
* **Setup › Rescan audio** (or `/rescan`) looks for sound devices again and reconnects. Use it after plugging the radio in while QSO-Zero is running.
* The Tools screen (Ctrl+T) shows the port and cards in use. The same settings are `rig_device`, `audio_in` and `audio_out` in the config file.

### Automatic frequency (auto QSY)

QSO-Zero drives the radio's VFO to the usual frequency for the current mode on each band (FT8 and WSPR: the WSJT-X defaults; JS8: the JS8Call defaults).

| Band | FT8 | FT4 | JS8 | WSPR (dial) |
|---|---|---|---|---|
| 160 m | 1.840 | — | 1.842 | 1.8366 |
| 80 m | 3.573 | 3.575 | 3.578 | 3.5686 |
| 60 m | 5.357 | — | 5.357 | 5.2872 |
| 40 m | 7.074 | 7.0475 | 7.078 | 7.0386 |
| 30 m | 10.136 | 10.140 | 10.130 | 10.1387 |
| 20 m | 14.074 | 14.080 | 14.078 | 14.0956 |
| 17 m | 18.100 | 18.104 | 18.104 | 18.1046 |
| 15 m | 21.074 | 21.140 | 21.078 | 21.0946 |
| 12 m | 24.915 | 24.919 | 24.922 | 24.9246 |
| 10 m | 28.074 | 28.180 | 28.078 | 28.1246 |
| 6 m | 50.313 | 50.318 | 50.318 | 50.293 |

* **At start-up** QSO-Zero opens in **WSPR on 20 m** (14.0956 MHz) and tunes the radio there, so you can beacon (Ctrl+W) and check how far you're being heard before you start calling. Switch modes with Tab and bands with the band keys as usual.
* **Changing mode or band** retunes the radio.
* **USB-D for every digital mode:** on connect, on every mode change (even with Auto frequency off) and on every frequency change, QSO-Zero puts the radio in its data mode with a 3 kHz passband: Hamlib `PKTUSB`, which is USB-D / U-DIG on the Xiegus and DATA-U on the FT-891. It reads the mode back to confirm. A radio without a data mode (such as the G90) refuses it, and QSO-Zero falls back to USB and says so once. The log records which mode was used.
* **Turning the VFO knob onto another band** is noticed within about 2 s, and the radio is moved to the mode's frequency on that band. Tuning within a band is left alone.
* **Bands per radio:**
  * QMX: 80–10 m
  * G90: 160–10 m
  * QMX+, X6100, X6200, FT-891: 160–6 m

  Band stepping skips the rest. If the radio refuses a frequency (a 5-band QMX, for example), QSO-Zero shows "radio won't tune" and skips that band for the session.
* **During transmit** the frequency never changes, and any queued messages are cleared on a band change.
* **Turning it off:** use ESC › **Auto frequency**, `/qsy off`, or `auto_qsy = 0` in the config. With it off, mode changes leave the VFO alone and the band display just follows the radio. Picking a band explicitly still tunes the radio.
* **Limiting the band list:** set `bands = 40,30,20` in the config, for example.

## FT8 / FT4 QSOs

Start a QSO by selecting a station's line and pressing **ENTER**, by calling CQ (Ctrl+Q), or by typing a standard message to a station you've heard, such as `W1AW KN4CRD EM73`. After that, the auto-sequencer answers each reply (report → R-report → RR73 → 73), logs the QSO, and repeats an unanswered message up to 6 times (10 for CQ). It transmits in the slot opposite the station's. A message addressed to you is only answered while a QSO is in progress. Every sequencer step is in the activity log under `[seq]`, including why a message was ignored.

## FT8 / FT4 decode timing

Each FT8/FT4 cycle is decoded twice:
* **Early pass:** right after an on-time transmission ends, at 13.35 s for FT8 (signals end at 13.14 s) and 5.65 s for FT4 (signals end at 5.54 s). Most decodes, and any reply the auto-sequencer queues, appear about 0.65 s sooner (FT8) or 0.25 s sooner (FT4).
* **Final pass:** at 14.0 s (FT8) or 5.9 s (FT4), over a slightly longer window. It catches stations whose clocks run a little late and adds only stations the early pass didn't already show.

Each cycle's timing is in the activity log, for example `[decode] FT8 21:45:00.0  early pass at +13.35 s: 9 in 410 ms (on screen +13.76 s)  final at +14.00 s: 1 more in 405 ms`. If the audio hasn't arrived in time for the early pass, the log says so and the final pass still runs. Switching modes (for example from WSPR) starts decoding in the new mode straight away.

## Decode speed check

`qso-zero --bench` builds a busy band for each mode: 20 FT8 signals, 15 FT4, 8 for each JS8 speed and 8 WSPR. It times the same decoder calls the app makes, and compares the slowest of 3 runs with the time available before a reply would have to start. The available time is 1.5 s for FT8 and 2.1 s for FT4 (decoding starts as soon as the last symbol is in), 1.7 to 4.4 s for JS8 depending on speed, and 6 s for WSPR. Run it on the Cardputer Zero. **OK** means well within the time, **TIGHT** fits with little margin, and **TOO SLOW** means replies would go out late.

## Log file (for troubleshooting)

QSO-Zero writes what it's doing to **`~/.cache/qso-zero/qso-zero.log`**. Each start begins a new file. Only the **5 newest logs** are kept: `qso-zero.log`, then `qso-zero.log.1` (the newest older one) up to `.4`. Each file is capped at 1 MB, rotating within a run if needed, so the logs never take more than about 5 MB. Tools shows the path.

```sh
qso-zero-setup --log        # the last 150 lines
qso-zero-setup --log all    # all 5 kept logs, oldest first
```

It records:
* **Startup:** version, user and groups, config, screen and keyboard.
* **Radio:** the CAT port and Hamlib model tried, whether the radio answered, each frequency change, and PTT failures.
* **Audio:** the sound devices found, which input and output were picked, every failed open or retry with ALSA's reason, input restarts, and the In/Out levels.
* **Transmissions:** what, where, how long the audio played, and on which device.
* **Other:** GPS, wsprnet results, every status message, and once a minute a health line (rig, input samples/s, peak, errors, clock, battery).
* **Exit:** how the app ended (quit, signal or crash).

Send the output of `qso-zero-setup --log all` when reporting a problem.

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
../CardputerZero-AppBuilder/czdev publish --deb qso-zero_0.1.5_arm64.deb
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
