# 🖥️ AutoPowerManager

**AutoPowerManager** is a lightweight Windows tray utility that dynamically adjusts your system’s power profile based on real-time activity, battery status, and active applications. Designed to run silently in the background, it intelligently balances **performance**, **power efficiency**, and **thermal comfort** — making it ideal for laptops and workstations used in mixed workloads like simulation, coding, and general productivity.

---

## ⚙️ Key Features

### 🔋 **Adaptive Power Tuning**

* Automatically switches between *Boost*, *Balanced*, and *Saver* processor profiles.
* Reacts to **CPU utilization**, **user activity**, **battery level**, and **display state**.
* Keeps your system snappy when needed, and energy-efficient when idle.

### 🧠 **Smart Context Awareness**

* Detects heavy applications (e.g., **COMSOL**, **MATLAB**, **Vivado**, **ANSYS**) and pre-boosts CPU performance.
* Lowers power draw automatically when the **display is off**, **system is locked**, or **running on battery**.

### 🎚️ **Polished Settings UI**

* Modern Win32 dialog with sliders and presets for fine-tuning:

  * Battery threshold
  * Sticky boost duration
  * Balanced/Saver residency timers
* Presets for **Recommended**, **Fast**, and **Eco** modes.
* Persistent configuration stored safely in the Windows registry.

### 🪶 **Lightweight & Non-intrusive**

* Runs as a small tray icon without background services.
* Uses less than 10 MB RAM and negligible CPU load.
* Works with all standard Windows power schemes (High Performance, Balanced, Power Saver).

---

## 🧩 How It Works

AutoPowerManager continuously samples:

* **CPU activity** (EWMA + median smoothing),
* **User input** (idle time),
* **Foreground applications**, and
* **System power events** (AC/DC source, display, session lock).

Based on these inputs, it selects a power profile:

| Activity Tier | Profile Applied | Behavior                             |
| ------------- | --------------- | ------------------------------------ |
| Active        | **Boost**       | Max CPU state, aggressive boost mode |
| Engaged       | **Balanced**    | Moderate CPU, partial unpark         |
| Idle          | **Saver**       | Minimum CPU, parking cores           |

All transitions are time-smoothed to avoid rapid toggling.

---

## 🧰 Build Instructions

1. Open the solution in **Visual Studio 2022** (x64 configuration).
2. Ensure the following SDKs and libraries are available:

   * Windows 10 or 11 SDK
   * `PowrProf.lib`, `Wtsapi32.lib`, `Comctl32.lib`, `Dwmapi.lib`
3. Compile the project (`Release x64`).
4. The resulting executable can be placed anywhere (no admin rights required).

---

## 🚀 Usage

1. Launch `AutoPowerManager.exe`.
2. A tray icon appears — right-click for options:

   * **Open Settings** → configure sliders and presets
   * **Apply Now** → force re-evaluation
   * **Exit** → quit the app
3. Settings are saved automatically and persist between sessions.

---

## 📈 Benefits

* Extends **battery life** without compromising responsiveness.
* Reduces **thermal throttling** under sustained workloads.
* Improves **system longevity** through adaptive power control.
* Great for **researchers, developers, and simulation users** running variable workloads.

---

## 🧪 Example Use Cases

* **Simulation workflows**: Automatically boosts CPU for COMSOL or MATLAB; throttles down when idle.
* **Coding sessions**: Keeps IDEs responsive but sleeps quietly when taking breaks.
* **Laptop optimization**: Seamless power management during travel or presentations.

---

## 🏗️ Architecture Overview

```
[System Events] ──► [CPU Sampler + Activity Monitor]
                           │
                           ▼
                    [Tier Decision Engine]
                           │
                           ▼
               [Power Profile Adjuster]
                           │
                           ▼
                    [Tray UI + Settings]
```

---

## 📄 License

MIT License — free for personal and commercial use.

---

## ✨ Acknowledgments

Developed to provide a practical, open-source power management utility for engineers and researchers seeking smarter system control without OEM bloatware.
