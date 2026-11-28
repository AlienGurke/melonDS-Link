// melonDS Link: play time on this PC, for Dashboard-Link.
//
// The same format as Azahar-Link (one file per emulator, shared pc.txt and titles.txt):
//   %APPDATA%\3DS-Link\activity\melonds.bin
//   header "3DSLPCPL", u32 version 1, u32 entry size 24
//   entry: u64 title id, u32 start (local time, seconds since 1.1.2000), u32 seconds played,
//          u32 game code, u16 flags (1 = with a 3DS, 2 = ended normally), u16 reserved
// A DS game gets the title id 00048004 + its game code (as on the 3DS for DS titles), so
// Dashboard-Link can put it together with the 3DS's own log. Pauses are not counted.

#pragma once

#include <string>

namespace melonDS::NDSCart { class CartCommon; }

namespace Link::Activity
{

void SetEnabled(bool on);
bool Enabled();
std::string Folder();

// called by the emulation thread on every loop: the running cart (nullptr = none) and
// whether the game runs right now
void Frame(const melonDS::NDSCart::CartCommon* cart, bool running);
// the session ends (melonDS closes)
void End();

}
