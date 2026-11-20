# QSO-Zero packages

Built from 74bd73babfa348b0b34f4beb08ca95c7da55993b on 2026-11-20 18:30 UTC.

| file | sha256 |
|---|---|
| qso-zero_0.1.9_arm64.deb | 40cf223270ba9595… |
| qso-zero_0.1.9_armhf.deb | 9a9bd7e67b81e397… |

On the Cardputer Zero: `dpkg --print-architecture` tells you which one you need (arm64 or armhf), then
`sudo apt install ./qso-zero_<version>_<arch>.deb`

DSP test results on each ARM architecture: `test-results-arm64.txt`, `test-results-armhf.txt`.
