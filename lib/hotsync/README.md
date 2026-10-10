# lib/hotsync

The desktop side of a Palm OS HotSync: a device that syncs with FujiNet gets the apps and databases
queued for it on the SD card, and its databases are backed up there.

## Layout
| File | Defines |
|---|---|
| `HotSyncLink.h` | `HotSyncLink`, the raw byte link a transport runs over (UART, socket, test fixture) |
| `Slp.h`, `Slp.cpp` | Serial Link Protocol framing and CRC |
| `PadpTransport.h`, `PadpTransport.cpp` | PADP fragments, ACK/retry, and the CMP handshake |
| `NetSyncTransport.h`, `NetSyncTransport.cpp` | NetSync framing and handshake |
| `DlpTransport.h` | `DlpTransport`, the request/response interface both transports implement |
| `Dlp.h`, `Dlp.cpp` | Desktop Link Protocol request and response encoding |
| `DlpClient.h`, `DlpClient.cpp` | typed DLP commands |
| `PalmDatabase.h`, `PalmDatabase.cpp` | `.pdb`/`.prc` files |
| `HotSyncSession.h`, `HotSyncSession.cpp` | one sync: identify, install, back up, stamp user info |
| `HotSyncStorage.h` | `HotSyncStorage`, the storage a session reads and writes |
| `HotSyncFsStorage.h`, `HotSyncFsStorage.cpp` | `HotSyncStorage` on a `FileSystem`, under `/palm` |
| `HotSyncLinks.h`, `HotSyncLinks.cpp` | `HotSyncLink` over `fnTcpClient` and over an `IOChannel`, and `HotSyncCradleLink`, the settle, deadline and count any serial cradle needs |
| `HotSyncSharedCradle.h` | `HotSyncSharedCradle`, a cradle on a line the bus owns and lends for each HotSync window, and `HotSyncBusCradle`, the adapter from a bus's cradle class |
| `HotSyncService.h`, `HotSyncService.cpp` | `HotSyncService`, the thread that listens and runs sessions |

## How it fits
- `src/main.cpp` starts one `HotSyncService` after the bus when the `[HotSync]` section of
  fnconfig.ini has `enabled=1` and an SD card is mounted. It serves network HotSync (TCP 14238),
  serial-over-TCP for POSE and CloudpilotEmu (TCP 6416), and a serial cradle: on FujiNet-PC the
  host device named by `serial_port`, on RS232 builds `serial_port=bus`, the bus line itself.
- On the bus line, `rs232HotSync` in [lib/device/rs232/](../device/rs232/) is the cradle;
  `HotSyncBusCradle<rs232HotSync>` adapts it to `HotSyncSharedCradle`, so this library depends on no
  bus. The bus never lends its line over BoIP, so `serial_port=bus` does nothing there. A HotSync opens at 9600 baud, which the bus cannot read at its own rate, so
  while no host has sent FujiBus for a minute the service claims the line for 1.5 s windows and
  listens for a WAKEUP; bytes that are not FujiBus end that minute early.
- On the SD card, `/palm/install/` holds files to install on the next sync, `/palm/installed/`
  the ones already installed, and `/palm/backup/<user>/` the backups.
- Everything up to `HotSyncSession` depends only on `include/global_types.h`;
  `tests/HotSyncTests.cpp` drives it with frames captured from a Palm OS 3.3 device and with a fake
  device.
- Design and the bus hand-off: `docs/hotsync.md`.
- Ported from [palm-sync](https://github.com/jichu4n/palm-sync) 0.2.1 (Apache-2.0, copyright
  Chuan Ji; see `LICENSE`); each header names the palm-sync source it follows.

## Build
ESP: every board, through the `lib/hotsync` glob in `src/CMakeLists.txt`; idle unless enabled.
PC: listed in `fujinet_pc.cmake`; `tests/CMakeLists.txt` also builds `hotsync_tests`.

## Notes
- Backups are one way; there are no record-sync conduits (Memo, Address, Date Book).
- Installs and backups hold a whole database in RAM.
