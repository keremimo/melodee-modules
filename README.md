# Melodee Modules

A CLAP plug-in that plays Melodee synth modules (`.mmod` files, ABI 3). Put module files in the `Modules` folder
beside `Melodee Modules.clap`: each file is one synth of the plug-in, and its presets pick the synth (see Presets).
Built for the MPC Sample running Melodee (Linux aarch64), and for Apple silicon Macs to test.

## Module files

A `.mmod` from the FM-1 holds its code for the FM-1's processor (JieLi pi32v2), which an ARM computer cannot run. The
plug-in runs the file's AA64 chunk instead: the same module built for AArch64 by the SDK's builder.

    python3 mod_build.py --native trio.mmod --src trio.c -o native/trio.mmod

(`mod_build.py` is in the SDK, melodee-module-sdk: the firmware's `firmware/modules/sdk` submodule; `--native` needs
LLVM's `clang` and `ld.lld`. In the firmware's tree the source is found by itself.) The chunk follows SIGN, as a WASM chunk does, so the file still
installs on the FM-1 once a tool leaves the chunk out. SDK modules build this way, and in the firmware's tree its
engine-source synths too (SID, CZ-1, FM6, the Prophet: `--native build/modules/sid.mmod`). Their AA64 code carries
its own copies of the firmware's tables (a firmware from 2026-11-27 on), so it asks this plug-in for none. Files built
before took them through the host table, which still has them: FM6's (and CZ-1's sine) from `src/mm_fm6_tables.h`,
written from a firmware checkout by `tools/sync_fm6_tables.py`.

The AA64 code runs in the plug-in as any plug-in's code does. Load only module files you trust: the signature covers
the FM-1's part of the file, not the AA64 chunk.

From the file the plug-in takes:

- its name and version (the head, IDNT), and its ID (as `mod_build.py --check` prints it): the synth's name in
  the presets, and the synth a state or a preset names. A newer version of the same module replaces the older one;
- its factory sounds (PRST): its presets;
- the patch a part starts with (SCHM), for modules with a patch (HYDRA, PPG).

## Presets

The plug-in is one plug-in (`studio.ellic.melodee-modules`), and a preset of a synth loads that synth. Preset
discovery gives each module file a location, named for the synth, holding its factory sounds; a SysEx file is a
location under the synth it is for (below), named `<synth>/<file>`, and the synth's own location is then
`<synth>/Factory`. In Melodee's PRESETS that is a folder for each synth, with its sounds, or with a Factory folder and
a folder for each of its SysEx files:

    HYDRA/   Factory/  HYDRA test/
    PPG/     Factory/  PPG 2.2 factory/  danger_in_dream/ ...
    TRIO/    (its sounds)
    WHEEL/   (its sounds)

A new part plays the first synth by name, its first sound. Each part maps only the code of the synth it plays; a
preset of another synth maps that synth's code on the main thread, and the audio thread takes it at its next block
and gives the old one back to be freed. The part's notes stop with the old synth.

## SysEx files

SysEx files (`.syx`) in the `SysEx` folder beside the plug-in are sounds too. A file is for the first synth (by
name) whose module answers a frame in it; a file no module answers is left out. Its sounds are those the module takes
from it: the file is given to the module as the FM-1 gives it SysEx that came in, in a copy of the module of its own,
and the sounds the module offers for user slots are the file's presets. Loading one gives the file to the part's
module again (the new synth's, when the part plays another) and takes that sound. HYDRA takes its own messages
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

The plug-in adds two things the FM-1 does not do:

- VEL (default 100%): how much the level follows velocity, 1 - VEL x (1 - velocity / 127). At 100% this is the FM-1's
  law for every module, including those with their own envelopes (PPG, FMINI), which the FM-1 leaves at full level
  unless the patch turns velocity on. At 0% the level is as the FM-1 plays it.
- AT FLT (default +25%, -64..63 as ENV FLT): each key's pressure (poly aftertouch, or MIDI channel aftertouch, or
  CLAP's pressure expression) opens the filter of the voices playing that key. It smooths over about 3 ms and adds to
  the module's cutoff, as ENV and LFO to filter do. The part's aftertouch, which a module reads, is the hardest
  pressed of the keys held.
- LEVEL as the FM-1's track level (default 104, -4 dB); a module's stereo (host 5) as the FM-1's mixer takes it.
- 44.1 kHz in blocks of 32 samples, as on the FM-1. At 48 and 96 kHz a windowed-sinc interpolator converts the
  output (16 taps each side, about 0.35 ms).

Not here: the modulation matrix, the arpeggiator, scales, chords, SPREAD, the FX, SysEx over MIDI, recorded kits (data
records), the modules' own drawing (the plug-in draws its own page: below), and the schema's dim (pactive)
parameters, shown as any other.

## Parameters and pages

A CLAP plug-in's parameters stay the same while it runs, so the plug-in has every synth's parameters; those of a
synth it does not play are at their defaults and an event for one does nothing. Their ids are the synth's tag (from its
module ID, bits 18 and up) and the synth's own id. They have no module (group): a host groups none, so Melodee shows
the pages only, which are those of the synth it plays.

A module with a schema (SCHM: HYDRA, PPG) has its schema's parameters (`src/mm_schema.c`): bits of the patch, the
part's own, or the module's `pget` / `pput`, with the module's own text (`pformat`) and ranges (`desc`) where it has
them. A change from the host is an edit as on the FM-1's pages: the patch written, then `patch_changed` (EDIT) when it
changed or the parameter asks for it; the host re-reads the values after (one `pput` may move others). Their own ids
are 0x10000 and the schema's id; their names the schema's long names.

The remote-control pages are the schema's (four knobs each, an instanced page once per instance), its shown
parameters on no page four to a page (MORE), then the engine's
two EDIT pages (unless the schema says they are no panel), ENV (unless the synth has its own envelopes), LFO, LFO 2
(TRACK LFO when the synth has its own LFO pages), VOICE and TOUCH (VEL, AT FLT); a part's parameter the schema
already shows is left out.
Melodee shows them under B1-B3. ATTACK to RELEASE and CUTOFF with RESO get Melodee's envelope and filter pictures.

`test_host --list DIR/melodee-modules.clap` prints each synth's pages and values.

## Its page in Melodee

In Melodee the plug-in draws its own page (Stage, B1 **PANEL**), through Melodee's screen extension
(`sdk/melodee_screen.h`, MIT, from Melodee's `sdk/melodee-screen`). The page is a faceplate: each synth has an enamel
panel of its own colour (from its module ID), printed in Michroma, with knob caps over **K1**-**K3**. Melodee keeps
**B1**-**B3** above it.

- **-** / **+** walk the pages: the synths, the sounds, then the synth's own pages.
- The synths: panels in a rack, the lit one pulled out with its maker and its sounds, a light on the one playing. The
  encoder walks them; Push plays the lit one (its first sound) and opens the sounds.
- The sounds: a program window, the lit sound in its middle, its factory sounds then each SysEx file's (the bank's name
  at the top right, a line where a bank ends). The encoder walks them, **SHIFT** + encoder jumps a bank, Push loads
  the lit one. **K1**-**K3** turn the sound's cutoff, resonance and release, those it has.
- The synth's pages, as the FM-1 has them: four values a page (a page of more is split in fours). **K1**-**K3** turn the
  first three, the encoder the fourth (a small cap at the right). Each page draws what its values shape (an envelope,
  a filter's curve, an LFO's wave), else the sound itself in a scope window. **SHIFT** + encoder steps the value a pot
  set last by one.
- A pot short of its value: the pointer faint and a ring on the scale where the pot sits.
- The band at the top: the synth, the sound loaded (a dot once it is changed), the page's place.

A value turned on the page goes to the audio thread and the host is told, as an edit from the host's own pages is.
A sound loaded on the page is told to the host as loaded by the plug-in (preset-load's `loaded`). The project keeps
which sound is loaded and whether it was changed (after the patch in the plug-in's state; an older plug-in reads no
further).

`test_host --screen DIR/melodee-modules.clap OUT STEP...` draws the page after each step, as Melodee would, and
writes `OUT/NAME.ppm` at `snap:NAME` (steps: `wheel:N`, `swheel:N` with **SHIFT**, `push`, `minus`, `plus`,
`note:KEY`, `off`, `k1:V`..`k3:V` a pot driving its value, `pot1:V`..`pot3:V` a pot short of its value,
`colour:RRGGBB` the track's colour).

## Build

    make             # build/<os>-<arch>/melodee-modules.clap
    make test        # the plug-in with tests/fixtures/*.mmod beside it, loaded as a host loads it
    make test-each   # each module file alone in the Modules folder: it lists, loads and plays
    make test-staged # two copies of the plug-in, each dlopen'd on its own beside a linked Modules folder, as
                     # Melodee stages one per rack entry
    make fixtures    # the test modules from the firmware (FIRMWARE=../Felucca) into tests/fixtures, and its
                     # engine-source synths (SID, CZ-1, FM6, the Prophet) into tests/fixtures/synths for test-each
    make mpc         # build/mpc-aarch64/melodee-modules.clap in Docker (arm64v8/gcc:12-bookworm), and the three
                     # tests above run there on Linux
    make dist        # dist/Melodee Modules.zip: the MPC build and an empty Modules folder

`MM_WAV_DIR=dir make test` writes each module's first sounds as WAVs.

## Install on the MPC

Drop `dist/Melodee Modules.zip` on the Device page at http://melodee.local, or copy the `Melodee Modules` folder to
`Melodee/Plugins` on the card. Then copy module files into `Melodee/Plugins/Melodee Modules/Modules` and SysEx
files into `Melodee/Plugins/Melodee Modules/SysEx`. The plug-in reads both folders when its library loads. While it
plays it looks at them every 2 seconds (`src/mm_watch.c`): once a module or SysEx file is added, removed or replaced
and has held still a second, it asks its host to restart it (CLAP's `request_restart`). Melodee then opens it again
with its sound, from a fresh copy of its library, which reads the folders again. Another host may only deactivate and
activate it, which does not; load the plug-in again there.

## Licence

GPL-3.0-only (`LICENSE`). The SDK headers in `sdk/` are MPL-2.0 (`LICENSES/MPL-2.0.txt`, `tools/sync_sdk.sh` copies
them); the CLAP headers in `third_party/clap` are MIT, and so is Melodee's screen header `sdk/melodee_screen.h`;
FM6's tables in `src/mm_fm6_tables.h` are MSFA's (Apache-2.0, `LICENSES/Apache-2.0-msfa.txt`) and Dexed's
(GPL-3.0-or-later). The
page's letters (`src/mm_font.c`) are Michroma's, Copyright 2011 The Michroma Project Authors, SIL Open Font License 1.1
(`LICENSES/OFL-1.1.txt`); `tools/gen_font.swift` draws them from the font file on a Mac. Synth modules are not in this
repository: they are the firmware's build outputs, and the plug-in only loads them. `make fixtures` builds the test
modules from a Melodee firmware checkout into `tests/fixtures` (ignored); the tests take the PPG module when the
checkout has its data.
