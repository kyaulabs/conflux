# Resident queue boundary and worker cancellation

The processed resident-loop reader can return a short prefix even when all
requested PCM is queued. The baseline is `9d079f2`; the correction permits one
additional bounded reader step. Best quality, the strict 4.095-second service-p99
target and the laptop release gate are unchanged.

The reader handles up to eight queued output slots. Loop OUT can split one slot
into two reads without retiring it, so consuming those eight slots may require
nine steps. A source read is at most 1,024 frames; the approved range is at least
2,048 frames and the nominal step is at most 1.06. It can therefore cross at most
one OUT. The correction keeps a fixed nine-step bound over the same eight-slot
queue. The callback reads cached PCM through the existing gain mix.

The isolated regression starts with a healthy active 3,072-frame resident range
at +6% with Master Tempo enabled. After joining its private output producer, it
inserts eight valid current-generation slots containing
`28, 34, 35, 35, 35, 35, 35, 19` frames, beginning near OUT. Their complete
256-frame prefix is immutable and available before the read. Only the selected
drum stem is mixed at gain 0.5. The fixture compares every returned sample and
checks the parked fractional cursor, active hold and producer retirement.

| Host | Original eight-step reader | Corrected nine-step reader |
| --- | --- | --- |
| Laptop | Returned 237; 19 valid frames remained queued | Returned all 256; none remained |
| Apollo | Returned 237; 19 valid frames remained queued | Returned all 256; none remained |

The original tests failed before the correction; all returned prefix samples,
transport state and cleanup were otherwise correct. The fixed tests passed.
This fixture establishes a valid queue-shape bug. It does not show that the real
DSP produced this exact slot shape during the earlier laptop timeout failure.
That original result, with 19 added starved frames at first failure and 51
including teardown, remains retained and requires current-code validation.

Before the correction, a separate 24-case matrix passed on each host. It also
passed with the correction, alongside the new fragment regression: 25 cases per
host. The matrix covers 2,048-, 2,049-, 3,071-, 3,072-, 8,191- and 8,192-frame
ranges; ±6% tempo; Master Tempo off/on; and callback lengths of
1, 7, 31, 237, 257, 511, 1,023 and 1,024 frames. A private single-consumer snapshot
requires enough current-generation PCM before each read. Every sample matches
the existing independent selected-stem file-loop reference exactly, with at
least four nominal wraps per case. Each host validates 557,036 matrix frames
plus the 256-frame regression. These CPU-only checks neither play audio nor
qualify realtime scheduling or latency. Original product archives are preserved.

The separate silent worker check observes an unfinished submitted Vulkan fence
wait in a privately instrumented runtime, then calls hard close. One fresh
Best-quality request ran on each host concurrently.

| Host | Hard-close duration | Result |
| --- | --- | --- |
| Laptop, Intel Vulkan | 88.965 ms | Worker killed and reaped; no partial chunk |
| Apollo, NVIDIA Vulkan | 204.116 ms | Worker killed and reaped; no partial chunk |

Both workers returned SIGKILL, left no completion marker for the selected wait,
retired the supervisor, removed the spool and left the ready queue empty. Exact
owner-unit and container cleanup is recorded. This qualifies the instrumented
worker interruption case; full-player shutdown, GPU-command preemption and
latency remain separate gates. No audio was played. Power limits, the thermal
guard and OneDrive were unchanged.

The first laptop cancellation attempt remains failed: its observation check
rejected the request and did not record the individual failed predicate. The
revised check records each predicate and permits incidental input-file reads
while still requiring the pinned native fence marker. The successful laptop run
observed two input reads; Apollo observed one. All 40 revised offline cases and
both unarmed host-entry checks passed. A real desktop fixture also reproduced
KDE's masked child exit status and verified that the revised owner propagates
the test result to systemd.

Local evidence is retained under `build/m2-fence-cancel-01/packet-01` and
`packet-02`, and under `build/m3-tempo-continuation-01/` in
`resident-buffered-boundary-01`, `resident-fragment-boundary-01` and
`resident-fragment-fixed-01`. Each contains pinned inputs, original results and
owner receipts. Private queue-observation/injection methods are excluded from
the product change.

A fresh Release (`-O2 -DNDEBUG`) product build, excluding the private test
methods, passed all 60 CTest targets with offscreen/software UI in 235.68 seconds.
The owner completed within its original ten-minute limit and was removed. This
code-validation run does not qualify actual-player performance.
