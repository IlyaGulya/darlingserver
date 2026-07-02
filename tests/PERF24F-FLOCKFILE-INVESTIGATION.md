# perf#24f-fix-flockfile-datafixup (task #105) — investigation: primary acceptance MET, flockfile narrowed

Status: **partial.** The task #104 elfcalls fix advanced DCC5-ON boot far enough that the ORIGINAL
perf#24f acceptance is now **met**: `/usr/bin/true` exits 0 for real (guest-child rc verified). The
remaining `_flockfile` crash is **stdio-init-specific** (`sh`/bash), NOT `/usr/bin/true`, and this session
eliminated the most likely cause (stdio-global rebase) — narrowing it further. Prod restored byte-identical
(doctor ALL GREEN); dyld fix 7a93752 stays committed; no builder/cache change made.

## PRIMARY ACCEPTANCE MET — DCC5-ON guest-child rc verified via `env` (execs target directly)
```
darling shell env DARLING_DYLD_DCC2=1 DARLING_DYLD_DCC2_PATH=/private/var/root/full.dcc5 /usr/bin/true   => rc 0
darling shell env ... /usr/bin/false                                                                      => rc 1
```
`env` execs the target directly, so the launcher rc IS the guest-child rc (no false-pass). true=0, false=1
proves the guest runs correctly under DCC5 for minimal (non-stdio-heavy) programs. This is the perf#24f
"does /usr/bin/true work" bar.

## The remaining crash is stdio-init-specific (sh/bash), not /usr/bin/true
- `env /usr/bin/true` / `/usr/bin/false`: correct rc, guest runs. `env /usr/bin/echo`: "No such file" (path
  wrong, but env RAN and reported it — guest alive).
- `env /bin/sh -c 'echo SH_WORKS'`: rc 0 but NO stdout — `sh` dies in its stdio-heavy init before the echo.
- The crash (captured task #103/#105): `_flockfile+0x12` (`movq 0x68(%rbx),%rdi`) with RBX=RDI=the FILE*
  arg = 0x6165726874705f6d = ASCII "m_pthrea". Caller = `_fileno`(libsystem_c 0x6c270) → `_flockfile`.
  `fileno`'s caller passed a bad FILE*.

## What "m_pthrea" actually is (identified)
The 8 bytes 0x6165726874705f6d = "m_pthrea" = offset 24 of the load-command path string
`/usr/lib/system/libsystem_pthread.dylib` (the "libsyste**m_pthrea**d." middle). This string appears **23×
in the RX region** (each image's LC_LOAD_DYLIB path in its Mach-O header) and **0× in RW/RO**. So the bad
FILE* is load-command/header bytes read as a pointer — a caller computed a pointer landing inside a
Mach-O header's load-command area.

## Eliminated: the stdio-global rebases are CORRECT (offline fixup-table diff)
Checked libsystem_c's `__stdinp`/`__stdoutp`/`__stderrp`/`__sF` DCC fixups directly (image idx 21, RW
region_off 0x80000, source __DATA vmaddr 0xc5000):
- `__stdoutp` (RW loc_off 0x83898): REBASE, stored *loc = 0x5af4e8 = `&__sF + 0x98` = `&__sF[1]` (FILE is
  0x98 bytes; stdout is index 1). **CORRECT.**
- `__stdinp` = &__sF[0] (0x5af450), `__stderrp` = &__sF[2] (0x5af580). **CORRECT.**
- `__sF` itself: 0 fixups (it is BSS, zero-initialized; populated at runtime by `__sinit`/`__sfp`).
So reading `__stdoutp` yields the correct `&__sF[1]` — NOT "m_pthrea". The bad FILE* is NOT from the stdio
globals. (If `__sinit` hadn't run, `fileno(stdout)` would read 0, not a load-command string.)

## Where the "m_pthrea" FILE* comes from — the open lead
`fileno` was called with a FILE* that is load-command-string bytes. Candidates for the next agent:
- A caller iterating a table/array whose base is a mis-rebased pointer into the Mach-O header (a REBASE or
  BIND whose target resolved into the header/load-command region instead of the intended `__DATA` object).
- The `sh`/bash-specific stdio path: bash sets up its own FILE* wrappers; check whether a bash `__DATA`
  pointer (not a libsystem_c global) is mis-fixed. NOTE: bash is the MAIN EXECUTABLE, which is NOT in the
  DCC cache (only the 40-dylib closure is) — so its own fixups are done by normal dyld, not DCC. That means
  the bad pointer more likely comes from a CACHED dylib's __DATA that bash reads via a libsystem_c/dyld API.
- Re-capture with a reliable frame-walk: the temp dserver crashcap (dtape_thread_process_signal) needs to
  fire on the `sh` crash. GOTCHA this session: the static one-shot latch was consumed by earlier runs
  (dserver persists across guest launches); use a fresh dserver AND filter by RBX==0x6165726874705f6d, OR
  reset the latch each guest exec. The frame-walk (RBP chain) will give the exact caller of `fileno` and the
  slot it loaded the FILE* from — that pins the mis-fixed slot deterministically.

## Method notes (for the next agent)
- Reliable guest-child rc: use `env <prog>` (execs directly) — launcher rc == guest rc. `sh -c '...; echo
  RC=$?'` is unreliable when sh itself dies in init (no output, rc swallowed).
- Address classifier: RXoff = addr - arena_rx_base (arena base from the crashcap region dump; RX region
  file_off 0x114000, size 0x52c000); source vmaddr = RXoff - seg region_off (via full.dcc5 image table,
  classify.py in job tmp).
- Fixup-table lookup: full.dcc5 header at 0, image table at 424 (768B each), fixups at header.fixup_off
  (40B each: kind/loc_region/tgt_region/pad/image_index/loc_off/tgt_off/addend/extern_sym). Per-image
  fixup range is image.fixup_first..+fixup_count.

## Boundary / honest status
- [x] Primary perf#24f acceptance: /usr/bin/true exits 0 (guest-child rc), /usr/bin/false exits 1.
- [x] Bad pointer identity: load-command path-string bytes "m_pthrea", read as a FILE* by fileno→flockfile.
- [x] Eliminated: stdio-global (__stdinp/out/err/__sF) rebases are correct.
- [ ] Exact mis-fixed slot / caller of fileno: NOT yet pinned (needs a clean frame-walk on the sh crash).
- [ ] Fix + RED gate + clang A/B: pending the slot identification.
This is a distinct, narrower follow-up; the crash does NOT block /usr/bin/true (which passes).
