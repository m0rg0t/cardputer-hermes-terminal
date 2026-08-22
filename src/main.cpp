#include <Arduino.h>

#include "hermes_terminal/app.h"

namespace {
hermes_terminal::App application;
}

void setup() { application.begin(); }
void loop() { application.update(); }
