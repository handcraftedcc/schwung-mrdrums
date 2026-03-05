# Move Anything - MrDrums Module

`MrDrums` is a 16-pad drum sampler module for [Move Anything](https://github.com/charlesvestal/move-anything).

MrDrums gives you 16 pads with independent sample paths and per-pad controls for gain, pan, tune, start, envelopes, choke, chance, and randomization.

## Quick Start

1. Install `MrDrums` from the Module Store.
2. Load `MrDrums` in a chain.
3. Open `Pad Settings`.
4. Select `Sample` and choose a `.wav` from:
   - `/data/UserData/UserLibrary/Samples`
5. Play pads/notes (`36..51`).

By default, all pad sample paths are empty, so MrDrums is silent until you set at least one sample.

## Features

- 16 drum pads mapped to notes `36..51`
- Fully persisted parameter model:
  - Global keys (`g_*`)
  - Per-pad keys (`p01_*` .. `p16_*`)
- Dynamic current-pad editing with optional auto-select from played notes
- File browser live preview:
  - Cursor audition while browsing
  - Back restores original sample
  - Select commits new sample
- Global timing and dynamics controls:
  - Velocity curve (`linear`, `soft`, `hard`)
  - Humanize timing (`g_humanize_ms`)
  - Random seed + loop-step controls
- Per-pad variation controls:
  - Chance
  - Random pan/volume/decay amounts

## Prerequisites

- [Move Anything](https://github.com/charlesvestal/move-anything) installed on your Ableton Move
- WAV files available on device (recommended path below)

## Requirements

Recommended sample folder:

```text
/data/UserData/UserLibrary/Samples
```

Supported input format:
- `.wav` files

## Controls

Top-level pages:
- `Global`
- `Pad Settings`

### Global

- `Master Vol`: Overall output level.
- `Polyphony`: Maximum active voices.
- `Velocity Curve`: `linear`, `soft`, or `hard`.
- `Humanize`: Random per-hit timing offset in milliseconds.
- `Random Seed`: Static seed for repeatable randomization behavior.
- `Rand Loop Steps`: Loop length for deterministic random sequences.

### Pad Settings

- `Auto Select`: When `on`, incoming pad notes change `Current Pad`.
- `Current Pad`: Pad target for alias controls.
- `Sample`: Per-pad WAV file path.
- `Vol`, `Pan`, `Tune`, `Start`
- `Attack`, `Decay`
- `Choke`, `Mode` (`gate` / `oneshot`)
- `Rand Pan`, `Rand Vol`, `Rand Decay`
- `Chance`

Pad aliases (for fast editing) map to the selected `Current Pad` while keeping all `pXX_*` values persisted per pad.

## File Browser Behavior

For pad `Sample`:
- Root path is constrained to user sample space.
- `start_path` prefers last-used sample directory when available.
- Live preview is enabled while browsing.
- Auto-select can be suspended during browsing through configured browser hooks.

## Installation

### Module Store (recommended)

Install `MrDrums` from Move Anything's Module Store.

### Build from Source

Requires Docker (recommended) or ARM64 cross-compiler.

```bash
git clone https://github.com/handcraftedcc/move-anything-mrdrums
cd move-anything-mrdrums
./scripts/build.sh
./scripts/install.sh
```

Install target:

```text
/data/UserData/move-anything/modules/sound_generators/mrdrums/
```

Restart Move Anything after install.

## Current Limitations

- WAV-only sample input
- Single sample per pad (no layer stacks)

## AI Assistance Disclaimer

This module is part of Move Everything and was developed with AI assistance, including Claude, Codex, and other AI assistants.

All architecture, implementation, and release decisions are reviewed by human maintainers.
AI-assisted content may still contain errors, so please validate functionality, security, and license compatibility before production use.

## Build

```bash
./scripts/build.sh
```

## Install

```bash
./scripts/install.sh
```

Install target:

```text
/data/UserData/move-anything/modules/sound_generators/mrdrums/
```
