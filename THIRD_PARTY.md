# Third-party components

## JUCE

JUCE8.0.12, pinned commit `29396c22c93392d6738e021b83196283d6e4d850`:
https://github.com/juce-framework/JUCE/tree/29396c22c93392d6738e021b83196283d6e4d850

JUCE is fetched at build time and is not vendored here. Its AGPL/commercial license terms apply to builds that include JUCE; obtain an appropriate JUCE license before distributing a closed-source product. Inspect the pinned revision's LICENSE.md for the authoritative terms. No JUCE commercial-license ownership is asserted by this repository.

The VST3 support used here is the version included in that JUCE revision. Follow the notices and license terms shipped with the dependency, including those applicable to the Steinberg SDK. VST is a trademark of Steinberg Media Technologies GmbH.

## Filter equations

Robert Bristow-Johnson, Audio EQ Cookbook, W3C working group note:
https://www.w3.org/TR/audio-eq-cookbook/

The implementation uses standard biquad equations and its own bounded controller code. Scientific papers and patents are referenced as research; no external model weights or paper code have been copied.

## User references

`assets/clipper-reference.html` and `docs/reference/signal-conditioned-eq.md` are the user-supplied design and research inputs. The latter is historical context; verified findings and differences are documented in `docs/RESEARCH.md`.

## Project source

No open-source license is granted for the NOPEAKS-specific source by this initial import. Copyright holders retain their rights. The user/repository owner should choose a distribution license deliberately before release.
