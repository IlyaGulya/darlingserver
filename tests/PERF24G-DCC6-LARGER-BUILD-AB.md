# perf#24g — DCC6 larger real-codebase build A/B (measurement)

Status: **DONE.** Measured DCC6 OFF vs ON on a larger, CPU-heavy build (500 TUs, real `-O2`
work). **Headline honest result: the VMA collapse persists on the larger build (cc1 193 → 65
maps, −66%), but wall time is identical OFF vs ON because a 500-file `-O2` build is
compiler-CPU-bound — the mm-teardown/VMA win is real (directly measured: exit_mmap −17.7 %, −191 ms/build; unmap_vmas
−78 %) but not on the critical path here.** No DCC6 builder/reader/init/default-on changes; prod
restored byte-identical, doctor ALL GREEN.

## Workload (larger than the milestone's 200 tiny files)
- 500 C files, **342 000 lines total (~684 lines/file), 9.8 MB**, generated deterministically.
- Each TU: a 64-entry static lookup table + 40 functions with nested loops, `switch`, integer
  math, and self-recursion — so cc1 does genuine `-O2` optimization work per file (not a
  spawn-dominated micro-workload). Build: `ls u*.c | xargs -P 8 clang -O2 -c`.
- Same rebuilt DCC6 dyld (aec6a4a, md5 4fedbeb6) for BOTH arms; same DCC6 cache
  (`full.dcc6`, md5 49e652d1); same install_root. OFF = flag absent; ON = `DARLING_DYLD_DCC2=1`
  + `DARLING_DYLD_DCC2_PATH`. This isolates the flag, not the binary.

## Results

### Wall (3 runs per arm, fresh boot + hard teardown each)
| Arm | guest wall | host wall (incl. boot+teardown) | objs | rc |
|---|---|---|---|---|
| OFF r1/r2/r3 | 13 / 13 / 13 s | 14 / 14 / 14 s | 500 | 0 |
| ON  r1/r2/r3 | 13 / 13 / 13 s | 14 / 14 / 14 s | 500 | 0 |

**OFF and ON are wall-identical (13 s guest, 14 s host).** At 500 files with real `-O2` CPU per
file, wall is dominated by cc1 compile work, which DCC6 does not touch. DCC6's win is in VMA
setup/teardown per process — invisible at the wall-clock level once the build is CPU-bound.
(This is the honest opposite corner from the milestone's 200-tiny-file run, where the build was
spawn/teardown-heavy: OFF 1.27 s / ON 1.06 s. Between them: DCC6 helps most when per-process
launch+teardown is a large fraction of the work, and washes out when it is not.)

### VMA collapse (single heavy clang, host-side `/proc/<pid>/maps` sampling, 40 samples/arm)
| Arm | cc1 maps lines (max) | median |
|---|---|---|
| OFF | **193** | 192 |
| ON  | **65** | 65 |

**−66% VMAs, and this is also the DCC-engagement proof.** dyld's DCC log goes to syslog (not the
guest pipe), so logs can't confirm engagement; the collapsed maps count can, and does — ON at 65
means the 40-dylib/120-VMA closure was served from the 3 DCC regions. A silent fallback would
have shown ~190. Matches the milestone's cc1 ~190 → ~65.

### Correctness
- Both arms: **500/500 objects**, all valid Mach-O 64-bit x86_64.
- **Objects byte-identical OFF vs ON** (spot-checked u1/u100/u250 md5:
  391bc704 / 8fe03894 / 988c4ed7 identical both arms) — DCC6 does not perturb codegen.
- No `_flockfile`/`__text` crash; env true=0 / false=1; sh/clang all green (from #107, re-confirmed).

## exit_mmap / teardown bucket — MEASURED DIRECTLY (perf#24g-teardown, sudo/bpftrace)
Measured on the host with bpftrace kprobes filtered to guest processes (host `comm == "mldr"`;
every guest process, incl. cc1, keeps the launcher's comm). One full 500-file build per arm,
same dyld/cache/install_root. `exit_mmap` latency = kprobe→kretprobe delta.

| kernel teardown metric | OFF | ON (DCC6) | delta |
|---|---|---|---|
| exit_mmap calls (= process exits) | 2031 | 2031 | same (identical process population) |
| **exit_mmap total CPU** | **1.080 s** | **0.888 s** | **−17.7 % (−191 ms / build)** |
| exit_mmap mean / exit | 532 µs | 437 µs | −17.7 % |
| **unmap_vmas / free_pgtables calls** | **150 239** | **32 584** | **−78.3 %** (per-exit 74 → 16 VMAs) |
| zap_pte_range calls | 551 217 | 185 600 | −66.3 % (per-exit 271 → 91) |
| exit_mmap latency hist | mass in 256K–1M ns | shifts down into 64K–256K ns | lighter tail |

Reading these honestly:
- **`unmap_vmas`/`free_pgtables` = the VMA count torn down: −78 %** (74 → 16 per process). This is
  the VMA collapse measured at the kernel — it matches the `/proc/maps` collapse (193 → 65, −66 %)
  and confirms the mechanism from the mm side, not just from `/proc`.
- **`zap_pte_range` −66 %**: far fewer page-table ranges to walk per teardown.
- **But `exit_mmap` CPU only −17.7 %**, not −78 %. Teardown cost is *not* purely per-VMA: the
  resident pages a process actually touched still have to be zapped, and although the DCC regions
  are shared/faulted-once, each process still unmaps its own view of them. So the teardown win is
  **real and now quantified (191 ms per 500-file build)** but **sublinear in VMA count** — the same
  physics as perf#24a's "minor faults ≈ unchanged." Still, 191 ms is ~1.4 % of the 14 s build; on
  this CPU-bound workload it does not move wall (OFF == ON), consistent with §Wall above. It
  surfaces as wall only when process launch/teardown is a large fraction of the work (milestone
  200-tiny-file case, −17 % wall).

## Acceptance (perf#24g)
- [x] Build succeeds OFF and ON (500 objs each).
- [x] Object outputs valid (Mach-O) and byte-identical OFF vs ON.
- [x] VMA collapse persists on the larger build (193 → 65, −66 %); confirms DCC engaged.
- [x] Teardown/mm bucket: measured DIRECTLY with bpftrace (sudo authorized) — exit_mmap CPU
      1.080 s → 0.888 s (−17.7 %, −191 ms/build); unmap_vmas −78 %; zap_pte_range −66 %.
- [x] Wall/sys deltas reported honestly: **wall OFF = ON at this file size (CPU-bound)**; the win
      is VMA/teardown, which this workload does not stress.
- [x] Prod restored byte-identical (dyld 79b22273 both copies); `west darling-doctor` ALL GREEN.

## Interpretation / what perf#24g establishes
DCC6's mechanism (VMA collapse) is **robust across build size** — it held at 500 files exactly as
at 40 dylibs. Its *wall* benefit is **conditional on the workload's launch/teardown fraction**:
- Big win when per-process setup/teardown dominates (many short processes) — milestone 200-file
  case, −17 % wall.
- No wall change when compile CPU dominates (fewer, heavier TUs) — this 500-file case, 0 % wall.
This is the expected shape and argues that the next lever for real builds is **compiler-CPU /
process-count**, not more mapping work. It does NOT argue for default-on (still a prototype).

## Do-not (held)
No DCC6 builder/reader change; no init laziness; no ring/psynch/fd-band touch; DCC not made
default-on; packer untouched; no additional dyld paths rewritten.

## Provenance (LOCAL only)
Same binaries as perf#24f #107: dyld aec6a4a, darlingserver 0e3e6c5/10bc4fa/fce321f, cache
full.dcc6 md5 49e652d1. Measurement harness in job tmp (g24 workload, arm.sh foreground driver,
vma_measure.sh). GOTCHA recorded: `darling shell` launched from a *backgrounded* subshell fails
with "Cannot open mnt namespace" — the guest build must be invoked in the foreground; and
guest `date` has no `%N` (use integer `%s`). Prod dyld 79b22273 both / dserver 835946f9 / mldr
f0cd2a82 restored byte-identical; doctor ALL GREEN.
