# Eden NX2 for Mario Kart World: documentation

This repository is a fork of [Takoroka's eden-nx2](https://git.eden-emu.dev/Takoroka/eden-nx2)
(experimental Nintendo Switch 2 emulation on top of the Eden emulator), aimed at one game:
**Mario Kart World 1.8.0** (program id `0400C3F00006E000`).

Everything specific to this fork is documented here. The emulator's own documentation is in
[`docs/`](../README.md), and Takoroka's engineering log of the Switch 2 work is
[`NX2_NOTES.md`](../../NX2_NOTES.md) (one row per problem solved).

## Pages

| Page | For whom | What it covers |
| --- | --- | --- |
| [Getting started](getting-started.md) | Players | Dump layout, keys, first launch, recommended settings, troubleshooting |
| [Building](building.md) | Developers | Toolchain, exact build commands (clang-cl and MSVC), packaging |
| [Changes in this fork](changes.md) | Developers, reviewers | Every change on top of Takoroka's code: why, what, files, commits |
| [Experiments that were dropped](experiments.md) | Developers | What was tried to remove shader stutter and raise the frame rate, with measurements, and why it is not in the release |
| [Known issues](known-issues.md) | Everyone | What still does not work |

Detailed design notes for the two main topics:

- [`NX2_SHADER_STUTTER.md`](../../NX2_SHADER_STUTTER.md): why new shaders froze the game and the
  "option C" changes that fixed it.
- [`NX2_VRAM.md`](../../NX2_VRAM.md): why video memory filled up (garbage textures, then a crash)
  and how the release prevents it.

## Branches

| Branch | Content |
| --- | --- |
| `release-c` | What the current release is built from (recommended) |
| `shader-c` | Takoroka's code + Mario Kart World 1.8.0 fixes + option C (previous release) |
| `mkw-1.8` | Takoroka's code + Mario Kart World 1.8.0 fixes only |

## AI disclosure

The changes on top of Takoroka's code were written with Claude Code (Claude Opus 5.5) and tested
by a human on Mario Kart World 1.8.0 (Ryzen 7 5800X, Radeon RX 6900 XT). Each commit says so.
If you submit any of this code to another project, follow that project's AI policy (for Eden:
[`docs/policies/AI.md`](../policies/AI.md)).
