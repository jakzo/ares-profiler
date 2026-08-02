# N64 guest profiler

This fork contains an opt-in guest profiler for GoldenEye performance work.
The same ares build can run normal CI tests or collect profiles. Normal
emulation only pays predictable disabled checks; instruction-level JIT hooks
are emitted only when profiling is configured before the N64 powers on.

Set both environment variables when launching ares:

```sh
ARES_N64_PROFILE_SYMBOLS=/path/to/ge007.u.elf \
ARES_N64_PROFILE_OUTPUT=/path/to/profile/ge007 \
ARES_N64_PROFILE_REPLAY=1 \
  ares --no-file-prompt /path/to/ge007.u.z64
```

The ELF must be the exact ELF used to build the ROM. The profiler reads its
ELF32 big-endian `STT_FUNC` symbols directly; no `nm` sidecar or debug-server
connection is required.

Capturing starts after a non-title `lvlStageLoad` returns and stops when
`lvlUnloadStageTextData` begins. Returning to the title and loading another
stage therefore creates another numbered capture. Each capture writes:

- `*-summary.csv`: total CPU cycles, frame count/average delta, TLB totals,
  a histogram of 0, 1, 2, 3, 4, 5-6, 7-8, 9-10, and 11+ dropped frames,
  and per-pool peak live memory usage and capacity.
- `*-functions.csv`: call counts plus self and inclusive guest CPU cycles.
- `*-tlb.csv`: per-8 KiB virtual-page access and TLB-cache hit/miss counts.
- `*-frames.csv`: VI frame start, stop, and delta cycles.
- `*-game-frames.csv`: when `ARES_N64_PROFILE_REPLAY=1`, GoldenEye rendered
  replay-frame tick cycles and software code-page loads. These are observed at
  normal release-ROM function boundaries without guest instrumentation.
- `*.folded`: guest-cycle weighted call stacks in folded-stack format.

## External GoldenEye replay

The profiler can replay a GoldenEye SRAM recording against an unmodified ROM.
The matching build ELF is used to locate functions and global objects for every
ROM version:

```sh
ARES_N64_PROFILE_SYMBOLS=/path/to/ge007.u.elf \
ARES_N64_PROFILE_OUTPUT=/path/to/profile/ge007 \
ARES_N64_REPLAY=/path/to/archives.ram \
ARES_N64_REPLAY_QUIT=1 \
  ares --no-file-prompt /path/to/ge007.u.z64
```

The emulator selects the recorded stage and difficulty, restores the initial
random seeds, makes recorded options authoritative at their getters and
setters, queues the controller sample consumed by player 1, and replaces the
`updateFrameCounters` `$a0` argument with each recorded delta. Before replacing
that argument, the profiler records the live frame delta as
`dropped frames = max(delta - 1, 0)` for each completed rendered game frame.
It reports
`TEST_COMPLETE` on success and
`TEST_FAILED` on divergence, premature level exit, or timeout.

Set `ARES_N64_REPLAY_QUIT=1` to exit after either result. Waiting for the level
to start times out after 20 seconds. Once replay starts, ares fails if the next
GoldenEye frame is not rendered within 20 seconds.

While replay is running, ares writes
`REPLAY_STATUS frame=<completed>/<total>` every 20 seconds so unattended test
runners can distinguish a long recording from a stalled emulator.

### ROM/ELF replay ABI

External replay treats the matching ELF as an ABI. Region-specific addresses
may change, but the following symbol names, C types, function signatures, and
relevant structure offsets must remain stable. Functions must be present in the
full ELF symbol table as `STT_FUNC` entries with nonzero sizes, and objects as
nonzero-sized `STT_OBJECT` entries. Stripping these symbols prevents replay
startup; local or global ELF binding is accepted.

Required object symbols:

| Symbol | Required C type | Use |
| --- | --- | --- |
| `g_StageNum` | `s32` | Selects the recorded stage. |
| `selected_difficulty` | `DIFFICULTY` (32-bit enum) | Sets front-end difficulty state. |
| `g_SelectedDifficulty` | `s32` | Sets live level and AI difficulty before stage load. |
| `g_ContData` | `struct contdata[2]` | Queues recorded controller samples. |
| `g_randomSeed` | `u64` | Restores and verifies the gameplay RNG. |
| `g_chrObjRandomSeed` | `u64` | Restores and verifies the character/object RNG. |
| `g_mempPools` | `MemoryPool[7]` | Samples per-level pool usage and capacity. |

Required function symbols and signatures:

```c
void bossMainloop(void);
void lvlStageLoad(s32 stage);
void lvlUnloadStageTextData(void);
void updateFrameCounters(s32 deltaFrames);
void dynSwapBuffers(void);

Gfx *dynGetMasterDisplayList(void);
```

Ares queues replay input at `dynGetMasterDisplayList` entry, starts game-frame
profiling when it returns, and ends the frame at `dynSwapBuffers` entry.

The controller guest layout used by replay is also ABI:

- `g_ContData[0]` must be the regular controller ring. `struct contsample`
  remains 24 bytes; the 20 samples begin at offset zero, `curlast` remains at
  `0x1e0`, `nextlast` at `0x1e8`, and `playbackcontcount` at `0x1f8`.

The replay fixture's bytes before its `0x600` replay-data offset contain the
save image captured by the recorder. Ares restores the cartridge's SRAM or
EEPROM, as selected by the ROM metadata, from this image before the game boots.
This allows GoldenEye to initialize controller and gameplay options through its
normal save-loading path. Later option changes are reproduced by the recorded
controller input.

The following difficulty setter symbols are optional compatibility hooks. When
present, ares overrides their first argument:

```c
set_selected_difficulty
lvlSetSelectedDifficulty
```

`tlbmanageTranslateLoadRomFromTlbAddress` is optional and enables per-game-frame
software TLB-load counts. `practice_replay_on_stage_load` and
`practice_replay_stop_playback` are optional boundaries used only when profiling
an instrumented guest replay rather than `ARES_N64_REPLAY`.

ares exits automatically after writing a capture. It also exits if capture has
not started within 60 seconds, or writes a partial capture and exits if an
active capture has not finished within 10 minutes. The generated file paths are
printed to stderr after they are written.

The profiler records emulated VR4300/CP0 cycles, not host wall time, so results
remain useful when the host is under load. Function call stacks are recovered
from JAL/JALR and return addresses; exception and tail-call entries are
resynchronised at symbol boundaries.

Generate a standalone interactive HTML flame graph from a folded capture with:

```sh
python3 tools/n64-profiler-flamegraph.py profile-001.folded
```
