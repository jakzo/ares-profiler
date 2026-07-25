# GoldenEye replay tests

This harness runs recorded GoldenEye SRAM replays against a ROM,
using its matching ELF for the external replay ABI and profiler symbols. Each
test uses a private ROM copy so ares save data cannot modify the source ROM.
It requires ares to report `TEST_COMPLETE`, rejects `TEST_FAILED`, and checks
that all profiler output files were flushed before ares exited.
The runner stops a hung process if it goes 20
seconds without `REPLAY_STARTED` or a `REPLAY_STATUS` heartbeat from ares.

Run every bundled US replay:

```sh
tests/n64-replay/run.py \
  --ares build/desktop-ui/rundir/ares \
  --rom /path/to/ge007.u.z64 \
  --elf /path/to/ge007.u.elf
```

Run one fixture and retain logs and profile CSVs:

```sh
tests/n64-replay/run.py \
  --ares /path/to/ares \
  --rom /path/to/ge007.u.z64 \
  --elf /path/to/ge007.u.elf \
  --replay frigate_00 \
  --artifacts /tmp/ares-replay-results
```

`ARES`, `GOLDENEYE_ROM`, and `GOLDENEYE_ELF` may be used instead of the three
path options. On Linux, invoke the script under `xvfb-run`.
