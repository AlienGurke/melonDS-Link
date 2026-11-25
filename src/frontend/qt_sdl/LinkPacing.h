/*
    melonDS Link - how evenly melonDS itself runs, for the log (like LinkPacing in Azahar-Link).

    The emulator thread reports every emulated frame (how long each part took), the window
    every picture it shows. The link's picture thread takes the summary every 10 s and writes
    it to the log - together with every frame that took noticeably longer than one refresh.
*/

#ifndef MELONDS_LINKPACING_H
#define MELONDS_LINKPACING_H

#include <string>
#include <vector>

namespace Link
{
namespace Pacing
{

// one emulated frame, times in ms
struct FrameTimes
{
    double emu = 0;       // RunFrame (or shader compiling)
    double capture = 0;   // the bottom screen for the 3DS (and screenshots)
    double draw = 0;      // drawScreen (OpenGL: including the swap, which waits for VSync)
    double audio = 0;     // audio sync (waits for the sound buffer)
    double limiter = 0;   // the frame limiter's sleep
    double input = 0;     // reading the keys / controllers (and local multiplayer)
    double messages = 0;  // melonDS's own messages from the window (after the last frame)
    double glCurrent = 0; // making the OpenGL context current
    double saves = 0;     // writing the game's save when it changed
    double compare = 0;   // checking which screens show a new picture
    double total = 0;     // the whole loop, start to end
    unsigned lines = 263; // scanlines the frame had (263 = a normal frame)
    bool fast = false;    // fast forward / slow motion: no timing statistics
};

// emulator thread: a frame is done
void Frame(const FrameTimes& t);
// the emulation paused (the gap to the next frame is not a stutter)
void Paused();
// a picture went to the window; swapMs: how long the swap / paint took
void Presented(double swapMs);
// which screens showed a new picture this frame, and whether it went to the window
void Pictures(bool newTop, bool newBottom, bool presented);
// setting: unchanged frames are not shown again (for frame generation)
bool SkipDuplicates();
void SetSkipDuplicates(bool on);
// the settings that matter for the timing; logged whenever they change
void Settings(const std::string& line);

// the summary since the last call (empty if nothing ran) plus the long frames
std::vector<std::string> Take();

}
}

#endif
