# NOPEAKS interface

## Design source and use

The supplied `clipper(3).html` is the visual authority. NOPEAKS keeps its cool charcoal face, pale silver lettering, thin separators, recessed plotting well, diagonal signal mark, rotary knobs and precise level scales. The native editor is drawn in JUCE rather than embedding a browser. It uses no decorative animation or simulated audio data.

This is an instrument used while listening in a dim studio. Small text remains light enough to read against the panel; amber marks side attenuation and the captured tonal target, and red marks the ceiling or ownership faults. White is the principal control and measurement colour. The interface distinguishes requested controls from applied corrections.

## Shared foundations

| Element | Value |
| --- | --- |
| Panel top | `#262b34` |
| Panel bottom | `#16181c` |
| Plot well | `#101318` |
| Rules and borders | `#3a424e` |
| Principal text | `#e5ebf4` |
| Secondary text | `#acb9cb` |
| Source trace / tone correction | `#d3deed` |
| Captured target / side correction | `#d4ad7c` |
| Peak ceiling / ownership fault | `#f75055` |
| Valid routing / connected status | `#a5c8b5` |
| Font | Platform sans; 9–19 px labels/data; 25 px brand |
| Plot corner radius | 3 px |
| Rule thickness | 1 px |
| Meter refresh | 30 Hz |
| Peak history | 180 measured editor snapshots, approximately six seconds |

The background gradient and knob shading reproduce the supplied panel's physical depth. There are no glass cards, neon halos, or promotional claims. Native system text remains appropriate to this operational surface. The logo is vector signal geometry derived from the Clipper reference, not an illustrative substitute.

## Master, 1100 × 720

The top strip holds the product name, link group, numeric project ID, Track control, Safety and Bypass. Group and project ID must match the intended Track instances. The routing status is always visible above the connected-track list.

The principal plot is a twelve-band source-energy display, normalized over all twelve bands, with a clearly different dashed amber trace after Capture balance. With linked tracks, its heading is Linked-source balance: both the measured line and captured target use the same sum of linked raw Track band-power features. This is an additive source-feature view, not the coherent summed Master spectrum. Without linked tracks the heading is Mix balance and the trace uses actual normalized Master band powers. The caption Relative analysis band power makes the measurement domain explicit. It is a band-energy measurement, not a full-resolution FFT or a claimed transfer-function measurement. Before capture there is no hidden reference curve. The plot's no-input state tells the user to play audio.

A second, shallow plot displays the measured input and output peak histories. The ceiling is red and dashed. Dragging within this plot edits the ceiling through a host-automatable gesture; the large Ceiling knob provides the same parameter with keyboard support.

Four large knobs share a single baseline: Peaks, Tone, Width and Ceiling. The first three have module enable switches immediately above them. The vertical level meters flank the source-energy plot. The right side is a scrollable list of the actual connected Tracks, their priority or hard-protection state, level activity, and current tone/side reduction. No decorative tracks or invented meter values appear when disconnected.

The right-side Conflict reduction control is subordinate to the three central functions. It is described as a heuristic and has a tooltip explaining that it is not a learned ideal EQ. Maximum commanded tone and side band settings appear below it in dB. They are not presented as the complete transfer of overlapping filters.

The footer explicitly reports Track control being off, unverified routing, active control, preserved-latency bypass, or competing Master ownership. A disconnected system must not look successfully connected. When routing is lost, the footer says that control is waiting; existing coefficients release toward neutral rather than being described as instantly zero.

## Track, 740 × 480

Track exposes a name, group and project ID, a large Priority knob, hard Protect track switch and Bypass. The graph shows the actual smoothed tone and side band settings on a common 0 to −18 dB scale. Overlapping filters can produce a different complete transfer function, so the graph is labelled Applied band settings. Input, output and peak-reduction readouts show current processor measurements. The footer names the Master connection state.

Priority means relative preservation. Protect track is a hard constraint on direct processing of that Track. Master Safety still acts on the whole mix, as the protection tooltip explains. Neither is labelled as an aesthetic quality setting. The descriptive copy says what higher priority changes, and distinguishes protection from a merely high score.

## Interaction and host behaviour

- Rotary controls support vertical or horizontal drag, wheel, keyboard input and double-click reset to the parameter default.
- All processor parameters use APVTS attachments, except the integer project-ID field, which emits a bounded host gesture on Return or focus loss.
- Track name commits on Return or focus loss and is stored by the processor.
- Bypass retains the processor's declared latency. The UI tooltip explains this.
- Disabled module knobs dim without disappearing. Parameter automation remains in the processor.
- The editor requests a fixed aspect ratio with scalable bounds: Master 1100 × 720 to 1650 × 1080; Track 740 × 480 to 1110 × 720.
- Focus indication, parameter titles and tooltips accompany the controls. Native lists and text entry preserve expected keyboard behaviour.
- Capture requires linked tracks, matched routing and non-silent input. It averages two seconds of non-silent linked-track band energies; the disabled button reads Capturing... during acquisition and Recapture balance after completion. Reset discards the stored target. Current source powers are smoothed over 250 ms.
- The editor reads only the processor's UI snapshot. It performs no bus access, DSP, model inference or filesystem activity.

## Measurement limits

The peak history is a UI-rate envelope history, not a sample waveform. Peak displays are sample peaks, not independently certified true peaks. Tone and side plots show commanded/applied band corrections rather than promising perfect source separation. Routing validity and ownership are factual status fields supplied by the processor. Aesthetic confidence is never manufactured.
