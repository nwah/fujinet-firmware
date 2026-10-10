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
| `HotSyncBusCradle.h` | `HotSyncBusCradle`, a cradle on a line the bus owns, which hands each HotSync over; `HotSyncBusCradleOf`, the adapter from a bus's handoff class |
| `HotSyncService.h`, `HotSyncService.cpp` | `HotSyncService`, the thread that listens and runs sessions |

## How it fits
- `src/main.cpp` starts one `HotSyncService` after the bus when the `[HotSync]` section of
  fnconfig.ini has `enabled=1` and an SD card is mounted. It serves network HotSync (TCP 14238),
  serial-over-TCP for POSE and CloudpilotEmu (TCP 6416), and a serial cradle: on FujiNet-PC the
  host device named by `serial_port`, and on RS232 builds `serial_port=bus`, the bus line itself.
- On the bus line, the bus decides when the line carries a HotSync and does its I/O; it hands
  each one over through `CradleHandoff` in [lib/bus/rs232/](../bus/rs232/), which
  `HotSyncBusCradleOf<CradleHandoff>` adapts, so this library depends on no bus. Design:
  [docs/hotsync.md](../../docs/hotsync.md).
- On the SD card, `/palm/install/` holds files to install on the next sync, `/palm/installed/`
  the ones already installed, and `/palm/backup/<user>/` the backups.
- Everything up to `HotSyncSession` depends only on `include/global_types.h`;
  `tests/HotSyncTests.cpp` drives it with frames captured from a Palm OS 3.3 device and with a fake
  device.
- Ported from [palm-sync](https://github.com/jichu4n/palm-sync) 0.2.1 (Apache-2.0, copyright
  Chuan Ji; see `LICENSE`); each header names the palm-sync source it follows.

## Build
ESP: every board, through the `lib/hotsync` glob in `src/CMakeLists.txt`; idle unless enabled.
PC: listed in `fujinet_pc.cmake`; `tests/CMakeLists.txt` also builds `hotsync_tests`.

## Notes
- Backups are one way; there are no record-sync conduits (Memo, Address, Date Book).
- Installs and backups hold a whole database in RAM.
