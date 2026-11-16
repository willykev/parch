#pragma once

// Registers parch's C++ types for QML under `import Parch`. The application
// and the tests both call it once, before any QML loads, so that both load
// the same types.
void registerQmlTypes();
