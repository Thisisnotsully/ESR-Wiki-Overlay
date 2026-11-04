#include "input_block.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>

#include "debug_log.hpp"

namespace esrwiki::inputblock {
namespace {

// Offsets from the game's main module (D2RLoader.exe in the loader process; base 0x140000000 when checked).
constexpr std::uintptr_t kCallSiteRva = 0x13B3A2;  // call rel32 to the key-transition handler
constexpr std::uintptr_t kHandlerRva = 0x13EDD0;   // void __fastcall(unsigned key, bool pressed)
constexpr std::uintptr_t kAxisRva = 0x13F470;      // void __fastcall(unsigned kind, float x, float y): stick values

// The call as the game has it: E8 rel32, rel32 = 0x13EDD0 - 0x13B3A7 = 0x3A29.
constexpr unsigned char kOriginalCall[5] = {0xE8, 0x29, 0x3A, 0x00, 0x00};

// The handler's first 21 bytes, the same witness the repo uses for the same function on this build.
constexpr unsigned char kHandlerPrologue[21] = {0x40, 0x55, 0x53, 0x57, 0x41, 0x55, 0x48, 0x8D, 0xAC, 0x24, 0xA8,
                                                0xFD, 0xFF, 0xFF, 0x48, 0x81, 0xEC, 0x58, 0x03, 0x00, 0x00};

// The axis dispatcher's first 27 bytes (up to its first relative call), from the repo's profile for this function.
// Its first 5 bytes are push rbx / push rbp / push r14: three whole position-independent instructions, which the
// stub runs itself before jumping back to the function at +5.
constexpr unsigned char kAxisPrologue[27] = {0x40, 0x53, 0x55, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x50, 0x0F, 0x29,
                                             0x74, 0x24, 0x40, 0x8B, 0xE9, 0x0F, 0x29, 0x7C, 0x24, 0x30, 0x0F,
                                             0x28, 0xF1, 0x0F, 0x28, 0xFA};
constexpr unsigned char kAxisStolen[5] = {0x40, 0x53, 0x55, 0x41, 0x56};

// Stub pages hold the code at the start and two absolute-address slots at fixed offsets.
constexpr std::size_t kButtonSlotBlock = 0x40;    // address of EsrBlockPress
constexpr std::size_t kButtonSlotHandler = 0x48;  // address of the original key handler
constexpr std::size_t kAxisSlotBlock = 0x48;      // address of EsrBlockAxis
constexpr std::size_t kAxisSlotResume = 0x50;     // address of the dispatcher + 5
constexpr std::size_t kStubSize = 0x1000;

struct Patch {
    std::uint8_t* site = nullptr;
    unsigned char original[5] = {};
    unsigned char patched[5] = {};
    bool installed = false;
};

std::mutex gLock;
std::atomic<bool> gActive{false};
std::function<bool()> gIsWikiOpen;
Patch gButtons;
Patch gAxes;
std::atomic<std::uint64_t> gPressesBlocked{0};
std::atomic<std::uint64_t> gAxesBlocked{0};

bool WikiOpen() noexcept {
    try {
        return gIsWikiOpen && gIsWikiOpen();
    } catch (...) {
        return false;
    }
}

// Called by the button stub on every controller key transition. `pressed` is 1 for a press, 0 for a release.
extern "C" bool EsrBlockPress(unsigned int pressed) noexcept {
    try {
        if (!gActive.load() || pressed == 0 || !WikiOpen()) return false;
        if (gPressesBlocked.fetch_add(1) == 0) DebugLog("controller press blocked while the wiki is open (first one)");
        return true;
    } catch (...) {
        return false;
    }
}

// Called by the axis stub on every stick update. Returns true when the stick values must be zeroed.
extern "C" bool EsrBlockAxis(unsigned int kind) noexcept {
    try {
        if (!gActive.load() || !WikiOpen()) return false;
        if (gAxesBlocked.fetch_add(1) == 0)
            DebugLog("controller stick zeroed while the wiki is open (first one, kind " + std::to_string(kind) + ")");
        return true;
    } catch (...) {
        return false;
    }
}

bool SameBytes(const std::uint8_t* at, const unsigned char* expected, std::size_t length) {
    return std::memcmp(at, expected, length) == 0;
}

std::string Hex(const std::uint8_t* at, std::size_t length) {
    static const char digits[] = "0123456789ABCDEF";
    std::string text;
    for (std::size_t i = 0; i < length; ++i) {
        if (i != 0) text += ' ';
        text += digits[at[i] >> 4];
        text += digits[at[i] & 0x0F];
    }
    return text;
}

// Finds a page within the reach of a rel32 jump (+/-2 GB) from `target`.
std::uint8_t* AllocateNear(std::uint8_t* target) {
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    const std::int64_t step = static_cast<std::int64_t>(info.dwAllocationGranularity);
    const std::int64_t origin = static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(target));
    const std::int64_t reach = 0x7FF00000;
    for (std::int64_t distance = 0; distance < reach; distance += step) {
        for (const std::int64_t sign : {std::int64_t{1}, std::int64_t{-1}}) {
            if (distance == 0 && sign < 0) continue;
            const std::int64_t candidate = origin + sign * distance;
            if (candidate <= 0x10000) continue;
            void* page = VirtualAlloc(reinterpret_cast<void*>(static_cast<std::uintptr_t>(candidate)), kStubSize,
                                      MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
            if (page != nullptr) return static_cast<std::uint8_t*>(page);
        }
    }
    return nullptr;
}

void PutPointer(std::uint8_t* stub, std::size_t offset, const void* value) {
    std::memcpy(stub + offset, &value, sizeof(value));
}

void PutInt32(std::uint8_t* at, std::int32_t value) { std::memcpy(at, &value, 4); }

// Writes 5 bytes over the game's code and flushes the instruction cache.
bool Rewrite(std::uint8_t* site, const unsigned char* bytes) {
    DWORD oldProtect = 0;
    if (!VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) return false;
    std::memcpy(site, bytes, 5);
    DWORD ignored = 0;
    VirtualProtect(site, 5, oldProtect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    return true;
}

// Button stub. Entered by the rewritten call, so the return address is the caller's continuation.
//   sub  rsp,0x38            save rcx/rdx in the shadow space, keep 16-byte alignment
//   mov  [rsp+0x20],rcx
//   mov  [rsp+0x28],rdx
//   movzx ecx,dl             pressed flag as the argument
//   call [rip+slotBlock]     EsrBlockPress(pressed)
//   mov  rcx,[rsp+0x20]
//   mov  rdx,[rsp+0x28]
//   add  rsp,0x38
//   test al,al
//   jz   pass
//   ret                      dropped: return to the caller without touching the game
// pass:
//   jmp  [rip+slotHandler]   the original handler, with the stack exactly as it was on entry
void WriteButtonStub(std::uint8_t* stub) {
    const unsigned char code[] = {
        0x48, 0x83, 0xEC, 0x38,                    // 0x00 sub rsp,0x38
        0x48, 0x89, 0x4C, 0x24, 0x20,              // 0x04 mov [rsp+0x20],rcx
        0x48, 0x89, 0x54, 0x24, 0x28,              // 0x09 mov [rsp+0x28],rdx
        0x0F, 0xB6, 0xCA,                          // 0x0E movzx ecx,dl
        0xFF, 0x15, 0x00, 0x00, 0x00, 0x00,        // 0x11 call [rip+slotBlock]  (fixed below)
        0x48, 0x8B, 0x4C, 0x24, 0x20,              // 0x17 mov rcx,[rsp+0x20]
        0x48, 0x8B, 0x54, 0x24, 0x28,              // 0x1C mov rdx,[rsp+0x28]
        0x48, 0x83, 0xC4, 0x38,                    // 0x21 add rsp,0x38
        0x84, 0xC0,                                // 0x25 test al,al
        0x74, 0x01,                                // 0x27 jz +1
        0xC3,                                      // 0x29 ret
        0xFF, 0x25, 0x00, 0x00, 0x00, 0x00,        // 0x2A jmp [rip+slotHandler] (fixed below)
    };
    static_assert(sizeof(code) <= kButtonSlotBlock, "button stub overlaps its slots");
    std::memcpy(stub, code, sizeof(code));
    PutInt32(stub + 0x13, static_cast<std::int32_t>(kButtonSlotBlock - 0x17));
    PutInt32(stub + 0x2C, static_cast<std::int32_t>(kButtonSlotHandler - 0x30));
}

// Axis stub. Entered by a jmp at the dispatcher's entry, so the stack is exactly as at entry. The arguments are
// rcx = kind, xmm1 = x, xmm2 = y.
//   sub  rsp,0x58            keeps 16-byte alignment (the entry rsp is 8 mod 16)
//   mov  [rsp+0x20],rcx
//   movups [rsp+0x30],xmm1
//   movups [rsp+0x40],xmm2
//   call [rip+slotBlock]     EsrBlockAxis(kind): true while the wiki is open
//   mov  rcx,[rsp+0x20]
//   movups xmm1,[rsp+0x30]
//   movups xmm2,[rsp+0x40]
//   add  rsp,0x58
//   test al,al
//   jz   run
//   xorps xmm1,xmm1          the stick is reported as centred
//   xorps xmm2,xmm2
// run:
//   push rbx / push rbp / push r14   the dispatcher's own first five bytes, run here
//   jmp  [rip+slotResume]    the dispatcher at +5
void WriteAxisStub(std::uint8_t* stub) {
    const unsigned char code[] = {
        0x48, 0x83, 0xEC, 0x58,                    // 0x00 sub rsp,0x58
        0x48, 0x89, 0x4C, 0x24, 0x20,              // 0x04 mov [rsp+0x20],rcx
        0x0F, 0x11, 0x4C, 0x24, 0x30,              // 0x09 movups [rsp+0x30],xmm1
        0x0F, 0x11, 0x54, 0x24, 0x40,              // 0x0E movups [rsp+0x40],xmm2
        0xFF, 0x15, 0x00, 0x00, 0x00, 0x00,        // 0x13 call [rip+slotBlock]  (fixed below)
        0x48, 0x8B, 0x4C, 0x24, 0x20,              // 0x19 mov rcx,[rsp+0x20]
        0x0F, 0x10, 0x4C, 0x24, 0x30,              // 0x1E movups xmm1,[rsp+0x30]
        0x0F, 0x10, 0x54, 0x24, 0x40,              // 0x23 movups xmm2,[rsp+0x40]
        0x48, 0x83, 0xC4, 0x58,                    // 0x28 add rsp,0x58
        0x84, 0xC0,                                // 0x2C test al,al
        0x74, 0x06,                                // 0x2E jz +6
        0x0F, 0x57, 0xC9,                          // 0x30 xorps xmm1,xmm1
        0x0F, 0x57, 0xD2,                          // 0x33 xorps xmm2,xmm2
        0x40, 0x53, 0x55, 0x41, 0x56,              // 0x36 push rbx / push rbp / push r14
        0xFF, 0x25, 0x00, 0x00, 0x00, 0x00,        // 0x3B jmp [rip+slotResume] (fixed below)
    };
    static_assert(sizeof(code) <= kAxisSlotBlock, "axis stub overlaps its slots");
    static_assert(sizeof(kAxisStolen) == 5, "the stolen bytes must be exactly the patched span");
    std::memcpy(stub, code, sizeof(code));
    PutInt32(stub + 0x15, static_cast<std::int32_t>(kAxisSlotBlock - 0x19));
    PutInt32(stub + 0x3D, static_cast<std::int32_t>(kAxisSlotResume - 0x41));
}

bool InstallButtons(std::uint8_t* base) {
    std::uint8_t* site = base + kCallSiteRva;
    std::uint8_t* handler = base + kHandlerRva;
    if (!SameBytes(site, kOriginalCall, sizeof(kOriginalCall))) {
        DebugLog("controller block: the button call site does not match this game build (found " + Hex(site, 5) +
                 "); buttons not blocked");
        return false;
    }
    if (!SameBytes(handler, kHandlerPrologue, sizeof(kHandlerPrologue))) {
        DebugLog("controller block: the key handler does not match this game build (found " + Hex(handler, 21) +
                 "); buttons not blocked");
        return false;
    }
    std::uint8_t* stub = AllocateNear(site + 5);
    if (stub == nullptr) {
        DebugLog("controller block: no memory near the game for the button stub; buttons not blocked");
        return false;
    }
    WriteButtonStub(stub);
    PutPointer(stub, kButtonSlotBlock, reinterpret_cast<const void*>(&EsrBlockPress));
    PutPointer(stub, kButtonSlotHandler, handler);

    gButtons.site = site;
    std::memcpy(gButtons.original, kOriginalCall, 5);
    gButtons.patched[0] = 0xE8;  // call rel32 to the stub
    PutInt32(gButtons.patched + 1, static_cast<std::int32_t>(reinterpret_cast<std::uintptr_t>(stub) -
                                                              reinterpret_cast<std::uintptr_t>(site + 5)));
    if (!Rewrite(site, gButtons.patched)) {
        DebugLog("controller block: could not make the button call site writable; buttons not blocked");
        return false;
    }
    gButtons.installed = true;
    return true;
}

bool InstallAxes(std::uint8_t* base) {
    std::uint8_t* site = base + kAxisRva;
    if (!SameBytes(site, kAxisPrologue, sizeof(kAxisPrologue))) {
        DebugLog("controller block: the stick dispatcher does not match this game build (found " + Hex(site, 27) +
                 "); sticks not blocked");
        return false;
    }
    std::uint8_t* stub = AllocateNear(site + 5);
    if (stub == nullptr) {
        DebugLog("controller block: no memory near the game for the stick stub; sticks not blocked");
        return false;
    }
    WriteAxisStub(stub);
    PutPointer(stub, kAxisSlotBlock, reinterpret_cast<const void*>(&EsrBlockAxis));
    PutPointer(stub, kAxisSlotResume, site + 5);

    gAxes.site = site;
    std::memcpy(gAxes.original, kAxisStolen, 5);
    gAxes.patched[0] = 0xE9;  // jmp rel32 to the stub
    PutInt32(gAxes.patched + 1, static_cast<std::int32_t>(reinterpret_cast<std::uintptr_t>(stub) -
                                                           reinterpret_cast<std::uintptr_t>(site + 5)));
    if (!Rewrite(site, gAxes.patched)) {
        DebugLog("controller block: could not make the stick dispatcher writable; sticks not blocked");
        return false;
    }
    gAxes.installed = true;
    return true;
}

void Restore(Patch& patch, const char* what) {
    if (!patch.installed) return;
    if (!SameBytes(patch.site, patch.patched, 5)) {
        DebugLog(std::string("controller block: the ") + what + " was changed by something else; left as it is");
    } else if (!Rewrite(patch.site, patch.original)) {
        DebugLog(std::string("controller block: could not restore the ") + what);
    }
    patch.installed = false;  // the stub page stays allocated: a thread may still be inside it
}

}  // namespace

bool Install(std::function<bool()> isWikiOpen) {
    std::lock_guard<std::mutex> guard(gLock);
    if (gButtons.installed || gAxes.installed) return true;

    auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
    if (base == nullptr) {
        DebugLog("controller block: no game module; not installed");
        return false;
    }
    gIsWikiOpen = std::move(isWikiOpen);  // set before either stub can run

    const bool buttons = InstallButtons(base);
    const bool axes = InstallAxes(base);
    if (!buttons && !axes) return false;

    gActive.store(true);
    DebugLog(std::string("controller block installed: buttons ") + (buttons ? "yes" : "no") + ", sticks " +
             (axes ? "yes" : "no") + "; both are held back only while the wiki is open");
    return true;
}

void Remove() {
    std::lock_guard<std::mutex> guard(gLock);
    if (!gButtons.installed && !gAxes.installed) return;
    gActive.store(false);  // the stubs pass everything through from now on
    Restore(gButtons, "button call");
    Restore(gAxes, "stick dispatcher");
    DebugLog("controller block removed");
}

bool Installed() { return gButtons.installed || gAxes.installed; }

}  // namespace esrwiki::inputblock
