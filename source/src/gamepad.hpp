// Reads the game controller for the wiki overlay. Two sources, whichever the person uses:
//   * XInput: Xbox controllers, and anything that presents itself as one (Steam Input, DS4Windows).
//   * PlayStation controllers (DualShock 4, DualSense) read directly from their HID device with the overlay's own
//     handle, so nothing the game registered or opened is touched; if the game holds the device exclusively the
//     controller is simply not seen and the log says so.
// Which kind is in use is reported in State::kind (PlayStation or Xbox) so the overlay can show the right help
// picture. See pad_logic.hpp for the button names and the report layouts.
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "pad_logic.hpp"

namespace esrwiki::pad {

class Gamepad {
public:
    Gamepad();
    ~Gamepad();
    Gamepad(const Gamepad&) = delete;
    Gamepad& operator=(const Gamepad&) = delete;

    // Opens the PlayStation controllers that are plugged in; Stop closes them. Both may be called repeatedly.
    void Start();
    void Stop();

    // The state of the controller used last. false when no controller is connected. now is GetTickCount64().
    bool Poll(State& out, std::uint64_t now);

    // Forces the help-picture kind (a PlayStation controller seen through Steam Input looks like an Xbox one).
    void SetKindOverride(Kind kind) { override_ = kind; }

private:
    struct Reader;
    void Rescan(std::uint64_t now);
    bool PollXInput(State& out, std::uint64_t now);
    void CloseReaders();

    std::mutex lock_;
    std::vector<std::unique_ptr<Reader>> readers_;
    State hid_;                      // latest report (guarded by lock_)
    std::uint64_t hidSeen_ = 0;      // when it arrived
    std::uint64_t hidActive_ = 0;    // when a button or stick was last used on it
    std::uint64_t nextScan_ = 0;
    bool started_ = false;

    HMODULE xinputLib_ = nullptr;
    void* xinputGet_ = nullptr;
    bool xinputTried_ = false;
    State xState_[4];
    bool xConnected_[4] = {};
    std::uint64_t xProbe_[4] = {};
    std::uint64_t xActive_[4] = {};
    std::uint32_t xPacket_[4] = {};
    int lastSource_ = 0;  // 0 none, 1 xinput, 2 playstation (for the log)
    Kind override_ = Kind::None;
};

}  // namespace esrwiki::pad
