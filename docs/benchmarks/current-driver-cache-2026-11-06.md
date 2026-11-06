# Current-driver shader-cache comparison

All eight complete Best-quality outputs matched their accepted per-host hashes, and all four native processes exited with code 0. The laptop still stalled after receiving a current-driver cache. This silent raw-runtime comparison supplies no player-service p99, audio/UI continuity or cancellation qualification; the strict target remains below 4.095 seconds at p99.

| Host and cache | First request | Second request |
| --- | ---: | ---: |
| Laptop, empty | 7.070 s | 1.908 s |
| Laptop, retained in a new process | 18.253 s | 15.532 s |
| Apollo, empty | 16.763 s | 0.670 s |
| Apollo, retained in a new process | 0.679 s | 0.702 s |

Both arms used identical per-host runtime bytes, the same Best model, two CPU threads, passive OpenMP waiting and the same two frozen Club XL inputs. Submission serialization was disabled in both arms. The runtime hashes are `b71e0ec5decb72ec42b752a0a1c5463c8664e298a47fa7468e072b3d3797640d` on Intel and `ebcc2a0576c9ca07720ad91316aa66e35b88f9f6fd4ee84b640ee9e6277ee43b` on NVIDIA. The model is unchanged at `f8c54ba35df95aafea7881eac214b58ec18a73cbcb6ff5a90f4b762631a286b6`. No runtime candidate was adopted.

The old Intel seed contained 64 shader entries from build `6ce609c1a667be4f4358bbbbdc5dd02a7ad6a5dd`. The installed Mesa 26.2.6 Intel build is `75a40bb22a8833b077a212b1a91afb83d2db5552`. The new seed contains 62 entries matching the installed build, renderer and pointer size. The read-only audit rejects the old seed for current-driver use. Matching headers do not prove cache hits or validate every compiler option; the old seed was not rewritten.

Both laptop processes started in thermal Power Saver with a 7.5 W MMIO long-term allowance, rather than the normal 15 W Balanced setting. The retained process used 0.857 and 0.277 seconds of inference CPU time, while its graph-compute wall intervals were 17.328 and 15.100 seconds. First-request CPU compilation alone therefore does not explain the remaining stall. These observations do not distinguish GPU execution, fence/queue waits, clock state or power effects, and do not establish that the laptop cannot meet the target.

The original laptop collector failed because [Mesa cache hit/miss statistics](https://docs.mesa3d.org/envvars.html#envvar-MESA_SHADER_CACHE_SHOW_STATS) were absent at native exit. That failed record remains unchanged. Only the two remaining authorized laptop requests were then completed within the same original twenty-minute deadline; no cold requests were repeated. The continuation ledger links its reused cold outputs to the original reaping proof. NVIDIA counters were also unavailable. No hit count was inferred from copied cache files.

Sampled laptop maxima during native work were 72°C for the empty-cache process and 52°C for the retained process. The earlier settling interval reached 95°C before native inference began. Two-second samples cannot exclude shorter peaks. The thermal guard, power settings and OneDrive were unchanged. This diagnostic admitted Power Saver and cannot qualify the normal Balanced configuration.

All model work finished inside the operator window. The empty Apollo service metadata was retired at 16:40:57 UTC, after the 16:39:22 deadline; its native processes and owned container were already reaped. The whole cleanup must not be described as finishing within the window. Completed staging was subsequently removed on both hosts after digest verification, retaining compact evidence and the small current-driver seeds needed for open work.

The [manifest](data/current-driver-cache-2026-11-06/verification.json) binds 84 members, including original failures, output hashes, phase timings, cache inventories, host samples, the remaining-request continuation and cleanup receipts. It also binds the unchanged 39-member Apollo archive. Media, weights, executables and generated cache contents are excluded. The offline verifier checks all digests and recomputes the reported output, thermal, power and lifecycle findings without extraction, network access or model execution:

```sh
python3 -B docs/benchmarks/data/current-driver-cache-2026-11-06/verify_evidence.py
```

It reuses the adjacent BRID report's pinned bounded archive loader and its existing Python/NumPy environment. The [reproduction result](data/current-driver-cache-2026-11-06/reproduction.json) is retained. M2/M3, automatic musical-analysis confidence/fallback and current primary-laptop sustained qualification remain open before `0.1.0`.
