# Palm HotSync and the cradle line

FujiNet can be the desktop side of a Palm OS HotSync. A Palm that syncs with it gets the apps and
databases queued on the SD card, and its databases are backed up there. The same Palm can also run
apps that talk to FujiNet over FujiBus. This note is about how one cradle line carries both.

## Pieces

| Piece | Where | Job |
|---|---|---|
| Protocol stack | `lib/hotsync/` (`Slp`, `PadpTransport`, `NetSyncTransport`, `Dlp*`, `HotSyncSession`) | SLP/PADP/CMP and NetSync framing, DLP commands, one sync session; depends only on `global_types.h` |
| `HotSyncService` | `lib/hotsync/HotSyncService.*` | Its own thread: TCP listeners, cradles, running sessions |
| `PalmCradle` | `lib/hardware/PalmCradle.h` | What a line a Palm sits on knows about it: what the Palm has the line open for, and the rate a HotSync opens at |
| `UARTCradle`, `PalmUSBChannel` | `lib/hardware/` | The serial cradle (a `UARTChannel`) and the USB cradle (Visor, ESP32-S3 as USB host) |
| `systemBus` | `lib/bus/rs232/rs232.*` | Owns the line; decides when it carries HotSync, and does all its I/O |
| `CradleHandoff` | `lib/bus/rs232/CradleHandoff.*` | Carries a HotSync the bus handed over to and from the service's thread. Standard C++ only |
| `HotSyncBusCradle` | `lib/hotsync/HotSyncBusCradle.h` | What the service sees of it, and the adapter `HotSyncBusCradleOf<CradleHandoff>`, so `lib/hotsync` depends on no bus |

A sync can arrive four ways: network HotSync (TCP 14238), serial-over-TCP from POSE or
CloudpilotEmu (TCP 6416), a cradle on a FujiNet-PC host serial device, and a cradle on the RS232
bus line. Only the last one shares anything with the bus.

## The problem

The Palm opens the cradle line for two different things. A Palm app talks FujiBus, like any host.
The HotSync app talks SLP/PADP/CMP, whose frames start with `BE EF ED`, and on a serial cradle it
opens at 9600 baud, whatever rate the bus runs FujiBus at. A 9600-baud byte read at 115200 is
garbage, so the bus can't hear a serial HotSync start at its own rate.

Earlier attempts gave the HotSync thread the bus's port between commands (turned down in #1824:
nothing outside the bus may touch its port), and then had the HotSync thread decide when to borrow
the line (which still moved bus management out of the bus). Here the bus is in charge of the bus.

## The design

**A cradle reports; the bus decides; the service syncs.**

- **The cradle** is the bus's own channel, with `PalmCradle` beside `IOChannel`. It says what the
  Palm has the line open for (`palmUse()`: nothing, an app, HotSync, or unknown) and what rate a
  HotSync opens at (`hotsyncBaudrate()`):

  | Cradle | `palmUse()` | `hotsyncBaudrate()` |
  |---|---|---|
  | `UARTCradle` (serial cradle, DB-9 through a null modem) | always unknown: an app and HotSync open the serial port the same way | 9600 |
  | `PalmUSBChannel` (Visor USB cradle) | from the Visor's connection info: two generic ports for an app, a HotSync port for HotSync | 0: no rate |

- **The bus** runs the line in `service()`, in one of three modes:

  ```
  FUJIBUS ---- cradle says HotSync (USB), or BE EF ED between FujiBus frames ----> HOTSYNC
     |  ^                                                                            |
     |  '-- window over (1.5 s) --.                                                 |
     |                           LISTENING (at 9600) --- BE EF ED ------------------>|
     '-- serial cradle: bytes that aren't FujiBus,                                   |
         or a minute without FujiBus; 1.5 s after the last window                    |
                                                                                     |
  FUJIBUS <-- service finished (or didn't pick it up in 5 s): bus rate, drop noise --'
  ```

  In HOTSYNC mode it moves the Palm's bytes into the `CradleHandoff`, sends what the service
  queued, and switches the rate when the service asks (CMP negotiates a faster one), once the bytes
  queued before the request are out. It never watches for HotSync over BoIP, or on a line to a
  Fujiversal cartridge: those aren't cradles.

- **The service** checks `HotSyncBusCradle::accept()` on each pass. When the bus has handed a
  HotSync over, it waits up to 4 s for a WAKEUP (the bus may hand over mid-frame; the Palm repeats
  it), answers it, runs the session, and calls `finish()`. It never claims, schedules or touches
  the line.

A Palm running an app can't HotSync, so while FujiBus packets flow a serial cradle is never
switched to 9600, and a Palm app's requests are not interrupted. A Palm app repeats a FujiBus
request it got no answer to, which covers a request that lands in a listening window.

## Using it

```ini
[HotSync]
enabled=1
user=FujiNet          ; given to a device that has never synced
backup=flagged        ; none | flagged | all
netsync_port=14238    ; 0 turns it off
emulator_port=6416    ; 0 turns it off
serial_port=bus       ; RS232 builds: the cradle is the bus line
```

An SD card is required: `/palm/install/` holds files to install on the next sync,
`/palm/installed/` the ones already installed, `/palm/backup/<user>/` the backups. A serial cradle
goes on the DB-9 through a null modem and needs no hardware flow control; the USB cradle plugs into
the ESP32-S3's OTG port on the `fujiversal-palm` build. `serial_port=bus` does nothing over BoIP.

## Testing

- `tests/HotSyncTests.cpp` (`hotsync_tests`) drives the protocol stack and sessions with frames
  captured from a Palm OS 3.3 device and with a fake device.
- `tests/CradleHandoffTests.cpp` (`cradle_handoff_tests`) checks the handoff against a fake bus
  thread, including that bytes written before a rate change go out before it.
- On hardware, check a full sync, then that a Palm app's FujiBus requests work again afterwards.

## Limits

- Backups are one way; there are no record-sync conduits (Memo, Address, Date Book).
- Installs and backups hold a whole database in RAM.
- During a listening window (1.5 s) or a whole sync, FujiBus requests are not answered.
