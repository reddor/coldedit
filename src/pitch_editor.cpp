#include "editor.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace coldedit {
namespace {
constexpr ImU32 panel = IM_COL32(22, 27, 33, 255), line = IM_COL32(43, 50, 59, 255);
constexpr ImU32 text = IM_COL32(225, 231, 236, 255), muted = IM_COL32(135, 148, 161, 255);
constexpr ImU32 teal = IM_COL32(98, 222, 193, 255), amber = IM_COL32(241, 189, 115, 255);
std::string noteName(double midi) {
    static const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    const int key = std::clamp(int(std::lround(midi)), 0, 127);
    return std::string(names[key % 12]) + std::to_string(key / 12 - 1);
}
}
void Editor::togglePitchView() {
    if (busy_ || recording_ || stretchDrag_ || pitchNoteDragging_ >= 0 || pitchMarquee_ || !document_.timeline()) return;
    pitchView_ = !pitchView_; compactTools_ = false;
    if (pitchView_) fitNotes();
}
void Editor::fitNotes() {
    const auto t = document_.timeline();
    if (!t || !t->pitchTime || t->pitchTime->notes.empty()) { pitchLow_ = 48; pitchHigh_ = 84; return; }
    double low = 127, high = 0;
    for (std::size_t i = 0; i < t->pitchTime->noteCount(); ++i) {
        const auto& note = t->pitchTime->noteAt(i);
        const double midi = note.midi + note.shift + t->pitchTime->pitch.semitones;
        low = std::min(low, midi); high = std::max(high, midi);
    }
    const double span = std::max(12., high - low + 8), centre = (high + low) * .5;
    pitchLow_ = centre - span * .5; pitchHigh_ = centre + span * .5;
}
void Editor::detectNotes() try {
    const auto before = document_.timeline();
    if (busy_ || !before || before->frames < 2) return;
    auto state = before->pitchTime ? *before->pitchTime : PitchTimeState::identity(before, stretchBpm_);
    auto analysis = std::make_shared<PitchAnalysis>();
    startJob("Detecting monophonic notes", [this, source = state.source, analysis](std::stop_token stop) {
        resolvePitchTime(*source, &jobProgress_, stop);
        *analysis = analyzePitch(*source, &jobProgress_, stop);
    }, [this, before, state, analysis]() mutable {
        if (document_.timeline() != before) return;
        state.analysis = analysis; state.notes = analysis->notes; state.harmonies = {};
        // Re-detection explicitly resets note edits, and may therefore need DSP.
        const bool render = before->pitchTime && before->pitchTime->hasNoteEdits();
        applyPitchTime(std::move(state), "Detect notes", render);
        pitchSelection_.clear(); pitchSelectionAnchor_ = {0, -1}; syncPitchSelection(); fitNotes();
        if (analysis->notes.empty()) status_ = "No stable monophonic notes found (50-1200 Hz).";
    });
} catch (const std::exception& e) { error_ = e.what(); }
void Editor::correctNotes(bool reset) {
    const auto t = document_.timeline();
    if (busy_ || !t || !t->pitchTime || t->pitchTime->notes.empty()) return;
    auto state = *t->pitchTime; bool changed = false;
    for (std::size_t i = 0; i < state.noteCount(); ++i) {
        if (!pitchSelection_.empty() && !pitchSelection_.contains(state.noteKey(i))) continue;
        auto& note = state.noteAt(i);
        const double audible = note.midi + note.shift + state.pitch.semitones;
        const double shift = reset ? 0 : std::clamp(note.shift + std::round(audible) - audible, -24., 24.);
        changed |= std::abs(shift - note.shift) > 1e-7; note.shift = shift;
        if (reset) { changed |= note.vibrato != 1; note.vibrato = 1; }
    }
    if (changed) applyPitchTime(std::move(state), reset ? "Reset note pitch" : "Correct note pitch");
}
void Editor::syncPitchSelection() {
    const auto t = document_.timeline();
    pitchNoteSelected_ = -1; pitchVibratoInput_ = 100; pitchVibratoMixed_ = false; harmonySelected_ = -1;
    if (!t || !t->pitchTime || t->pitchTime->noteCount() == 0) { pitchSelection_.clear(); pitchSelectionAnchor_ = {0, -1}; return; }
    const auto& state = *t->pitchTime;
    std::erase_if(pitchSelection_, [&](NoteKey key) {
        if (key.first > state.harmonies.size()) return true;
        const auto& notes = state.voiceNotes(key.first);
        auto it = std::lower_bound(notes.begin(), notes.end(), key.second, [](const PitchNote& note, Frame frame) { return note.first < frame; });
        return it == notes.end() || it->first != key.second;
    });
    if (!pitchSelection_.empty() && pitchSelection_.begin()->first > 0 && pitchSelection_.begin()->first == pitchSelection_.rbegin()->first) {
        harmonySelected_ = int(pitchSelection_.begin()->first);
        harmonyLevel_ = float(state.harmonies[harmonySelected_ - 1].gain * 100);
    }
    bool first = true;
    for (std::size_t i = 0; i < state.noteCount(); ++i) {
        const auto& note = state.noteAt(i); const auto key = state.noteKey(i);
        if (key == pitchSelectionAnchor_ && pitchSelection_.contains(key)) pitchNoteSelected_ = int(i);
        if (!pitchSelection_.empty() && !pitchSelection_.contains(key)) continue;
        if (first) { pitchVibratoInput_ = float(note.vibrato * 100); first = false; }
        else pitchVibratoMixed_ |= std::abs(pitchVibratoInput_ - note.vibrato * 100) > .01;
    }
}
void Editor::selectAllNotes() {
    const auto t = document_.timeline(); if (busy_ || !t || !t->pitchTime) return;
    pitchSelection_.clear(); for (std::size_t i = 0; i < t->pitchTime->noteCount(); ++i) pitchSelection_.insert(t->pitchTime->noteKey(i));
    pitchSelectionAnchor_ = pitchSelection_.empty() ? NoteKey{0, -1} : *pitchSelection_.begin(); syncPitchSelection();
}
void Editor::setNoteVibrato() {
    const auto t = document_.timeline(); if (busy_ || !t || t != pitchVibratoBase_ || !t->pitchTime) return;
    auto state = *t->pitchTime; bool changed = false;
    for (std::size_t i = 0; i < state.noteCount(); ++i) {
        auto& note = state.noteAt(i);
        if (!pitchVibratoSelection_.empty() && !pitchVibratoSelection_.contains(state.noteKey(i))) continue;
        const double scale = std::clamp(double(pitchVibratoInput_) / 100, 0., 2.);
        changed |= std::abs(note.vibrato - scale) > 1e-7; note.vibrato = scale;
    }
    if (changed) applyPitchTime(std::move(state), "Change note vibrato");
}
void Editor::splitPitchNote(std::size_t index, Frame sourceFrame) {
    const auto t = document_.timeline(); if (busy_ || !t || !t->pitchTime) return;
    auto state = *t->pitchTime;
    if (!state.splitNote(index, sourceFrame)) { status_ = "Split at least 20 ms from either edge (maximum 8192 notes)."; return; }
    const auto first = state.noteKey(index);
    applyPitchTime(std::move(state), "Split pitch note", false);
    pitchSelection_ = {first, {first.first, sourceFrame}}; pitchSelectionAnchor_ = first; syncPitchSelection();
}
void Editor::addHarmony() try {
    const auto t = document_.timeline(); if (busy_ || !t || !t->pitchTime) return;
    auto state = *t->pitchTime; std::vector<std::size_t> selected;
    for (std::size_t i = 0; i < state.noteCount(); ++i) if (pitchSelection_.contains(state.noteKey(i))) selected.push_back(i);
    const unsigned voice = state.addHarmony(selected, harmonyInterval_);
    std::set<NoteKey> copies; for (const auto& note : state.voiceNotes(voice)) copies.emplace(voice, note.first);
    applyPitchTime(std::move(state), "Add harmony voice", true, false, [this, copies] {
        pitchSelection_ = copies; pitchSelectionAnchor_ = *copies.begin(); syncPitchSelection(); fitNotes();
    });
} catch (const std::exception& e) { error_ = e.what(); }
void Editor::removeHarmonyNotes() {
    const auto t = document_.timeline(); if (busy_ || !t || !t->pitchTime || pitchSelection_.empty()) return;
    auto state = *t->pitchTime; bool changed = false;
    for (unsigned voice = 1; voice <= state.harmonies.size(); ++voice) {
        changed |= std::erase_if(state.voiceNotes(voice), [&](const PitchNote& note) { return pitchSelection_.contains({voice, note.first}); }) != 0;
        if (state.voiceNotes(voice).empty()) state.harmonies[voice - 1].gain = .35;
    }
    if (changed) applyPitchTime(std::move(state), "Remove harmony notes");
}
void Editor::setHarmonyLevel() {
    const auto t = document_.timeline(); if (busy_ || !t || t != harmonyLevelBase_ || !t->pitchTime || harmonyLevelVoice_ < 1) return;
    auto state = *t->pitchTime; auto& gain = state.harmonies[harmonyLevelVoice_ - 1].gain;
    const double value = std::clamp(double(harmonyLevel_) / 100, 0., 1.);
    if (std::abs(gain - value) < 1e-7) return;
    gain = value; applyPitchTime(std::move(state), "Change harmony level");
}
void Editor::pitchEditor(ImVec2 p, ImVec2 size) {
    wavePos_ = p; waveSize_ = size; notePositions_.clear();
    noteControlPositions_.assign(13, {});
    const auto t = document_.timeline();
    if (!t || (opening_ && busy_)) { emptyState(p, size); return; }
    const auto state = stretchDrag_ ? std::shared_ptr<const PitchTimeState>{} : t->pitchTime;
    const auto* displayed = stretchDrag_ ? stretchDrag_.get() : state.get();
    const bool hasNotes = displayed && !displayed->notes.empty();
    const bool singleRow = size.x >= 960;
    auto d = drawList(); auto& io = ImGui::GetIO(); const auto mouse = logical(io.MousePos);
    d.AddRectFilled(p, {p.x + size.x, p.y + size.y}, panel, 8);
    const auto record = [&](std::size_t i) {
        const auto a = logical(ImGui::GetItemRectMin()), b = logical(ImGui::GetItemRectMax());
        if (i >= noteControlPositions_.size()) noteControlPositions_.resize(i + 1);
        noteControlPositions_[i] = {(a.x + b.x) * .5f, (a.y + b.y) * .5f};
    };
    const auto group = [&](float row) {
        if (singleRow) ImGui::SameLine();
        else ImGui::SetCursorScreenPos(px({p.x + 8, p.y + row}));
    };
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(px(singleRow ? 6.f : 8.f), ImGui::GetStyle().ItemSpacing.y));
    ImGui::BeginDisabled(busy_ || bool(stretchDrag_) || pitchNoteDragging_ >= 0 || pitchMarquee_);
    ImGui::SetCursorScreenPos(px({p.x + 8, p.y + 8}));
    if (button("Waveform", {singleRow ? 80.f : 86.f, 28})) pitchView_ = false; record(5);
    ImGui::SameLine();
    if (button(displayed && displayed->analysis ? "Re-detect" : singleRow ? "Detect" : "Detect notes", {singleRow ? 80.f : 104.f, 28})) detectNotes(); record(0);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Analyse the retained source (50-1200 Hz). Re-detect resets note edits.\nAudible gaps are estimated from nearby notes; silence stays empty.\nAmber dashed lines are estimates. Use a single voice or melodic instrument.");
    ImGui::SameLine();
    if (button(singleRow ? "Fit" : "Fit notes", {singleRow ? 36.f : 80.f, 28})) fitNotes(); record(4);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Fit all notes and harmony voices into the pitch range. Alt+wheel zooms vertically.");
    if (!singleRow && size.x >= 510) label({p.x + 322, p.y + 14}, "Dashed = estimated pitch", amber);
    group(42);
    ImGui::BeginDisabled(!hasNotes);
    if (button(singleRow ? "Correct" : pitchSelection_.empty() ? "Correct all" : pitchSelection_.size() == 1 ? "Correct note" : "Correct notes", {singleRow ? 64.f : 116.f, 28})) correctNotes(false); record(1);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Move selected note centres to the nearest semitone (all notes if none selected).\nPreserve vibrato and pitch variation.");
    ImGui::SameLine();
    if (button(singleRow ? "Reset" : pitchSelection_.empty() ? "Reset all" : pitchSelection_.size() == 1 ? "Reset note" : "Reset notes", {singleRow ? 52.f : 100.f, 28})) correctNotes(true); record(2);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Reset selected notes to source pitch and 100%% vibrato (all notes if none selected).\nKeep splits, harmony voices and timing.");
    ImGui::EndDisabled(); ImGui::SameLine(); ImGui::Checkbox("Snap", &pitchSnap_); record(3);
    group(76);
    ImGui::BeginDisabled(!hasNotes);
    if (button(singleRow ? "All" : "All notes", {singleRow ? 34.f : 90.f, 28})) selectAllNotes(); record(8);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Select every note in every voice (Ctrl+A).");
    ImGui::SameLine();
    if (button(!singleRow && pitchSplitMode_ ? "Split: on" : "Split", {singleRow ? 48.f : 80.f, 28}, pitchSplitMode_)) pitchSplitMode_ = !pitchSplitMode_; record(7);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("S toggles the split tool; click a note where you want to cut.\nDouble-click also splits. Both halves keep their sound and settings. Escape exits.");
    ImGui::SameLine(); ImGui::SetNextItemWidth(px(singleRow ? 110.f : 150.f));
    if (ImGui::SliderFloat("##Note vibrato", &pitchVibratoInput_, 0, 200, pitchVibratoMixed_ ? "Mixed %.0f%%" : "Vibrato %.0f%%", ImGuiSliderFlags_AlwaysClamp)) pitchVibratoMixed_ = false;
    record(6);
    if (ImGui::IsItemActivated()) {
        pitchVibratoCancelled_ = false; pitchVibratoBase_ = t; pitchVibratoSelection_ = pitchSelection_;
    }
    if ((pitchVibratoEditing_ || ImGui::IsItemActive()) && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) pitchVibratoCancelled_ = true;
    pitchVibratoEditing_ = ImGui::IsItemActive();
    if (pitchVibratoCancelled_) syncPitchSelection();
    const bool vibratoPreview = pitchVibratoEditing_ && !pitchVibratoCancelled_;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Vibrato depth for selected notes (all notes if none selected).\n0%%: flat; 100%%: original; 200%%: double. Ctrl-click to type a value.\nScales measured pitch variation, including bends, around the note centre.\nEstimated regions stay unchanged. Release to render one undo step.");
    if (ImGui::IsItemDeactivatedAfterEdit() && !pitchVibratoCancelled_) setNoteVibrato();
    if (!pitchVibratoEditing_) { pitchVibratoCancelled_ = false; pitchVibratoBase_.reset(); pitchVibratoSelection_.clear(); }
    if (!singleRow && size.x >= 620) label({p.x + 374, p.y + 82}, std::to_string(pitchSelection_.size()) + " selected / drag empty space to select", muted);
    group(110);
    if (button("Add voice", {singleRow ? 78.f : 84.f, 28})) addHarmony(); record(9);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Copy selected notes into a harmony voice; no selection copies the original.\nChoose an interval first, then add.\nCopies can be dragged, split and edited independently. Re-detect removes copies.");
    ImGui::SameLine(); ImGui::SetNextItemWidth(px(singleRow ? 64.f : 74.f));
    ImGui::DragFloat("##Harmony interval", &harmonyInterval_, .1f, -24, 24, "%+.1f st", ImGuiSliderFlags_AlwaysClamp); record(10);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Interval for the next voice: +3 minor third, +4 major third, +7 fifth, +12 octave.\n0 doubles the note. Negative intervals go lower. Ctrl-click to type.");
    ImGui::SameLine();
    const bool removable = std::any_of(pitchSelection_.begin(), pitchSelection_.end(), [](const auto& key) { return key.first > 0; });
    ImGui::BeginDisabled(!removable);
    if (button("Remove", {singleRow ? 66.f : 74.f, 28})) removeHarmonyNotes(); record(11);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove selected harmony notes (Delete / Backspace). Original notes stay in place.");
    ImGui::EndDisabled(); ImGui::SameLine(); ImGui::SetNextItemWidth(px(singleRow ? 86.f : 90.f));
    ImGui::BeginDisabled(harmonySelected_ < 1 && !harmonyLevelEditing_);
    ImGui::SliderFloat("##Harmony level", &harmonyLevel_, 0, 100, "Lvl %.0f%%", ImGuiSliderFlags_AlwaysClamp); record(12);
    if (ImGui::IsItemActivated()) {
        harmonyLevelCancelled_ = false; harmonyLevelBase_ = t; harmonyLevelVoice_ = harmonySelected_;
    }
    if ((harmonyLevelEditing_ || ImGui::IsItemActive()) && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) harmonyLevelCancelled_ = true;
    harmonyLevelEditing_ = ImGui::IsItemActive();
    if (harmonyLevelCancelled_) syncPitchSelection();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Level for this entire harmony voice. Select notes in one harmony to adjust.\n0%% mutes it; 100%% is full source level. Original stays at 100%%.\nCtrl-click to type. Escape cancels; release renders one undo step.");
    if (ImGui::IsItemDeactivatedAfterEdit() && !harmonyLevelCancelled_) setHarmonyLevel();
    if (!harmonyLevelEditing_) { harmonyLevelCancelled_ = false; harmonyLevelBase_.reset(); harmonyLevelVoice_ = -1; }
    ImGui::EndDisabled();
    if (!singleRow && size.x >= 620) label({p.x + 374, p.y + 116}, harmonySelected_ > 0 ? "Harmony " + std::to_string(harmonySelected_) + " / voice level" : "Add harmonies from a single voice", muted);
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    ImGui::PopStyleVar();
    std::unique_ptr<PitchTimeState> visual;
    if (displayed && (pitchNoteDragging_ >= 0 || vibratoPreview)) {
        visual = std::make_unique<PitchTimeState>(*displayed);
        for (std::size_t i = 0; i < visual->noteCount(); ++i) {
            auto& note = visual->noteAt(i);
            const auto& selection = pitchNoteDragging_ >= 0 ? pitchSelection_ : pitchVibratoSelection_;
            if (!selection.empty() && !selection.contains(visual->noteKey(i))) continue;
            if (pitchNoteDragging_ >= 0) note.shift += pitchDragShift_ - pitchDragOriginal_;
            else note.vibrato = pitchVibratoInput_ / 100.;
        }
        displayed = visual.get();
    }
    const float rulerY = p.y + (singleRow ? 44 : 146), left = p.x + 46, right = p.x + size.x;
    const float top = rulerY + 30 + (stretchMode_ ? 40 : 0), bottom = p.y + size.y;
    pitchRulerY_ = rulerY; pitchPlotMin_ = {left, top}; pitchPlotMax_ = {right, bottom};
    if (bottom - top < 24) return;
    const float width = std::max(1.f, right - left), height = std::max(1.f, bottom - top);
    pitchStep_ = height / float(pitchHigh_ - pitchLow_);
    const auto xAt = [&](double frame) { return left + float((frame - view_.start) / view_.span) * width; };
    const auto yAt = [&](double midi) { return bottom - float((midi - pitchLow_) / (pitchHigh_ - pitchLow_)) * height; };
    const auto outputAt = [&](double frame) { return displayed ? displayed->time.outputAt(frame) : frame; };
    d.PushClipRect({p.x, top}, {right, bottom}, true);
    for (int midi = int(std::floor(pitchLow_)); midi <= int(std::ceil(pitchHigh_)); ++midi) {
        const int key = (midi % 12 + 12) % 12; const bool black = key == 1 || key == 3 || key == 6 || key == 8 || key == 10;
        const float y0 = yAt(midi + .5), y1 = yAt(midi - .5);
        d.AddRectFilled({p.x, y0}, {right, y1}, black ? IM_COL32(18, 23, 29, 255) : IM_COL32(28, 34, 42, 255));
        d.AddLine({p.x, y1}, {right, y1}, line);
        if (y1 - y0 >= 13 && midi >= 0 && midi <= 127) label({p.x + 3, yAt(midi) - 8}, noteName(midi), key == 0 ? teal : muted);
    }
    d.PopClipRect();
    d.PushClipRect({left, rulerY}, {right, bottom}, true);
    const double seconds = view_.span / t->sampleRate;
    double tick = std::pow(10., std::floor(std::log10(std::max(.001, seconds * 90 / width))));
    while (tick * t->sampleRate / view_.span * width < 65) tick *= 2;
    for (double sec = std::ceil(view_.start / t->sampleRate / tick) * tick; sec * t->sampleRate <= view_.start + view_.span; sec += tick) {
        const float x = xAt(sec * t->sampleRate); d.AddLine({x, rulerY + 26}, {x, bottom}, line);
        char labelText[40]; std::snprintf(labelText, sizeof(labelText), "%.2f s", sec); label({x + 4, rulerY + 4}, labelText, muted);
    }
    d.PopClipRect();
    int hot = -1; std::vector<NoteKey> hitNotes;
    d.PushClipRect({left, top}, {right, bottom}, true);
    if (hasNotes) {
        notePositions_.resize(displayed->noteCount());
        // Selected notes draw last and remain draggable when voices overlap.
        for (int selectedPass = 0; selectedPass < 2; ++selectedPass) for (std::size_t i = 0; i < displayed->noteCount(); ++i) {
            const bool chosen = pitchSelection_.contains(displayed->noteKey(i));
            if (chosen != bool(selectedPass)) continue;
            const auto& note = displayed->noteAt(i);
            const double midi = note.midi + note.shift + displayed->pitch.semitones;
            const float a = xAt(outputAt(double(note.first))), b = xAt(outputAt(double(note.last))), y = yAt(midi);
            const float h = std::clamp(height / float(pitchHigh_ - pitchLow_) * .72f, 5.f, 30.f);
            notePositions_[i] = {{(a + b) * .5f, y}, h};
            if (b < left || a > right) continue;
            if (mouse.x >= std::max(a, left) && mouse.x < std::min(b, right) && mouse.y >= std::max(top, y - h * .5f) && mouse.y <= std::min(bottom, y + h * .5f)) {
                hot = int(i); hitNotes.push_back(displayed->noteKey(i));
            }
            const auto voice = displayed->locateNote(i).first;
            const auto fill = voice ? IM_COL32(89 + ((voice - 1) % 3) * 12, 68, 141 + ((voice - 1) % 3) * 16, 255) : note.inferredFraction >= .5f ? IM_COL32(88, 82, 58, 255) : IM_COL32(44, 119, 110, 255);
            d.AddRectFilled({a, y - h * .5f}, {std::max(a + 2, b), y + h * .5f}, chosen ? IM_COL32(197, 148, 77, 255) : fill, 3);
            if (note.fadeFirst >= 0) d.AddLine({a, y - h * .5f}, {a, y + h * .5f}, muted);
            if (b - a > 42 && h >= 16) label({a + 5, y - 8}, noteName(midi) + (voice ? " H" + std::to_string(voice) : ""), text);
        }
        // Draw only visible analysis points, decimating to roughly screen pixels.
        const auto& contour = displayed->analysis->contour;
        const double sourceFirst = displayed->time.sourceAt(view_.start), sourceLast = displayed->time.sourceAt(view_.start + view_.span);
        auto begin = std::lower_bound(contour.begin(), contour.end(), sourceFirst, [](const PitchFrame& f, double at) { return f.sourceFrame < at; });
        auto end = std::upper_bound(begin, contour.end(), sourceLast, [](double at, const PitchFrame& f) { return at < f.sourceFrame; });
        for (unsigned voice = 0; voice <= displayed->harmonies.size(); ++voice) {
            const auto& voiceNotes = displayed->voiceNotes(voice);
            if (voiceNotes.empty()) continue;
            ImVec2 previous{}; bool connected = false, previousInferred = false;
            double previousMidi = 0; int previousNote = -1;
            auto noteAt = std::lower_bound(voiceNotes.begin(), voiceNotes.end(), sourceFirst,
                [](const PitchNote& note, double frame) { return note.last <= frame; });
            for (auto it = begin; it != end; ++it) {
                if (!it->confidence) { connected = false; continue; }
                while (noteAt != voiceNotes.end() && noteAt->last <= it->sourceFrame) ++noteAt;
                const int noteIndex = noteAt != voiceNotes.end() && noteAt->first <= it->sourceFrame ? int(noteAt - voiceNotes.begin()) : -1;
                if (voice && noteIndex < 0) { connected = false; continue; }
                const double shift = displayed->noteShiftAt(double(it->sourceFrame), voice);
                const double midi = it->midi + displayed->pitch.semitones + shift;
                const ImVec2 point{xAt(outputAt(double(it->sourceFrame))), yAt(midi)};
                // Never draw a vertical connector across a note boundary or a
                // remaining discontinuity, even when decimating a zoomed-out view.
                if (noteIndex != previousNote || std::abs(midi - previousMidi) > 2) connected = false;
                previousNote = noteIndex; previousMidi = midi;
                const bool inferred = it->inferred || previousInferred;
                if (connected && point.x - previous.x < .6f && it + 1 != end && it->inferred == previousInferred) continue;
                if (connected && !inferred) d.AddLine(previous, point, voice ? IM_COL32(192, 172, 255, 255) : teal, 1.3f);
                else if (connected && point.x > previous.x) {
                    // Dash phase stays fixed in screen space, so short frame
                    // segments still form readable dashes at every zoom level.
                    for (float x = previous.x; x < point.x;) {
                        float phase = std::fmod(x - left, 8.f); if (phase < 0) phase += 8;
                        const float next = std::min(point.x, x + std::max(.01f, (phase < 4 ? 4 : 8) - phase));
                        const auto at = [&](float px) { return ImVec2{px, previous.y + (point.y - previous.y) * (px - previous.x) / (point.x - previous.x)}; };
                        if (phase < 4) d.AddLine(at(x), at(next), amber, 1.5f);
                        x = next;
                    }
                }
                previous = point; previousInferred = it->inferred; connected = true;
            }
        }
    }
    const float playX = xAt(double(cursor_)); d.AddLine({playX, top}, {playX, bottom}, amber, 1.5f);
    if (pitchSplitMode_ && hot >= 0 && mouse.y >= top && mouse.y <= bottom)
        d.AddLine({mouse.x, top}, {mouse.x, bottom}, amber);
    if (pitchMarquee_) {
        const ImVec2 a{std::max(left, std::min(pitchMarqueeStart_.x, mouse.x)), std::max(top, std::min(pitchMarqueeStart_.y, mouse.y))};
        const ImVec2 b{std::min(right, std::max(pitchMarqueeStart_.x, mouse.x)), std::min(bottom, std::max(pitchMarqueeStart_.y, mouse.y))};
        d.AddRectFilled(a, b, IM_COL32(98, 222, 193, 30)); d.AddRect(a, b, teal);
    }
    d.PopClipRect();
    if (!hasNotes) label({left + 12, top + 22}, busy_ ? "Detecting / rendering..." : "Detect notes in a single voice or melodic instrument.", muted);
    ImGui::SetCursorScreenPos(px({left, rulerY}));
    ImGui::InvisibleButton("Pitch time ruler", px({width, 30}));
    if (!busy_ && ImGui::IsItemHovered() && ImGui::IsMouseClicked(0)) {
        cursor_ = std::clamp(Frame(view_.start + (mouse.x - left) / width * view_.span), Frame(0), t->frames); playback_.seek(cursor_);
    }
    ImGui::SetCursorScreenPos(px({left, top}));
    ImGui::InvisibleButton("Pitch notes", px({width, height}), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    if (!busy_ && !stretchDrag_ && hovered && !pitchMarquee_) {
        if (hot >= 0) {
            const auto& note = displayed->noteAt(hot);
            ImGui::SetMouseCursor(pitchSplitMode_ ? ImGuiMouseCursor_Hand : ImGuiMouseCursor_ResizeNS);
            ImGui::SetTooltip("%s / %s / %+.1f cents / vibrato %.0f%% / estimated %.0f%%\nDrag to transpose selection. Alt: fine tuning. Escape: cancel.\nCtrl-click: toggle; Shift-click: range; drag empty space: box select.\nDouble-click or Split tool: split here. Ctrl+A: all notes.\nRight-click overlapping notes to choose a voice.",
                noteName(note.midi + note.shift + displayed->pitch.semitones).c_str(),
                displayed->locateNote(hot).first ? ("Harmony " + std::to_string(displayed->locateNote(hot).first)).c_str() : "Original", note.shift * 100, note.vibrato * 100, note.inferredFraction * 100);
        }
        if (ImGui::IsMouseClicked(1) && !hitNotes.empty() && pitchNoteDragging_ < 0) {
            std::sort(hitNotes.begin(), hitNotes.end()); pitchContextNotes_ = hitNotes; pitchContextBase_ = t;
            ImGui::OpenPopup("Choose pitch voice");
        }
        if (ImGui::IsMouseClicked(0)) {
            const bool toggle = io.KeyCtrl || io.KeySuper;
            if (hot >= 0 && (pitchSplitMode_ || (ImGui::IsMouseDoubleClicked(0) && !toggle && !io.KeyShift))) {
                const auto frame = Frame(std::llround(displayed->time.sourceAt(view_.start + (mouse.x - left) / width * view_.span)));
                splitPitchNote(std::size_t(hot), frame);
            } else if (hot >= 0) {
                const auto key = displayed->noteKey(hot);
                if (io.KeyShift && pitchSelectionAnchor_.second >= 0) {
                    if (!toggle) pitchSelection_.clear();
                    for (std::size_t i = 0; i < displayed->noteCount(); ++i) {
                        const auto candidate = displayed->noteKey(i);
                        if (candidate >= std::min(key, pitchSelectionAnchor_) && candidate <= std::max(key, pitchSelectionAnchor_)) pitchSelection_.insert(candidate);
                    }
                } else if (toggle) {
                    if (!pitchSelection_.erase(key)) pitchSelection_.insert(key);
                    pitchSelectionAnchor_ = key;
                } else {
                    if (!pitchSelection_.contains(key)) pitchSelection_ = {key};
                    pitchSelectionAnchor_ = key;
                    pitchNoteDragging_ = hot; pitchDragBase_ = t; pitchDragY_ = mouse.y; pitchDragMoved_ = false;
                    pitchDragShift_ = pitchDragOriginal_ = t->pitchTime->noteAt(hot).shift;
                    pitchTimeRenderer_.suspend();
                }
                syncPitchSelection();
            } else if (!pitchSplitMode_) {
                pitchMarqueeBefore_ = pitchSelection_;
                pitchMarqueeAnchor_ = pitchSelectionAnchor_;
                if (!toggle && !io.KeyShift) pitchSelection_.clear();
                pitchMarqueeBase_ = pitchSelection_; pitchMarqueeStart_ = mouse; pitchMarquee_ = true; pitchDragBase_ = t;
                syncPitchSelection();
            }
        }
        if (io.MouseWheel != 0 && pitchNoteDragging_ < 0 && !pitchMarquee_) {
            if (io.KeyAlt) {
                const double fraction = std::clamp(double(bottom - mouse.y) / height, 0., 1.), at = pitchLow_ + fraction * (pitchHigh_ - pitchLow_);
                const double span = std::clamp((pitchHigh_ - pitchLow_) * std::exp(-io.MouseWheel * .15), 6., 96.);
                pitchLow_ = at - fraction * span; pitchHigh_ = pitchLow_ + span;
            } else if (io.KeyCtrl == wheelZooms_) view_.pan(-io.MouseWheel * view_.span * .12);
            else view_.zoom(std::exp(-io.MouseWheel * .22), (mouse.x - left) / width);
        }
        if (io.MouseWheelH != 0) view_.pan(-io.MouseWheelH * view_.span * .12);
    }
    if (pitchMarquee_) {
        if (document_.timeline() != pitchDragBase_ || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            pitchSelection_ = pitchMarqueeBefore_; pitchSelectionAnchor_ = pitchMarqueeAnchor_;
            pitchMarquee_ = false; pitchDragBase_.reset(); syncPitchSelection();
        } else if (!ImGui::IsMouseDown(0)) {
            pitchMarquee_ = false; pitchDragBase_.reset(); syncPitchSelection();
        } else if (ImGui::IsMouseDragging(0)) {
            pitchSelection_ = pitchMarqueeBase_;
            for (std::size_t i = 0; displayed && i < displayed->noteCount(); ++i) {
                const auto& note = displayed->noteAt(i); const auto& pos = notePositions_[i];
                if (xAt(outputAt(double(note.first))) <= std::max(pitchMarqueeStart_.x, mouse.x) && xAt(outputAt(double(note.last))) >= std::min(pitchMarqueeStart_.x, mouse.x) &&
                    pos.centre.y + pos.height * .5f >= std::min(pitchMarqueeStart_.y, mouse.y) && pos.centre.y - pos.height * .5f <= std::max(pitchMarqueeStart_.y, mouse.y))
                    pitchSelection_.insert(displayed->noteKey(i));
            }
            pitchSelectionAnchor_ = pitchSelection_.empty() ? NoteKey{0, -1} : *pitchSelection_.begin(); syncPitchSelection();
        }
    }
    if (!busy_ && pitchNoteDragging_ < 0 && ImGui::IsItemActive() && ImGui::IsMouseDragging(2))
        view_.pan(-logical(io.MouseDelta).x / width * view_.span, true);
    if (pitchNoteDragging_ >= 0) {
        if (document_.timeline() != pitchDragBase_ || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            pitchNoteDragging_ = -1; pitchDragBase_.reset(); syncPitchSelection();
        } else if (ImGui::IsMouseDown(0)) {
            if (ImGui::IsMouseDragging(0)) {
                pitchDragMoved_ = true;
                const auto& note = t->pitchTime->noteAt(pitchNoteDragging_);
                double target = note.midi + displayed->pitch.semitones + pitchDragOriginal_ + (pitchDragY_ - mouse.y) / height * (pitchHigh_ - pitchLow_);
                if (pitchSnap_ && !io.KeyAlt) target = std::round(target);
                double low = -48, high = 48;
                for (std::size_t i = 0; i < t->pitchTime->noteCount(); ++i) if (pitchSelection_.contains(t->pitchTime->noteKey(i))) {
                    const auto& selected = t->pitchTime->noteAt(i);
                    low = std::max(low, -24 - selected.shift); high = std::min(high, 24 - selected.shift);
                }
                pitchDragShift_ = pitchDragOriginal_ + std::clamp(target - note.midi - displayed->pitch.semitones - pitchDragOriginal_, low, high);
            }
        } else {
            const int index = pitchNoteDragging_; pitchNoteDragging_ = -1; pitchDragBase_.reset();
            if (std::abs(pitchDragShift_ - pitchDragOriginal_) > 1e-7) {
                auto next = *t->pitchTime;
                for (std::size_t i = 0; i < next.noteCount(); ++i) if (pitchSelection_.contains(next.noteKey(i))) next.noteAt(i).shift += pitchDragShift_ - pitchDragOriginal_;
                applyPitchTime(std::move(next), "Move pitch notes");
            } else if (!pitchDragMoved_) { pitchSelection_ = {t->pitchTime->noteKey(index)}; syncPitchSelection(); }
        }
    }
    if (ImGui::BeginPopup("Choose pitch voice")) {
        if (busy_ || pitchContextBase_.lock() != document_.timeline()) ImGui::CloseCurrentPopup();
        else for (const auto key : pitchContextNotes_) {
            const auto caption = key.first ? "Harmony " + std::to_string(key.first) : "Original";
            if (ImGui::MenuItem(caption.c_str(), nullptr, pitchSelection_.contains(key))) {
                pitchSelection_ = {key}; pitchSelectionAnchor_ = key; syncPitchSelection();
            }
            record(13 + key.first);
        }
        ImGui::EndPopup();
    }
    if (stretchMode_) beatRuler({p.x, rulerY + 30}, size.x);
}
}
