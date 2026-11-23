# Melodee Modules

A CLAP plug-in that plays Melodee synth modules (`.mmod` files, ABI 3). Put module files in the `Modules` folder
beside `Melodee Modules.clap`: each file is one plug-in. Built for the MPC Sample running Melodee (Linux aarch64),
and for Apple silicon Macs to test.

## Module files

A `.mmod` from the FM-1 holds its code for the FM-1's processor (JieLi pi32v2), which an ARM computer cannot run. The
plug-in runs the file's AA64 chunk instead: the same module built for AArch64 by the SDK's builder.

    python3 mod_build.py --native trio.mmod --src trio.c -o native/trio.mmod

(`mod_build.py` is in the firmware's `firmware/modules/sdk`; `--native` needs LLVM's `clang` and `ld.lld`. In the
firmware's tree the source is found by itself.) The chunk follows SIGN, as a WASM chunk does, so the file still
installs on the FM-1 once a tool leaves the chunk out. SDK modules build this way; the firmware's engine-source modules
(SID, DRUM, CZ-1, FM6, the Prophet) do not.

The AA64 code runs in the plug-in as any plug-in's code does. Load only module files you trust: the signature covers
the FM-1's part of the file, not the AA64 chunk.

From the file the plug-in takes:

- its name, vendor and version (the head, IDNT), and its ID, which is the plug-in's ID: `studio.ellic.melodee-modules.`
  and the ID as `mod_build.py --check` prints it. A newer version of the same module replaces the older one;
- its factory sounds (PRST): preset discovery lists them under the module's plug-in;
- the patch a part starts with (SCHM), for modules with a patch (HYDRA, PPG).

## SysEx files

SysEx files (`.syx`) in the `SysEx` folder beside the plug-in are sounds too. Preset discovery gives each file a
location of its own, named for the file (in Melodee's PRESETS a folder each, beside Factory), and lists in it the sounds
a module takes from it: the file is given to the module as the FM-1 gives it SysEx that came in, in a copy of the
module of its own, and the sounds the module offers for user slots are the file's presets, under that module's
plug-in. Loading one gives the file to the part's module again and takes that sound. HYDRA takes its own messages
(`F0 7D 48 59 01`, a sound each, as an FM-1 sends them); PPG takes a PPG Wave 2.2 or 2.3 bank of the V8 OS
(`F0 29 01 0D`, 100 programs, named PROGRAM 00..99). A SysEx sound is the module's patch: the part's own values (ENV,
LFO, VOICE) start from the file's first sound. Files over 1 MB are left out.

Each instance maps its own copy of the code (`src/mm_image.c`): code and constants read-only and executable, data
writable, so every part has the module's statics to itself.

## What the host does

What the FM-1 does around a module, per part (`src/mm_engine.c`, written from `mod_abi3.h`):

- Voices: POLY (round robin or reuse), MONO, LEGATO, UNISON with DETUNE, note priority, voice stealing that spares
  the lowest held note.
- The ADSR, ENV to filter, pitch and shape, LFO (5 waves, fade, NOTE or FREE, BI or UNI, SYNC to the host's tempo)
  to pitch, filter, shape and level, glide by RATE or TIME, accent above velocity 110.
- Pitch bend (BEND semitones, or the module's own range), mod wheel, breath, foot, aftertouch, sustain pedal.
- LEVEL as the FM-1's track level (default 104, -4 dB); a module's stereo (host 5) as the FM-1's mixer takes it.
- 44.1 kHz in blocks of 32 samples, as on the FM-1. At 48 and 96 kHz a windowed-sinc interpolator converts the
  output (16 taps each side, about 0.35 ms).

Not here: the modulation matrix, the arpeggiator, scales, chords, SPREAD, the FX, SysEx over MIDI, recorded kits (data
records), the modules' own drawing, and the schema's dim (pactive) parameters, shown as any other.

## Parameters and pages

A module with a schema (SCHM: HYDRA, PPG) has its schema's parameters (`src/mm_schema.c`): bits of the patch, the
part's own, or the module's `pget` / `pput`, with the module's own text (`pformat`) and ranges (`desc`) where it has
them. A change from the host is an edit as on the FM-1's pages: the patch written, then `patch_changed` (EDIT) when it
changed or the parameter asks for it; the host re-reads the values after (one `pput` may move others). Their CLAP ids
are 0x10000 and the schema's id; their names the schema's long names.

The remote-control pages are the schema's (four knobs each, an instanced page once per instance), then the engine's
two EDIT pages (unless the schema says they are no panel), ENV (unless the synth has its own envelopes), LFO, LFO 2
(TRACK LFO when the synth has its own LFO pages) and VOICE; a part's parameter the schema already shows is left out.
Melodee shows them under B1-B3. ATTACK to RELEASE and CUTOFF with RESO get Melodee's envelope and filter pictures.

`test_host --list DIR/melodee-modules.clap` prints each plug-in's pages and values.

## Build

    make             # build/<os>-<arch>/melodee-modules.clap
    make test        # the plug-in with tests/fixtures/*.mmod beside it, loaded as a host loads it
    make test-each   # each module file alone in the Modules folder: it lists, loads and plays
    make test-staged # two copies of the plug-in, each dlopen'd on its own beside a linked Modules folder, as
                     # Melodee stages one per rack entry
    make fixtures    # the test modules from the firmware (FIRMWARE=../Felucca) into tests/fixtures
    make mpc         # build/mpc-aarch64/melodee-modules.clap in Docker (arm64v8/gcc:12-bookworm), and the three
                     # tests above run there on Linux
    make dist        # dist/Melodee Modules.zip: the MPC build and an empty Modules folder

`MM_WAV_DIR=dir make test` writes each module's first sounds as WAVs.

## Install on the MPC

Drop `dist/Melodee Modules.zip` on the Device page at http://melodee.local, or copy the `Melodee Modules` folder to
`Melodee/Plugins` on the card. Then copy module files into `Melodee/Plugins/Melodee Modules/Modules`.

## Licence

GPL-3.0-only (`LICENSE`). The SDK headers in `sdk/` are MPL-2.0 (`LICENSES/MPL-2.0.txt`, `tools/sync_sdk.sh` copies
them); the CLAP headers in `third_party/clap` are MIT. Synth modules are not in this repository: they are the
firmware's build outputs, and the plug-in only loads them. `make fixtures` builds the test modules from a Melodee
firmware checkout into `tests/fixtures` (ignored); the tests take the PPG module when the checkout has its data.
