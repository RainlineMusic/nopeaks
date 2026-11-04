# NoPeaks: mathematical design and validation contract

This document is a derivation and engineering reference. Equations marked **derivation** are not claims of a published NoPeaks algorithm. Alternative solvers and future research paths are explicitly identified. The README and executable source describe the shipped behavior; the contracts below define how its guarantees must be bounded.

Scientific and patent sources are collected in [RESEARCH.md](RESEARCH.md). No trained preference model is bundled or assumed.

## 1. Signal model and supported topology

Let \(x_{i,c}[n]\) be the post-tone/post-width signal published by Track instance \(i\), channel \(c\), before its retained delay and peak envelope. If \(g_i[n]\) is the actually rendered peak envelope, trusted reconstruction requires the Master input to be representable by

\[
y_c[n]=\sum_{i=1}^{N}g_i[n-D]x_{i,c}[n-D]
\]

up to documented, measured numerical error, with a common delay \(D\). This requires a **non-overlapping source frontier**: use Track on each group, or on each contributing track, but do not count a group and its children together. Every route entering the Master must be covered exactly once. A signal also reaching the Master through a return is an additional contribution, not the same signal twice by assumption.

This reference design uses up to **64 Tracks**, **12 bands**, **128-sample frames**, **128 history frames**, and an **8192-sample Track delay**. At 44.1/48/96/192 kHz that delay is approximately **185.76/170.67/85.33/42.67 ms**. The history covers 16384 samples. Constants in `include/nopeaks/SharedBus.h` are authoritative.

The Track insert must be at a point whose output actually corresponds to the contribution reaching the Master. Ordinary VST3 inserts do not reveal post-insert fader, pan, sends, or downstream effect state. Consequently:

- unity post-insert gains, a direct route, and matched upstream timing form the simplest verifiable topology;
- changing a fader or processing after Track invalidates that simple model;
- a learned scalar routing gain cannot represent arbitrary stereo pan matrices, nonlinear compression, time-varying effects, or unobserved returns;
- multiple open projects must have different manually shared session namespaces; a group number alone is not a project identity.

The Master safety path acts on the measured Master input and remains necessary whenever attribution is unavailable or model assumptions are not verified. It is a separate, explicitly switchable stereo-linked sample-peak limiter with **256 samples** of latency and an **80 ms** release. Track → Master total latency is **8448 samples**, or **176 ms at 48 kHz**, even when effect bypass retains the delays.

### Why a Master command needs Track delay

Suppose Track has already emitted sample \(n\) when the Master detects its peak. A later command cannot alter that emitted sample. Lowering a gain in the next callback only changes later audio. The design instead publishes a tagged Track frame **before the delayed samples leave Track**; Master predicts a sum from matching source frames and writes a command for those retained samples.

This is cooperative prediction with lookahead, not retroactive correction. It works only while the host scheduling lead remains inside the delay budget. Wall-clock speed is irrelevant to offline rendering; frame order and retention are what matter. A background worker that depends on sleeping or periodic GUI timers cannot guarantee offline delivery.

## 2. Timeline and data validity

A usable frame key is

\[
(\text{session},\text{group},\text{slot token},\text{timeline epoch},\text{sample position}).
\]

A ring index is not a frame identity. Slot ownership can change after unload, transport can loop to an old sample position, and a Master can be replaced after a crash. Commands must be bound to the live Track token and live Master token.

For each cooperative decision, require matching sample positions, rate, and frame length across included sources, and the correct **individual epoch of each source**. Epoch counters are independent per Track; their numerical values need not match one another. Missing, torn, stale, or nonfinite data must not be interpreted as a silent contribution. A second Master must not write commands into another Master's session. Timeline jumps, sample-rate changes, connection changes, and invalid host position require a command/history reset or an explicit neutral fallback.

The transport uses a two-second owner lease; lease expiry is a liveness mechanism, not an audio alignment tolerance. It must not authorize replay of old commands. Release or expiry must invalidate capability-bound commands even when their ring data remain in memory.

Shared-memory setup, allocation, name construction, operating-system initialization locks, and ownership acquisition occur outside the audio callback. Runtime communication uses bounded copies and native lock-free atomics; a seqlock counter around **non-atomic C++ payload fields** is not by itself a valid data-race solution. No callback may wait for another Track to finish.

### Routing residual: useful diagnostic, limited proof

The processor reconstructs the delayed post-EQ Track sum using the previous/current/next command envelope. `routingMatch()` fits one aggregate scalar and evaluates a root-normalized residual:

\[
\hat a=\frac{\sum_{n,c}\hat y_c[n]y_c[n]}{\sum_{n,c}\hat y_c[n]^2},\qquad
e_c[n]=y_c[n]-\hat a\hat y_c[n],\qquad
r=\sqrt{\frac{\sum_{n,c}e_c[n]^2}{\sum_{n,c}y_c[n]^2}}.
\]

The actual acceptance bounds are **\(r\le0.08\)** and **\(0.9\le\hat a\le1.1\)**, with nonsilent/nonconstant observations. **24 consecutive matches** authorize routing. A failed active match or source topology/epoch change revokes it; prolonged missing history revokes it after the 72-frame allowance. Track control is also an explicit user opt-in. Host blocks greater than 4096 samples do not authorize cooperation; backlog beyond 8192 samples revokes trust.

Only samples with matching tags and known emitted gains belong in this comparison. Comparing pre-correction telemetry with already corrected Master audio produces a false failure. A small residual over one passage cannot identify a silent missing track, a latent return, or a later routing change. This is not a probability of correct routing. The fitted scalar allows a tolerance around the unity model; it does not estimate or compensate individual track faders.

## 3. Peak attribution uses signed samples

For each channel and sample, the predicted sum is

\[
\hat y_c[n]=\sum_i x_{i,c}[n].
\]

The peak is \(P=\max_{n,c}|\hat y_c[n]|\). Summing Track RMS values, absolute peaks, or powers does **not** compute this quantity. Cross-source phase and cancellation matter.

For an offending sample, define

\[
s=\operatorname{sign}(\hat y_c[n]),\quad
a_i=sx_{i,c}[n],\quad E=|\hat y_c[n]|-T,
\]

where \(T=10^{C/20}\) is a linear sample threshold for ceiling \(C\) dBFS. A positive \(a_i\) contributes in the offending direction. A negative \(a_i\) is currently cancelling that peak; cutting it can make the peak larger.

**Cancellation test:** sources \(+1.0\) and \(-0.8\) sum to \(+0.2\). Halving the second source creates \(+0.6\). An allocator based only on source peak magnitude fails this case.

### 3.1. Priority-aware single-constraint solution — derivation

Let \(d_i=1-g_i\) be the fraction of source amplitude removed. Preservation priority \(p_i\in[0,1]\) maps to a positive cost \(w_i\); larger \(w_i\) means more expensive to change. A fully protected source has \(d_{i,\max}=0\), not merely a large finite weight.

For one offending sample, solve

\[
\min_{\mathbf d}\frac12\sum_i w_i d_i^2,
\quad
\sum_i a_i d_i\ge E,
\quad 0\le d_i\le d_{i,\max}.
\]

In the branch where only aligned positive contributors are eligible,

\[
d_i(\lambda)=\min\left(d_{i,\max},\frac{\lambda\max(a_i,0)}{w_i}\right).
\]

Choose \(\lambda\ge0\) by bounded bisection until the constraint is met or all available contributors saturate. This follows from the Karush–Kuhn–Tucker conditions for a convex weighted least-squares projection. Its mathematical optimum applies to this single constraint, not automatically to a sequence of peaks.

The implemented priority mapping is **\(w_i=1+99p_i^2\)**. This is an **engineering choice**, not a psychoacoustic law. Test its response monotonically: raising only one source's priority must not make that source cheaper to alter under the same data and constraints.

### 3.2. Complete frame constraints — reference problem

A constant source gain across an \(F\)-sample stereo frame gives the convex problem

\[
\min_{\mathbf d}\frac12\mathbf d^T W\mathbf d,
\quad
-T\le\sum_i(1-d_i)x_{i,c}[n]\le T
\quad\forall(n,c),
\quad0\le d_i\le d_{i,\max}.
\]

`solvePeaks()` implements bounded dual-coordinate ascent for this box-constrained quadratic problem, checking the signed constraints in both channels. The plug-in requests **16 iterations** per frame; the core API caps any request at 96. The processor's ordinary source gain floor is **0.25 linear**, approximately **−12.04 dB**, while protected sources stay at unity. Amount interpolates the final allocation toward unity, so values below 100% intentionally relax the request.

The implementation recomputes the complete predicted peak and reports feasibility/residual after allocation. Hitting an iteration limit is not proof of feasibility or global optimality. Its optional common scaling of free sources is accepted only when the actual signed constraints admit it; it does not blindly remove cancellation.

If all sources can be attenuated, a common scaling of the entire predicted sum is a feasible fallback. Protected sources and finite cut budgets can make the constraints infeasible. In that case retain honest unmet-error telemetry and let the explicitly enabled Master safety stage handle the actual sum.

### 3.3. Commands versus the audible gain envelope

Piecewise constant 128-sample gain changes can click and cause modulation. The actual Track uses **32-sample boundary ramps**, looking at adjacent frame commands. Routing reconstruction uses the same envelope. But smoothing changes the sample gains; it can invalidate the ceiling proved for constant frame gains. Test the rendered, smoothed output, not only the command values. A per-source release held into a later frame can also remove a cancelling component and raise the later sum.

An actual Master limiter remains the final measured safeguard. If it bounds only input samples, call the result sample-peak bounded. Intersample peaks require a verified reconstructed/oversampled path. No allpass method or Track attribution alone guarantees true-peak safety.

## 4. Band analysis

For a common stereo filter bank \(B_b\), form \(x_{i,b,L},x_{i,b,R}\). With \(M=(L+R)/2\), \(S=(L-R)/2\), retain smoothed linear powers

\[
P_{i,b}=\mathbb E[(L^2+R^2)/2],\quad
P^M_{i,b}=\mathbb E[M^2],\quad
P^S_{i,b}=\mathbb E[S^2],\quad
C^{LR}_{i,b}=\mathbb E[LR].
\]

Power conversion uses \(10\log_{10}(P+\epsilon)\); amplitude conversion uses \(20\log_{10}|a|\). Filter-bank normalization must be fixed before comparing different bands. Silence gates are relative digital thresholds; without a calibrated monitoring SPL they are not absolute hearing thresholds.

Frame duration and feature time constant are distinct. At 48 kHz a 128-sample frame lasts 2.67 ms, too short on its own to infer stable low-frequency energy. A stateful filter bank or a longer analysis window supplies spectral resolution; power smoothing supplies temporal stability. Neither should be confused with peak prediction latency.

The implemented analyzer uses unity-peak RBJ bandpasses, **Q=1.4**, at **50, 80, 125, 200, 315, 500, 800, 1250, 2000, 3150, 5000, 10000 Hz** (clamped to 0.45 times the sample rate). Powers have a **250 ms** exponential smoother. These overlapping bands are neither a perfect-reconstruction decomposition nor equal-bandwidth power spectral density. Their normalized sums are repeatable features, not an exact division of the signal's total energy. Telemetry retains **pre-EQ** source powers for absolute spectral allocation and **post-EQ** samples for coherent peak prediction.

## 5. Tonal control without an invented ideal curve

### 5.1. Two explicitly different modes

**Overlap control:** detect competing active sources in the same broad bands and favor the specified priority. The implemented heuristic accumulates higher-priority competing power

\[
H_i=\sum_{j:p_j>p_i+0.001}P_j(p_j-p_i),
\]

and uses the normalized overlap

\[
o_i=\frac{2\min(P_i,H_i)}{P_i+H_i+\epsilon}.
\]

Own-band activity must exceed the largest band contribution times **\(10^{-4}\)**, as well as the numerical floor. Equal-priority pairs contribute no conflict request. The plug-in applies a maximum **1.5 dB** conflict request before Amount and the aggregate budget; the core API supports up to 3 dB. The default Conflicts control is **20%**, multiplied by default Tone Amount **50%**. This is an engineering heuristic, not a complete simultaneous-masking model or a measure of musical quality.

**Target following:** `Capture balance` integrates **two seconds of active, smoothed, pre-EQ linked-source band powers**, then freezes the resulting normalized feature profile and persists it in plug-in state. No manual target editor or learned target generator is implemented. With \(P_b=\sum_iP_{i,b}^{\rm raw}\), the comparison is

\[
q_b=\frac{\bar P_b}{\sum_k\bar P_k+\epsilon},\quad
\Delta_b=10\log_{10}\frac{q_b^{\rm target}+\epsilon}{q_b^{\rm current}+\epsilon}.
\]

Normalization prevents common broadband gain from masquerading as feature-shape error. Each negative target offset is clamped to **−3..0 dB** and multiplied by Tone Amount; positive offsets request no boost. The current/raw feature difference generates an **absolute cut request**, not a new cut added to the already processed output on each frame. This avoids an accumulating tone-feedback loop. Without Capture and with Conflicts at zero, Tone is neutral.

Capturing the current song establishes a user-approved reference to itself; it does not discover a better aesthetic target. A target that is relearned continuously from the corrected output can create moving-goal feedback and must have explicit freeze/reset semantics.

### 5.2. Source allocation — bounded approximation

Tone and Width share a convex allocator in **retained power** \(r_i\), not retained amplitude:

\[
\min_{\mathbf r}\frac12\sum_iw_i(1-r_i)^2,
\quad\sum_iP_i r_i\le T_b,
\quad r_{i,\min}\le r_i\le1.
\]

The solution is \(r_i=\operatorname{clip}(1-\lambda P_i/w_i,r_{i,\min},1)\); a 56-step bounded bisection finds \(\lambda\). Protected sources have \(r_i=1\). The command is **\(10\log_{10}r_i\)** dB, corresponding to amplitude gain \(\sqrt{r_i}\), not \(r_i\). For Tone the plug-in uses a 3 dB allocation bound, adds the conflict request, and clamps the combined per-band target to **−1.5..0 dB**. The receiving Track projects the twelve targets to a **6 dB aggregate absolute-gain budget**.

The DSP accepts transport/utility targets as deep as −6 dB per tone section, but projects the **smoothed current** gain vector to the same 6 dB aggregate budget before creating filter coefficients. Thus old-band release plus new-band attack cannot double that current control budget. The realized frozen response of the serial bell bank and the abstract independent-band prediction are different objects; transient waveform peaks require the separate safety path.

For correlated sources,

\[
P_b^{\rm sum}=\sum_iP_{i,b}+2\sum_{i<j}\operatorname{Re}C_{ij,b}.
\]

Consequently summed raw source powers are not the exact master band power. Source-power contribution weights are an approximation, especially for multiple microphones and duplicated tracks. **The present Tone controller does not close a measured-Master spectral feedback loop:** it follows the captured raw-source feature profile through bounded absolute commands. The measured Master spectrum is diagnostic. A coherent Master-constrained tonal controller would be a future extension and must handle these cross terms explicitly.

An advanced coherent linearization, not required for the initial controller, is

\[
\frac{\partial\,[10\log_{10}|Z_b|^2]}{\partial g_i^{\rm dB}}
=\frac{\operatorname{Re}(X_{i,b}Z_b^*)}{|Z_b|^2},
\quad Z_b=\sum_iX_{i,b}.
\]

Near cancellation this derivative is ill-conditioned; gate low-power bins and regularize. A bounded optimizer could minimize target error plus priority-weighted intervention and spectral curvature penalties. Implementing the formula alone is not evidence of perceptual improvement.

## 6. Width and negative correlation

### 6.1. Exact M/S identity — derivation

For zero-mean band signals define \(A=\mathbb E[M^2]\), \(B=\mathbb E[S^2]\), \(C=\mathbb E[MS]\). Then

\[
\rho_{LR}=\frac{A-B}{\sqrt{(A+B)^2-4C^2}}.
\]

Except at silence/zero-variance degeneracy, \(\rho<0\) precisely when \(B>A\). The commonly used expression \((A-B)/(A+B)\) equals Pearson correlation only when \(C=0\), equivalently balanced channel powers. It is still a useful normalized side-balance measure if labeled correctly.

If the aggregate side is multiplied by \(k\in[0,1]\), then \(B\mapsto k^2B\), \(C\mapsto kC\), while \(M\) is unchanged. A target of nonnegative correlation requires

\[
k^2B\le A.
\]

For a stricter \(r>0\), use the complete expression or bounded bisection. The shortcut

\[
k^2\le\frac{A}{B}\frac{1-r}{1+r}
\]

assumes \(C=0\). Apply floors and a minimum signal level; a correlation number in silence is undefined, not evidence of a wide track.

### 6.2. Attribute the summed side, not just the widest source

Because \(S_\Sigma=\sum_iS_i\), source side powers do not add exactly:

\[
\mathbb E[S_\Sigma^2]=\sum_i\mathbb E[S_i^2]
+2\sum_{i<j}\mathbb E[S_iS_j].
\]

The local sensitivity to removing a fraction of source side is proportional to \(\mathbb E[S_iS_\Sigma]\). A negative value indicates a source currently cancelling other side content; blindly cutting it can increase total side power. The actual Width allocator has only power summaries and approximates the desired reduction using

\[
T_b^{\rm stems}=\left(\sum_iP^S_{i,b}\right)
\operatorname{clip}\left(\frac{P^M_b}{P^S_b},0,1\right).
\]

It applies the retained-power allocator from section 5.2. The plug-in uses a minimum-correlation argument of zero; this corresponds to nonnegative uncentered L/R crosspower, not a claim to estimate exact Pearson correlation from M/S powers alone. Side attribution is a bounded power-share heuristic, not an exact causal decomposition. The actual Master is remeasured after changes, but this does not establish a monotonic improvement for every coherent mix.

Prefer active lower-priority sources; protected sources are ineligible. Side control changes \(S_i\), reconstructing

\[
L'_i=M_i+H^S_iS_i,\qquad R'_i=M_i-H^S_iS_i.
\]

With identical linear filter operations on both reconstruction branches,
\((L'_i+R'_i)/2=M_i\): the mono fold-down is preserved by the side-only operation. The core utility accepts side cuts down to −18 dB per section; the plug-in requests at most −12 dB, then holds each requested band no deeper than **−6 dB**, before the Track enforces a **12 dB aggregate Side budget**. That budget is also enforced on the current smoothed vector. Broad-band mono preservation does not guarantee a particular finite-band correlation after a bank of overlapping IIR filters or during parameter transitions.

The actual problem gate is **Master Side power > 1.01 × Mid power**, with Side above \(10^{-12}\). While that gate holds, the controller retains the stronger cut; otherwise it releases toward neutral at **0.6 dB/s**. Width Amount scales the requested intervention, and the effect has its own enable switch. Coefficient smoothing adds the time constants in section 8. Pure anti-phase content has no Mid: finite Side attenuation cannot make its correlation positive. Negative correlation can also be intentional. DC removal or explicitly zero-mean covariance is needed if claiming true Pearson correlation rather than uncentered normalized cross-power.

## 7. Filters, transitions, and phase

The standard peaking biquad uses \(A=10^{G/40}\), \(\omega_0=2\pi f_0/f_s\), \(\alpha=\sin\omega_0/(2Q)\):

\[
(b_0,b_1,b_2)=(1+\alpha A,-2\cos\omega_0,1-\alpha A),
\]
\[
(a_0,a_1,a_2)=(1+\alpha/A,-2\cos\omega_0,1-\alpha/A).
\]

Normalize every coefficient by \(a_0\). These are the [RBJ/W3C cookbook](https://www.w3.org/TR/audio-eq-cookbook/) coefficients. Clamp frequency below Nyquist, use positive finite Q, and test poles for the full supported bounds. Smooth the gain parameter or crossfade complete filter paths with valid states. Arbitrary coefficient interpolation and state reset can create transient artifacts despite stable endpoint filters.

For a fixed real allpass \(H\), \(|H(e^{j\omega})|=1\) preserves the individual source magnitude, but

\[
\left|H_1X_1+H_2X_2\right|\ne|X_1+X_2|
\]

in general. That is why automatic independent phase rotation is not a harmless substitute for gain control. Crossfading two different allpass outputs also need not preserve magnitude. Future phase candidates require full sum rendering, bounded delay/group delay, identical stereo treatment when required, protected phase-linked source groups, and an identity candidate.

## 8. Controller bounds are not listening-validated constants

| Quantity | Current transport/design bound | Meaning |
|---|---:|---|
| Source count | 64 | Hard session capacity |
| Telemetry frame | 128 samples | Timestamped command unit |
| Retained delay | 8192 samples | Causality/scheduling budget |
| History | 16384 samples | Bounded ring retention |
| Priority | 0..1 | Relative preservation preference |
| Priority cost | \(1+99p^2\) | Engineering weighting, not a perceptual law |
| Peak command in the plug-in | 0.25..1 linear, or unity if protected | Attenuation only; not a true-peak promise |
| Peak solver iterations | 16 requested; 96 API cap | Feasibility separately measured |
| Applied Tone target | −1.5..0 dB per band; 6 dB aggregate | Combined target/conflict request |
| Applied Side target | −6..0 dB per band; 12 dB aggregate | Stateful held intervention |
| Feature power smoother | 250 ms | Sample-rate-aware exponential time constant |
| Tone gain smoothing | 60 ms attack / 350 ms release | Gain-domain smoothing |
| Side gain smoothing | 25 ms attack / 250 ms release | Gain-domain smoothing |
| Coefficient update period | 32 samples | Regenerate valid PEQ from smoothed gains |
| Capture | 2 seconds of active linked-source feature frames | Normalized raw-source feature anchor |
| Master safety delay / release | 256 samples / 80 ms | Stereo-linked sample-peak limiter |
| Owner lease | 2 seconds | Connection liveness; never audio alignment tolerance |

These values were checked against `PluginProcessor.cpp`, `Dsp.cpp`, `Optimizer.cpp`, and the public headers. No publication cited here establishes a universal musical optimum for them. Stronger quality claims require the listening evidence listed in RESEARCH.md.

## 9. Required verification matrix

| Test | Required observation |
|---|---|
| No-op identity | Neutral modules equal delayed input within floating-point tolerance |
| Delay impulse | Track latency equals reported latency in mono and stereo |
| Coincident positive sources | Allocator reduces the coherent sum and respects priority |
| Cancelling sources | No attribution by absolute amplitude; every modified sum is rechecked |
| Two stereo extrema | Both channels and every sample are considered |
| Protected sources | Their cooperative commands remain neutral |
| Infeasible request | No false success; actual Master safeguard is reported separately |
| Gain-boundary transition | Rendered output checked for overshoot and clicks |
| Side-only operation | Mono fold-down nulls against original mid within tolerance |
| Side cancellation | Controller does not infer summed side from source powers alone |
| Silence and DC | No undefined/nonfinite values; correlation interpretation remains honest |
| Tone gain invariance | Common broadband gain does not trigger target-shape error |
| Constant musical note | Broad analysis does not label a harmonic automatically undesirable |
| Unload/reload and slot reuse | Old Track/Master commands cannot be replayed |
| Loop, seek, stop/start | Epochs prevent old frames from controlling a new pass |
| Two projects | Session namespaces isolate telemetry and commands |
| Missing/suspended Track | No blocking and no implicit zero contribution |
| Host buffer 1..large | Frame assembly and timestamps remain correct across boundaries |
| Offline export | No GUI/worker timing dependency; output is reproducible |
| Post-insert fader/pan/effect | Unsupported coherent model is exposed, not silently trusted |
| NaN/Inf and huge values | Output and shared state remain finite under documented sanitization |

Unit and process-isolation tests establish only the contracts they exercise. They do not replace real Ableton/Cubase/Reaper runs, VST3 validation, listening tests, or macOS/Windows packaging/signing checks.
