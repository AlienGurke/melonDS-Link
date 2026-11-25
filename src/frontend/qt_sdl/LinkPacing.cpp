/*
    melonDS Link - how evenly melonDS runs, see LinkPacing.h
*/

#include "LinkPacing.h"

#include <QTime>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace Link
{
namespace Pacing
{

namespace
{

constexpr double kFrameMs = 1000.0 / 59.8261;   // one DS frame (263 lines)

double nowMs()
{
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

std::string strf(const char* f, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, f);
    vsnprintf(buf, sizeof(buf), f, ap);
    va_end(ap);
    return buf;
}

struct Part
{
    double sum = 0, max = 0;
    void add(double v) { sum += v; max = std::max(max, v); }
};

struct Stats
{
    double since = 0;              // first frame of this period
    unsigned frames = 0, fastFrames = 0;
    // time between the ends of two frames
    unsigned gapsOver20 = 0, gapsOver25 = 0, gapsOver34 = 0, gapsUnder12 = 0;
    double gapMax = 0;
    Part emu, capture, draw, audio, limiter, other;
    unsigned drawOver8 = 0;
    // speed per second (emulated time / real time)
    double speedMin = 1e9, speedMax = 0;
    // pictures to the window
    unsigned presents = 0, presentApart[4] = {}, presentOffGrid = 0, presentRhythm = 0;
    double presentGapMax = 0;
    Part swap;
    // long frames, with what took the time
    std::vector<std::string> hitches;
    unsigned hitchesMore = 0;
};

std::mutex g_mutex;
Stats g_s;
double g_lastEnd = 0;          // end of the last frame (0: none / paused)
double g_secStart = 0, g_secLines = 0;
double g_lastPresent = 0;
int g_lastApart = 0;
std::string g_settings, g_settingsLogged;

}

void Frame(const FrameTimes& t)
{
    const double now = nowMs();
    std::lock_guard<std::mutex> lock(g_mutex);
    Stats& s = g_s;
    if (s.since == 0) s.since = now;

    if (t.fast)
    {
        s.fastFrames++;
        g_lastEnd = 0;
        g_secStart = 0;
        return;
    }
    s.frames++;
    s.emu.add(t.emu);
    s.capture.add(t.capture);
    s.draw.add(t.draw);
    s.audio.add(t.audio);
    s.limiter.add(t.limiter);
    const double other = std::max(0.0, t.total - t.emu - t.capture - t.draw - t.audio - t.limiter);
    s.other.add(other);
    if (t.draw > 8) s.drawOver8++;

    if (g_lastEnd != 0)
    {
        const double gap = now - g_lastEnd;
        if (gap > 20) s.gapsOver20++;
        if (gap > 25) s.gapsOver25++;
        if (gap > 34) s.gapsOver34++;
        if (gap < 12) s.gapsUnder12++;
        s.gapMax = std::max(s.gapMax, gap);
        if (gap > 22)
        {
            if (s.hitches.size() < 20)
                s.hitches.push_back(strf("%s: %.1f ms since the last frame - emulation %.1f, bottom screen for "
                                         "the 3DS %.1f, drawing %.1f, audio sync %.1f, frame limiter %.1f, "
                                         "rest %.1f ms",
                                         QTime::currentTime().toString("HH:mm:ss.zzz").toStdString().c_str(),
                                         gap, t.emu, t.capture, t.draw, t.audio, t.limiter, other));
            else
                s.hitchesMore++;
        }
    }
    g_lastEnd = now;

    // speed per second
    if (g_secStart == 0) { g_secStart = now; g_secLines = 0; }
    g_secLines += t.lines;
    if (now - g_secStart >= 1000)
    {
        const double emulated = g_secLines / 263.0 * kFrameMs;
        const double speed = 100.0 * emulated / (now - g_secStart);
        s.speedMin = std::min(s.speedMin, speed);
        s.speedMax = std::max(s.speedMax, speed);
        g_secStart = now;
        g_secLines = 0;
    }
}

void Paused()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_lastEnd = 0;
    g_secStart = 0;
    g_lastPresent = 0;
}

void Presented(double swapMs)
{
    const double now = nowMs();
    std::lock_guard<std::mutex> lock(g_mutex);
    Stats& s = g_s;
    s.presents++;
    s.swap.add(swapMs);
    if (g_lastPresent != 0 && now - g_lastPresent < 500)
    {
        const double gap = now - g_lastPresent;
        int apart = std::max(1, (int)(gap / kFrameMs + 0.5));
        if (std::abs(gap - apart * kFrameMs) > 4) s.presentOffGrid++;
        if (apart != g_lastApart && g_lastApart != 0) s.presentRhythm++;
        g_lastApart = apart;
        s.presentApart[std::min(apart, 4) - 1]++;
        s.presentGapMax = std::max(s.presentGapMax, gap);
    }
    g_lastPresent = now;
}

void Settings(const std::string& line)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_settings = line;
}

std::vector<std::string> Take()
{
    const double now = nowMs();
    std::vector<std::string> out;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_settings.empty() && g_settings != g_settingsLogged)
    {
        g_settingsLogged = g_settings;
        out.push_back("melonds: settings - " + g_settings);
    }
    Stats& s = g_s;
    if (s.frames > 0)
    {
        const double n = s.frames;
        const double secs = (now - s.since) / 1000.0;
        out.push_back(strf(
            "melonds: %u frames in %.1f s (speed per second %.1f-%.1f %%)%s; time between frames over "
            "20/25/34 ms %u/%u/%u (longest %.1f ms), under 12 ms %u; a frame on average: emulation "
            "%.1f ms (%.1f at most), bottom screen for the 3DS %.1f ms (%.1f at most), drawing %.1f ms "
            "(%.1f at most, over 8 ms %u), audio sync %.1f ms (%.1f at most), frame limiter %.1f ms, "
            "rest %.1f ms (%.1f at most)",
            s.frames, secs, s.speedMax > 0 ? s.speedMin : 0.0, s.speedMax,
            s.fastFrames ? strf(", fast forward %u", s.fastFrames).c_str() : "",
            s.gapsOver20, s.gapsOver25, s.gapsOver34, s.gapMax, s.gapsUnder12,
            s.emu.sum / n, s.emu.max, s.capture.sum / n, s.capture.max, s.draw.sum / n, s.draw.max,
            s.drawOver8, s.audio.sum / n, s.audio.max, s.limiter.sum / n, s.other.sum / n, s.other.max));
        if (s.presents > 0)
            out.push_back(strf(
                "melonds: to the window %u pictures; 1/2/3/4+ DS frames apart %u/%u/%u/%u, rhythm changes "
                "%u, off the frame grid (over 4 ms) %u, longest gap %.1f ms; a swap / paint took %.1f ms "
                "on average (%.1f at most)",
                s.presents, s.presentApart[0], s.presentApart[1], s.presentApart[2], s.presentApart[3],
                s.presentRhythm, s.presentOffGrid, s.presentGapMax, s.swap.sum / s.presents, s.swap.max));
        for (const std::string& h : s.hitches) out.push_back("melonds: long frame " + h);
        if (s.hitchesMore) out.push_back(strf("melonds: %u more long frames", s.hitchesMore));
    }
    else if (s.fastFrames > 0)
    {
        out.push_back(strf("melonds: %u frames in fast forward / slow motion", s.fastFrames));
    }
    g_s = Stats();
    g_s.since = now;
    return out;
}

}
}
