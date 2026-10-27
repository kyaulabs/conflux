# Preparation cache revision after MPEG EOF correction

Prepared artifacts now use pipeline revision **2**. The
[MPEG EOF correction](mp3-length-2026-10-27.md) changed how CONFLUX consumes
estimated codec lengths without changing the linked decoder/converter version
strings. Leaving the pipeline at revision 1 could reuse an artifact prepared
under the earlier decode contract.

The existing identity participates directly in normal prepared-audio keys and
in the output-conversion identity of prepared stem banks. Advancing it gives
new requests a keyed miss and a bounded rebuild. Existing playback owners keep
their open immutable PCM; old entries remain subject to the normal cache budget
and eviction rules. Newly prepared revision-2 entries are reusable on subsequent
requests. No manifest schema or compatibility path was added.

Both regressions failed before the revision changed: normal preparation reused
the v1 entry, and read-only stem discovery accepted the v1 bank as compatible.
Afterward, normal preparation rebuilt the entry and stem discovery reported a
miss until regeneration. Held normal/bank owners remained readable with exact
samples; the legacy PCM/manifest stayed unchanged, and a fresh lookup reused
the rebuilt artifact. The complete prepared-track-store suite passed:
**63 results, zero failures**.

The [evidence manifest](data/m2-preparation-revision-2026-10-27/manifest.json)
retains the intentional red, focused/full green results and tested source hashes.
This is a cache-correctness change, with no new laptop latency qualification.
The [FMAK follow-up](key-cnn-fmak-2026-10-27.md) records the separate experimental
key-model result and leaves that candidate disabled.
