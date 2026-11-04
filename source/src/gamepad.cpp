#include "gamepad.hpp"

#include <hidsdi.h>
#include <Xinput.h>

#include <algorithm>
#include <string>

#include "debug_log.hpp"

#pragma comment(lib, "hid.lib")

namespace esrwiki::pad {

namespace {
using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);

std::string Hex(unsigned value) {
    char text[16];
    std::snprintf(text, sizeof text, "0x%04X", value);
    return text;
}
}  // namespace

// One open PlayStation controller and the thread that reads its reports.
struct Gamepad::Reader {
    Gamepad* owner = nullptr;
    HANDLE file = INVALID_HANDLE_VALUE;
    HANDLE thread = nullptr;
    HANDLE stop = nullptr;
    SonyModel model = SonyModel::DualShock4;
    std::uint16_t product = 0;
    DWORD reportSize = 64;
    bool alive = true;

    static DWORD WINAPI Run(LPVOID parameter) {
        auto* self = static_cast<Reader*>(parameter);
        std::vector<std::uint8_t> buffer(std::max<DWORD>(self->reportSize, 128));
        OVERLAPPED overlapped{};
        overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        while (overlapped.hEvent != nullptr) {
            ResetEvent(overlapped.hEvent);
            DWORD got = 0;
            if (!ReadFile(self->file, buffer.data(), static_cast<DWORD>(self->reportSize), nullptr, &overlapped) &&
                GetLastError() != ERROR_IO_PENDING) {
                break;
            }
            const HANDLE waits[2] = {overlapped.hEvent, self->stop};
            const DWORD woke = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
            if (woke != WAIT_OBJECT_0) {
                CancelIoEx(self->file, &overlapped);
                break;
            }
            if (!GetOverlappedResult(self->file, &overlapped, &got, FALSE) || got == 0) break;
            State parsed;
            if (ParseSonyReport(self->model, buffer.data(), got, parsed)) {
                const std::uint64_t now = GetTickCount64();
                std::lock_guard<std::mutex> guard(self->owner->lock_);
                self->owner->hid_ = parsed;
                self->owner->hidSeen_ = now;
                if (Active(parsed)) self->owner->hidActive_ = now;
            } else {
                DebugLogOnce("pad-report-" + std::to_string(buffer[0]) + "-" + std::to_string(got),
                             "controller: unknown PlayStation report id " + Hex(buffer[0]) + ", " + std::to_string(got) +
                                 " bytes (product " + Hex(self->product) + ")");
            }
        }
        if (overlapped.hEvent != nullptr) CloseHandle(overlapped.hEvent);
        self->alive = false;
        return 0;
    }
};

Gamepad::Gamepad() = default;

Gamepad::~Gamepad() {
    Stop();
    if (xinputLib_ != nullptr) FreeLibrary(xinputLib_);
}

void Gamepad::Start() {
    started_ = true;
    nextScan_ = 0;
    Rescan(GetTickCount64());
}

void Gamepad::Stop() {
    started_ = false;
    CloseReaders();
}

void Gamepad::CloseReaders() {
    for (auto& reader : readers_) {
        if (reader->stop != nullptr) SetEvent(reader->stop);
    }
    for (auto& reader : readers_) {
        if (reader->thread != nullptr) {
            WaitForSingleObject(reader->thread, 1500);
            CloseHandle(reader->thread);
        }
        if (reader->stop != nullptr) CloseHandle(reader->stop);
        if (reader->file != INVALID_HANDLE_VALUE) CloseHandle(reader->file);
    }
    readers_.clear();
    std::lock_guard<std::mutex> guard(lock_);
    hidSeen_ = 0;
    hidActive_ = 0;
}

// Opens every PlayStation controller that is plugged in and not already open.
void Gamepad::Rescan(std::uint64_t now) {
    nextScan_ = now + 3000;
    // Drop readers whose device went away.
    for (std::size_t i = readers_.size(); i-- > 0;) {
        if (readers_[i]->alive) continue;
        if (readers_[i]->thread != nullptr) { WaitForSingleObject(readers_[i]->thread, 200); CloseHandle(readers_[i]->thread); }
        if (readers_[i]->stop != nullptr) CloseHandle(readers_[i]->stop);
        if (readers_[i]->file != INVALID_HANDLE_VALUE) CloseHandle(readers_[i]->file);
        readers_.erase(readers_.begin() + static_cast<std::ptrdiff_t>(i));
    }
    UINT count = 0;
    if (GetRawInputDeviceList(nullptr, &count, sizeof(RAWINPUTDEVICELIST)) != 0 || count == 0) return;
    std::vector<RAWINPUTDEVICELIST> list(count);
    if (GetRawInputDeviceList(list.data(), &count, sizeof(RAWINPUTDEVICELIST)) == static_cast<UINT>(-1)) return;
    for (UINT i = 0; i < count; ++i) {
        if (list[i].dwType != RIM_TYPEHID) continue;
        RID_DEVICE_INFO info{};
        info.cbSize = sizeof(info);
        UINT size = sizeof(info);
        if (GetRawInputDeviceInfoW(list[i].hDevice, RIDI_DEVICEINFO, &info, &size) == static_cast<UINT>(-1)) continue;
        if (info.dwType != RIM_TYPEHID || info.hid.dwVendorId != kSonyVendor) continue;
        SonyModel model;
        if (!SonyModelOf(static_cast<std::uint16_t>(info.hid.dwProductId), model)) continue;
        if (info.hid.usUsagePage != 1 || info.hid.usUsage != 5) continue;  // the gamepad interface only
        UINT nameLength = 0;
        GetRawInputDeviceInfoW(list[i].hDevice, RIDI_DEVICENAME, nullptr, &nameLength);
        if (nameLength == 0) continue;
        std::wstring path(nameLength, L'\0');
        if (GetRawInputDeviceInfoW(list[i].hDevice, RIDI_DEVICENAME, path.data(), &nameLength) == static_cast<UINT>(-1)) continue;
        while (!path.empty() && path.back() == L'\0') path.pop_back();
        bool known = false;
        for (const auto& reader : readers_) {
            // The same device is not opened twice (its handle path is kept in the log only).
            if (reader->alive && reader->product == info.hid.dwProductId && reader->file != INVALID_HANDLE_VALUE) {
                // Several controllers of one model are rare; the first is used.
                known = true;
            }
        }
        if (known) continue;
        const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                        OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            DebugLogOnce("pad-open-" + std::to_string(info.hid.dwProductId),
                         "controller: PlayStation controller (product " + Hex(info.hid.dwProductId) +
                             ") found but could not be opened (error " + std::to_string(GetLastError()) +
                             "); another program may hold it exclusively");
            continue;
        }
        DWORD reportSize = 64;
        PHIDP_PREPARSED_DATA preparsed = nullptr;
        if (HidD_GetPreparsedData(file, &preparsed)) {
            HIDP_CAPS caps{};
            if (HidP_GetCaps(preparsed, &caps) == HIDP_STATUS_SUCCESS && caps.InputReportByteLength > 0)
                reportSize = caps.InputReportByteLength;
            HidD_FreePreparsedData(preparsed);
        }
        auto reader = std::make_unique<Reader>();
        reader->owner = this;
        reader->file = file;
        reader->model = model;
        reader->product = static_cast<std::uint16_t>(info.hid.dwProductId);
        reader->reportSize = reportSize;
        reader->stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        reader->thread = CreateThread(nullptr, 0, Reader::Run, reader.get(), 0, nullptr);
        if (reader->thread == nullptr || reader->stop == nullptr) {
            if (reader->stop != nullptr) CloseHandle(reader->stop);
            CloseHandle(file);
            continue;
        }
        DebugLog(std::string("controller: reading PlayStation controller, ") +
                 (model == SonyModel::DualSense ? "DualSense" : "DualShock 4") + " (product " + Hex(info.hid.dwProductId) +
                 "), input report " + std::to_string(reportSize) + " bytes");
        readers_.push_back(std::move(reader));
    }
}

bool Gamepad::PollXInput(State& out, std::uint64_t now) {
    if (!xinputTried_) {
        xinputTried_ = true;
        for (const wchar_t* name : {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"}) {
            xinputLib_ = LoadLibraryW(name);
            if (xinputLib_ != nullptr) break;
        }
        if (xinputLib_ != nullptr) xinputGet_ = reinterpret_cast<void*>(GetProcAddress(xinputLib_, "XInputGetState"));
    }
    if (xinputGet_ == nullptr) return false;
    const auto get = reinterpret_cast<XInputGetStateFn>(xinputGet_);
    int best = -1;
    for (DWORD slot = 0; slot < 4; ++slot) {
        if (!xConnected_[slot] && now < xProbe_[slot]) continue;  // an empty slot is checked every two seconds
        XINPUT_STATE raw{};
        if (get(slot, &raw) != ERROR_SUCCESS) {
            xConnected_[slot] = false;
            xProbe_[slot] = now + 2000;
            continue;
        }
        xConnected_[slot] = true;
        if (raw.dwPacketNumber != xPacket_[slot] || xActive_[slot] == 0) {
            xPacket_[slot] = raw.dwPacketNumber;
            State s;
            const WORD b = raw.Gamepad.wButtons;
            auto set = [&s](Button button, bool value) { s.down[static_cast<int>(button)] = value; };
            set(Button::Up, (b & XINPUT_GAMEPAD_DPAD_UP) != 0);
            set(Button::Down, (b & XINPUT_GAMEPAD_DPAD_DOWN) != 0);
            set(Button::Left, (b & XINPUT_GAMEPAD_DPAD_LEFT) != 0);
            set(Button::Right, (b & XINPUT_GAMEPAD_DPAD_RIGHT) != 0);
            set(Button::Start, (b & XINPUT_GAMEPAD_START) != 0);
            set(Button::Back, (b & XINPUT_GAMEPAD_BACK) != 0);
            set(Button::L3, (b & XINPUT_GAMEPAD_LEFT_THUMB) != 0);
            set(Button::R3, (b & XINPUT_GAMEPAD_RIGHT_THUMB) != 0);
            set(Button::LB, (b & XINPUT_GAMEPAD_LEFT_SHOULDER) != 0);
            set(Button::RB, (b & XINPUT_GAMEPAD_RIGHT_SHOULDER) != 0);
            set(Button::A, (b & XINPUT_GAMEPAD_A) != 0);
            set(Button::B, (b & XINPUT_GAMEPAD_B) != 0);
            set(Button::X, (b & XINPUT_GAMEPAD_X) != 0);
            set(Button::Y, (b & XINPUT_GAMEPAD_Y) != 0);
            s.lx = std::max(-1.0f, raw.Gamepad.sThumbLX / 32767.0f);
            s.ly = std::max(-1.0f, raw.Gamepad.sThumbLY / 32767.0f);
            s.rx = std::max(-1.0f, raw.Gamepad.sThumbRX / 32767.0f);
            s.ry = std::max(-1.0f, raw.Gamepad.sThumbRY / 32767.0f);
            DeadZone(s.lx, s.ly, 0.22f);
            DeadZone(s.rx, s.ry, 0.22f);
            s.lt = raw.Gamepad.bLeftTrigger / 255.0f;
            s.rt = raw.Gamepad.bRightTrigger / 255.0f;
            s.kind = Kind::Xbox;
            xState_[slot] = s;
            if (Active(s)) xActive_[slot] = now;
        }
        if (best < 0 || xActive_[slot] > xActive_[best]) best = static_cast<int>(slot);
    }
    if (best < 0) return false;
    out = xState_[best];
    return true;
}

bool Gamepad::Poll(State& out, std::uint64_t now) {
    if (started_ && now >= nextScan_) {
        bool anyAlive = false;
        for (const auto& reader : readers_) anyAlive = anyAlive || reader->alive;
        if (!anyAlive || true) Rescan(now);  // also picks up a controller plugged in while the wiki is open
    }
    State hid;
    std::uint64_t hidSeen = 0, hidActive = 0;
    {
        std::lock_guard<std::mutex> guard(lock_);
        hid = hid_;
        hidSeen = hidSeen_;
        hidActive = hidActive_;
    }
    const bool haveHid = hidSeen != 0 && now - hidSeen < 1500;
    State x;
    const bool haveX = PollXInput(x, now);
    if (!haveHid && !haveX) {
        lastSource_ = 0;
        return false;
    }
    std::uint64_t xActive = 0;
    for (int i = 0; i < 4; ++i) if (xConnected_[i]) xActive = std::max(xActive, xActive_[i]);
    const bool useHid = haveHid && (!haveX || hidActive >= xActive);
    out = useHid ? hid : x;
    if (override_ != Kind::None) out.kind = override_;
    const int source = useHid ? 2 : 1;
    if (source != lastSource_) {
        lastSource_ = source;
        DebugLog(std::string("controller: using ") + (useHid ? "the PlayStation controller" : "an XInput controller") +
                 (override_ != Kind::None ? " (help picture forced by controller.txt)" : ""));
    }
    return true;
}

}  // namespace esrwiki::pad
