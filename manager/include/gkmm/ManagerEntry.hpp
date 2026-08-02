#pragma once

// The in-game manager UI is hosted by the runtime DLL.  The runtime calls
// GkmmInitialize once its catalog is ready, which is the condition the manager
// used to poll for across a DLL boundary.
extern "C" bool GkmmInitialize();
extern "C" void GkmmShutdown();
