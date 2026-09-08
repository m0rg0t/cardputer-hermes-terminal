#pragma once

#include <cstddef>
#include <cstdint>

namespace hermes_terminal {

enum class ControlKind : std::uint8_t {
    kNone, kBranch, kCompress, kUndo, kSteer, kSlash, kCommand
};

// One session mutation at a time. Taking a response consumes its ID before
// callbacks run, so aliases/fallbacks can start and duplicate replies cannot.
class PendingControl {
public:
    bool busy() const { return id_ != 0; }
    bool start(ControlKind kind, std::uint32_t id) {
        if (busy() || !id || kind == ControlKind::kNone) return false;
        kind_ = kind;
        id_ = id;
        return true;
    }
    ControlKind take(std::uint32_t id) {
        if (!id || id != id_) return ControlKind::kNone;
        const ControlKind kind = kind_;
        reset();
        return kind;
    }
    void reset() { id_ = 0; kind_ = ControlKind::kNone; }
private:
    std::uint32_t id_ = 0;
    ControlKind kind_ = ControlKind::kNone;
};

// Separate storage prevents temporary password/answer screens from replacing
// an unsent prompt. No extra copies of the potentially 4 KiB prompt are needed.
template <typename Text>
struct DraftBuffers {
    Text prompt;
    Text answer;
    Text wifi;
};

// Returns whether an edit occurred. Enter and Escape belong to navigation;
// every other printable letter, including an initial V, is literal text.
template <typename Text, typename Word>
bool editDraft(Text& draft, char key, const Word& word, std::size_t limit)
{
    if (key == '\n' || key == '`') return false;
    if (key == '\b') {
        if (!draft.length()) return false;
        std::size_t start = draft.length() - 1;
        while (start && (static_cast<unsigned char>(draft[start]) & 0xC0) == 0x80)
            --start;
        draft.remove(start);
        return true;
    }
    bool changed = false;
    for (char character : word) {
        if (draft.length() >= limit) break;
        draft += character;
        changed = true;
    }
    return changed;
}

inline bool wifiNeedsPassword(bool secured, bool known, bool edit,
                              bool previousAttemptFailed)
{
    return secured && (!known || edit || previousAttemptFailed);
}

}  // namespace hermes_terminal
