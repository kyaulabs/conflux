# Analysis-process exit observation — 2026-10-28

The original analysis RSS sampler rejected all 64 controlled process exits across the laptop and Apollo. The isolated correction accepted all 64 after observing terminal state on the same process lifetime. Ten regression checks passed. This verifies the generated exit boundary; the earlier 24-case analysis failures remain failed, and no musical, latency or release gate is closed.

Each case first kept a child alive behind a pipe handshake and verified positive RSS. After releasing it to exit, the original sampler read two nonterminal process states with no `VmRSS`, then raised `Live owned RSS missing`. The recorded identity remained unchanged and each child subsequently returned zero and was reaped. A preliminary probe also reproduced 32 failures per host; its missing live-memory control is explicit in the retained setup evidence. The table uses the later comparison with a live-memory control in every case.

| Generated comparison | Original sampler | Candidate sampler |
| --- | ---: | ---: |
| Laptop execution sandbox | 32/32 rejected exits | 32/32 observed terminal state and completed |
| Apollo private CPU container | 32/32 rejected exits | 32/32 observed terminal state and completed |
| Positive live-RSS controls | 64/64 passed | 64/64 passed |
| Child exit/reaping | 64/64 clean | 64/64 clean |

The sequence is consistent with Linux's implementation: [`exit_mm()` precedes `exit_notify()`](https://github.com/torvalds/linux/blob/v6.18/kernel/exit.c#L900), and [`proc_pid_status()` emits memory fields only when it obtains an address space](https://github.com/torvalds/linux/blob/v6.18/fs/proc/array.c#L414). A running state and missing memory fields can overlap during exit. This source reference explains the generated observation; the original corpus receipt did not retain those states, so its exact failure sequence remains unproven.

The candidate rechecks missing RSS within a requested 20 ms window, with at most 32 reads per process. It clamps that window to the original absolute analysis deadline. Every observation revalidates PID, start ticks, process group and session. It accepts a real RSS value or a witnessed terminal state; an absent process retains the original disappearance handling. Missing live RSS at the bound, malformed values, PID/group/session changes and unproved ownership still fail. A later valid RSS value is counted rather than replaced with zero. The window is a scheduling budget, not a hard per-call wall-time guarantee.

Two focused regressions failed against the original sampler. All ten candidate checks passed, covering delayed terminal state, RSS restoration, permanently missing live RSS, a stalled clock/read cap, the original deadline, PID/group/session replacement, malformed or duplicate memory fields, missing state and ownership loss. The candidate also forwards the existing whole-command deadline at its runner call site. Its full model/corpus path has not run.

Both generated comparisons used the same probe with sequential original and candidate arms per host. They ran without media, model inference, GPU work or audio. The laptop ran inside the command execution sandbox. Apollo used CONFLUX's private Docker daemon and pinned image, one CPU quota, 256 MiB memory, 32 PIDs, no network, read-only inputs/root and dropped capabilities. Its exact owned container was removed; no default Docker, Kairos or Hexforged resource changed. All 128 paired-comparison children were reaped. Elapsed times are execution-budget records, not cross-host performance measurements.

The [evidence manifest](data/analysis-rss-exit-2026-10-28/manifest.json) binds the original/candidate source, readable [correction patch](data/analysis-rss-exit-2026-10-28/candidate.patch), probes, controls, tests, raw process observations, summaries and cleanup receipt. No old source or failed receipt was rewritten. The application and the frozen player build remain unchanged; the corrected private sampler requires a new prospective analysis plan before use. Current beat accuracy stays 9/24, the Best-quality laptop service target stays strictly below 4.095 seconds at p99, and M2/M3/0.1.0 remain open.
