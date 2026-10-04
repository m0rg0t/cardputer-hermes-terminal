#include <cassert>
#include "hermes_terminal/timing_rules.h"

using namespace hermes_terminal;

int main()
{
    assert(!timeReached(999, 1000));
    assert(timeReached(1000, 1000));
    assert(timeReached(1001, 1000));
    assert(!timeReached(0xfffffff0U, 20));
    assert(!timeReached(19, 20));
    assert(timeReached(20, 20));
    assert(timeReached(20, 0xfffffff0U));
    assert(withinTimeout(0xfffffff5U, 0xfffffff0U, 100));
    assert(withinTimeout(83, 0xfffffff0U, 100));
    assert(!withinTimeout(84, 0xfffffff0U, 100));
    assert(!withinTimeout(85, 0xfffffff0U, 100));
}
