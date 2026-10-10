# Palm HotSync and the RS232 cradle

FujiNet can be the desktop side of a Palm OS HotSync. A Palm that syncs with it gets the apps and
databases queued on the SD card, and its databases are backed up there. The same Palm can also run
apps that talk to FujiNet over FujiBus, like any RS232 host. This note is about how one serial line
carries both.

## Pieces

| Piece | Where | Job |
|---|---|---|
| Protocol stack | `lib/hotsync/` (`Slp`, `PadpTransport`, `NetSyncTransport`, `Dlp*`, `HotSyncSession`) | SLP/PADP/CMP and NetSync framing, DLP commands, one sync session; depends only on `global_types.h` |
| `HotSyncService` | `lib/hotsync/HotSyncService.*` | Its own thread: TCP listeners, cradle windows, running sessions |
| `HotSyncSharedCradle` | `lib/hotsync/HotSyncSharedCradle.h` | What the service needs from a cradle on a line the bus owns |
| `HotSyncBusCradle` | `lib/hotsync/HotSyncSharedCradle.h` | Header-only adapter from a bus's cradle class to `HotSyncSharedCradle`, so `lib/hotsync` depends on no bus |
| `rs232HotSync` | `lib/device/rs232/rs232HotSync.*` | The cradle on the RS232 bus: byte buffers both ways, the baud rate it wants, whether a host is active. Std-only, not a `virtualDevice` |
| `systemBus` | `lib/bus/rs232/rs232.*` | Owns the port; lends the line to the cradle and does all the I/O while it is lent |

A sync can arrive four ways: network HotSync (TCP 14238), serial-over-TCP from POSE or
CloudpilotEmu (TCP 6416), a cradle on a FujiNet-PC host serial device, and a cradle on the RS232
bus line. Only the last one shares anything with the bus.

## The problem

A HotSync opens at 9600 baud. The bus runs FujiBus at 115200, and a 9600-baud byte read at 115200
is mostly a line break the UART drops, so the bus cannot see a HotSync start reliably at its own
rate. Someone has to listen at 9600 from time to time, and while they do, FujiBus is off the line.

The first version had the HotSync thread take the bus's `IOChannel` between commands, change its
baud rate and talk to the Palm directly. It was turned down in review (#1824): nothing outside the
bus may touch the bus port.

## The design

The bus keeps the port. The cradle is an object that the bus *lends the line to*, the way the bus
already hands non-FujiBus bytes to `rs232Modem` and the whole loop to `rs232CPM`.

```
 HotSync thread        HotSyncBusCradle -> rs232HotSync          systemBus::service() (main loop)
 --------------        --------------------------------          ---------------------------------
 host_active()? ------------->  packets / stray bytes  <------  bus_packet_handled(), bus_stray_bytes()
 claim() -------------------->  wanted = 9600
                                                       <------  line_wanted() == 9600:
                                                                flush last reply, set 9600
                                served = 9600          <------  line_served(9600)
 read()/write() ------------->  in / out buffers       <----->  port bytes in, buffered bytes out
 set_baud_rate(57600) ------->  wanted = 57600         <------  set 57600, line_served(57600)
 release() ------------------>  wanted = 0             <------  restore 115200, drop noise,
                                                                line_served(0); FujiBus again
```

- **The bus does all port I/O.** While `rs232HotSync::line_wanted()` is non-zero, `service()`
  skips FujiBus: it sets the requested rate, moves bytes from the port into the cradle's input
  buffer and from its output buffer to the port, flushes, and reports the rate it served. When the
  cradle releases the line it restores `_rs232Baud`, drops what arrived at the cradle's rate, and
  goes back to reading packets. It never lends the line over BoIP.
- **The cradle is passive.** `rs232HotSync` holds the buffers and the wanted rate behind one mutex
  and condition variable. Its calls block the HotSync thread until the bus has acted: `claim()`
  until the line is at 9600, `write()` until the bytes are on the wire (as a UART write and flush
  would), `set_baud_rate()` until the bus has switched. It takes no FujiBus commands and is not in
  the daisy chain; `systemBus::setHotSyncCradle()` attaches it, and `main.cpp` wraps it in a
  `HotSyncBusCradle<rs232HotSync>` for the service.
- **The service keeps the timing.** Every 1.5 s it asks `host_active()`. If no host has sent
  FujiBus for a minute, it claims the line, listens 1.5 s for a WAKEUP, and releases it. A Palm
  repeats its WAKEUP for longer than one 3 s cycle, and a Palm app repeats a FujiBus request it got
  no answer to. Bytes that are not FujiBus, arriving with no packets, end the minute early: that is
  what a HotSync start looks like at 115200. Once a WAKEUP is answered the session runs to the end,
  including the CMP switch to a faster rate.
- **A Palm running an app cannot HotSync**, so while packets flow the cradle never claims the
  line, and FujiBus traffic is not interrupted.

## Using it

```ini
[HotSync]
enabled=1
user=FujiNet          ; given to a device that has never synced
backup=flagged        ; none | flagged | all
netsync_port=14238    ; 0 turns it off
emulator_port=6416    ; 0 turns it off
serial_port=bus       ; RS232 builds: the cradle shares the bus line
```

An SD card is required: `/palm/install/` holds files to install on the next sync,
`/palm/installed/` the ones already installed, `/palm/backup/<user>/` the backups. The cradle goes
on the DB-9 through a null modem and needs no hardware flow control. `serial_port=bus` does nothing
over BoIP: the bus never lends its line there, so no cradle is created.

## Testing

- `tests/HotSyncTests.cpp` (`hotsync_tests`) drives the protocol stack and sessions with frames
  captured from a Palm OS 3.3 device and with a fake device.
- The hand-off between the HotSync thread and the bus has no unit test; it needs the bus. On
  hardware, check a full sync, then that a Palm app's FujiBus requests work again afterwards.

## Limits

- Backups are one way; there are no record-sync conduits (Memo, Address, Date Book).
- Installs and backups hold a whole database in RAM.
- While the line is lent (up to 1.5 s per window, or a whole sync), FujiBus requests are not
  answered.
