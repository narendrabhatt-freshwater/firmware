#pragma once

struct App;

void DrawFilterCard(App &app);
void DrawVmProgramHeader(App &app);
/** Start the currently selected VM upload without requiring a UI click. */
void StartSelectedVmLoad(App &app);
void DrawEffectPanel(App &app);
