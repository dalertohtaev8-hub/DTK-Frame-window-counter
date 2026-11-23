# Window Counter (Android) — Geode mod

**Version:** `v0.2.0`  
**Target game:** Geometry Dash `2.2081`  
**Target platforms:** Android ARMv7 (`Android32`) and ARM64 (`Android64`)  
**Minimum Geode:** `5.11.2`

Window Counter replays a macro in-game, probes nearby input ticks, and exports `nandl.json` for the HyperClick trainer.

> **Build/test status:** source preparation for 0.2.0 is complete, but this package has not yet been built against the Geode Android SDK or tested inside Geometry Dash. A successful GitHub Actions build and an in-game test are still required before calling it release-ready.

## 0.2.0 changes

- Updated the declared minimum Geode version to stable `5.11.2` for GD `2.2081`.
- Hardened JSON parsing: validates JSON termination, escape sequences, Unicode surrogate pairs, numeric syntax, nesting depth, and input size.
- Added macro validation for frame rate, event count, frame values, button, `down`, and `2p` fields.
- Added bounds checks for simulation settings before starting a run.
- Reset per-run probe/checkpoint state explicitly.
- Kept the Android32/Android64 GitHub Actions build matrix.

## How it works

1. The macro is replayed from the beginning in practice mode. Checkpoints are captured before scheduled inputs.
2. For each input, the relevant checkpoint is restored and that input is shifted by a number of 240-tick steps. The simulator checks whether the player survives the configured horizon.
3. The measured window is exported to `nandl.json` in the mod save directory.

The current algorithm is an experimental approximation. Checkpoint restoration may not reproduce every trigger/RNG state, and tick alignment must be validated in-game. A result with base desynchronizations is not reliable.

## Build on GitHub (no PC required)

1. Create a GitHub repository and upload all project files, including `.github/workflows/build.yml`.
2. Open **Actions → Window Counter Android Build → Run workflow** (or push a commit).
3. Wait for the Android32 and Android64 jobs and the **Build Output** artifact.
4. Download the artifact and extract the `.geode` package.
5. Copy it to the Geode Android mods folder, usually:
   `Android/media/com.geode.launcher/game/geode/mods/`
6. Restart Geometry Dash through the Geode launcher.

The workflow uses `sdk: latest`, which the Geode build action defines as the latest stable SDK release. The `mod.json` minimum loader version is `5.11.2`.

## Use

1. Export `macro.json` from the trainer.
2. Place it in the mod save directory shown by the mod when the file is missing. The expected filename is `macro.json`.
3. Open the same level in Geometry Dash, pause, and press **Count**.
4. Do not touch the screen while counting. The progress label appears in the top-left corner.
5. Import the generated `nandl.json` into the trainer.

## Settings

- **Max window** — maximum search distance in ticks on each side (default 12).
- **Survive after input** — number of ticks to survive after a shifted input (default 90).
- **Input tick offset** — calibration adjustment for input injection (default 0).
- **Work per frame** — simulation time budget per rendered frame (default 12 ms).

## Current limitations and test checklist

- Only the jump button is supported; platformer controls are not supported.
- The algorithm depends on practice checkpoints and may desynchronize on levels with stateful triggers or RNG.
- One macro is intended for one level; CBF is not supported.
- Before relying on output, test a short known macro, compare a few inputs manually, check both player states in dual mode, and confirm the exported JSON imports correctly.
- If the base replay desynchronizes, first verify that the macro belongs to the current level, then test `Input tick offset` values `-1`, `0`, and `1`.
