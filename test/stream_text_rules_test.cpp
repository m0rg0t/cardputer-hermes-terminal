#include <cassert>
#include <cstring>

#include "hermes_terminal/stream_text_rules.h"

using hermes_terminal::CompletionAppendKind;
using hermes_terminal::decideCompletionAppend;

namespace {

auto decide(const char* streamed, const char* interim, const char* complete)
{
    return decideCompletionAppend(streamed, std::strlen(streamed), interim,
                                  std::strlen(interim), complete,
                                  std::strlen(complete));
}

}  // namespace

int main()
{
    auto decision = decide("answer", "", "answer");
    assert(decision.kind == CompletionAppendKind::kSuffix);
    assert(decision.offset == 6);

    decision = decide("partial", "", "partial plus recovered tail");
    assert(decision.kind == CompletionAppendKind::kSuffix);
    assert(decision.offset == 7);

    decision = decide("", "tool preamble", "tool preamble");
    assert(decision.kind == CompletionAppendKind::kSuffix);
    assert(decision.offset == 13);

    decision = decide("", "tool preamble", "tool preamble and final");
    assert(decision.kind == CompletionAppendKind::kSuffix);
    assert(decision.offset == 13);

    decision = decide("", "tool preamble", "distinct final answer");
    assert(decision.kind == CompletionAppendKind::kFullWithLabel);
    assert(decision.offset == 0);

    decision = decide("", "", "completion recovered without deltas");
    assert(decision.kind == CompletionAppendKind::kFullWithLabel);

    decision = decide("partial A", "", "different B");
    assert(decision.kind == CompletionAppendKind::kFullWithLabel);

    decision = decide("", "", "");
    assert(decision.kind == CompletionAppendKind::kNone);
}
