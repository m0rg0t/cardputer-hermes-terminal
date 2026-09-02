#pragma once
#include <M5GFX.h>

// Only the display is needed by the compiled drawing unit.
struct M5CardputerStub {
    M5GFX Display;
};
extern M5CardputerStub M5Cardputer;
