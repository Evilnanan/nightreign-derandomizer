# Nightreign Derandomizer

**English** | [中文](README.zh-CN.md)

This project produces the following Windows x64 artifacts:

- `derandomizer.dll` — the mod.
- `Seed Reroller.exe` — a companion tool that searches for / predicts seeds and
  generates `derandomizer.ini`.
- `derandomizer.ini` — mod configuration. Settings are in the `[settings]` section:

| Setting | Description |
| --- | --- |
| `nightlord` | Nightlord ID (`0`–`9`); `-1` disables Nightlord locking. |
| `everdark` | `0` forces Normal, `1` forces Everdark; `-1` disables variant locking. |
| `seed` | Seed to use (32-bit integer, decimal or hexadecimal); `-1` disables seed locking. |
| `log` | `0` disables logging, `1` enables it. |

Configuration supports hot reload: changes to `derandomizer.ini` take effect
automatically after saving, without restarting the game.

## Project layout

```text
src/
├── app/                 # Seed Reroller
│   ├── main.cpp         # Win32 UI and program entry point
│   ├── cli.*            # Command-line parsing and output
│   ├── model.*          # Seed / pattern prediction model
│   └── config.*         # Mod configuration generation
└── mod/
    ├── derandomizer.cpp      # DLL entry, state observation, runtime scheduling
    ├── boss_hooks.*          # Nightlord / Everdark hooks
    ├── seed_hook.*           # Seed injection and fast field correction
    ├── range_diagnostics.*   # Optional Range() reverse-engineering diagnostics
    ├── game_module.*         # PE version identification and signature scanning
    ├── memory_patch.*        # Memory patches and stub allocation
    ├── config.*              # derandomizer.ini loading and validation
    └── logging.*             # Runtime logging
```

## Building

You need Visual Studio 2026 with the "Desktop development with C++" workload
(v145 toolset) and CMake 4.2 or newer (required for the VS 2026 generator):

```powershell
cmake --preset vs2026-x64
cmake --build --preset release
```

Release artifacts land in a single directory:

```text
build/dist/Release/
├── derandomizer.dll
├── derandomizer.ini
└── Seed Reroller.exe
```

For a debug build use `cmake --build --preset debug`; the artifacts then live in
`build/dist/Debug/`.

## Command-line usage

Running without arguments still opens the graphical interface. The command line
exposes two subcommands:

```powershell
# Search for a seed that produces a pattern
& '.\Seed Reroller.exe' find 1042

# Predict the pattern for a seed and Nightlord
& '.\Seed Reroller.exe' predict 0x066BB95B --nightlord 7
```

Both accept the following options:

```text
--mode deep|normal                 Mode; defaults to deep
--variant auto|normal|everdark     Variant written to the config; defaults to auto
--config <path>                    Also generate the given derandomizer.ini
--json                             Emit the result as a single-line JSON object
```

For example, search for a seed for pattern 1042 and generate the config in one
step:

```powershell
& '.\Seed Reroller.exe' find 1042 --variant everdark --config .\derandomizer.ini
```

Relative `--config` paths resolve against the current working directory. Run
`& '.\Seed Reroller.exe' --help` for the full help text; on invalid arguments
the process exits with a non-zero exit code.
