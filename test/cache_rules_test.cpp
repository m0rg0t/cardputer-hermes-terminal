#include <cassert>
#include <cstdint>
#include <cstring>

#include "hermes_terminal/cache_rules.h"

using namespace hermes_terminal;

int main()
{
    const char* sample = "123456789";
    assert(cacheCrc32(reinterpret_cast<const std::uint8_t*>(sample),
                      std::strlen(sample)) == 0xCBF43926U);
    std::uint32_t split = 0xFFFFFFFFU;
    split = cacheCrc32Update(split,
        reinterpret_cast<const std::uint8_t*>(sample), 4);
    split = cacheCrc32Update(split,
        reinterpret_cast<const std::uint8_t*>(sample + 4), 5);
    assert(~split == 0xCBF43926U);
    assert(cacheSchemaCompatible(1));
    assert(!cacheSchemaCompatible(0));
    const char utf8[] = {'a', static_cast<char>(0xD0), static_cast<char>(0x91),
                         'b', '\0'};
    assert(utf8SafeStart(utf8, 4, 1) == 1);
    assert(utf8SafeStart(utf8, 4, 2) == 3);
    assert(utf8SafeStart(utf8, 4, 4) == 4);
    return 0;
}
