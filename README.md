# Winter Desktop

A lightweight, native Windows winter desktop overlay written in C++20. After your PC sits idle, snow drifts around your active windows, contours smoothly along rounded window edges and the taskbar, gets cleared by red snowplows, and can be scooped into snowballs or stacked into snowmen—while occasional penguin groups stroll across your taskbar.

Built directly on the Win32 API and GDI+ with zero runtime dependencies—just a single standalone `.exe` using roughly **10–18 MB of RAM**.

## Demo

<!-- Drag & drop your .mp4 into the GitHub web editor or a blank Issue draft, then replace the line below with the generated URL -->
https://github.com/user-attachments/assets/c2fba59a-2668-4f11-a9ff-000e0695ea72

## Features

* **Configurable Idle Timer:** Snowfall and ambient events stay quiet while you work and begin automatically once your system has been idle (defaults to **2 minutes**). Pick from quick tray presets (`Immediate`, `30s`, `1m`, `2m`, `5m`, `10m`) or set any **Custom** duration in minutes & seconds—automatically saved across reboots.
* **3-Level Blizzard Intensity:** Switch between **Light** (gentle flurries), **Moderate** (brisk crosswinds), and **Severe** (heavy wind-streaked snow with rapid accumulation) straight from the system tray—saved automatically across reboots.
* **Distraction-Free Overlay:** Falling snowflakes and Santa's sleigh clip cleanly around your actively focused window. Maximized apps, fullscreen videos, and games pause and hide the overlay completely (~0% CPU).
* **Rounded Window Corners & Sliding Snow Dust:** Fine 4px snowbanks follow Windows 11 curved window corners and gently shed sliding snow dust off the rounded edges, with strict Z-order precedence so background windows never spawn ghost snow.
* **Window-Hopping & Taskbar Snowplows:** A red snowplow with glowing headlights pushes a growing snow mound across snowy windows and drops onto lower surfaces—plus an independent plow clears the taskbar and sends snowmen flying.
* **Auto-Aligning Snowmen & Snowdust Collapse:** Scoop snow from any snowbank (or `Shift + Click` anywhere) to throw splatting snowballs. Dropping a snowball onto another within **±30°** of vertical smoothly glides it into center alignment and completes a 2-ball snowman with a **top hat, red scarf, coal eyes, carrot nose, smile, and stick branch arms**. Trying to stack a 3rd snowball on top—or dropping snowballs/snowmen off a window onto the taskbar—collapses them into swirling snowdust (window-to-window drops stay intact).
* **Taskbar Penguin Parade:** Groups of 4–5 waddling penguins periodically stroll across the taskbar in either direction, kicking up tiny snow puffs and hopping over approaching snowplows.
* **Physics Spill on Move/Close:** Moving, resizing, minimizing, or closing a snowy window spills its snow downward onto the windows or taskbar below.
* **Santa Sleigh Flyby & Low-End Friendly:** Santa and four reindeer occasionally glide across the top of the screen with a stardust trail. Includes **20 FPS Eco Mode** (with adaptive flake scaling per weather level) and a native **Start with Windows** toggle.

## Download & Install (No Coding Required)

1. Go to the **[Releases](https://github.com/probalkar/winter-desktop/releases)** page.
2. Download **`WinterDesktop.exe`** from the latest release.
3. Move `WinterDesktop.exe` to a permanent folder (for example, `C:\Apps\WinterDesktop\`) and double-click it to run.
4. *(Optional)* Right-click the **snowflake icon** in your Windows system tray (bottom-right) to customize your **Idle Timer**, adjust **Blizzard Intensity**, or enable **Start with Windows**.

> **Note on Windows SmartScreen:** Because this is a free, unsigned open-source app, Windows may show a *"Windows protected your PC"* popup on first launch. Click **More info → Run anyway** (or build from source below).

## Controls & System Tray Menu

| Action | How to Do It |
| :--- | :--- |
| **Scoop a snowball** | Click and drag from any snowbank on a window edge or the taskbar |
| **Throw a snowball** | Release while moving the mouse quickly (fast hits splat on impact) |
| **Build a snowman** | Drop a snowball onto another within ±30° of vertical—it auto-aligns! |
| **Destroy a snowman** | Try stacking a 3rd snowball on top, or drop it from a window onto the taskbar |
| **Spawn snowball anywhere** | Hold `Shift` + click and drag anywhere on the desktop |
| **Open settings & actions** | Right-click the snowflake icon in the system tray |

**Tray Menu Options:**
* **Idle Timer (...)** — Choose an idle delay preset (`Immediate`, `30s`, `1m`, `2m`, `5m`, `10m`) or click **Custom...** to set any duration (saved in Registry).
* **Blizzard Intensity (...)** — Switch between `Light`, `Moderate`, and `Severe` weather conditions (saved in Registry).
* **Force Snowfall (Skip Idle)** — Temporarily triggers snowfall right away without changing your saved idle timer.
* **Plow Top Window / Plow Taskbar Now** — Summons a red snowplow on demand.
* **Summon Penguins / Summon Santa Sleigh** — Triggers a taskbar penguin stroll or Santa flyby immediately.
* **Eco Mode (20 FPS)** — Reduces frame rate and particle count for low-spec laptops.
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

Distributed under the [MIT License](LICENSE).
