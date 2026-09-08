#include <cassert>
#include <string>

#include "hermes_terminal/interaction_rules.h"

using namespace hermes_terminal;

struct Text : std::string {
    using std::string::string;
    using std::string::operator=;
    void remove(std::size_t start) { erase(start); }
};

int main()
{
    PendingControl pending;
    // Repeated keyboard commands cannot replace the outstanding request.
    assert(pending.start(ControlKind::kBranch, 10));
    assert(!pending.start(ControlKind::kUndo, 11));
    assert(pending.take(11) == ControlKind::kNone && pending.busy());
    assert(pending.take(0) == ControlKind::kNone && pending.busy());
    assert(pending.take(10) == ControlKind::kBranch && !pending.busy());
    assert(pending.take(10) == ControlKind::kNone);
    // Immediate send failures leave the gate open for a user retry.
    assert(!pending.start(ControlKind::kSteer, 0));
    assert(!pending.busy());
    assert(pending.start(ControlKind::kSteer, 12));
    assert(pending.take(12) == ControlKind::kSteer);
    // A slash response is consumed before fallback/alias dispatch.
    assert(pending.start(ControlKind::kSlash, 13));
    assert(pending.take(13) == ControlKind::kSlash);
    assert(pending.start(ControlKind::kCommand, 14));
    assert(pending.take(13) == ControlKind::kNone && pending.busy());
    assert(pending.take(14) == ControlKind::kCommand);
    // Leaving a session/disconnecting cancels local tracking, not the remote
    // action. A late response must not mutate the newly resumed session.
    assert(pending.start(ControlKind::kCompress, 15));
    pending.reset();
    assert(pending.start(ControlKind::kUndo, 16));
    assert(pending.take(15) == ControlKind::kNone && pending.busy());
    assert(pending.take(16) == ControlKind::kUndo);

    DraftBuffers<Text> drafts;
    assert(editDraft(drafts.prompt, 'V', std::string("Verify archive"), 4000));
    assert(drafts.prompt == "Verify archive");
    editDraft(drafts.wifi, 'p', std::string("password"), 63);
    editDraft(drafts.answer, 'y', std::string("yes"), 1000);
    assert(drafts.prompt == "Verify archive");
    // Enter/Escape are actions, never inserted into any text field.
    assert(!editDraft(drafts.prompt, '`', std::string("`"), 4000));
    assert(!editDraft(drafts.prompt, '\n', std::string("\n"), 4000));
    drafts.wifi = "";
    drafts.answer = "";
    assert(drafts.prompt == "Verify archive");
    for (std::size_t limit : {63U, 1000U, 4000U}) {
        Text draft(limit - 1, 'a');
        editDraft(draft, 'b', std::string("bc"), limit);
        assert(draft.size() == limit && draft.back() == 'b');
        assert(!editDraft(draft, 'd', std::string("d"), limit));
    }
    drafts.prompt = "hi \xD0\x94";
    assert(editDraft(drafts.prompt, '\b', std::string(), 4000));
    assert(drafts.prompt == "hi "); // Delete a complete UTF-8 code point.
    drafts.prompt = "";
    assert(!editDraft(drafts.prompt, '\b', std::string(), 4000));

    assert(!wifiNeedsPassword(true, true, false, false));
    assert(wifiNeedsPassword(true, true, false, true));
    assert(wifiNeedsPassword(true, true, true, false));
    assert(wifiNeedsPassword(true, false, false, false));
    assert(!wifiNeedsPassword(false, false, true, true));
}
