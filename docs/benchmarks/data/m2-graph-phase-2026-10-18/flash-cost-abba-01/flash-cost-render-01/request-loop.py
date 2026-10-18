def run_runtime(label, executable, cases, baseline_hashes, np, NativeRuntime, args, output, report, deadline):
    record = {"label": label, "executable": str(executable), "requests": [], "cleanup": {"complete": False}}
    report["runtimes"].append(record)
    native = None
    report["native_started"] = True  # Conservative even if construction fails.
    try:
        native = NativeRuntime(executable, MODEL, "vulkan", THREADS,
                               min(args.request_timeout, check_deadline(deadline)), passive_openmp=True)
        record["pid"] = native.process.pid
        record["temporary_directory"] = str(native.root)
        started = time.monotonic()
        ready = native.start()
        record["startup_wall_ms"] = (time.monotonic() - started) * 1000
        record["ready"] = ready
        require(ready.get("backend") == "vulkan" and ready.get("threads") == THREADS
                and ready.get("revision") == REVISION and ready.get("model") == str(MODEL)
                and ready.get("pid") == native.process.pid and ready.get("weight_storage") == "native",
                "native ready backend/thread/model/revision/PID differs")
        require(ready.get("fence_wait_mode") == "blocking", "native fence wait mode is not blocking")
        require(ready.get("process_environment") == {"OMP_WAIT_POLICY": "PASSIVE", "GOMP_SPINCOUNT": "0"},
                "native child passive OpenMP settings differ")
        validate_number(ready, "load_ms")
        checkpoint(output, report)
        for request_id, case in enumerate(cases, 1):
            native.timeout = min(args.request_timeout, check_deadline(deadline))
            samples = case["samples"]
            row = {key: value for key, value in case.items() if key != "samples"}
            row.update(request_id=request_id, input_sha_match=False, output_matches_arm_proof=False,
                       phase_fields_valid=False)
            record["requests"].append(row)
            produced = None
            try:
                started = time.monotonic()
                produced, metrics = native.infer(samples, request_id)
                row["request_wall_ms"] = (time.monotonic() - started) * 1000
                row["native_metrics"] = metrics
                for name in COMMON_TIMINGS:
                    validate_number(metrics, name)
                if label == "diagnostic":
                    for name in PHASE_TIMINGS:
                        validate_number(metrics, name)
                    for name in SWITCH_COUNTS:
                        validate_number(metrics, name, integral=True)
                    row["phase_fields_valid"] = True
                require(produced.shape == (4, WINDOW, CHANNELS) and produced.dtype.str == "<f4"
                        and produced.flags.c_contiguous and np.isfinite(produced).all(),
                        "native output geometry/dtype/finiteness differs")
                written, _ = inspect_file(native.root / "input.f32", WINDOW * CHANNELS * 4,
                                          deadline, expected=case["input_sha256"])
                require(written["bytes"] == WINDOW * CHANNELS * 4
                        and hashlib.sha256(memoryview(samples)).hexdigest() == case["input_sha256"],
                        "normalized input changed or native input file differs")
                row["input_sha_match"] = True
                row["output_shape"], row["output_dtype"] = list(produced.shape), "<f4"
                row["output_bytes"] = produced.nbytes
                row["output_sha256"] = hashlib.sha256(memoryview(produced)).hexdigest()
                if label == "baseline":
                    baseline_hashes[case["id"]] = row["output_sha256"]
                else:
                    row["output_matches_arm_proof"] = row["output_sha256"] == baseline_hashes[case["id"]]
                    require(row["output_matches_arm_proof"], f"PCM differs from closed arm proof for {case['id']}")
                row["completed"] = True
                report["completed_cases_per_runtime"][label] += 1
                report["total_completed_requests"] += 1
            finally:
                produced = None
                # No separated music/raw buffers are retained in evidence.
                for name in ("input.f32", "output.f32"):
                    (native.root / name).unlink(missing_ok=True)
                checkpoint(output, report)
    finally:
        # Repeated SIGINT/SIGTERM must not interrupt bounded child reaping.
        handlers = {sig: signal.signal(sig, signal.SIG_IGN) for sig in (signal.SIGINT, signal.SIGTERM)}
        try:
            if native is not None:
                closed = False
                try:
                    try:
                        record["stderr"] = save_stderr(native, output, label)
                    finally:
                        native.close()
                        closed = True
                finally:
                    record["cleanup"] = {"complete": closed and native.process.poll() is not None and not native.root.exists(),
                                         "close_returned": closed, "reaped": native.process.poll() is not None,
                                         "returncode": native.process.returncode,
                                         "temporary_directory_removed": not native.root.exists()}
                require(record["cleanup"]["complete"], "native cleanup incomplete")
        finally:
            for sig, handler in handlers.items():
                signal.signal(sig, handler)
            checkpoint(output, report)
