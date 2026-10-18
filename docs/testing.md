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
Ubuntu). It runs the existing routing, cue-loss, recorder and source-handoff
checks on a private virtual daemon. It uses no hardware output or host session
manager. The coverage run selects functional-only mode: sample, routing,
buffer, command and cleanup assertions remain active; execution-deadline
checks are skipped and reported as such. Normal private-routing checks retain
their deadline gates.

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

The [18 October baseline](benchmarks/data/coverage-baseline-2026-10-18.json)
passed 22/22 CTest entries, the offscreen smoke and 82 private-routing checks.
It measured the following with GCC/gcov 16.2.1, gcovr 8.7 and coverage.py 7.16.2:

| Scope | Lines | Branches |
| --- | --- | --- |
| C++ `src/` | 2,449 / 3,321 (73.74%) | 2,525 / 5,332 (47.36%) |
| Python `tools/stems/` | 387 / 416 (93.03%) | 89 / 110 (80.91%) |

The C++ line target remains unmet. Its largest uncovered areas are the
standalone probe and successful controller playback paths. Core render tests
cover every reported render-core line, including transport, mixing and ramps;
that narrower result does not replace the full C++ denominator. Coverage
counts depend on the compiler and configuration, so compare results with their
recorded toolchain.

Coverage instrumentation changes execution costs. Never use this build for
callback latency, live-stem service targets, thermal qualification or audio
acceptance. The retained M0/M1 benchmark evidence belongs to its pinned normal
builds.
