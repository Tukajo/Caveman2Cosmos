# Building and Installing Caveman2Cosmos (C2C)

This document provides instructions for compiling the game core DLL (`CvGameCoreDLL.dll`), executing unit tests, and installing or linking the mod to your Civilization IV Beyond the Sword installation.

---

## 1. Directory Placeholders Used in this Guide

| Placeholder | Description | Example Path |
|-------------|-------------|--------------|
| `<REPO_DIR>` | Root directory of the Caveman2Cosmos git repository | `C:\Code\Caveman2Cosmos` or `~/Code/Caveman2Cosmos` |
| `<CIV4_BTS_DIR>` | Civilization IV Beyond the Sword root folder (containing `Civ4BeyondSword.exe`) | `C:\Program Files (x86)\Steam\steamapps\common\Sid Meier's Civilization IV Beyond the Sword\Beyond the Sword` |
| `<CIV4_MODS_DIR>` | Beyond the Sword mods folder (`<CIV4_BTS_DIR>/Mods`) | `<CIV4_BTS_DIR>/Mods` |

---

## 2. Prerequisites

### Windows
- **Git for Windows**
- **PowerShell** (5.1 or later)
- Windows 7/10/11 with administrative privileges (for junction creation during dev setup)

### Linux (Wine)
- **Wine** (Wine 7.0+ or wine-staging / Proton)
- **MinGW / GCC (g++)** with C++11 support (for running standalone unit tests natively)
- **7-Zip** (`p7zip-full`)

---

## 3. Dependency Setup

The C2C compilation toolchain requires the Microsoft Visual C++ Toolkit 2003 compiler and Civ IV SDK dependencies. These are bundled as a self-extracting archive in `Tools/deps.exe`.

### Windows
Run:
```cmd
cd <REPO_DIR>\Tools
call InstallDeps.bat
```
This extracts the toolchain into `<REPO_DIR>\Build\deps`.

### Linux (Wine)
Extract dependencies via Wine:
```bash
cd <REPO_DIR>/Tools
wine cmd /c InstallDeps.bat
```
*(Alternatively, extract `Tools/deps.exe` manually to `<REPO_DIR>/Build/deps` using 7z).*

---

## 4. Building the DLL (`CvGameCoreDLL.dll`)

FastBuild (`FBuild.exe`) is used to compile the solution with multi-threaded precompiled headers and unity builds.

### Target Configurations
- `Release` — Standard production build (highest runtime performance).
- `Debug` — Full debug symbols and unoptimized assertions.
- `Profile` — Optimized build with profiling hooks enabled.

---

### Option A: Native Windows Build

#### Quick One-Step Build & Deploy to Repo Assets:
```cmd
cd <REPO_DIR>\Tools
call MakeDLLRelease.bat
```
This calls `_MakeDLL.bat Release rebuild deploy`, which compiles the DLL to `<REPO_DIR>\Build\Release\` and copies `CvGameCoreDLL.dll` and `CvGameCoreDLL.pdb` into `<REPO_DIR>\Assets\`.

#### Incremental / Custom Build via PowerShell:
```powershell
cd <REPO_DIR>\Sources
powershell.exe -ExecutionPolicy Bypass -File "..\Tools\_Build.ps1" Release build deploy
```

---

### Option B: Linux Build via Wine

From the repository root, run FastBuild headless via Wine:

```bash
cd <REPO_DIR>/Sources
WINEDEBUG=-all wine ../Tools/FBuild.exe -summary -wait Release-build
```

#### Deploying Compiled Artifacts to Repository Assets:
After FBuild completes successfully:
```bash
cp -f <REPO_DIR>/Build/Release/CvGameCoreDLL.dll <REPO_DIR>/Assets/
cp -f <REPO_DIR>/Build/Release/CvGameCoreDLL.pdb <REPO_DIR>/Assets/
```

> [!NOTE]
> **Linux / Wine File System Case Sensitivity:** Ensure there are no duplicate symlinks or folders that differ only in casing in parent paths (e.g., having both `~/code` and `~/Code` mapped in Wine). Wine’s case-folding layer may alias header search paths differently, leading to duplicate include errors (such as `error C2011: type redefinition`).

---

## 5. Running the Unit Tests

### Standalone C++ Test Suites (Linux & Windows)
The standalone tests under `Test/CvSafetyPruning/` verify safety pathfinding and pruning logic independently of the full Civ IV engine:

- **Linux / MinGW (Native GCC):**
  ```bash
  cd <REPO_DIR>/Test/CvSafetyPruning
  g++ -O2 -std=c++11 -Wall -Wextra -o safety_tests CvSafetyPruningTests.cpp
  ./safety_tests          # Runs full ~4.5M case randomized sweep
  ./safety_tests --quick  # Quick interactive run
  ```

- **Windows (MSVC):**
  ```cmd
  cd <REPO_DIR>\Test\CvSafetyPruning
  cl /EHsc /O2 CvSafetyPruningTests.cpp
  safety_tests.exe
  ```

---

## 6. Installing to Beyond the Sword

To test in-game, Civilization IV must be able to load the mod directory from `<CIV4_BTS_DIR>/Mods/Caveman2Cosmos`.

### Method 1: Developer Symlinks / Junctions (Recommended)

Using links allows changes made in Git (or freshly compiled DLLs copied to `<REPO_DIR>/Assets/`) to take effect in the game immediately without copying gigabytes of mod assets.

#### Windows (Command Prompt as Administrator):
```cmd
mkdir "<CIV4_BTS_DIR>\Mods\Caveman2Cosmos"
cd /d "<CIV4_BTS_DIR>\Mods\Caveman2Cosmos"

mklink /J "Assets" "<REPO_DIR>\Assets"
mklink /J "PrivateMaps" "<REPO_DIR>\PrivateMaps"
mklink /J "PublicMaps" "<REPO_DIR>\PublicMaps"
mklink /J "Resource" "<REPO_DIR>\Resource"

copy "<REPO_DIR>\Caveman2Cosmos.ini" .
```

#### Linux / Steam Proton:
```bash
mkdir -p "<CIV4_BTS_DIR>/Mods/Caveman2Cosmos"
cd "<CIV4_BTS_DIR>/Mods/Caveman2Cosmos"

ln -s "<REPO_DIR>/Assets" Assets
ln -s "<REPO_DIR>/PrivateMaps" PrivateMaps
ln -s "<REPO_DIR>/PublicMaps" PublicMaps
ln -s "<REPO_DIR>/Resource" Resource

# Also provide lowercase aliases for Wine compatibility if needed:
ln -s Assets assets
ln -s Resource resource

cp "<REPO_DIR>/Caveman2Cosmos.ini" ./
```

---

### Method 2: Automated Windows Setup (`DevSetup.bat`)

If running on Windows, you can automate directory location, junction creation, and initial build:
```cmd
cd <REPO_DIR>
DevSetup.bat
```
When prompted, select your `<CIV4_BTS_DIR>/Mods` directory.

---

## 7. Verifying Installation

Verify that the active DLL in your game folder matches your latest build:

### Linux / bash:
```bash
sha256sum "<REPO_DIR>/Build/Release/CvGameCoreDLL.dll" \
          "<REPO_DIR>/Assets/CvGameCoreDLL.dll" \
          "<CIV4_BTS_DIR>/Mods/Caveman2Cosmos/Assets/CvGameCoreDLL.dll"
```
All checksums should match identically.

### Windows PowerShell:
```powershell
Get-FileHash "<REPO_DIR>\Build\Release\CvGameCoreDLL.dll", `
             "<REPO_DIR>\Assets\CvGameCoreDLL.dll", `
             "<CIV4_BTS_DIR>\Mods\Caveman2Cosmos\Assets\CvGameCoreDLL.dll"
```
