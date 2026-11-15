#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include "PluginProcessor.h"

class MasterLookAndFeel : public juce::LookAndFeel_V4
{
public:
    MasterLookAndFeel();
    juce::Font getTextButtonFont (juce::TextButton&, int h) override;
    juce::Font getLabelFont (juce::Label& l) override { return l.getFont(); }
    juce::Font getPopupMenuFont() override;
    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour&, bool hi, bool down) override;
    void drawToggleButton (juce::Graphics&, juce::ToggleButton&, bool hi, bool down) override;
    void drawScrollbar (juce::Graphics&, juce::ScrollBar&, int x, int y, int w, int h, bool vertical,
                        int thumbStart, int thumbSize, bool over, bool down) override;
};

// Segmented row of rusty buttons: AUTO + one per genre profile
class GenreBar : public juce::Component, public juce::SettableTooltipClient
{
public:
    void setGenres (juce::StringArray ids, juce::StringArray names);
    void setSelected (const juce::String& id) { if (selected != id) { selected = id; repaint(); } }
    void setDetected (const juce::String& id) { if (detected != id) { detected = id; repaint(); } }
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override { hover = -1; repaint(); }
    std::function<void (const juce::String&)> onPick;
private:
    int segmentAt (juce::Point<int>) const;
    juce::StringArray ids, names;
    juce::String selected { "auto" }, detected;
    int hover = -1;
};

// "SOUNDS LIKE": the auto genre detector's best guesses
class SoundsLike : public juce::Component
{
public:
    void paint (juce::Graphics&) override;
    juce::String first, second;
    float p1 = 0, p2 = 0;
    bool ready = false, locked = false;
    juce::String lockedName;
};

// Tonal balance: your mix vs. the genre, band by band (deviation from the genre average)
class TonalChart : public juce::Component
{
public:
    void paint (juce::Graphics&) override;
    void resized() override { grid = {}; }
    std::array<float, mst::kNumBands> dev {}, sd {};
    bool ready = false;
    juce::String genre;
private:
    void buildGrid();
    juce::Image grid;
};

// Horizontal gauges: your number on the genre's range
class Vitals : public juce::Component
{
public:
    struct Row { juce::String label, value; float lo, hi, v, mean, sd; bool hasRange = true, ready = false; bool warn = false; };
    void paint (juce::Graphics&) override;
    std::vector<Row> rows;
};

// Big stencilled score + tallies
class ScorePanel : public juce::Component
{
public:
    void paint (juce::Graphics&) override;
    int score = 0, fixes = 0, checks = 0, goods = 0;
    bool ready = false, saved = false;
    juce::String genre;
};

// The list of suggestions (lives in a Viewport)
class VerdictList : public juce::Component
{
public:
    void setItems (const std::vector<mst::Suggestion>& items, int width);
    void paint (juce::Graphics&) override;
private:
    struct Laid { mst::Suggestion s; juce::TextLayout body, tip; int y = 0, h = 0, bodyH = 0, tipH = 0; };
    std::vector<Laid> laid;
};

class PilotLamp : public juce::Component
{
public:
    void paint (juce::Graphics&) override;
    float heat = 0.0f;
};

// In-window modal message, drawn over the editor
class MasterDialog : public juce::Component
{
public:
    MasterDialog (const juce::String& title, const juce::String& message, juce::StringArray buttons,
                  std::function<void (int)> onClose);
    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;
    void parentHierarchyChanged() override;
    void mouseDown (const juce::MouseEvent&) override {}
private:
    void finish (int index);
    juce::String title, message;
    juce::OwnedArray<juce::TextButton> buttons;
    std::function<void (int)> onClose;
    juce::Rectangle<int> panel;
};

class MasterEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit MasterEditor (MasterProcessor&);
    ~MasterEditor() override;
    void paint (juce::Graphics&) override;
    void resized() override;

    // used by the headless snapshot test
    void refreshNow() { lastSnap = -1; timerCallback(); }

private:
    struct Plate { juce::String title; juce::Rectangle<int> bounds; bool hot = false; };
    void timerCallback() override;
    void buildBackdrop();
    void refreshGenres();
    void updateReport();
    void showProfilesMenu();
    void showMessage (const juce::String& title, const juce::String& text);
    void copyReport();

    MasterProcessor& proc;
    MasterLookAndFeel lnf;
    juce::TooltipWindow tooltips { this, 700 };

    PilotLamp lamp;
    juce::ToggleButton listenButton { "LISTEN" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> listenAtt;
    juce::TextButton resetButton { "RESET" }, profilesButton { "PROFILES" }, copyButton { "COPY" };
    GenreBar genreBar;
    SoundsLike soundsLike;
    TonalChart chart;
    Vitals vitals;
    ScorePanel score;
    juce::Viewport viewport;
    VerdictList verdict;
    std::unique_ptr<MasterDialog> dialog;
    std::unique_ptr<juce::FileChooser> chooser;

    std::vector<Plate> plates;
    juce::Rectangle<int> logoArea, badgeArea, crestArea, controlArea, stripeArea, statusArea;
    juce::Image backdrop, logoImage;
    juce::String statusText;
    float flicker = 0.0f, activity = 0.0f;
    int lastSnap = -1, lastLib = -1;
    juce::String lastGenre, lastDetected;
    mst::Report report;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MasterEditor)
};
