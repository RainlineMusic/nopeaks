# Validation record

Version 0.1.0. Synthetic tests establish engineering properties, not listener preference or general DAW compatibility.

## Core checks

| Area | What is checked |
| --- | --- |
| DSP | Exact neutral path,8192 sample source delay,256 sample safety latency, mono preservation, gain scaling, sanitized nonfinite input, stable changing coefficients |
| Safety | Actual output sample peak at or under ceiling, stereo link, bypass latency, block partition invariance, multiple sample rates |
| Tone | Actual rendered 500 Hz transfer, not only abstract band-power prediction; protected source remains unchanged |
| Width | M/S invariants, actual Side reduction, mono preservation, pure anti-phase residual |
| Optimizer | Hard protect, normalized priorities, signed cancellation, infeasible constraints, right-only peaks, silence, finite outputs |
| Transport | Separate processes, concurrent snapshots,64 slot capacity, project isolation, negative preroll, ring wrap, independent epochs, stale handles/capabilities, lease expiry |
| Scheduling |300 frames with 3 sources, shuffled source orders, exact future tags, actual delay/envelope reconstruction, late-history eviction |
| Routing | Omitted/doubled sources, post-insert individual/aggregate gain changes, silent/DC rejection |

The combined core suite runs with AddressSanitizer and UndefinedBehaviorSanitizer. LeakSanitizer is disabled only in this container because its `/proc` inspection is blocked; CI retains the normal configuration. Windows cross-process behavior and native host behavior require their platform test runs.

## Synthetic rendered results

These values are reproducible from `testAudioQuality()` and printed during the suite:

- A three-source synthetic mix peaks at approximately+2.620 dBFS before the cooperative allocation and−1.000 dBFS afterward. All 18 exercised overload frames are feasible; the protected vocal source stays at gain 1.
- A requested−1.000 dB correction at 500 Hz measures approximately−0.995 dB through the actual overlapping filter implementation.
- A bounded side correction improves a particular test mixture's correlation from approximately−0.385 to−0.239. This deliberately demonstrates remaining error; it is not presented as successful positive-correlation repair.
- A pure anti-phase source remains negatively correlated under finite Side attenuation and reports a nonzero unresolved residual.

A synthetic 64 source benchmark includes analysis and all four solver calls but excludes JUCE, IPC and the UI. It uses much of one CPU core in this container. It is a diagnostic, not a claim that 64 active sources meet every host's real-time deadline. Measure the actual target project and buffer size.

## Native processor harness

`nopeaks_host_tests` uses the actual `NoPeaksAudioProcessor` classes for both roles, including their parameter, state, delay, telemetry, coordinator and safety code. It verifies stopped-transport monitoring, latency-preserving host bypass, a two-source rendered session, hard source protection, actual low-priority attenuation, target capture, an independently re-prepared Track, routing revocation after downstream gain and state round trips. An allocation guard checks the audio callbacks for calls to global new/new[].

The same executable can render the actual editor to PNG using `--screenshots <absolute-folder>` for visual inspection. No HTML mock is substituted for the native editor.

## Local native result

Both Linux VST3 bundles and both standalone applications built. The final native processor run measured a Master output peak of −1 dBFS, an exact zero null error for the protected source, and real attenuation on the lower-priority source. Width Amount 100 → 0 reduced the strongest measured band setting from about −1.073 dB to −0.0018 dB while negative correlation persisted. All native processor checks passed with zero counted callback `new`/`new[]` allocations.

[Master render](images/master.png) and [Track render](images/track.png) show actual editors with synthetic linked-source data.

## Still required before release

- Ableton Live 12.4.x on Windows: scan/load/unload, project reopen, offline export, playback/loop/seek, hostPDC on representative group/return topologies, multiple windows and instances.
- Other hosts: callback scheduling, sandboxed bridges, latency and offline block changes.
- Windows/macOS platform binaries and artifact integrity; macOS signing/notarization if distributing commercially.
- Loudness-matched listening of protected vocals/drums, correlated multi-microphone sources, sparse versus dense arrangements, and the perceptual tradeoffs of conflict/width corrections.
- True-peak behavior if a later product version adds an inter-sample protection claim.
- Jurisdiction-specific patent/license review for commercial distribution.

Do not replace these gates with a claim that a high numerical score proves a better mix.
