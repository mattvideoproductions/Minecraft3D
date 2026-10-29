#pragma once
// v0.3.3 "Swap A/B buttons" (the project's design notes (not in this repo)).
// The game reads the pad only through the SDK ExtraPadReader, once per frame
// from its input poll: ReadLatest (fnPadReadLatest) and Read (fnPadRead).
// Both are hooked (MITM, register arguments only); after the original
// returns, bits 0 (A) and 1 (B) of hold, trigger and release are exchanged in
// every status the call wrote (pica::SwapAB), consistently for all three
// words, so the game's own edge detection stays right. HID shared memory is
// never written, so the CTRPF menu (libctru's own reader, hotkey Select)
// sees the real buttons. The swap in effect follows the menu toggle only
// while A and B are untouched (pica::AbLatchStep): no stuck press when the
// toggle flips mid-press. Game main thread only; the menu sets the wish.
#include <3ds/types.h>
#include <string>

namespace mc3d::input
{
    void        Install();              // PatchProcess (cfg hook "input")
    bool        Available();            // both pad-read hooks installed
    void        SetSwap(bool on);       // plugin thread (menu); start-up value: cfg abswap=on|off
    bool        SwapWanted();
    bool        SwapActive();           // what the game sees right now
    std::string HookStatus();           // one line for "Plugin status"
}
