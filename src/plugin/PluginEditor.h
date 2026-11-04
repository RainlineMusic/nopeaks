#pragma once

#include "PluginProcessor.h"
#include <array>
#include <memory>
#include <utility>

// The editor reads a bounded processor snapshot. It never touches the shared bus.
class NoPeaksAudioProcessorEditor final : public juce::AudioProcessorEditor,
                                         private juce::Timer,
                                         private juce::ListBoxModel
{
public:
    explicit NoPeaksAudioProcessorEditor (NoPeaksAudioProcessor&);
    ~NoPeaksAudioProcessorEditor() override;
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

private:
    class PanelLookAndFeel;
    using Snapshot = decltype (std::declval<NoPeaksAudioProcessor>().getUiState());
    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;
    using ComboAttachment = juce::AudioProcessorValueTreeState::ComboBoxAttachment;

    NoPeaksAudioProcessor& processor;
    std::unique_ptr<PanelLookAndFeel> panelLook;
    Snapshot state {};
    const bool master;
    juce::TooltipWindow tooltips { this, 650 };
    juce::ComboBox group;
    juce::TextEditor session, trackName;
    juce::ToggleButton bypass { "Bypass" }, safety { "Safety" }, cooperative { "Track control" };
    juce::ToggleButton protect { "Protect track" };
    std::array<juce::ToggleButton, 3> modules;
    std::array<juce::Slider, 4> knobs;
    juce::Slider conflict;
    juce::TextButton capture { "Capture balance" }, reset { "Reset" };
    juce::ListBox tracks { "Connected tracks", this };
    std::array<std::unique_ptr<SliderAttachment>, 4> knobAttachments;
    std::array<std::unique_ptr<ButtonAttachment>, 3> moduleAttachments;
    std::unique_ptr<SliderAttachment> conflictAttachment;
    std::unique_ptr<ButtonAttachment> bypassAttachment, safetyAttachment, cooperativeAttachment, protectAttachment;
    std::unique_ptr<ComboAttachment> groupAttachment;
    std::array<float, 180> inputHistory {}, outputHistory {};
    int historyWrite = 0, historyCount = 0;
    bool draggingCeiling = false;

    void timerCallback() override;
    int getNumRows() override;
    void paintListBoxItem (int, juce::Graphics&, int, int, bool) override;
    void setSessionFromEditor();
    void setCeilingFromY (float);
    void drawMaster (juce::Graphics&);
    void drawTrack (juce::Graphics&);
    void drawBandGraph (juce::Graphics&, juce::Rectangle<float>, bool correctionsOnly);
    void drawPeakHistory (juce::Graphics&, juce::Rectangle<float>);
    void drawMeter (juce::Graphics&, juce::Rectangle<float>, float amplitude, const juce::String& name);
    juce::Point<float> logicalPoint (juce::Point<float>) const;
    float baseWidth() const noexcept { return master ? 1100.0f : 740.0f; }
    float baseHeight() const noexcept { return master ? 720.0f : 480.0f; }
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NoPeaksAudioProcessorEditor)
};
