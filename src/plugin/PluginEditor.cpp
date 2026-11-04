#include "PluginEditor.h"
#include <algorithm>
#include <cmath>

namespace
{
const juce::Colour faceTop { 0xff262b34 }, faceBottom { 0xff16181c };
const juce::Colour well { 0xff101318 }, edge { 0xff3a424e };
const juce::Colour text { 0xffe5ebf4 }, secondary { 0xffacb9cb };
const juce::Colour silver { 0xffd3deed }, sideColour { 0xffd4ad7c };
const juce::Colour red { 0xfff75055 }, good { 0xffa5c8b5 };
constexpr std::array<float, 12> frequencies { 50, 80, 125, 200, 315, 500, 800, 1250, 2000, 3150, 5000, 10000 };

juce::Font font (float height, bool bold = false)
{
    return juce::Font (juce::FontOptions (height, bold ? juce::Font::bold : juce::Font::plain));
}

void label (juce::Graphics& g, juce::String value, juce::Rectangle<float> r,
            float height = 12.0f, juce::Colour colour = secondary,
            juce::Justification alignment = juce::Justification::centredLeft)
{
    g.setColour (colour);
    g.setFont (font (height));
    g.drawText (value, r, alignment, true);
}

float db (float value)
{
    return std::isfinite (value) && value > 0.00001585f ? 20.0f * std::log10 (value) : -96.0f;
}

float finiteDb (float value, float low = -96.0f, float high = 12.0f)
{
    return std::isfinite (value) ? juce::jlimit (low, high, value) : low;
}

juce::String dbText (float value, bool amplitude = false)
{
    const float v = amplitude ? db (value) : finiteDb (value);
    return v <= -95.5f ? juce::String ("-inf") : juce::String (v, 1);
}

void inset (juce::Graphics& g, juce::Rectangle<float> r)
{
    g.setColour (well);
    g.fillRoundedRectangle (r, 3.0f);
    g.setColour (edge);
    g.drawRoundedRectangle (r, 3.0f, 1.0f);
    g.setGradientFill (juce::ColourGradient (juce::Colours::black.withAlpha (0.36f), r.getX(), r.getY(),
                                           juce::Colours::transparentBlack, r.getX(), r.getY() + 12.0f, false));
    g.fillRoundedRectangle (r.reduced (1.0f), 2.0f);
}

void parameterValue (juce::AudioProcessorValueTreeState& parameters, const char* id, float value)
{
    if (auto* p = parameters.getParameter (id))
        p->setValueNotifyingHost (p->convertTo0to1 (value));
}
} // namespace

class NoPeaksAudioProcessorEditor::PanelLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    float scale = 1.0f;
    PanelLookAndFeel()
    {
        setColour (juce::Label::textColourId, text);
        setColour (juce::ComboBox::textColourId, text);
        setColour (juce::ComboBox::backgroundColourId, well);
        setColour (juce::ComboBox::outlineColourId, edge);
        setColour (juce::PopupMenu::backgroundColourId, juce::Colour (0xff20252d));
        setColour (juce::PopupMenu::textColourId, text);
        setColour (juce::PopupMenu::highlightedBackgroundColourId, juce::Colour (0xff465363));
        setColour (juce::PopupMenu::highlightedTextColourId, juce::Colours::white);
        setColour (juce::TextEditor::backgroundColourId, well);
        setColour (juce::TextEditor::textColourId, text);
        setColour (juce::TextEditor::outlineColourId, edge);
        setColour (juce::TextEditor::focusedOutlineColourId, silver);
        setColour (juce::TextEditor::highlightColourId, juce::Colour (0xff526274));
        setColour (juce::TextEditor::highlightedTextColourId, juce::Colours::white);
        setColour (juce::CaretComponent::caretColourId, silver);
        setColour (juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
        setColour (juce::ScrollBar::thumbColourId, juce::Colour (0xff647183));
        setColour (juce::TooltipWindow::backgroundColourId, juce::Colour (0xffe5ebf4));
        setColour (juce::TooltipWindow::textColourId, juce::Colour (0xff101318));
        setColour (juce::TooltipWindow::outlineColourId, juce::Colour (0xff101318));
    }

    void drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                           float position, float startAngle, float endAngle, juce::Slider& slider) override
    {
        const auto bounds = juce::Rectangle<float> ((float) x, (float) y, (float) width, (float) height).reduced (12.0f);
        const float radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
        const auto centre = bounds.getCentre();
        const float angle = startAngle + position * (endAngle - startAngle);
        const float alpha = slider.isEnabled() ? 1.0f : 0.38f;
        auto ring = juce::Rectangle<float> (radius * 2.0f, radius * 2.0f).withCentre (centre);
        g.setColour (juce::Colours::black.withAlpha (0.40f));
        g.fillEllipse (ring.translated (0.0f, 3.0f).expanded (1.0f));
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff343c48), ring.getX(), ring.getY(),
                                               juce::Colour (0xff111318), ring.getRight(), ring.getBottom(), false));
        g.fillEllipse (ring);
        g.setColour (juce::Colour (0xff090b0d));
        g.drawEllipse (ring, 1.5f);
        juce::Path track;
        track.addCentredArc (centre.x, centre.y, radius - 3.0f, radius - 3.0f, 0.0f, startAngle, endAngle, true);
        g.setColour (juce::Colour (0xff4b5665).withAlpha (alpha));
        g.strokePath (track, juce::PathStrokeType (1.3f));
        juce::Path arc;
        arc.addCentredArc (centre.x, centre.y, radius - 3.0f, radius - 3.0f, 0.0f, startAngle, angle, true);
        g.setColour (silver.withAlpha (alpha));
        g.strokePath (arc, juce::PathStrokeType (2.3f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        auto disk = ring.reduced (radius * 0.25f);
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff090c10), centre.x, disk.getY(),
                                               juce::Colour (0xff1c2128), centre.x, disk.getBottom(), false));
        g.fillEllipse (disk);
        g.setColour (juce::Colour (0xff3c454f));
        g.drawEllipse (disk, 1.0f);
        juce::Path needle;
        needle.startNewSubPath (centre.x + std::sin (angle) * (radius - 6.0f), centre.y - std::cos (angle) * (radius - 6.0f));
        needle.lineTo (centre.x + std::sin (angle) * (radius - 14.0f), centre.y - std::cos (angle) * (radius - 14.0f));
        g.setColour (text.withAlpha (alpha));
        g.strokePath (needle, juce::PathStrokeType (2.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        auto value = slider.getTextFromValue (slider.getValue());
        g.setFont (font ((value.length() > 7 ? 14.0f : 18.0f) * scale));
        g.drawText (value, disk.reduced (3.0f), juce::Justification::centred, true);
        if (slider.hasKeyboardFocus (true))
        {
            g.setColour (silver.withAlpha (0.7f));
            g.drawEllipse (ring.expanded (4.0f), 1.0f);
        }
    }

    void drawToggleButton (juce::Graphics& g, juce::ToggleButton& button, bool highlighted, bool down) override
    {
        auto r = button.getLocalBounds().toFloat();
        const auto box = juce::Rectangle<float> (r.getX() + 2.0f, r.getCentreY() - 5.0f, 10.0f, 10.0f);
        const auto ink = button.isEnabled() ? text : secondary.withAlpha (0.4f);
        g.setColour (button.getToggleState() ? ink : well);
        g.fillRoundedRectangle (box, 1.0f);
        g.setColour (highlighted || down || button.hasKeyboardFocus (true) ? silver : edge.brighter (0.3f));
        g.drawRoundedRectangle (box, 1.0f, 1.0f);
        g.setColour (ink);
        g.setFont (font (12.0f * scale));
        g.drawText (button.getButtonText(), r.withTrimmedLeft (20.0f), juce::Justification::centredLeft, true);
    }

    void drawButtonBackground (juce::Graphics& g, juce::Button& button, const juce::Colour&, bool highlighted, bool down) override
    {
        auto r = button.getLocalBounds().toFloat().reduced (0.5f);
        g.setColour (down ? juce::Colour (0xff3c4653) : highlighted ? juce::Colour (0xff2c343e) : juce::Colour (0xff20262e));
        g.fillRoundedRectangle (r, 3.0f);
        g.setColour (button.hasKeyboardFocus (true) ? silver : edge.brighter (0.1f));
        g.drawRoundedRectangle (r, 3.0f, 1.0f);
    }

    void drawButtonText (juce::Graphics& g, juce::TextButton& button, bool, bool) override
    {
        label (g, button.getButtonText(), button.getLocalBounds().toFloat().reduced (6.0f), 12.0f * scale,
               button.isEnabled() ? text : secondary.withAlpha (0.4f), juce::Justification::centred);
    }

    juce::Font getComboBoxFont (juce::ComboBox&) override { return font (13.0f * scale); }
    juce::Font getPopupMenuFont() override { return font (13.0f * scale); }
    void drawComboBox (juce::Graphics& g, int width, int height, bool, int, int, int, int, juce::ComboBox& box) override
    {
        auto r = juce::Rectangle<float> (0, 0, (float) width, (float) height).reduced (0.5f);
        g.setColour (well);
        g.fillRoundedRectangle (r, 3.0f);
        g.setColour (box.hasKeyboardFocus (true) ? silver : edge);
        g.drawRoundedRectangle (r, 3.0f, 1.0f);
        juce::Path arrow;
        const float cx = (float) width - 14.0f, cy = (float) height * 0.5f;
        arrow.startNewSubPath (cx - 3.0f, cy - 2.0f);
        arrow.lineTo (cx, cy + 1.0f);
        arrow.lineTo (cx + 3.0f, cy - 2.0f);
        g.setColour (silver);
        g.strokePath (arrow, juce::PathStrokeType (1.3f));
    }

    void drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height,
                           float sliderPos, float, float, juce::Slider::SliderStyle, juce::Slider& slider) override
    {
        auto r = juce::Rectangle<float> ((float) x, (float) y, (float) width, (float) height);
        const float cy = r.getCentreY();
        g.setColour (well);
        g.fillRoundedRectangle (r.getX(), cy - 2.0f, r.getWidth(), 4.0f, 2.0f);
        g.setColour (slider.isEnabled() ? silver : edge);
        g.fillRoundedRectangle (r.getX(), cy - 2.0f, juce::jmax (0.0f, sliderPos - r.getX()), 4.0f, 2.0f);
        g.fillEllipse (sliderPos - 4.0f, cy - 4.0f, 8.0f, 8.0f);
    }
};

NoPeaksAudioProcessorEditor::NoPeaksAudioProcessorEditor (NoPeaksAudioProcessor& p)
    : juce::AudioProcessorEditor (p), processor (p), panelLook (std::make_unique<PanelLookAndFeel>()), master (p.isMaster())
{
    setLookAndFeel (panelLook.get());
    setOpaque (true);
    for (int i = 1; i <= 16; ++i)
        group.addItem ("Group " + juce::String (i), i);
    group.setTooltip ("Master and Track must use the same group and project ID.");
    addAndMakeVisible (group);
    groupAttachment = std::make_unique<ComboAttachment> (processor.parameters, "group", group);

    session.setInputRestrictions (4, "0123456789");
    session.setFont (font (13));
    session.setJustification (juce::Justification::centredLeft);
    session.setSelectAllWhenFocused (true);
    session.setTooltip ("Use a unique project ID (1 to 9999). Other open projects must use a different ID.");
    session.onReturnKey = [this] { setSessionFromEditor(); session.giveAwayKeyboardFocus(); };
    session.onFocusLost = [this] { setSessionFromEditor(); };
    addAndMakeVisible (session);

    addAndMakeVisible (bypass);
    bypass.setTooltip ("Bypass correction while keeping the declared latency.");
    bypassAttachment = std::make_unique<ButtonAttachment> (processor.parameters, "bypass", bypass);

    for (auto& knob : knobs)
    {
        knob.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        knob.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        knob.setRotaryParameters (juce::MathConstants<float>::pi * 1.25f, juce::MathConstants<float>::pi * 2.75f, true);
        knob.setMouseDragSensitivity (220);
        knob.setWantsKeyboardFocus (true);
        knob.setScrollWheelEnabled (true);
    }

    if (master)
    {
        const std::array<const char*, 4> ids { "peakAmount", "toneAmount", "widthAmount", "ceiling" };
        const std::array<const char*, 4> names { "Peak amount", "Tone amount", "Width amount", "Peak ceiling" };
        for (size_t i = 0; i < knobs.size(); ++i)
        {
            addAndMakeVisible (knobs[i]);
            knobs[i].setName (names[i]);
            knobs[i].setTitle (names[i]);
            knobAttachments[i] = std::make_unique<SliderAttachment> (processor.parameters, ids[i], knobs[i]);
            if (auto* parameter = processor.parameters.getParameter (ids[i]))
                knobs[i].setDoubleClickReturnValue (true, parameter->convertFrom0to1 (parameter->getDefaultValue()));
            knobs[i].setTextValueSuffix (i == 3 ? " dB" : "%");
            knobs[i].setNumDecimalPlacesToDisplay (i == 3 ? 1 : 0);
            knobs[i].textFromValueFunction = [i] (double value) { return juce::String (value, i == 3 ? 1 : 0); };
        }
        knobs[0].setTooltip ("Amount of priority-weighted peak reduction on linked tracks. Protected tracks remain unchanged.");
        knobs[1].setTooltip ("Amount of per-track EQ toward the captured mix balance. Band plots show filter settings, not the complete overlapping-filter transfer. No target is imposed before capture.");
        knobs[2].setTooltip ("Amount of side attenuation assigned to lower-priority tracks in negatively correlated bands. Band plots show filter settings, not the complete overlapping-filter transfer.");
        knobs[3].setTooltip ("Sample-peak ceiling. The safety limiter does not claim intersample true-peak protection.");

        const std::array<const char*, 3> moduleIds { "peakEnabled", "toneEnabled", "widthEnabled" };
        const std::array<const char*, 3> moduleNames { "Peaks", "Tone", "Width" };
        for (size_t i = 0; i < modules.size(); ++i)
        {
            modules[i].setButtonText (moduleNames[i]);
            modules[i].setTitle (juce::String (moduleNames[i]) + " enabled");
            addAndMakeVisible (modules[i]);
            moduleAttachments[i] = std::make_unique<ButtonAttachment> (processor.parameters, moduleIds[i], modules[i]);
        }
        addAndMakeVisible (safety);
        safety.setTooltip ("Stereo-linked sample-peak safety limiter on Master. Available while track routing is unverified.");
        safetyAttachment = std::make_unique<ButtonAttachment> (processor.parameters, "safetyEnabled", safety);
        addAndMakeVisible (cooperative);
        cooperative.setTooltip ("Enable track control only for non-overlapping stems at unity gain after each Track insert. Routing must also pass validation.");
        cooperativeAttachment = std::make_unique<ButtonAttachment> (processor.parameters, "cooperative", cooperative);
        addAndMakeVisible (capture);
        capture.setTooltip ("Requires linked tracks and matched routing. Capture two seconds of non-silent linked-track band balance. Later excess energy is reduced on lower-priority tracks; no band is boosted.");
        capture.onClick = [this] { processor.captureTarget(); };
        addAndMakeVisible (reset);
        reset.setTooltip ("Discard the captured balance and return tonal target correction to zero.");
        reset.onClick = [this] { processor.resetTarget(); };
        addAndMakeVisible (conflict);
        conflict.setSliderStyle (juce::Slider::LinearHorizontal);
        conflict.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        conflict.setName ("Conflict reduction");
        conflict.setTooltip ("Conservative overlap reduction around higher-priority tracks. This is a heuristic, not a learned ideal EQ.");
        conflictAttachment = std::make_unique<SliderAttachment> (processor.parameters, "conflictAmount", conflict);
        addAndMakeVisible (tracks);
        tracks.setRowHeight (48);
        tracks.setOutlineThickness (0);
        tracks.setMultipleSelectionEnabled (false);
        tracks.setColour (juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
        tracks.setWantsKeyboardFocus (true);
        setSize (1100, 720);
        setResizeLimits (1100, 720, 1650, 1080);
    }
    else
    {
        addAndMakeVisible (knobs[0]);
        knobs[0].setName ("Track priority");
        knobs[0].setTitle ("Track priority");
        knobs[0].setTooltip ("Higher values protect this track relative to lower-priority tracks. Use Protect track for a strict no-change constraint.");
        knobs[0].setNumDecimalPlacesToDisplay (0);
        knobAttachments[0] = std::make_unique<SliderAttachment> (processor.parameters, "priority", knobs[0]);
        knobs[0].textFromValueFunction = [] (double value) { return juce::String (juce::roundToInt (value)); };
        if (auto* parameter = processor.parameters.getParameter ("priority"))
            knobs[0].setDoubleClickReturnValue (true, parameter->convertFrom0to1 (parameter->getDefaultValue()));
        addAndMakeVisible (protect);
        protect.setTooltip ("Hard protection blocks direct Track correction. Master Safety still acts on the whole mix.");
        protectAttachment = std::make_unique<ButtonAttachment> (processor.parameters, "protect", protect);
        trackName.setText (processor.getTrackName(), false);
        trackName.setFont (font (14.0f));
        trackName.setTextToShowWhenEmpty ("Name this track", secondary);
        trackName.setInputRestrictions (40);
        trackName.setTooltip ("This name appears in Master's connected-track list.");
        trackName.onReturnKey = [this] { processor.setTrackName (trackName.getText()); trackName.giveAwayKeyboardFocus(); };
        trackName.onFocusLost = [this] { processor.setTrackName (trackName.getText()); };
        addAndMakeVisible (trackName);
        setSize (740, 480);
        setResizeLimits (740, 480, 1110, 720);
    }
    setResizable (true, true);
    if (auto* constrainer = getConstrainer())
        constrainer->setFixedAspectRatio (baseWidth() / baseHeight());
    timerCallback();
    startTimerHz (30);
}

NoPeaksAudioProcessorEditor::~NoPeaksAudioProcessorEditor()
{
    stopTimer();
    tracks.setModel (nullptr);
    setLookAndFeel (nullptr);
}

void NoPeaksAudioProcessorEditor::setSessionFromEditor()
{
    const int value = juce::jlimit (1, 9999, session.getText().getIntValue());
    if (auto* parameter = processor.parameters.getParameter ("session"))
    {
        parameter->beginChangeGesture();
        parameterValue (processor.parameters, "session", (float) value);
        parameter->endChangeGesture();
    }
    session.setText (juce::String (value), false);
}

void NoPeaksAudioProcessorEditor::timerCallback()
{
    state = processor.getUiState();
    if (! session.hasKeyboardFocus (true))
        if (auto* value = processor.parameters.getRawParameterValue ("session"))
            session.setText (juce::String ((int) value->load()), false);
    inputHistory[(size_t) historyWrite] = db (state.inputPeak);
    outputHistory[(size_t) historyWrite] = db (state.outputPeak);
    historyWrite = (historyWrite + 1) % (int) inputHistory.size();
    historyCount = juce::jmin (historyCount + 1, (int) inputHistory.size());
    if (master)
    {
        tracks.updateContent();
        tracks.repaint();
        capture.setEnabled (! state.targetCapturing && state.inputPeak > 0.0001f && state.connectedTracks > 0 && state.routingValid);
        capture.setButtonText (state.targetCapturing ? "Capturing..." : state.targetCaptured ? "Recapture balance" : "Capture balance");
        reset.setEnabled (state.targetCaptured || state.targetCapturing);
        for (size_t i = 0; i < modules.size(); ++i)
            knobs[i].setEnabled (modules[i].getToggleState() && ! bypass.getToggleState());
        knobs[3].setEnabled (! bypass.getToggleState());
        conflict.setEnabled (modules[1].getToggleState() && ! bypass.getToggleState());
    }
    repaint();
}

void NoPeaksAudioProcessorEditor::resized()
{
    const float sx = (float) getWidth() / baseWidth();
    const float sy = (float) getHeight() / baseHeight();
    panelLook->scale = juce::jmin (sx, sy);
    session.setFont (font (13.0f * sy));
    trackName.setFont (font (14.0f * sy));
    const auto place = [sx, sy] (juce::Component& c, int x, int y, int w, int h)
    {
        c.setBounds (juce::roundToInt ((float) x * sx), juce::roundToInt ((float) y * sy),
                     juce::roundToInt ((float) w * sx), juce::roundToInt ((float) h * sy));
    };
    if (master)
    {
        place (group, 432, 37, 94, 30);
        place (session, 544, 37, 84, 30);
        place (cooperative, 650, 37, 132, 30);
        place (safety, 804, 37, 90, 30);
        place (bypass, 952, 37, 98, 30);
        place (capture, 510, 101, 136, 30);
        place (reset, 656, 101, 78, 30);
        for (size_t i = 0; i < modules.size(); ++i)
            place (modules[i], 88 + (int) i * 182, 520, 112, 24);
        for (size_t i = 0; i < knobs.size(); ++i)
            place (knobs[i], 78 + (int) i * 182, 550, 132, 128);
        place (tracks, 772, 192, 280, 335);
        tracks.setRowHeight (juce::roundToInt (48.0f * sy));
        place (conflict, 790, 573, 244, 24);
    }
    else
    {
        place (group, 344, 37, 94, 30);
        place (session, 454, 37, 84, 30);
        place (bypass, 608, 37, 94, 30);
        place (trackName, 36, 111, 456, 34);
        place (knobs[0], 540, 175, 164, 154);
        place (protect, 564, 339, 142, 28);
    }
}

void NoPeaksAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.addTransform (juce::AffineTransform::scale ((float) getWidth() / baseWidth(), (float) getHeight() / baseHeight()));
    auto panel = juce::Rectangle<float> (0, 0, baseWidth(), baseHeight());
    g.setGradientFill (juce::ColourGradient (faceTop, panel.getCentreX(), 0, faceBottom, panel.getCentreX(), panel.getBottom(), false));
    g.fillRect (panel);
    g.setColour (juce::Colour (0xff404752));
    g.drawRect (panel.reduced (0.5f), 1.0f);
    g.setColour (juce::Colour (0xff414953));
    g.drawHorizontalLine (84, 30, panel.getRight() - 30);
    g.drawHorizontalLine (688, 30, panel.getRight() - 30);

    // Brand geometry follows the supplied Clipper's diagonal signal mark.
    juce::Path mark;
    mark.startNewSubPath (34, 49); mark.lineTo (51, 32);
    mark.startNewSubPath (34, 61); mark.lineTo (49, 46); mark.lineTo (66, 46);
    g.setColour (silver);
    g.strokePath (mark, juce::PathStrokeType (2.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.setFont (font (25, true));
    g.setColour (text);
    g.drawText ("NOPEAKS", 82, 26, 220, 30, juce::Justification::centredLeft);
    label (g, master ? "MASTER" : "TRACK", { 83, 56, 110, 15 }, 10.0f);
    label (g, "LINK", { master ? 432.0f : 344.0f, 17, 94, 16 }, 10.0f);
    label (g, "PROJECT ID", { master ? 544.0f : 454.0f, 17, 84, 16 }, 10.0f);
    if (master) drawMaster (g); else drawTrack (g);
}

void NoPeaksAudioProcessorEditor::drawMaster (juce::Graphics& g)
{
    label (g, state.connectedTracks > 0 ? "Linked-source balance" : "Mix balance", { 74, 99, 250, 31 }, 15.0f, text);
    label (g, "RELATIVE ANALYSIS BAND POWER", { 74, 131, 280, 15 }, 9.0f);
    drawBandGraph (g, { 74, 153, 660, 238 }, false);
    drawMeter (g, { 36, 153, 13, 238 }, state.inputPeak, "IN");
    drawMeter (g, { 740, 153, 13, 238 }, state.outputPeak, "OUT");
    label (g, "Peak history", { 74, 407, 200, 20 }, 12.0f, text);
    label (g, "Safety reduction " + juce::String (juce::jmax (0.0f, finiteDb (state.gainReduction, 0, 96)), 1) + " dB", { 374, 407, 240, 20 }, 10.0f, silver, juce::Justification::centredRight);
    label (g, "6 seconds", { 616, 407, 118, 20 }, 10.0f, secondary, juce::Justification::centredRight);
    drawPeakHistory (g, { 74, 434, 660, 66 });

    g.setColour (edge);
    g.drawVerticalLine (759, 102, 668);
    label (g, "Linked tracks", { 780, 101, 170, 30 }, 15.0f, text);
    label (g, juce::String (state.connectedTracks), { 1004, 101, 40, 30 }, 15.0f, text, juce::Justification::centredRight);
    const bool trusted = state.routingValid && cooperative.getToggleState();
    juce::String status = ! state.connectionError.isEmpty() ? "Link unavailable" : state.duplicateMaster ? "Another Master owns this link" : trusted ? "Routing matched" : "Routing unverified";
    label (g, status, { 780, 137, 270, 20 }, 11.0f, state.duplicateMaster ? red : trusted ? good : sideColour);
    label (g, "TRACK", { 780, 171, 160, 15 }, 9.0f);
    label (g, "PRIORITY", { 972, 171, 72, 15 }, 9.0f, secondary, juce::Justification::centredRight);
    if (state.trackCount == 0)
    {
        label (g, "Add NOPEAKS Track to your stems.", { 786, 222, 250, 26 }, 12.0f, text);
        label (g, "Match their group and project ID.", { 786, 248, 250, 24 }, 11.0f);
    }
    g.setColour (edge);
    g.drawHorizontalLine (544, 782, 1046);
    label (g, "Conflict reduction", { 790, 551, 208, 20 }, 12.0f, text);
    const auto* conflictValue = processor.parameters.getRawParameterValue ("conflictAmount");
    const float conflictAmount = conflictValue != nullptr ? conflictValue->load() : 0;
    label (g, juce::String (juce::roundToInt (conflictAmount * (conflict.getMaximum() <= 1.0 ? 100.0f : 1.0f))) + "%",
           { 980, 551, 54, 20 }, 11.0f, silver, juce::Justification::centredRight);
    label (g, "Max tone setting", { 790, 610, 112, 20 }, 11.0f);
    label (g, "Max side setting", { 930, 610, 112, 20 }, 11.0f, sideColour);
    float tone = 0, side = 0;
    for (size_t i = 0; i < state.toneDb.size(); ++i)
    { tone = juce::jmin (tone, finiteDb (state.toneDb[i], -18, 0)); side = juce::jmin (side, finiteDb (state.sideDb[i], -18, 0)); }
    label (g, juce::String (tone, 1) + " dB", { 790, 632, 112, 25 }, 17.0f, silver);
    label (g, juce::String (side, 1) + " dB", { 930, 632, 112, 25 }, 17.0f, sideColour);

    label (g, "CEILING", { 633, 520, 112, 24 }, 12.0f, text, juce::Justification::centred);
    for (float x : { 235.0f, 417.0f, 599.0f })
    { g.setColour (edge); g.drawVerticalLine ((int) x, 553, 660); }
    label (g, "RAINLINE MUSIC", { 32, 694, 170, 16 }, 9.0f);
    juce::String footer;
    if (! state.connectionError.isEmpty()) footer = state.connectionError;
    else if (state.duplicateMaster) footer = "Choose a different group or project ID for this Master.";
    else if (bypass.getToggleState()) footer = "Bypassed  |  Latency preserved";
    else if (! cooperative.getToggleState()) footer = "Track control off  |  Safety " + juce::String (safety.getToggleState() ? "on" : "off");
    else if (! state.routingValid) footer = "Track control waiting for matching routing";
    else footer = "Track control active  |  Higher priorities are preserved";
    label (g, footer, { 220, 694, 830, 16 }, 10.0f, secondary, juce::Justification::centredRight);
}

void NoPeaksAudioProcessorEditor::drawTrack (juce::Graphics& g)
{
    label (g, "TRACK NAME", { 36, 91, 300, 16 }, 10.0f);
    label (g, "Applied band settings", { 36, 163, 320, 23 }, 14.0f, text);
    drawBandGraph (g, { 36, 201, 456, 142 }, true);
    label (g, "PRIORITY", { 540, 151, 164, 24 }, 12.0f, text, juce::Justification::centred);
    label (g, "Higher values preserve this track.", { 516, 381, 200, 18 }, 10.0f, secondary, juce::Justification::centred);
    g.setColour (edge);
    g.drawVerticalLine (516, 110, 406);
    g.drawHorizontalLine (376, 36, 492);
    const std::array<juce::String, 3> titles { "INPUT", "OUTPUT", "PEAK REDUCTION" };
    const std::array<juce::String, 3> values { dbText (state.inputPeak, true) + " dB", dbText (state.outputPeak, true) + " dB",
                                             juce::String (juce::jmax (0.0f, finiteDb (state.gainReduction, 0, 96)), 1) + " dB" };
    for (size_t i = 0; i < titles.size(); ++i)
    {
        label (g, titles[i], { 36.0f + (float) i * 158.0f, 386, 150, 16 }, 9.0f);
        label (g, values[i], { 36.0f + (float) i * 158.0f, 405, 150, 26 }, 19.0f, text);
    }
    g.setColour (edge);
    g.drawHorizontalLine (445, 30, 710);
    juce::String status = ! state.connectionError.isEmpty() ? "Link unavailable: " + state.connectionError : state.duplicateMaster ? "Multiple Masters: choose another link" : state.masterConnected ? "Master connected" : "Waiting for Master";
    if (protect.getToggleState()) status += "  |  Protected";
    if (bypass.getToggleState()) status += "  |  Bypassed";
    label (g, status, { 36, 451, 540, 18 }, 10.0f, state.masterConnected ? good : secondary);
    label (g, "RAINLINE MUSIC", { 566, 451, 140, 18 }, 9.0f, secondary, juce::Justification::centredRight);
}

void NoPeaksAudioProcessorEditor::drawBandGraph (juce::Graphics& g, juce::Rectangle<float> outer, bool correctionsOnly)
{
    inset (g, outer);
    auto plot = outer.reduced (15.0f, 12.0f).withTrimmedBottom (19.0f).withTrimmedLeft (21.0f);
    const auto xAt = [&plot] (float hz)
    { return plot.getX() + std::log (hz / 50.0f) / std::log (200.0f) * plot.getWidth(); };
    const auto yAt = [&plot, correctionsOnly] (float value)
    {
        const float low = correctionsOnly ? -18.0f : -72.0f;
        return plot.getBottom() - juce::jlimit (0.0f, 1.0f, (finiteDb (value, low, 0) - low) / -low) * plot.getHeight();
    };
    const std::array<float, 4> ticks = correctionsOnly ? std::array<float, 4> { 0, -6, -12, -18 } : std::array<float, 4> { 0, -24, -48, -72 };
    for (float tick : ticks)
    {
        const float y = yAt (tick);
        g.setColour (edge.withAlpha (0.5f));
        g.drawHorizontalLine (juce::roundToInt (y), plot.getX(), plot.getRight());
        label (g, juce::String ((int) tick), { outer.getX() + 2, y - 7, 28, 14 }, 9, secondary, juce::Justification::centredRight);
    }
    for (size_t i = 0; i < frequencies.size(); ++i)
    {
        if (i % 2 != 0 && i != frequencies.size() - 1) continue;
        float hz = frequencies[i];
        const float x = xAt (hz);
        label (g, hz < 1000 ? juce::String ((int) hz) : juce::String (hz / 1000, hz == 1250 ? 2 : hz == 3150 ? 2 : 0) + "k",
               { x - 20, outer.getBottom() - 21, 40, 15 }, 9, secondary, juce::Justification::centred);
    }
    const auto makePath = [&] (const auto& values)
    {
        juce::Path path;
        for (size_t i = 0; i < frequencies.size(); ++i)
        {
            auto point = juce::Point<float> (xAt (frequencies[i]), yAt (values[i]));
            if (i == 0) path.startNewSubPath (point); else path.lineTo (point);
        }
        return path;
    };
    if (correctionsOnly)
    {
        g.setColour (silver);
        g.strokePath (makePath (state.toneDb), juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour (sideColour);
        g.strokePath (makePath (state.sideDb), juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        label (g, "Tone", { outer.getRight() - 125, outer.getY() - 22, 52, 18 }, 10, silver);
        label (g, "Side", { outer.getRight() - 57, outer.getY() - 22, 52, 18 }, 10, sideColour);
    }
    else
    {
        auto path = makePath (state.bandDb);
        auto area = path;
        area.lineTo (plot.getRight(), plot.getBottom());
        area.lineTo (plot.getX(), plot.getBottom());
        area.closeSubPath();
        g.setGradientFill (juce::ColourGradient (silver.withAlpha (0.16f), plot.getX(), plot.getY(),
                                               silver.withAlpha (0.015f), plot.getX(), plot.getBottom(), false));
        g.fillPath (area);
        g.setColour (silver);
        g.strokePath (path, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        if (state.targetCaptured)
        {
            auto target = makePath (state.targetDb);
            juce::Path dashed;
            const float lengths[] { 4, 4 };
            juce::PathStrokeType (1.2f).createDashedStroke (dashed, target, lengths, 2);
            g.setColour (sideColour);
            g.fillPath (dashed);
            label (g, "Captured balance", { outer.getRight() - 158, outer.getY() + 7, 140, 18 }, 10, sideColour, juce::Justification::centredRight);
        }
        if (state.inputPeak <= 0.00001f)
            label (g, "Play audio to analyse the mix", { plot.getX(), plot.getCentreY() - 12, plot.getWidth(), 24 }, 12.0f, secondary, juce::Justification::centred);
    }
}

void NoPeaksAudioProcessorEditor::drawPeakHistory (juce::Graphics& g, juce::Rectangle<float> outer)
{
    inset (g, outer);
    auto plot = outer.reduced (8.0f, 7.0f);
    const auto yAt = [&plot] (float value)
    { return plot.getBottom() - juce::jlimit (0.0f, 1.0f, (finiteDb (value, -48, 3) + 48) / 51) * plot.getHeight(); };
    const auto historyPath = [&] (const auto& values)
    {
        juce::Path path;
        for (int i = 0; i < historyCount; ++i)
        {
            int index = (historyWrite - historyCount + i + (int) values.size()) % (int) values.size();
            float x = plot.getRight() - (float) (historyCount - i - 1) * plot.getWidth() / (float) (values.size() - 1);
            float y = yAt (values[(size_t) index]);
            if (i == 0) path.startNewSubPath (x, y); else path.lineTo (x, y);
        }
        return path;
    };
    const auto* ceilingValue = processor.parameters.getRawParameterValue ("ceiling");
    const float ceilingDb = ceilingValue != nullptr ? ceilingValue->load() : -1.0f;
    const float y = yAt (ceilingDb);
    g.setColour (red.withAlpha (0.5f));
    const float dash[] { 4, 4 };
    g.drawDashedLine (juce::Line<float> (plot.getX(), y, plot.getRight(), y), dash, 2, 1.0f);
    g.setColour (secondary.withAlpha (0.4f));
    g.strokePath (historyPath (inputHistory), juce::PathStrokeType (1.0f));
    g.setColour (silver);
    g.strokePath (historyPath (outputHistory), juce::PathStrokeType (1.5f));
}

void NoPeaksAudioProcessorEditor::drawMeter (juce::Graphics& g, juce::Rectangle<float> r, float amplitude, const juce::String& name)
{
    g.setColour (juce::Colour (0xff080b0f));
    g.fillRect (r);
    g.setColour (edge);
    g.drawRect (r, 1);
    const float level = juce::jlimit (0.0f, 1.0f, (db (amplitude) + 72.0f) / 72.0f);
    const auto fill = r.reduced (2).withTrimmedTop ((r.getHeight() - 4) * (1 - level));
    g.setColour (amplitude > 1.0f ? red : silver);
    g.fillRect (fill);
    label (g, name, { r.getX() - 12, r.getY() - 21, 38, 15 }, 9, secondary, juce::Justification::centred);
    label (g, dbText (amplitude, true), { r.getX() - 17, r.getBottom() + 8, 46, 15 }, 9, secondary, juce::Justification::centred);
}

int NoPeaksAudioProcessorEditor::getNumRows() { return juce::jlimit (0, 64, state.trackCount); }

void NoPeaksAudioProcessorEditor::paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected)
{
    if (row < 0 || row >= getNumRows()) return;
    const auto& track = state.tracks[(size_t) row];
    const float scale = (float) getWidth() / baseWidth();
    g.addTransform (juce::AffineTransform::scale (scale));
    auto r = juce::Rectangle<float> (0, 0, (float) width / scale, (float) height / scale);
    if (selected) { g.setColour (juce::Colour (0xff303946)); g.fillRect (r.reduced (1)); }
    auto name = track.name.isEmpty() ? "Track " + juce::String (row + 1) : track.name;
    label (g, name, { 8, 3, r.getWidth() - 94, 21 }, 12, track.connected ? text : secondary);
    label (g, track.protect ? "Protected" : juce::String (juce::roundToInt (track.priority * 100.0f)), { r.getWidth() - 84, 3, 76, 21 }, 11,
           track.protect ? sideColour : silver, juce::Justification::centredRight);
    const float barY = r.getHeight() - 13;
    g.setColour (edge.withAlpha (0.6f));
    g.fillRect (8.0f, barY, r.getWidth() - 106, 3.0f);
    const float activity = juce::jlimit (0.0f, 1.0f, (finiteDb (track.peakDb, -72, 0) + 72) / 72);
    g.setColour (silver.withAlpha (0.55f));
    g.fillRect (8.0f, barY, (r.getWidth() - 106) * activity, 3.0f);
    const float tone = std::abs (finiteDb (track.toneReduction, -18, 18));
    const float side = std::abs (finiteDb (track.sideReduction, -18, 18));
    label (g, juce::String (tone, 1) + " / " + juce::String (side, 1), { r.getWidth() - 90, barY - 6, 82, 16 }, 9, secondary, juce::Justification::centredRight);
    g.setColour (edge.withAlpha (0.3f));
    g.drawHorizontalLine ((int) r.getHeight() - 1, 8, r.getWidth() - 8);
}

juce::Point<float> NoPeaksAudioProcessorEditor::logicalPoint (juce::Point<float> p) const
{
    return { p.x * baseWidth() / (float) getWidth(), p.y * baseHeight() / (float) getHeight() };
}

void NoPeaksAudioProcessorEditor::setCeilingFromY (float y)
{
    constexpr float top = 441.0f, height = 52.0f;
    const float value = juce::jlimit (-24.0f, 0.0f, -48.0f + (top + height - y) / height * 51.0f);
    parameterValue (processor.parameters, "ceiling", value);
}

void NoPeaksAudioProcessorEditor::mouseDown (const juce::MouseEvent& event)
{
    if (! master || bypass.getToggleState()) return;
    auto p = logicalPoint (event.position);
    if (juce::Rectangle<float> (74, 434, 660, 66).contains (p))
    {
        draggingCeiling = true;
        if (auto* parameter = processor.parameters.getParameter ("ceiling")) parameter->beginChangeGesture();
        setCeilingFromY (p.y);
    }
}

void NoPeaksAudioProcessorEditor::mouseDrag (const juce::MouseEvent& event)
{
    if (draggingCeiling) setCeilingFromY (logicalPoint (event.position).y);
}

void NoPeaksAudioProcessorEditor::mouseUp (const juce::MouseEvent&)
{
    if (draggingCeiling)
    {
        draggingCeiling = false;
        if (auto* parameter = processor.parameters.getParameter ("ceiling")) parameter->endChangeGesture();
    }
}
