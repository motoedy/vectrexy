#pragma once
#include "core/BitOps.h"
#include "emulator/EngineClient.h"

class SDLEngine {
public:
    void RegisterClient(IEngineClient& client);

    // Blocking call, returns when application is exited (window closed, etc.)
    bool Run(int argc, char** argv);

private:
    void PollEvents(bool& quit);
    double UpdateFrameTime();
    void UpdateMenu(bool& quit, EmuEvents& emuEvents);
};
