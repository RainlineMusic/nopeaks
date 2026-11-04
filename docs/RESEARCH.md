# NoPeaks: research basis and limits of the evidence

Research checked: **4 November 2026**. This document distinguishes published results, mathematical identities, and NoPeaks engineering choices. It is not a claim that the automatic decisions are perceptually optimal, novel, or cleared for commercial use.

## What the supplied research changes

The supplied *Signal-Conditioned Perceptual EQ* document correctly separates a proposed preference optimizer from a trained and independently validated model. No such model, weights, or listening dataset was supplied. NoPeaks therefore does **not** pretend to implement a learned preference critic, an ideal popular-music spectrum, or a numerical confidence in sounding better.

The multitrack setting is nevertheless materially different from blind stereo EQ: each source is available, has an explicit preservation priority, and can be processed independently. This makes source-specific intervention possible. It does not determine the intended artistic balance. A guitar/vocal overlap may be an arrangement choice, and a negative correlation may be an intentional spatial effect.

There are three separate optimization tasks:

| Task | Observable quantity | Useful objective | What this cannot prove |
|---|---|---|---|
| Peaks | Aligned signed sample contributions | Reduce a predicted sum peak with bounded source attenuation | Audibility or quality of the resulting envelope |
| Tone | Source band powers and master spectrum | Reduce selected overlaps or follow an explicitly learned/manual target | A unique ideal timbre from the input alone |
| Width | Bandwise M/S power and L/R cross-power | Reduce problematic side energy while preserving the mono component | That all negative correlation is objectionable |

Equations and implementation contracts are in [ALGORITHMS.md](ALGORITHMS.md). The feature/status description in the repository README takes precedence over research extensions described here.

## Primary research directly relevant to multitrack EQ

### R1. Two-source real-time psychoacoustic processing, 2024

G. Presti, N. Degiorgi, A. Fresia, A. Servetti. *Real-time Psychoacoustic Frequency Masking Compensation for Audio Signals with Overlapping Spectra*. SMC 2024, pp. 439–444. DOI: [10.5281/zenodo.14361929](https://doi.org/10.5281/zenodo.14361929).

- [University record](https://air.unimi.it/handle/2434/1130983)
- [Author/institution-hosted full paper](https://air.unimi.it/retrieve/handle/2434/1130983/2623842/SMC2024_paper_id126.pdf)
- [Conference-hosted paper](https://smcnetwork.org/smc2024/papers/SMC2024_paper_id126.pdf)

**Published result:** TheMasker is a VST3 prototype receiving separate main and masking sources. A psychoacoustic model drives dynamic equalization, and the authors report a music-production user study. Its principal intervention boosts masked main-source components; it is not simply the lower-priority subtractive EQ proposed for NoPeaks.

**Use here:** evidence that source-separated, perceptually informed control is implementable in a real-time plug-in. A compact 12-band overlap detector remains an approximation, not a replication of their auditory model. The paper does not validate automatic correction of arbitrary mixes or the NoPeaks constants.

### R2. Directional time-frequency overlap, 2024

W. Zhao and F. Pérez-Cota. *Adaptive Filtering for Multi-Track Audio Based on Time–Frequency Masking Detection*. **Signals 5(4)**, 633–641, published online 2 October 2024. DOI: [10.3390/signals5040035](https://doi.org/10.3390/signals5040035).

- [Publisher full text](https://www.mdpi.com/2624-6120/5/4/35)
- [University publication record](https://nottingham-repository.worktribe.com/output/40590952/adaptive-filtering-for-multi-track-audio-based-on-time-frequency-masking-detection)

For STFT magnitudes \(T_1,T_2\), their equation (2) is a signed overlap proxy:

\[
M'_{12}(t,f)=\frac{T_1(t,f)T_2(t,f)[T_1(t,f)-T_2(t,f)]}
{[\operatorname{mean}(T_1)+\operatorname{mean}(T_2)]^3}.
\]

**Published settings:** a **180 ms Hann window**, **90 ms spacing**, temporal aggregation, and subtractive filters bounded by a user-selected attenuation; the paper illustrates a −3 dB bound. The expression is level-invariant under a common positive gain and changes sign when source dominance reverses.

**Use here:** motivation for directionality, overlap gating, bounded cuts, and scale-invariant diagnostics. These offline settings do not justify sample-accurate peak control. That proxy is not a complete psychoacoustic masking threshold, and its values must not be labeled a probability of improved sound.

### R3. Autonomous multitrack masking reduction, 2015

S. Hafezi and J. D. Reiss. *Autonomous Multitrack Equalization Based on Masking Reduction*. **JAES 63(5)**, 312–323. DOI: [10.17743/jaes.2015.0021](https://doi.org/10.17743/jaes.2015.0021).

- [AES publication and abstract](https://aes.org/publications/elibrary/elibrary-page/?id=17637)
- [Author-hosted manuscript](https://www.eecs.qmul.ac.uk/~josh/documents/2015/Hafezi%20Reiss%20-%202015.pdf)

**Published result:** offline and real-time systems are compared with raw and manually made mixes. The authors report reductions in objective and perceived masking, while explicitly observing that objective masking measures do not reliably quantify musical masking perception in every case.

**Use here:** establish historical feasibility and the need for listening tests. Their listening results cannot be transferred to this implementation. This is substantial prior art, not a new problem invented by NoPeaks. The accessible publisher record was checked; manuscript retrieval was intermittent.

### R4. Conventional unmasking practices, 2015

J. Wakefield and C. Dewey. *An Investigation into the Efficacy of Methods Commonly Employed by Mix Engineers to Reduce Frequency Masking in the Mixing of Multitrack Musical Recordings*. AES Convention 138, paper 9341.

- [AES record](https://aes.org/publications/elibrary/elibrary-page/?id=17765)
- [University record and presentation](https://eprints.hud.ac.uk/id/eprint/25979/)

The study compares mirrored EQ, spectrum sharing, and panning in controlled two-instrument scenarios. It supports testing specific audible tasks rather than treating all spectral overlap as an error. It does not establish one global optimum for a full musical arrangement.

### R5. A withdrawn subgroup paper, not implementation evidence

D. Ronan et al. *Automatic Minimisation of Masking in Multitrack Audio using Subgroups*, [arXiv:1803.09960](https://arxiv.org/abs/1803.09960).

The current arXiv record is **withdrawn** (5 January 2021), with an intellectual-property ownership comment. Do not cite this as a current validated implementation recipe or silently distribute its previous content. Its presence in search results is not evidence that the current publication is active.

## Phase rotation and peak reduction

### R6. Controlled evidence for static phase manipulation, 2022

V. Välimäki, L. Fierro, S. J. Schlecht, J. Backman. *Audio Peak Reduction Using Ultra-Short Chirps*. **JAES 70(6)**, 485–494. DOI: [10.17743/jaes.2022.0011](https://doi.org/10.17743/jaes.2022.0011).

- [Author/institution publication record](https://research.aalto.fi/en/publications/audio-peak-reduction-using-ultra-short-chirps/)
- [AES record](https://aes.org/publications/elibrary-page/?id=21801)

**Published numbers:** on the authors' test collection the reported practical reduction spans **0 to approximately 3.5 dB**. Their listening experiment supports limiting the examined linear chirps to **4 ms or less**. Their extended allpass phase-rotator method is computationally suitable for real-time use.

**What follows:** phase manipulation can reduce crest factor on some material without changing a time-invariant filter's magnitude response. It offers no guaranteed reduction on every signal. The cited 4 ms limit is specific to that experiment and method; it is not a universal perceptual bound for arbitrary allpass changes. The accessible author/AES abstracts were checked; the full-text download was not consistently retrievable.

**NoPeaks engineering decision:** automatic allpass switching is excluded from the dependable default path. Independently rotating a source changes its phase relative to the other sources, including other microphones recording the same instrument. It can undo cancellation, move a larger peak elsewhere, or change the mix spectrum. A time-varying allpass is not covered by the time-invariant magnitude argument. A future phase candidate must be rendered through the real filter state, compared against the complete sum, and rejected if it worsens any protected measure.

## Normative and implementation references

### R7. Biquad parameterization

R. Bristow-Johnson, *Audio EQ Cookbook*, published as a [W3C Working Group Note](https://www.w3.org/TR/audio-eq-cookbook/). This is the reference for standard peaking/allpass coefficient formulas, not evidence that a particular automatic control strategy sounds good. Coefficients must be normalized by \(a_0\); valid frequency/Q/gain bounds and smooth automation remain the implementation's responsibility.

### R8. Sample peak versus true peak

[ITU-R BS.1770-5, November 2023](https://www.itu.int/rec/R-REC-BS.1770-5-202311-I/en), *Algorithms to measure audio programme loudness and true-peak audio level*. Its true-peak guidance concerns reconstruction/interpolation between input samples. A sample-bounded limiter is not automatically a true-peak limiter. NoPeaks must use **dBFS/sample peak** labeling unless a verified reconstruction detector and corresponding oversampled safety path are present; it must not relabel a sample meter as dBTP.

### R9. Host time and latency contracts

- Steinberg [`ProcessContext`](https://steinbergmedia.github.io/vst3_doc/vstinterfaces/structSteinberg_1_1Vst_1_1ProcessContext.html)
- Steinberg [`IProcessContextRequirements`](https://steinbergmedia.github.io/vst3_doc/vstinterfaces/classSteinberg_1_1Vst_1_1IProcessContextRequirements.html)
- Steinberg [VST3 technical documentation](https://steinbergmedia.github.io/vst3_dev_portal/pages/Technical%2BDocumentation/Index.html)

Project sample positions and reported latency are transport inputs; neither exposes the whole DAW routing graph, post-insert fader gains, arbitrary processing after an insert, or the acoustic origin time of every source. A shared project position is therefore necessary but insufficient for trustworthy coherent summation. Host validation is a separate requirement.

## Patent landscape: direct relevance, not clearance

The following documents were inspected as technical prior art, including the indicated claim passages. Google Patents bibliographic/legal labels are not authoritative legal opinions. NoPeaks has no freedom-to-operate determination. Priority-aware source EQ is especially close to existing patent language; changing a product name or replacing a target curve does not establish clearance.

| Document | Technical overlap worth reviewing | Passage inspected |
|---|---|---|
| [US8929561B2, Apple, 2015](https://patents.google.com/patent/US8929561B2/en) | Frequency collision detection, priority assignment, dynamic attenuation of the lower-priority source before combining | Independent claims 1, 14, 27; dependent claims on analysis and filters |
| [US9654869B2, LANDR/QMUL, 2017](https://patents.google.com/patent/US9654869B2/en) | Multitrack feature extraction and processing rules involving individual and multiple tracks; feedback and spectral mastering | Claim 1; claims 2–18 |
| [EP4510126A1, Sony Interactive Entertainment, published 19 February 2025](https://data.epo.org/publication-server/rest/v1.2/publication-dates/2025-02-19/patents/EP4510126NWA1/document.pdf) | Masking patterns, protected bands, priority-based spectral attenuation; examples focus on game audio | Claims 1–15, especially 7, 9, 10; paragraphs 0006–0014 and 0056–0058 |

These are more directly related to source-priority processing than patent families about selecting a mastering target profile. The supplied Dolby/Gracenote references remain useful leads if a future learned target generator is developed, but they are not evidence that the present multitrack method is novel or cleared.

## Evidence required before strong product claims

1. **DSP correctness:** deterministic tests for cancellation, coherent sums, attenuation limits, mono preservation, latency, invalid input, and stale/discontinuous transport data.
2. **Routing correctness:** verify sums and timelines in supported hosts using impulses, null tests, changing buffer sizes, offline export, and bridges/sandboxing. Include a track routed both directly and through a bus as a deliberate failure case.
3. **Artifact assessment:** musical and synthetic tests for modulation sidebands, pumping, transient loss, filter transitions, stereo collapse, and boundary clicks. The mathematical feasibility of an unsmoothed command does not establish that its audible implementation has those same bounds.
4. **Listening validation:** randomized, level-matched comparisons with original and manual settings. Include well-mixed material where the best action may be no change, listeners who mix professionally, different genres, and different monitoring conditions.
5. **Ablations:** separate cooperative track peak correction from the Master safety path; separate tonal collision control from target following; separate side-only control from level differences. Attribute improvements to the correct module.

Do not report a numerical probability of better sound without a calibrated held-out preference model. A routing residual, amount of gain reduction, or optimization objective is a diagnostic, not perceptual confidence.

## Кратко по-русски

Мультитрек действительно даёт то, чего нет у обычного мастер-EQ: можно обработать гитару отдельно от вокала и учитывать приоритет. Но однозначной «правильной АЧХ» из самого сигнала не следует. Надёжная основа первой реализации — ограниченные вмешательства, явная цель, проверка суммы и возможность ничего не менять. Поворот фазы не гарантирует уменьшение пика и способен изменить отношения между микрофонами. У приоритетной автоматической эквализации есть прямые научные и патентные предшественники; заявления об уникальности и патентной свободе здесь не делаются.
