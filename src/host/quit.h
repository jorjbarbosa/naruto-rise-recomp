#pragma once

// One way out of the process for every exit: the window's close, the menu's QUIT, and the guest's own
// exits (XamLoaderLaunchTitle, XamLoaderTerminateTitle, HalReturnToFirmware, the end of its main
// thread). The host main thread does the quitting, after its event loop ends: it waits for a save
// being written (a content package mounted), saves the port's settings, stops the pads' motors and
// exits.

namespace host
{
    // Asks the main thread to quit; returns at once. Logs the reason. Any thread.
    void Quit(const char* reason);
    // Quit, from a guest thread that must not run on (the guest's exits): blocks it for good.
    [[noreturn]] void QuitFromGuest(const char* reason);
    // Host main thread, once its event loop has returned: the quit itself. Never returns.
    [[noreturn]] void FinishQuit();
}
