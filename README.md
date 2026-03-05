# Move Anything - MrDrums Module

`MrDrums` is a 16-pad drum sampler module for [Move Anything](https://github.com/charlesvestal/move-anything).

This branch is actively being converted from a copied scaffold. The target UX is:

- Top-level pages: `Global` and `Pad Settings`
- Dynamic pad editing: hit pads to switch current pad while staying on the same focused parameter
- Fully persisted keys per pad (`p01_*` .. `p16_*`) plus global keys (`g_*`)

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
