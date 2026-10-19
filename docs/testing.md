# Testing and coverage

Aim for more than 80% line coverage of CONFLUX code. Add tests for observable
functionality, security boundaries and failures. Do not add assertions solely
to raise a percentage or exclude working application code to make the target
pass. Report uncovered behavior and unmeasured source explicitly. Branch
coverage is reported separately from line coverage.

For new behavior and bug fixes, start with a failing behavior test when
practical, then implement the change and check the relevant regressions.
Existing work has regression tests; it was not all written with strict
test-first TDD. UI, listening and hardware checks remain necessary where a
headless test cannot establish the result.

The normal build and tests are described in the [README](../README.md).
Coverage uses a separate GCC Debug build with matching `gcov`, gcovr and
coverage.py. These are development tools, not application dependencies.
Install the normal build dependencies, Python's venv support and NumPy, then:

```sh
python3 -m venv --system-site-packages build/coverage-venv
build/coverage-venv/bin/python -m pip install -r tools/coverage/requirements.txt
build/coverage-venv/bin/python tools/coverage/run.py --private-routing
```

`--private-routing` also requires `pipewire` and `pw-dump` (`pipewire-bin` on
Ubuntu). It runs the routing, cue-loss, recorder, source-handoff and preparation
controller checks on private virtual daemons. The preparation check verifies
queued-completion cancellation, exact peer-deck samples, cached retry and
immediate shutdown from a completion notification. It uses no hardware output
or host session manager. The coverage run selects functional-only mode: sample, routing,
buffer, command and cleanup assertions remain active; execution-deadline
checks are skipped and reported as such. Normal private-routing checks retain
their deadline gates. Functional runs allow a 240-second harness guard for
instrumented preparation; the normal guard remains 90 seconds.

The prepared-bank check starts its master recording before expensive imports
and compares every steady frame, plus the settled tail after an explicit routing
rebuild, with independent asymmetric samples at 1e-6 tolerance. Functional-only
mode requires virtual sink delivery and reports all sink discrepancies separately;
its sink sample-quality gate is disabled. Normal mode keeps that gate. Source
clocks, ownership, cancellation, read health and recorded samples remain strict in
both modes. These checks do not establish physical audio continuity.
The routing handoff check also keeps nonzero receiver samples exact in both
modes; only its downstream receiver-zero gate follows execution mode. It reports
the raw operands and still checks every recorded master frame. The latest normal
routing run passed all 130 checks with zero receiver faults or deadline misses.

The runner resets counters and reports in `build/coverage`, builds the
instrumented executables and runs the existing CTest suite and offscreen smoke
check. Reports are written under `build/coverage/reports`:

- `summary.json` records raw line/branch counts, test results, scope gaps and
  whether each measured line ratio is strictly greater than 80%.
- `cpp.html` and `python-html/index.html` show source-level coverage.
- JSON and Cobertura XML reports support further inspection and CI tools.
- Test logs and private-routing evidence remain available after failures.
  Generated recordings stay out of the uploaded reports.

CI runs coverage as a separate job and uploads the reports even when a stage
fails. Build, test and measurement errors fail the job. The initial coverage
target is reported without enforcement while existing gaps are addressed; a
green job does not mean the 80% target has been met.

The C++ report includes `src/`, its entrypoints and instrumented inline headers.
Unexecuted compiled files remain in the denominator. Missing implementation
files make the measurement incomplete. The Python report measures
`tools/stems/`, including the supervisor's process and failure handling.
Tests, benchmark/experiment tools, build helpers and brand-generation tools
are separate support code and do not inflate these product percentages.

QML, including the shipped `brand/integration/qtquick/ConfluxTheme.qml`, has
functional UI tests but no validated numeric coverage measurement yet. The
native stem adapter in `tools/runtime/htdemucs_segment.cpp` is built separately
and also remains unmeasured. Python fake-child tests exercise the supervisor's
protocol handling, not that native adapter's implementation. There is no
whole-product coverage percentage until these gaps are resolved.

The [M2 live controller measurement](benchmarks/data/m2-controller-live-2026-10-19/coverage.json)
passed 36/36 CTest entries, the offscreen smoke, 124 private-routing checks and
75 private preparation/library checks. Nine isolated normal-build CLI checks
also passed. It measured the following with GCC/gcov 16.2.1,
gcovr 8.7 and coverage.py 7.16.2:

| Scope | Lines | Branches |
| --- | --- | --- |
| C++ `src/` | 5,647 / 6,574 (85.90%) | 8,088 / 15,293 (52.89%) |
| Python `tools/stems/` | 1,235 / 1,354 (91.21%) | 430 / 526 (81.75%) |

Both measured line ratios now exceed 80%. C++ increased from 73.74% in the
[initial baseline](benchmarks/data/coverage-baseline-2026-10-18.json). Its largest
uncovered areas remain the standalone probe and controller. Core render tests
cover every reported render-core line, including transport, mixing and ramps;
that narrower result does not replace the full C++ denominator. Coverage
counts depend on the compiler and configuration, so compare results with their
recorded toolchain.

Coverage instrumentation changes execution costs. Never use this build for
callback latency, live-stem service targets, thermal qualification or audio
acceptance. The retained M0/M1 benchmark evidence belongs to its pinned normal
builds.

The [receiver follow-up](benchmarks/data/m2-controller-live-2026-10-19/ci-followup/verification.json)
preserves a later coverage CI failure and the helper's single-capture correction.
Its exact nonzero oracle and existing zero policies remain intact. Revised
normal/instrumented private runs passed 130/124 checks; product source is
unchanged, so these helper checks add no new coverage percentage.

The additional bridge ThreadSanitizer run failed in QtTest watchdog teardown
after 53 observed PASS lines, before final Qt totals. A standalone Qt-only
control reproduced the same report. The
[verification record](benchmarks/data/m2-live-bridge-2026-10-19/verification.json)
retains the failed runs and installed-library evidence; this is an incomplete
sanitizer result. No suppression or watchdog disabling was used. It does not
change the passing normal and coverage results above.

The session-priming run passed ThreadSanitizer for the two existing pure C++
owner/recovery targets. That scope excludes its three new Qt integration cases,
which exercise synthetic service traces and unstarted `LiveSession` constructors.
The [session verification record](benchmarks/data/m2-session-prime-2026-10-19/verification.json)
links the earlier sanitizer limitation without rerunning or reclassifying it.

The converter's first instrumented run hit its 240-second test guard. Its normal
run had passed. The test now checks the advertised output plus adjacent overwrite
sentinels, exercises tiny output partitions on short input and compares every
sample of long seek tails using full output blocks. Product code and the timeout
are unchanged; the instrumented test passed in 84.19 seconds. The verification
record retains the timeout, original source pins and exact test-only change.
