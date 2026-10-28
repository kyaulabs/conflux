# Numerical reference for the music key candidate

The exact Winterreise weight set used in the SC06 and FMAK diagnostics passed **99/99 generated layer/output comparisons** against TensorFlow 1.15.2. Normalization matched bit for bit, and all four top classes agreed at batch sizes 1, 2 and 4. This supplies a numerical reference for that weight set; musical accuracy, confidence and production use remain unqualified.

| Check | Result |
|---|---|
| Model | `winterreise_n_fold16`, SHA-256 `b425f2e8d5165415c8863b6666aa82d7573121ee675bd2fd8b59d7594f5f11e9` |
| Audited graph | 40 built-in layers; 74 raw float tensors; 294,160 parameters |
| Generated input | Four float64 `168×60×1` windows |
| Whole-population normalization through float16 | Bit-exact normalized float32 input |
| TensorFlow layer/output comparisons | 99/99 passed |
| Laptop candidate batching comparisons | 99/99 passed |
| Intermediate tolerance | `atol=2e-5, rtol=2e-5` |
| Posterior tolerance | `atol=2e-6, rtol=2e-6` |
| Largest independently checked posterior difference | `1.3113022e-6` |
| Top-one agreement | All four windows, every batch size |

The zero, frequency/time ramp, asymmetric-impulse and seeded-uniform fixtures reuse the earlier numerical screen's definitions. Their feature population, expected outputs and tolerances were frozen before reference results. No music, annotation, CQT extraction, local harmonic boundary or confidence threshold was evaluated. The tolerances were not changed after seeing a result.

The retained candidate ran with NumPy 1.23.5 on the laptop. Apollo reconstructed the original graph using `tensorflow.python.keras` in Python 3.7.17, TensorFlow 1.15.2 and NumPy 1.16.0, then assigned every audited raw tensor. It did not deserialize the HDF5 model or execute a custom serialized class. Both host checks were dispatched concurrently; this was a correctness comparison, not a cross-host performance benchmark. Separate posterior checks verified the original bound and top classes for all three batches.

The first Apollo attempt stopped before inference because the earlier checker's class allowlist omitted `GlorotUniform`, which this model's configuration declares. A metadata check confirmed that TensorFlow 1.15.2 supplies and deserializes that built-in initializer with the recorded configuration. The corrected checker adds only that class. It changes no graph, weights, inputs, tolerance or model-selection rule. The initial failure and original checker remain in the evidence.

The corrected reference completed in 6.484 seconds inside an 8.372-second owner run. The container had one CPU core, 2 GiB memory, 128 PIDs, no GPU, no network, read-only input/root mounts, dropped capabilities and user 1000. Execution had a 120-second outer bound. All 113 input files retained their hashes, and the exact owned containers were removed from CONFLUX's private Docker daemon. No laptop physical-test readiness, audio device, power-profile setting or protected Kairos/Hexforged resource was used or changed.

This is a different weight set from the [DeepSpec reference](keycnn-tf115-reference-2026-10-28.md). That earlier 96/99 full-graph failure remains unchanged. Its smaller final-probability differences do not qualify the Winterreise model, and this result does not qualify DeepSpec.

Numerical agreement does not improve the existing musical scores: [79.82% on two exposed SC06 recordings](key-cnn-diagnostic-2026-10-27.md) and [47.62% across 21 inferred FMAK tracks](key-cnn-fmak-2026-10-27.md), or 41.67% with the three source failures retained. Training membership, independent material, source-time behavior and calibrated confidence still need validation. The production key backend stays disabled and confidence remains Unknown.

The [evidence manifest](data/keycnn-winterreise-reference-2026-10-28/manifest.json) retains the frozen plans, source, full comparison results, initializer correction, independent posterior checks and cleanup receipts. Weights, numeric arrays, runtimes and images are omitted. No latency, cancellation, sustained audio/UI or release gate is closed by this numerical reference.
