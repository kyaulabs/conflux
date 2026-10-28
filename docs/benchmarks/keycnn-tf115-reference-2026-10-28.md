# Legacy TensorFlow numerical reference

The experimental data-only classifier's final probabilities matched TensorFlow 1.15.2 within **5.9604645×10⁻⁸** on four generated feature windows at batch sizes 1, 2 and 4. Normalization matched bit for bit. The stricter full-graph comparison nevertheless **failed**: 96 of 99 intermediate/output comparisons passed, while `Conv8` exceeded the predeclared tolerance in each batch. No tolerance, model or classification threshold was changed after observing the result.

| Check | Result |
|---|---|
| Whole-population float64 normalization through float16 | Bit-exact normalized float32 input |
| Computed layer/output comparisons | 96/99 passed |
| Failed layer | `Conv8`, once per batch size |
| Failed layer maximum absolute difference | 2.6702881×10⁻⁵ |
| Frozen intermediate tolerance | `atol=2e-5, rtol=2e-5` |
| Final probability comparisons | 3/3 passed at `atol=2e-6, rtol=2e-6` |
| Independent top-one check | All four classes match in every batch size |
| Same-input `Conv8` diagnostic | Passed the unchanged intermediate tolerance |

The same-input diagnostic feeds the numeric prototype's exact `BN7` output into the original TensorFlow graph and evaluates `Conv8` through its existing pool/dropout path. Its maximum absolute difference was 2.0980835×10⁻⁵, with no element outside the combined absolute/relative bound. This is consistent with upstream floating-point differences contributing to the full-path failure; it does not prove a unique arithmetic cause, fix the original failure or qualify another model.

This check used the previously audited **DeepSpec** model, SHA-256 `0a99c26b7d8a4f7123b3cdade75c7986ef686d6805378c43cada874497f53f9a`. It did **not** exercise the separate Winterreise model used in [the earlier local SC06 diagnostic](key-cnn-diagnostic-2026-10-27.md), whose file hash begins `b425f2e8`. Generated numerical agreement for one weight set does not validate that other weight set or its musical accuracy.

The frozen plan defines four float64 arrays of shape `168×60×1`: zero, a frequency/time ramp, three asymmetric impulses and seeded uniform values. Together they form the population used for mean/standard-deviation normalization before conversion through float16. All expected intermediate arrays were produced before the reference execution. No music, annotations, key changes, CQT extraction or confidence calibration was measured.

Apollo reconstructed the audited 40-layer built-in graph through `tensorflow.python.keras`, assigned all 74 hashed raw float tensors (290,376 parameters) and compared all 33 computed outputs for each batch size. Serialized classes were restricted to the existing built-in layers and initializers. The checker did not deserialize the HDF5 model or execute custom serialized code. This references the pinned inference framework; it does not reproduce or verify the original training environment or released-weight membership.

The isolated image used Python 3.7.17, TensorFlow 1.15.2, NumPy 1.16.0, Protobuf 3.20.3 and h5py 2.10.0. The pinned base digest, installed dependency report, image identity and full commands are retained. Numerical containers used one CPU core, 2 GiB memory, 128 PIDs, no GPU, no network, a read-only root filesystem, dropped capabilities, `no-new-privileges`, user 1000 and read-only input mounts. Each had a 120-second execution bound. The final run took 8.507 seconds including inspection/removal, with 6.710 seconds reported by the checker.

The first dependency build failed on private-bridge DNS before model work. A second dependency build used host networking and succeeded; numerical execution still had no network. The first numerical attempt failed because old TensorFlow rejects a tensor that is both fed and fetched. Removing that redundant input fetch left the separately verified bit-exact input check and all frozen tolerances intact. The second execution completed the 99 comparisons and failed at `Conv8`; the third preserved that failure and added the same-input diagnostic. These failures remain in the evidence.

All 113 transferred input files retained their hashes. Numerical containers and the exact exited dependency-build container were removed from CONFLUX's private Docker daemon. Nothing named or labelled Kairos or Hexforged was modified, and the default Docker daemon was not used. No laptop physical-test readiness, audio device, power-profile setting or thermal policy was consumed or changed by this reference.

The [evidence manifest](data/keycnn-tf115-reference-2026-10-28/manifest.json) contains the frozen plan, checker and its prior revisions, input hash/shape metadata, dependency/build records, all numerical results, independent probability checks and cleanup receipts. Model weights, generated numeric arrays, images and installed runtimes are excluded. One full container inspection is represented by its identity/state/command/mount subset and original hash; unneeded base-image environment is omitted after a public signing-key fingerprint triggered a scanner false positive. The original receipt and initial scan remain local; no scanner or hook was disabled. The production key backend remains unconfigured and confidence remains **Unknown**. Musical accuracy, model provenance, independent-material validation, local key boundaries, laptop live-stem latency and the sustained player gates remain open; this reference supplies no release qualification.
