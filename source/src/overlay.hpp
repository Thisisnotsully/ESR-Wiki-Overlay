// Win32 overlay that browses the ESR wiki data. Hosted on its own UI thread,
// like the D2RR Item Database overlay; the rest of the plugin never touches
// window handles.
#pragma once

#include <functional>
#include <memory>
#include <string>

#include "wiki_model.hpp"

namespace esrwiki {

struct OverlayState;  // window and UI state, defined in overlay.cpp

class Overlay {
public:
    Overlay();
    ~Overlay();
    Overlay(const Overlay&) = delete;
    Overlay& operator=(const Overlay&) = delete;

    // `database` and the image folder must outlive the overlay. `dataDir` is the
    // folder that holds images\<key>.png. `title` is shown in the header, and
    // `instanceId` keeps this window's class names unique when two overlays run.
    // If `switchLabel` is set, a button with that label calls `onSwitch` (for
    // example to open the other overlay). Returns false if the window cannot start.
    bool Start(const Database& database, const std::wstring& dataDir, const std::wstring& title,
               const std::wstring& instanceId, const std::wstring& switchLabel = L"",
               std::function<void()> onSwitch = {});
    void Stop();
    void Toggle();  // show or hide; safe to call from any thread
    void Open();    // show without toggling; safe to call from any thread
    // Replaces the data shown with `next`, which the overlay then owns. Safe to call from any thread; the
    // switch happens on the overlay's own thread. Returns false if the overlay is not running or `next` is empty.
    bool ReplaceDatabase(std::unique_ptr<Database> next);
    // Key that closes the overlay while its own window has keyboard focus (the game's actions
    // do not fire then). Call before Start.
    void SetCloseKey(std::function<bool(unsigned int virtualKey)> isCloseKey);

private:
    std::unique_ptr<OverlayState> impl_;
};

}  // namespace esrwiki
