# Winter Desktop

A lightweight, native Windows winter desktop overlay written in C++20. Snow falls around your active apps, piles up on idle window edges and the taskbar, gets cleared by red snowplows, and can be scooped into throwable snowballs or stacked into snowmen.

Built directly on the Win32 API and GDI+ with zero runtime dependencies—just a single standalone `.exe` using roughly **10–18 MB of RAM**.

## Features

* **Distraction-Free Snowfall:** Snowflakes and Santa's sleigh automatically clip around your actively focused window. Maximized apps, fullscreen videos, and games pause and hide the overlay completely (~0% CPU).
* **Topmost Window & Taskbar Accumulation:** Snow settles on the top edges of idle windows (with strict Z-order precedence so hidden background windows never spawn ghost snow) and along the Windows taskbar.
* **Window-Hopping & Taskbar Snowplows:** A red snowplow with glowing headlights pushes a growing mound of snow across windows and drops onto lower surfaces—plus an independent plow sweeps the taskbar and sends any snowmen in its path flying.
* **Interactive Snowballs & Snowmen:** Scoop snow straight off any snowbank (or hold `Shift + Click` anywhere) to throw splatting snowballs, or gently stack 2–3 snowballs to create a snowman with coal eyes, a carrot nose, and a top hat.
* **Physics Spill on Move/Close:** Moving, resizing, minimizing, or closing a snowy window spills its snow downward onto the windows or taskbar underneath.
* **Idle Blizzard & Santa Flyby:** Leave your PC unattended for 3 minutes to watch snow blanket the screen, then return to watch the plow carve zig-zag tunnels to clear your view. Santa's sleigh occasionally flies across the top of the screen with a stardust trail.
* **Low-End Laptop Friendly:** Includes a **20 FPS Eco Mode** and a native **Start with Windows** toggle in the system tray.

## Download & Install (No Coding Required)

1. Go to the **[Releases](https://github.com/probalkar/winter-desktop/releases)** page.
2. Download **`WinterDesktop.exe`** from the latest release.
3. Move `WinterDesktop.exe` to a permanent folder (for example, `C:\Apps\WinterDesktop\`) and double-click it to run.
4. *(Optional)* Right-click the **snowflake icon** in your Windows system tray (bottom-right) and enable **Start with Windows**.

> **Note on Windows SmartScreen:** Because this is a free, unsigned open-source app, Windows may show a *"Windows protected your PC"* popup on first launch. Click **More info → Run anyway** (or build from source below).

## Controls & System Tray Menu

| Action | How to Do It |
| :--- | :--- |
| **Scoop a snowball** | Click and drag from any snowbank on a window edge or the taskbar |
| **Throw a snowball** | Release while moving the mouse quickly (fast hits splat on impact) |
| **Build a snowman** | Gently drop 2 or 3 snowballs on top of each other |
| **Spawn snowball anywhere** | Hold `Shift` + click and drag anywhere on the desktop |
| **Open settings & actions** | Right-click the snowflake icon in the system tray |

**Tray Menu Options:**
* **Plow Top Window** — Summons a red snowplow on the snowiest topmost window.
* **Plow Taskbar Now** — Sends a snowplow across the taskbar (and tosses any snowmen on it).
* **Test Blizzard + Clear** — Previews the full-screen blizzard and zig-zag plow clearing sequence.
* **Summon Santa Sleigh** — Triggers Santa's flyby immediately.
* **Eco Mode (20 FPS)** — Reduces frame rate and particle count for minimal CPU usage.
* **Start with Windows** — Toggles automatic startup via `HKCU\...\Run` (no admin rights needed).
* **Clear All Snow / Quit** — Resets the desktop or exits cleanly.

## Build from Source

Requires Windows 10/11, CMake 3.20+, and either **MSVC (Visual Studio Build Tools)** or **MinGW-w64 (GCC 12+)**.

**With MSVC:**
```powershell
cmake -B build -A x64
cmake --build build --config Release
.\build\Release\WinterDesktop.exe
```

**With MinGW-w64 + Ninja:**
```powershell
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
.\build\WinterDesktop.exe
```

## License

MIT License
