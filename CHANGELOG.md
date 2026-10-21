# Changelog

Changes follow [Conventional Commits](https://www.conventionalcommits.org/en/v1.0.0/)
and [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## Unreleased

### Features
- **m0:** Add native build and audio feasibility harness ([7cda380](https://github.com/kyaulabs/conflux/commit/7cda38049d6d9d57aecb1c87721e768eaff8911a))
- **m0:** Add persistent cancellable stem worker ([a484a72](https://github.com/kyaulabs/conflux/commit/a484a7214ab61ddb6dd34036414e91cffb88ac41))
- **m0:** Add controlled repaint cadence probe ([d00692f](https://github.com/kyaulabs/conflux/commit/d00692ffe1b7654a9946b5e2ed84f8c044586fc2))
- **m0:** Soak prepared music sources ([850225c](https://github.com/kyaulabs/conflux/commit/850225c998b3af40d27d21e496178bed7eb91d7f))
- **m0:** Pace stem delivery on a fixed playback clock ([2223d6a](https://github.com/kyaulabs/conflux/commit/2223d6ae5e27d0c9e4b357b4a5dc70d51daef086))
- **audio:** Begin M1 with decoded file rendering ([7000f37](https://github.com/kyaulabs/conflux/commit/7000f3777574d34d7394a32982c8321607f15b49))
- **audio:** Add two-deck mixing and queued transport ([5e8312f](https://github.com/kyaulabs/conflux/commit/5e8312f068d587eb02d54ccf0a2aa4c42644babc))
- **ui:** Connect manual decks to live audio ([a3d501a](https://github.com/kyaulabs/conflux/commit/a3d501ab9ba192578e3a898b240e4926d673be6d))
- **ui:** Adopt CDJ-3000 deck and mixer layout ([e1a172d](https://github.com/kyaulabs/conflux/commit/e1a172dbdc7a2338b057ca00a88b7120a5b8b708))
- **audio:** Implement temporary deck cue transport ([98795bb](https://github.com/kyaulabs/conflux/commit/98795bbc55b699f675c0712d82fb343febdc0ae5))
- **audio:** Stream prepared tracks through bounded read-ahead ([83ce168](https://github.com/kyaulabs/conflux/commit/83ce168e0ea026dbef9abb15e91daa1f3c1a45df))
- **audio:** Route master and cue with metering and live recording ([f16fa6d](https://github.com/kyaulabs/conflux/commit/f16fa6d660ab4b5a285c67d33199af3f6dc9aa65))
- **audio:** Keep peer playback running through deck replacement ([26d6ff8](https://github.com/kyaulabs/conflux/commit/26d6ff8d6472bd592f15ddabdd10628e3718048f))
- **library:** Persist prepared audio and support cancellation ([9272e60](https://github.com/kyaulabs/conflux/commit/9272e60c6174e066c7dcb2c0b9039a943a092a9d))
- **library:** Import and load a persistent local catalog ([c3bcdcc](https://github.com/kyaulabs/conflux/commit/c3bcdccbd7175b0390a06505eaf838be86e1d4dd))
- **stems:** Add opt-in full-F32 Vulkan Flash build ([4334995](https://github.com/kyaulabs/conflux/commit/433499543185eb1deda1b3cf2450e000eb259d8c))
- **audio:** Mix and retain aligned prepared stem banks ([eb74176](https://github.com/kyaulabs/conflux/commit/eb74176f675d8a28b103e21d2af58bed3ca5f5e6))
- **stems:** Preserve float input and playback conversion ([3fc872c](https://github.com/kyaulabs/conflux/commit/3fc872c234d2126ed0f6d8f316e49310415ce3c6))
- **stems:** Export and cache complete prepared stem banks ([8296b14](https://github.com/kyaulabs/conflux/commit/8296b146b14b0019ec758d6f0181aedbfd472f1c))
- **stems:** Load prepared banks and queue stem gains ([6ed4e5f](https://github.com/kyaulabs/conflux/commit/6ed4e5f82027305bce79672ebf74ca952f9114bf))
- **stems:** Add bounded live stem packet buffer ([815232e](https://github.com/kyaulabs/conflux/commit/815232ea5af200bfb9b46ed12afd7cb555f73f84))
- **stems:** Serve bounded persistent stem chunks ([591a247](https://github.com/kyaulabs/conflux/commit/591a247ed2a9e4b1047e8b333d2ca207705ae335))
- **audio:** Convert continuous stem chunks at 48 kHz ([744c423](https://github.com/kyaulabs/conflux/commit/744c42398d1b34b6c5cb3a943a9c6292fbfb9975))
- **audio:** Retain live playback owners through retirement ([4cbb559](https://github.com/kyaulabs/conflux/commit/4cbb5595d45a3a6152a7cb511f7e4e04ddb2814f))
- **stems:** Validate bounded service JSONL messages ([d98de79](https://github.com/kyaulabs/conflux/commit/d98de79553146afee9490e15809d2bae12f85350))
- **audio:** Gate live bank recovery on a shared reserve ([5e3f91a](https://github.com/kyaulabs/conflux/commit/5e3f91aadb51adca8b995381bf4e7d804832011a))
- **library:** Deliver persistent stem chunks to live playback ([44b8e0d](https://github.com/kyaulabs/conflux/commit/44b8e0d707b8df937b9c47cdbb1229e173146d0c))
- **audio:** Reuse untouched live reserves during session setup ([e859368](https://github.com/kyaulabs/conflux/commit/e8593686564390dcd631127718474fefddb11550))
- **library:** Discover validated stem banks without export ([299d6c9](https://github.com/kyaulabs/conflux/commit/299d6c99c8da969bfa8027e121ebdc6cec421f68))
- **ui:** Load persistent live stems on cache misses ([5c3e4d0](https://github.com/kyaulabs/conflux/commit/5c3e4d0d7578516c9179b2ecbc34139db473ce30))
- **library:** Expose bounded live stem timing observations ([9300fb1](https://github.com/kyaulabs/conflux/commit/9300fb165bd13e340c2e265c254ef21a01b4c1bb))
- **stems:** Retain bounded worker phase timings ([45130aa](https://github.com/kyaulabs/conflux/commit/45130aa127382bd1af243f7a357e82ad6ff1c001))
- **library:** Import CUE-sheet track starts ([b1a8234](https://github.com/kyaulabs/conflux/commit/b1a8234f25e59c9fca14a5583b6c99eb90de84e5))
- **library:** Measure source RMS and sample peak ([6393f63](https://github.com/kyaulabs/conflux/commit/6393f63f029a12817c0ad0ff648c7c2a86fdede9))
- **ui:** Show artwork and song details on deck LCDs ([6a3bae2](https://github.com/kyaulabs/conflux/commit/6a3bae237f034c2ff7767ebf66f7201fcc359962))

### Fixes
- **m0:** Build probes with Ubuntu runtime dependencies ([5df8645](https://github.com/kyaulabs/conflux/commit/5df864583f6d6efb7abf8c6c6005d059987e63e7))
- **m0:** Clean benchmark fixtures and capture GPU stalls ([f23de26](https://github.com/kyaulabs/conflux/commit/f23de263af119ae9e5c03817075c3a213f4fdbe8))
- **library:** Reject buffered catalog flush failures ([2bbd212](https://github.com/kyaulabs/conflux/commit/2bbd212cfac17e7729e22fa6f2e6b01571738688))
- **stems:** Reap native children when startup fails ([e2ca892](https://github.com/kyaulabs/conflux/commit/e2ca892de4e9a3b0d6ebd7ce4beb42117264fa09))
- **repo:** Route session-prime development through develop ([759a5f9](https://github.com/kyaulabs/conflux/commit/759a5f939b7b7efd1b03b97433d1361eee59c3df))
- **test:** Capture receiver samples once for validation ([fd7236e](https://github.com/kyaulabs/conflux/commit/fd7236e27710600fa601a17409974229e85a0249))
- **stems:** Retain interrupted player measurements ([f183838](https://github.com/kyaulabs/conflux/commit/f18383805de3680ceb72200d2afa2de95bef1210))
- **stems:** Retain proven live process command history ([d59d631](https://github.com/kyaulabs/conflux/commit/d59d6316c7153b7fc6b38bb68b06f90baa157cde))
- **bench:** Handle exiting process observations without hiding failures ([a59b80b](https://github.com/kyaulabs/conflux/commit/a59b80b65047fd416a00b7b3a8e5a47dd861f0ed))
- **benchmarks:** Enforce hidden-window exposure ([f97cd55](https://github.com/kyaulabs/conflux/commit/f97cd55ff5e865c0803fb41f22efeddf2b3c9897))
- **stems:** Finish finite benchmark generation before shutdown ([946e453](https://github.com/kyaulabs/conflux/commit/946e453af7f5f2f434da60c9a94c510da2c72a4b))

### Performance
- **m0:** Profile and reduce stem delivery stalls ([008eedc](https://github.com/kyaulabs/conflux/commit/008eedc7fcf06f35479aed34b9b3bd415791dcc5))
- **stems:** Expose existing graph phase wall timings ([419607f](https://github.com/kyaulabs/conflux/commit/419607fa78cdadd13a6d1ce4743a564e9c9687e9))
- **audio:** Share best-quality stem resampling across channels ([408c2f3](https://github.com/kyaulabs/conflux/commit/408c2f39c93cec28ee62498f40a6d4014b05b4ff))

### Reverts
- Restore main after an incorrectly targeted development merge ([7ddf5c0](https://github.com/kyaulabs/conflux/commit/7ddf5c0a90fa7d440a37abfb3f822c9e8e423789))

### Security
- **library:** Support protected TagLib 1.13 frame headers ([a472fa2](https://github.com/kyaulabs/conflux/commit/a472fa2097f02fe64d27983e829828c4c746d71a))

