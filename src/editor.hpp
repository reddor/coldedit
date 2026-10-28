#pragma once
#include "document.hpp"
#include "processing.hpp"
#include "pitch_time.hpp"
#include "playback.hpp"
#include "recording.hpp"
#include "viewport.hpp"
#include "imgui.h"
#include "interaction.hpp"
#include "scaled_draw.hpp"
#include <functional>
#include <mutex>
#include <set>
struct GLFWwindow;
namespace coldedit {
class Editor {
public:
    explicit Editor(GLFWwindow* window);
    ~Editor();
    void render();
    // Call between frames, before the OpenGL backend uploads the font atlas.
    void updateDpi(float contentScale, float framebufferScale);
    float uiScale() const { return dpi_.layout; }
    void requestOpen(const std::filesystem::path& path);
    void requestPasteFile(const std::filesystem::path& path);
    void requestClose();
    void requestRecording();
    void loadSmokeAudio();
    bool shouldClose() const { return close_; }
    bool animating() const;
    void smokeStep(int frame);
    void smokeInput(int frame);
    bool smokePassed() const { return smokePassed_ && smokeEdgesPassed_ && smokeBoundaryChecks_ == 4; }
    std::string diagnostics() const;
#ifdef __EMSCRIPTEN__
    bool canImport() const { return !recording_ && !busy_ && !confirmDiscard_ && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId); }
    bool beginRecording(unsigned rate, unsigned channels);
    bool appendRecording(const float* samples, unsigned frames);
    bool endRecording(bool keep);
    const std::string& recordingError() const { return recordingError_; }
#endif
private:
    GLFWwindow* window_;
    Document document_;
    Playback playback_;
    Viewport view_;
    // File metadata must not keep obsolete source PCM alive after clearing undo.
    struct SourceInfo {
        unsigned sampleRate, bits;
        bool floating;
        Frame frames;
        std::string format;
        explicit SourceInfo(const std::shared_ptr<AudioFile>& f) : sampleRate(f->sampleRate), bits(f->bits),
            floating(f->floating), frames(f->frames), format(f->format) {}
    };
    std::unique_ptr<SourceInfo> source_;
    std::filesystem::path sourcePath_, pendingPath_;
    std::vector<Piece> clipboard_;
    unsigned clipboardRate_ = 0, clipboardChannels_ = 0;
    Frame anchor_ = 0, head_ = 0, cursor_ = 0;
    bool selecting_ = false, loop_ = false, follow_ = false, close_ = false;
    bool fullSelectionAtClick_ = false;
    bool rulerPanning_ = false;
    bool rulerClickPending_ = false;
    Frame rulerClickFrame_ = 0;
    bool wheelZooms_ = true;
    bool compact_ = false, compactTools_ = false;
    SelectionBoundaryDrag boundaryDrag_;
    SelectionRangeDrag rangeDrag_;
    Frame boundaryDragLimit_ = 0;
    bool pendingClose_ = false, confirmDiscard_ = false, help_ = false, openPathPopup_ = false;
    bool continueAfterExport_ = false;
    bool opening_ = false;
    bool recording_ = false, recordPopup_ = false, pendingRecord_ = false;
    std::string recordingError_;
#ifdef __EMSCRIPTEN__
    std::unique_ptr<AudioRecording> recordingAudio_;
#else
    std::unique_ptr<MicrophoneRecording> recordingAudio_;
#endif
    void showRecorder();
    void acceptRecording(std::shared_ptr<AudioFile> audio);
    std::string openingName_;
    bool pasteFilePath_ = false;
    bool exportPathPopup_ = false, selectionExport_ = false, insertSilencePopup_ = false;
    double silenceSeconds_ = 1;
    Frame silenceFrames_ = 48000, silenceAt_ = 0;
    int fadeCurve_ = 0;
    bool snapZero_ = false, autoEdgeFades_ = false;
    float edgeFadeMs_ = 3;
    bool cleanupPopup_ = false, cleanupClose_ = false, cleanupBypass_ = false;
    int cleanupKind_ = 0;
    float normalizeDb_ = -1, highPassHz_ = 80, lowPassHz_ = 12000;
    Frame cleanupFirst_ = 0, cleanupLast_ = 0, cleanupCursor_ = 0;
    std::shared_ptr<const Timeline> cleanupBase_;
    std::shared_ptr<Timeline> cleanupPreview_;
    float gainDb_ = 0, volumeDb_ = -2, verticalZoom_ = 1;
    bool stretchMode_ = false, stretchSnap_ = true, stretchPopup_ = false;
    double stretchBpm_ = 120, stretchBpmInput_ = 120;
    int stretchDragging_ = -1, stretchContextAnchor_ = -1;
    double stretchDragOffset_ = 0, stretchContextFrame_ = 0;
    std::unique_ptr<PitchTimeState> stretchDrag_;
    std::shared_ptr<Timeline> stretchAudition_;
    PitchTimeRenderQueue pitchTimeRenderer_;
    std::uint64_t pitchTimeRevision_ = 0;
    std::string rhythmStatus_;
    std::array<ImVec2, 5> stretchControlPositions_{}; // logical positions for UI diagnostics
    bool pitchPopup_ = false;
    PitchSettings pitchInput_;
    std::array<ImVec2, 7> pitchControlPositions_{};
    bool pitchView_ = false, pitchSnap_ = true;
    int pitchNoteSelected_ = -1, pitchNoteDragging_ = -1;
    using NoteKey = PitchTimeState::NoteKey;
    std::set<NoteKey> pitchSelection_, pitchMarqueeBase_, pitchMarqueeBefore_;
    NoteKey pitchSelectionAnchor_{0, -1};
    NoteKey pitchMarqueeAnchor_{0, -1};
    bool pitchSplitMode_ = false, pitchMarquee_ = false, pitchDragMoved_ = false;
    ImVec2 pitchMarqueeStart_{};
    float pitchVibratoInput_ = 100;
    bool pitchVibratoMixed_ = false;
    bool pitchVibratoEditing_ = false, pitchVibratoCancelled_ = false;
    std::shared_ptr<const Timeline> pitchVibratoBase_;
    std::set<NoteKey> pitchVibratoSelection_;
    float harmonyInterval_ = 7, harmonyLevel_ = 35;
    int harmonySelected_ = -1, harmonyLevelVoice_ = -1;
    bool harmonyLevelEditing_ = false, harmonyLevelCancelled_ = false;
    std::shared_ptr<const Timeline> harmonyLevelBase_;
    float pitchDragY_ = 0;
    double pitchDragShift_ = 0, pitchDragOriginal_ = 0;
    double pitchLow_ = 48, pitchHigh_ = 84;
    float pitchStep_ = 1;
    std::shared_ptr<const Timeline> pitchDragBase_;
    std::vector<ImVec2> noteControlPositions_ = std::vector<ImVec2>(13);
    std::vector<NoteKey> pitchContextNotes_;
    std::weak_ptr<const Timeline> pitchContextBase_;
    ImVec2 pitchPlotMin_{}, pitchPlotMax_{};
    float pitchRulerY_ = 0;
    struct NotePosition { ImVec2 centre; float height; };
    std::vector<NotePosition> notePositions_;
    int exportFormat_ = 1;
    std::string status_ = "Ready", error_, title_ = "Untitled";
    ImFont* bodyFont_ = nullptr;
    ImFont* titleFont_ = nullptr;
    ImFont* monoFont_ = nullptr;
    ImGuiStyle baseStyle_;
    DpiMetrics dpi_{};
    bool fontsLoaded_ = false;
    std::jthread job_;
    std::atomic<bool> busy_{false}, jobDone_{false};
    std::atomic<float> jobProgress_{0};
    std::string jobLabel_, jobError_, jobSuccess_;
    std::function<void()> onJobDone_;
    std::function<void()> onJobRetry_, memoryRetry_;
    std::uint64_t jobMemoryNeeded_ = 0, memoryNeeded_ = 0;
    bool syncPlayback_ = false;
    std::mutex jobMutex_;
    char pathInput_[2048]{};
    ImVec2 wavePos_{}, waveSize_{};
    std::vector<Peak> overviewCache_;
    std::vector<Peak> waveformCache_;
    std::vector<ImVec2> envelopeUpper_, envelopeLower_;
    const Timeline* waveformTimeline_ = nullptr;
    double waveformStart_ = -1, waveformSpan_ = 0;
    double waveformRasterLeft_ = -1, waveformRasterRight_ = -1;
    int waveformColumns_ = 0;
    bool waveformReady_ = false;
    const Timeline* overviewTimeline_ = nullptr;
    double overviewExtent_ = 0;
    bool overviewReady_ = false;
    float frameMs_ = 0;
    std::uint64_t resident_ = 0;
    double lastMemoryPoll_ = 0;
    double lastInteraction_ = 0;
    bool smokePassed_ = true;
    double smokeSpan_ = 0;
    Frame smokeFirst_ = 0, smokeLast_ = 0;
    bool smokeEdgesPassed_ = true;
    unsigned smokeBoundaryChecks_ = 0;
    unsigned dpiChanges_ = 0;
    float px(float value) const { return value * dpi_.layout; }
    ImVec2 px(ImVec2 p) const { return {px(p.x), px(p.y)}; }
    ImVec2 logical(ImVec2 p) const { return {p.x / dpi_.layout, p.y / dpi_.layout}; }
    ScaledDrawList drawList() const { return {ImGui::GetWindowDrawList(), dpi_.layout}; }
    Frame first() const { return std::min(anchor_, head_); }
    Frame last() const { return std::max(anchor_, head_); }
    bool selected() const { return first() < last(); }
    Frame selectionExtent() const { const auto t = document_.timeline(); return std::max(t ? t->frames : Frame(0), last()); }
    void openNow(const std::filesystem::path& path);
    void acceptFile(std::shared_ptr<AudioFile> file, const std::filesystem::path& path, bool saved = true);
    void openDialog(bool pasteFromFile = false);
    void exportDialog(bool selectionOnly = false);
    void exportTo(const std::filesystem::path& path, bool selectionOnly = false);
    void afterEdit(const std::string& label);
    void action(const std::string& name);
    void togglePlay();
    void normalize();
    void openCleanup(Cleanup kind);
    void prepareCleanup(bool apply);
    void resetCleanup();
    void previewCleanup();
    void stretchControls();
    void openPitch();
    void pitchControls();
    void pitchEditor(ImVec2 pos, ImVec2 size);
    void detectNotes();
    void correctNotes(bool reset);
    void syncPitchSelection();
    void selectAllNotes();
    void setNoteVibrato();
    void addHarmony();
    void removeHarmonyNotes();
    void setHarmonyLevel();
    void splitPitchNote(std::size_t index, Frame sourceFrame);
    void fitNotes();
    void togglePitchView();
    void beatRuler(ImVec2 pos, float width);
    void applyPitchTime(PitchTimeState map, const std::string& label, bool renderAudio = true, bool auditioned = false, std::function<void()> afterCommit = {});
    void detectRhythm(bool anchors);
    void addStretchAnchor(double frame);
    void auditionStretch(std::shared_ptr<const Timeline> timeline);
    void snapSelection(bool movingEdgeOnly = false);
    Frame edgeFrames() const;
    void startJob(const std::string& label, std::function<void(std::stop_token)> task, std::function<void()> done = {}, std::function<void()> retry = {});
    std::uint64_t reclaimableAudioBytes() const;
    std::size_t currentPeakBytes() const;
#ifdef __EMSCRIPTEN__
    void audioBudgetSetting();
    void clearAudioBuffers();
#endif
    void shortcuts();
    void header(float width);
    void mainMenu();
    enum class ToolIcon { Undo, Redo, Cut, Copy, Paste, Delete, Trim, Fit, Selection };
    bool toolButton(const char* id, ToolIcon icon, const char* caption, const std::string& tooltip, bool enabled = true, bool active = false);
    void transport();
    void playbackPanel(ImVec2 pos, ImVec2 size);
    void waveform(ImVec2 pos, ImVec2 size);
    void overview(ImVec2 pos, ImVec2 size);
    void inspector(ImVec2 pos, ImVec2 size);
    void modals();
    void emptyState(ImVec2 pos, ImVec2 size);
    bool button(const char* label, ImVec2 size = {}, bool accent = false);
    bool iconButton(const char* id, int icon, bool active, const char* tooltip);
    void label(ImVec2 pos, const std::string& text, ImU32 color, ImFont* font = nullptr);
};
}
