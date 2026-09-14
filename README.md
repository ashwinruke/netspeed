# NetSpeed
 
A small background utility written in C that shows live network activity as a
system tray icon on Windows. The icon is redrawn every second as a scrolling
sparkline, so you can see what the connection is doing without hovering over
anything.
 
**Status:** Milestones 0–5 complete. Polish and shipping (M6) in progress.
 
---
 
## Screenshots
 
### The tray icon
 
<!-- TODO: screenshot of the sparkline icon in the taskbar, zoomed if possible -->
![Sparkline icon in the taskbar](docs/screenshot-tray.png)
 
### Tooltip
 
<!-- TODO: screenshot of the hover tooltip showing Down / Up / Ping -->
![Tooltip with exact figures](docs/screenshot-tooltip.png)
 
### Interface detection
 
<!-- TODO: screenshot of `netspeed.exe --list` output -->
![Filtered interface list](docs/screenshot-list.png)
 
---
 
## What it does
 
- Draws a live sparkline into the 16×16 tray icon — download above the
  midline in green, upload below in orange, 16 seconds of history scrolling
  right to left
- Hover for exact figures: download, upload, and round-trip latency
- Detects active network interfaces automatically — Wi-Fi, Ethernet, or a
  phone hotspot, with no configuration
- Right-click menu to reset counters or exit
- Survives Explorer restarts — the icon comes back on its own
- `--list` shows which interfaces are being counted and why
---
 
## Build
 
Requires [MSYS2](https://www.msys2.org/) with the UCRT64 toolchain.
 
```
pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-make
```
 
Add `C:\msys64\ucrt64\bin` to `PATH`, then from the project root:
 
```
make           # debug build, keeps the console for printf
make release   # optimised, console hidden (-mwindows)
```
 
---
 
## Usage
 
```
netspeed.exe           # run in the tray
netspeed.exe --list    # show detected interfaces and exit
```
 
Right-click the tray icon for the menu, or double-click it to exit.
 
Windows hides new tray icons by default. To keep NetSpeed visible:
**Settings → Personalization → Taskbar → Other system tray icons → NetSpeed → On**
 
---
 
## How it works
 
### Measuring speed
 
Windows keeps a cumulative byte counter for every network interface.
`GetIfTable2()` returns the whole table; the program reads it once a second and
derives speed from the difference between consecutive readings:
 
```
speed = (bytes_now - bytes_before) / seconds_elapsed
```
 
Two details that the obvious implementation gets wrong:
 
**Elapsed time is measured, not assumed.** `Sleep(1000)` and `WM_TIMER`
guarantee only a minimum interval. Each reading is timestamped with
`GetTickCount64()` and the real elapsed time is used, so readings stay accurate
when the system is loaded.
 
**Counters can go backwards.** Switching networks or resetting an adapter
restarts its counter at zero. Since the counters are unsigned, a naive
subtraction wraps to an enormous positive number instead of going negative, so
the difference is guarded with a comparison rather than a sign check.
 
Latency uses `IcmpSendEcho` from the IP Helper API, which needs no administrator
rights. It is sampled every fifth tick rather than every tick because the call
blocks until a reply arrives or the timeout expires.
 
### Interface selection
 
Windows reports far more interfaces than a machine actually has — this one lists
54. Most are filter drivers layered on a real adapter, and each reports the
*same* bytes as the adapter beneath it. Summing them all inflates the reading
several times over.
 
Interfaces are filtered on the `MIB_IF_ROW2` status flags:
 
| Rule | Reason |
|---|---|
| `HardwareInterface` must be set | excludes virtual switches (Hyper-V, WSL2) |
| `FilterInterface` must be clear | excludes stacked driver layers that double-count |
| `OperStatus` must be `Up` | excludes disconnected adapters |
| Type must not be loopback or tunnel | excludes local traffic and VPN double-counting |
 
On a typical laptop this reduces 54 rows to one.
 
### Drawing the icon
 
The tray accepts only a 16×16 icon — there is no API for putting text in the
taskbar. So a fresh icon is generated every second: a memory DC, a colour
bitmap and a mask bitmap, the sparkline drawn with `FillRect`, then
`CreateIconIndirect` and `Shell_NotifyIcon(NIM_MODIFY)`.
 
**Why a sparkline and not digits.** The first attempt drew the speed as text.
At 16 pixels only two or three characters fit, there is no room for a unit
suffix, and encoding the unit as text colour turned out to be unreadable at a
glance. Shapes survive the resolution where glyphs do not, so the icon shows
*shape and trend* and the tooltip carries the exact numbers.
 
**Scaling is logarithmic.** Throughput spans five orders of magnitude, from a
few hundred B/s of background chatter to tens of MB/s. Scaling linearly against
a rolling peak makes everything except the peak invisible, and the meaning of
"tall" changes constantly. A fixed `log10` scale means a given bar height always
represents the same speed.
 
### The GDI handle trap
 
`CreateIconIndirect` copies the bitmaps it is given, so they must be deleted
immediately afterwards — and the icon installed on the *previous* tick must be
destroyed once the shell has taken the new one.
 
Miss either and the process leaks one GDI handle per second. Windows caps a
process at 10,000 GDI objects; at one per second the ceiling arrives in under
three hours, `CreateIconIndirect` starts returning `NULL`, and the icon silently
stops updating. No crash, no error, no message.
 
<!-- TODO: before/after screenshot of the GDI objects column in Task Manager -->
 
Task Manager's **GDI objects** column (Details tab → right-click headers →
Select columns) makes it visible: a leaking build climbs steadily, a correct one
sits flat.
 
### Surviving Explorer restarts
 
When `explorer.exe` restarts, the taskbar is rebuilt and every tray icon is
wiped — the process keeps running, but invisibly. Windows broadcasts a
`TaskbarCreated` message when the new taskbar is ready; registering for it with
`RegisterWindowMessage` and re-adding the icon on receipt fixes this.
 
This is also why the program creates a hidden top-level window rather than a
message-only (`HWND_MESSAGE`) one. Message-only windows are excluded from
broadcasts by design, so the tempting shortcut would have made this feature
impossible without restructuring.
 
---
 
## Known limitations
 
- Windows only — built entirely on Win32 APIs
- Reports **bytes**, not bits. Speed test sites report megabits;
  divide their figure by 8 to compare
- Uses binary units (1 MB = 1024² bytes), unlike most network tools
- Polls once a second, so brief spikes are averaged away
- Traffic through a VPN is counted at the physical adapter, so it reflects
  encrypted wire traffic rather than the plaintext inside the tunnel
- The latency probe runs on the UI thread and blocks for up to a second on a
  dead connection
---
 
## Roadmap
 
- [x] M0 — Toolchain, Makefile, repo
- [x] M1 — Live readout with clock-drift and counter-reset handling
- [x] M2 — Automatic interface detection, `--list`
- [x] M3 — Tray icon with live tooltip
- [x] M4 — Right-click menu, survives Explorer restarts
- [x] M5 — Sparkline drawn into the icon, no GDI leaks
- [ ] M6 — Config file, daily usage log, autostart, real icon
### Future work
 
- `--list-all` to show the full unfiltered interface table
- Move the latency probe to a worker thread so it can never block the UI
- Exclude interfaces by alias from the config file
---