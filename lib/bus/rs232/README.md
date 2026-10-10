# lib/bus/rs232

Generic serial FujiNet bus for `BUILD_RS232`: SLIP-framed `FujiBusPacket` frames at 115200 baud
over UART, USB CDC-ACM or BoIP. It serves the PC/MS-DOS RS-232 boards and every `fujiversal-*`
board, where the RP2xxx cartridge is the other end of the link.

## Layout
| File | Defines |
|---|---|
| `rs232.h`, `rs232.cpp` | `virtualDevice` (devices implement `rs232_process()`) and `systemBus` (`readBusPacket()`, `writeBusPacket()`, `sendReplyPacket()`, `sendCommand()`, a per-unit network map; `nativeEOL()` is CR LF) |
| `FujiBusPacket.h`, `FujiBusPacket.cpp` | `FujiBusPacket`: SLIP encode and decode, device ID, command, `PacketParam` list and optional data; this bus's `FUJI_COMMAND_PACKET` |
| `CradleHandoff.h`, `CradleHandoff.cpp` | `CradleHandoff`: how the bus hands a HotSync it found on a Palm cradle line to the HotSync service's thread, carrying its bytes and rate changes |

## How it fits
- Selected by `BUILD_RS232` in [lib/bus/bus.h](../bus.h); devices live in
  [lib/device/rs232/](../../device/rs232/).
- `FujiBusPacket` is also the packet type of [lib/bus/mac/](../mac/) and of the fujiversal path in
  [lib/bus/drivewire/](../drivewire/); the cartridge firmware under [pico/](../../../pico/) carries
  its own copy.
- Ports come from [lib/hardware/](../../hardware/): `UARTChannel`, `ACMChannel` (USB host on
  fujiversal boards) or `BoIPChannel` (PC).

## A Palm cradle on the line
When the line is a `PalmCradle` (a UART, or the USB Palm cradle; never BoIP) and the HotSync
service is attached with `attachHotSync()`, the bus decides what the line carries and does all
its I/O:
- FujiBus, normally.
- HotSync, when the cradle reports the HotSync app (USB), or SLP frames (`BE EF ED`) arrive
  between FujiBus frames. The bus hands the HotSync over and pumps its bytes until the service
  finishes, then serves FujiBus again at its own rate.
- Listening at 9600 baud for 1.5 s windows, on a serial cradle: a HotSync opens at that rate,
  which the bus can't hear at its own. It listens once bytes that aren't FujiBus show up, or after
  a minute with no FujiBus, and leaves at least 1.5 s for FujiBus between windows.

See [docs/hotsync.md](../../../docs/hotsync.md).

## Build
ESP: globbed by `src/CMakeLists.txt`; `rs232.cpp` compiles only under `BUILD_RS232`, while
`FujiBusPacket.cpp` has no guard and is built into every target. PC: `FUJINET_TARGET=RS232` lists
both pairs and `CradleHandoff`; `tests/FujiBusPacketTests.cpp` unit-tests the framing and
`tests/CradleHandoffTests.cpp` the handoff.
