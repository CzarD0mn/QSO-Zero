# QSO-Zero packages

Built from fdeaae51bc6b183da7d6a92090ee83bd3b8db645 on 2026-11-28 03:13 UTC.

| file | sha256 |
|---|---|
| qso-zero_0.2.9_arm64.deb | 7558e4320fdb105e… |
| qso-zero_0.2.9_armhf.deb | 95b7a618e400f37c… |

On the Cardputer Zero: `dpkg --print-architecture` tells you which one you need (arm64 or armhf), then
`sudo apt install ./qso-zero_<version>_<arch>.deb`

DSP test results on each ARM architecture: `test-results-arm64.txt`, `test-results-armhf.txt`.
