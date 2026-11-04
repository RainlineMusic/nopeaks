# Architecture and failure behavior

## Products and audio paths

The same processor implementation is compiled into two distinct VST3 products with distinct IDs. The runtime role is fixed by its product; the test host can instantiate either role explicitly.

| Product | Processing order | Declared delay |
| --- | --- | ---: |
| Track | sanitize → raw band analysis → stereo tone / Side EQ → telemetry → delay → scheduled peak envelope | 8192 samples |
| Master | sanitize → measured band analysis / routing validation / coordinator → linked sample-peak limiter | 256 samples |

Track telemetry contains actual post-EQ stereo samples and separately smoothed pre-EQ powers. The former predicts the coherent sum; the latter determines absolute spectral allocation. Publishing post-EQ samples avoids assuming that the optimizer's band-power model exactly describes the phase and amplitude response of the serial filters.

No audio is transported over sockets or the internet. The full fixed-size telemetry frame goes through a local mapped-memory ring. The Track delay is an audio buffer owned by that processor, not shared audio output.

## Transport contract

- 128 stereo samples per telemetry frame, 128 history cells per source, up to 64 source slots.
- Frame identity is `(slot token, timeline epoch, original sample position)`.
- Timeline epochs are **independent per Track**. Their numerical values need not match. A coordinator reads each source using that source's current epoch and emits commands with its own epoch.
- Every control frame is additionally bound to a live Master capability. A duplicate Master cannot overwrite it.
- Versioned snapshots contain atomic payload words. Plain-data seqlock reads would otherwise be undefined C++ data races.
- Readers use three bounded attempts. A concurrently unavailable, overwritten, expired or mismatched command returns unity/zero correction.
- Setup and slot claiming happen off the audio thread. Audio methods do not lock, allocate, open files or wait for another source.
- Audio processing refreshes the active Master lease; Track publication refreshes its source lease. The processor timer also maintains/reconnects leases while the UI is closed or transport stopped.

One Project ID/group mapping is 12,066,880 bytes. Windows uses a named local mapping. POSIX uses a 0600 owner-checked, symlink-rejecting file-backed mapping to avoid a small `/dev/shm` quota. The project identifier is a user coordination setting, not cryptographic authentication or automatic DAW-project detection.

## Causal scheduling

At source sample time `t`, Track publishes post-EQ samples and holds them until `t+8192` in its own stream. Master can read the fresh source frames after their callbacks complete, solve their coherent predicted mix, and return a frame-tagged gain before that held audio reaches Track output. Different ordering of source callbacks is supported; no source callback blocks while waiting for its peers.

The coordinator processes up to 64 fresh frames per invocation and rejects a scheduling backlog greater than 8192 samples. Host blocks greater than 4096 do not authorize cooperation. When frames are late or unavailable, Track falls back to unity rather than waiting.

The supplied host harness models equal host sample positions for parallel source callbacks. VST3 does not standardize an automatic inference of arbitrary routing, pre-insert delay or downstream faders from this data. The first version supports the explicitly described unity summing topology; real DAW PDC behavior must be verified. Protect prevents direct changes at Track; the optional Master safety limiter can still attenuate the complete mix, including protected content.

## Routing trust

Master reconstructs its currently observed input from source samples at `master_position−8192` and the same previous/current/next control envelope used by Track. It fits a positive aggregate scalar and measures normalized residual. A match requires residual≤0.08 and fitted gain in [0.9, 1.1], as well as non-silent, nonconstant data.

Twenty-four consecutive valid frames authorize routing. These are engineering thresholds; a short observation cannot prove that no future unrepresented source will play. Any failed active match or source token/epoch topology change revokes trust. After missing history persists 72 frames, trust is also revoked. Enabling Track control is an additional explicit topology opt-in.

This model rejects many mistakes, including omitted sources, a group plus its children, divergent downstream faders, additional effects and mismatched timing. It does not estimate per-track fader weights. Silence cannot establish trust. Once a new audible source or automation invalidates the model, addressable changes revert toward neutral and the independent Master safety path remains available.

## Spectral control

Raw source and measured Master band powers have a sample-rate-aware 250 ms exponential envelope. Tone coefficients use 60 ms attack/350 ms release; Side coefficients 25 ms/250 ms. Capture accumulates two seconds of active linked-source powers, normalizes their distribution and persists it with the processor state. It is an artistic anchor supplied by the user, not an inferred ideal.

Twelve bell sections overlap. The processor first limits each target, then normalizes the aggregate target budgets. The DSP also constrains **current smoothed** control vectors before forming coefficients, preventing old-band release plus new-band attack from doubling the control budget. Bounds are 6 dB total Tone and 12 dB total Side. Frozen filter response and transient waveform peak bounds are different: the independent Master limiter covers the latter.

The Width controller is gated by measured Master `SidePower>MidPower`, and allocates side cuts using stem power shares. It holds the intervention while the measured problem persists, releases at 0.6 dB/s, and applies Amount. This is conservative feedback, not a full coherent covariance optimizer. Negative correlation may remain; a finite cut cannot manufacture missing Mid.

## Real-time lifecycle

Parameter atomic pointers are cached during construction; callbacks do not construct `juce::String` objects for parameter lookup. Audio buffers are allocated in `prepareToPlay`. Connection mappings are prepared off-thread and published through an atomic pointer. A hazard publication/recheck protects the mapping during a callback. Retired mappings are reclaimed off-thread, keeping repeated configuration changes from retaining unbounded mapped memory.

A new connection, transport transition or sample-clock discontinuity clears delay/filter state, changes the local epoch and revokes routing. Internal bypass and the host bypass callback retain declared delay. With transport stopped, a local continuous sample counter preserves delayed live monitoring; cooperative scheduling is disabled.

Names, group, Project ID, priorities, parameters and captured target are saved with the plug-in state. Project ID and group should be changed while stopped; simultaneous projects must use distinct IDs.

## Failure table

| Condition | Track behavior | Master behavior |
| --- | --- | --- |
| No Master / expired capability | Peak unity; slow corrections return neutral | Safety can run alone |
| Missing or stale frame/control | Unity for that frame | No late command; revalidation if sustained |
| Protected or bypassed source | Raw source, latency retained; no new automatic correction | Prediction mismatch can revoke trust until matched |
| Duplicate Master | Existing source capability stays authoritative | Duplicate instance reports collision; its safety path remains local |
| Seek / loop / changed topology | Old epoch commands rejected | Must earn routing trust again |
| Downstream fader/effect mismatch | New plan becomes neutral after revocation | Local safety independent of routing |
| Peak constraints infeasible | Bounded allocation only; protected sources unchanged | Residual handled by enabled safety limiter |
| Pure anti-phase band | Finite Side reduction only | No promise of positive correlation |

## Cleanup

POSIX backing files deliberately persist so that a process opening the same group cannot split it into a second mapping while another process still holds the first. After **all** NOPEAKS instances in every DAW using that Project ID are closed, the corresponding `/tmp/rainline-nopeaks-v1-<uid>-<hex-project-id>-<group>.ipc` file may be removed. Do not delete it while instances are open. Windows releases the named object after the last mapping handle closes.
