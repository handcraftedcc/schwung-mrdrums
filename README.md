# Move Anything - MrDrums Module

`MrDrums` is a 16-pad drum sampler module for [Move Anything](https://github.com/charlesvestal/move-anything).

Current behavior:

- Top-level pages: `Global` and `Pad Settings`
- Dynamic pad editing with notes `36..51` (`p01_*` .. `p16_*`)
- Fully persisted per-pad and global parameters
- File browser live preview with cancel-to-restore / select-to-commit
- `Auto Select` toggle to lock editing to the current pad while playing
- Global humanize timing, velocity curve, and randomization controls

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
