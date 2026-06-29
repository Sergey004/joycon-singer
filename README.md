# This project has been permanently closed 

# 🎵 JoyCon Singer

Plays MIDI files through **Joy-Con / Pro Controller HD Rumble** motors.  
Port of [SteamHapticsSinger](https://github.com/CrazyCritic89/SteamHapticsSinger) for Nintendo Switch.

---

## Project structure

```
joycon-singer/
├── Makefile.switch         ← Switch build (devkitPro)
├── Makefile.pc             ← PC build (Linux/macOS)
├── main_pc.cpp            ← PC version (hidapi over Bluetooth)
├── main_switch.cpp         ← Switch homebrew (libnx)
├── minimidi.h              ← Minimal MIDI parser (no deps, shared)
├── rumble.h                ← Joy-Con HD Rumble encoding (shared)
└── .github/workflows/
    └── build.yml           ← GitHub Actions: auto-build + release
```

---

## Switch version (CFW / Homebrew)

### Requirements

- Nintendo Switch with CFW (Atmosphere recommended)
- devkitPro + devkitA64 + libnx

### Setup (local build)

**Linux / macOS:**
```bash
# 1. Install devkitPro
# Follow: https://devkitpro.org/wiki/Getting_Started
# or use the pacman method:
sudo dkp-pacman -S switch-dev

# 2. Set environment
export DEVKITPRO=/opt/devkitpro
source /opt/devkitpro/devkit-env.sh   # or re-login

# 3. Build
make -f Makefile.switch -j$(nproc)
```

**Windows (MSYS2 / devkitPro shell):**
```bash
# Open: Start → devkitPro → MSYS2
pacman -S switch-dev    # if not installed yet
cd /path/to/joycon-singer
make -f Makefile.switch
```

**Docker (no local install needed):**
```bash
docker run --rm -v "$(pwd):/project" -w /project \
    devkitpro/devkita64:latest \
    make -f Makefile.switch -j$(nproc)
```

### Install on Switch

1. Copy `joycon-singer.nro` to `sdmc:/switch/joycon-singer/`
2. Put your `.mid` files in `sdmc:/switch/joycon-singer/`
3. Launch via hbmenu

### Controls (will be rewritten bc on New TUI)

| Button | Action |
|--------|--------|
| `A` | Play / Pause |
| `B` | Back to file list |
| `D-pad Up` | Next song |
| `D-pad Down` | Previous song |
| `HOME` | Exit |

---

## PC version (Joy-Con via Bluetooth)

### Requirements

- Joy-Con or Pro Controller paired via Bluetooth  
- `libhidapi-dev` (Linux) or `brew install hidapi` (macOS)

### Linux udev rule (needed to access HID without root)

Create `/etc/udev/rules.d/70-nintendo.rules`:
```
SUBSYSTEM=="hidraw", ATTRS{idVendor}=="057e", MODE="0666"
```
Then: `sudo udevadm control --reload-rules && sudo udevadm trigger`

### Build & run

```bash
make -f Makefile.pc -j$(nproc)
./joycon-singer song.mid
./joycon-singer -p -a 0.8 song.mid    # repeat, amplitude 0.8
```

Options:
```
-p         Repeat song
-a AMP     Amplitude 0.0–1.0 (default: 0.7)
-l         Force Joy-Con L only
-r         Force Joy-Con R only
-b         Force Joy-Con L+R pair
-P         Force Pro Controller
```

### MIDI channel mapping

| MIDI Channel | Motor |
|---|---|
| 0 | Right (Joy-Con R or Pro right) |
| 1 | Left  (Joy-Con L or Pro left)  |
| ... |
| 15 | Right (Joy-Con R or Pro right) |
| 16 | Left  (Joy-Con L or Pro left)  | 

---

## GitHub Actions (auto-build)

Push to `main` → artifact is uploaded automatically.  
Tag a release → NRO zip is attached to GitHub Release.

```bash
git tag v1.0.0
git push origin v1.0.0
```

The workflow uses the official `devkitpro/devkita64:latest` Docker image —
no setup needed on the CI runner.

---

## Notes

- Frequency range: 41–626 Hz (Joy-Con LF band)
- Amplitude is clamped to 0.8 to protect the LRA motor
- MIDI channels > 1 are silently ignored (only ch 0 and 1 are used, 16 ch not tested only have 1 pair of Joy-Cons)
- Switch 2 is compatible, on PC of course (same Joy-Con protocol, also untested)
