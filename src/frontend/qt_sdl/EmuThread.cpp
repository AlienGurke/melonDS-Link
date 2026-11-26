/*
    Copyright 2016-2026 melonDS team

    This file is part of melonDS.

    melonDS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    melonDS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with melonDS. If not, see http://www.gnu.org/licenses/.
*/

#include <stdlib.h>
#include <time.h>
#include <stdio.h>
#include <string.h>

#include <optional>
#include <vector>
#include <string>
#include <algorithm>
#include <chrono>

#include <SDL2/SDL.h>

#include "main.h"

#include "types.h"
#include "version.h"

#include "ScreenLayout.h"

#include "Args.h"
#include "NDS.h"
#include "NDSCart.h"
#include "GBACart.h"
#include "GPU.h"
#include "SPU.h"
#include "Wifi.h"
#include "Platform.h"
#include "LocalMP.h"
#include "Config.h"
#include "RTC.h"
#include "DSi.h"
#include "DSi_I2C.h"
#include "GPU_Soft.h"
#include "GPU_OpenGL.h"

#include "Savestate.h"

#include "EmuInstance.h"
#include "LinkClient.h"
#include "LinkLang.h"
#include "LinkSession.h"
#include "LinkPacing.h"
#include <QDateTime>
#include <QDir>
#include <QImage>

using namespace melonDS;


#ifdef OGLRENDERER_ENABLED
// GL objects of the capture. They belong to the GL context that was current
// when they were made: forget them whenever that context goes away (switching
// between software and OpenGL creates a new one), otherwise their names may
// point at nothing - or at melonDS' own objects in the new context.
static GLuint linkCapReadFb = 0, linkCapDrawFb = 0, linkCapRb = 0;
// the read back goes into pixel buffers and is picked up when the GPU is done (a fence),
// later in the frame or the next one - glReadPixels straight to memory waited for the GPU
// (the log: up to 22 ms, a frame too late on the PC)
static GLuint linkCapPbo[2] = {0, 0};
static GLsync linkCapFence[2] = {nullptr, nullptr};
static uint64_t linkCapFrame[2] = {0, 0};
static int linkCapNext = 0;   // the slot the next read back goes into
#endif

// melonDS Link: GL objects for spotting unchanged pictures (see linkFrameChanged)
#ifdef OGLRENDERER_ENABLED
static GLuint linkCmpProg = 0, linkCmpVao = 0, linkCmpFb = 0, linkCmpRb = 0, linkCmpPrev = 0;
static GLuint linkCmpQuery[2] = {0, 0}, linkCmpCopyFb[2] = {0, 0};
static GLint linkCmpW = 0, linkCmpH = 0;
static bool linkCmpFailed = false;
#endif
static std::vector<uint32_t> linkPrevSoft[2];

// contextAlive: the context is still current (renderer change) -> delete them
static void linkCaptureReset(bool contextAlive)
{
#ifdef OGLRENDERER_ENABLED
    if (contextAlive && linkCapReadFb)
    {
        glDeleteFramebuffers(1, &linkCapReadFb);
        glDeleteFramebuffers(1, &linkCapDrawFb);
        glDeleteRenderbuffers(1, &linkCapRb);
        glDeleteBuffers(2, linkCapPbo);
        for (GLsync& f : linkCapFence) if (f) glDeleteSync(f);
    }
    linkCapReadFb = linkCapDrawFb = linkCapRb = 0;
    linkCapPbo[0] = linkCapPbo[1] = 0;
    linkCapFence[0] = linkCapFence[1] = nullptr;
    linkCapNext = 0;
    if (contextAlive && linkCmpProg)
    {
        glDeleteProgram(linkCmpProg);
        glDeleteVertexArrays(1, &linkCmpVao);
        glDeleteFramebuffers(1, &linkCmpFb);
        glDeleteFramebuffers(2, linkCmpCopyFb);
        glDeleteRenderbuffers(1, &linkCmpRb);
        glDeleteTextures(1, &linkCmpPrev);
        glDeleteQueries(2, linkCmpQuery);
    }
    linkCmpProg = linkCmpVao = linkCmpFb = linkCmpRb = linkCmpPrev = 0;
    linkCmpQuery[0] = linkCmpQuery[1] = linkCmpCopyFb[0] = linkCmpCopyFb[1] = 0;
    linkCmpW = linkCmpH = 0;
    linkCmpFailed = false;
#endif
    linkPrevSoft[0].clear();
    linkPrevSoft[1].clear();
}

// melonDS Link: did the picture of each screen change since the last frame? (for showing
// only new pictures - frame generation then gets no doubles - and for counting them).
// Software renderer: the two framebuffers compared in RAM. OpenGL: on the GPU at full
// resolution - a pass that drops every pixel equal to the last frame's, an occlusion query
// counts what is left; then the frame is kept for the next time. Unknown: "changed".
static void linkFrameChanged(melonDS::NDS* nds, bool& top, bool& bottom)
{
    top = bottom = true;
    void* fb[2] = {nullptr, nullptr};
    if (nds->GPU.GetFramebuffers(&fb[0], &fb[1]))
    {
        bool* out[2] = {&top, &bottom};
        for (int i = 0; i < 2; i++)
        {
            if (!fb[i]) continue;
            const uint32_t* px = (const uint32_t*)fb[i];
            auto& prev = linkPrevSoft[i];
            *out[i] = prev.size() != 256 * 192 || memcmp(prev.data(), px, 256 * 192 * 4) != 0;
            prev.assign(px, px + 256 * 192);
        }
        return;
    }
#ifdef OGLRENDERER_ENABLED
    if (!fb[0] || linkCmpFailed) return;
    GLuint tex = *(GLuint*)fb[0];

    GLint prevRead = 0, prevDraw = 0, prevProg = 0, prevVao = 0, prevActive = 0, prevTex0 = 0, prevTex1 = 0;
    GLint prevView[4];
    GLboolean prevMask[4];
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevRead);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevDraw);
    glGetIntegerv(GL_CURRENT_PROGRAM, &prevProg);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActive);
    glGetIntegerv(GL_VIEWPORT, prevView);
    glGetBooleanv(GL_COLOR_WRITEMASK, prevMask);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D_ARRAY, &prevTex0);
    glActiveTexture(GL_TEXTURE1);
    glGetIntegerv(GL_TEXTURE_BINDING_2D_ARRAY, &prevTex1);

    GLint w = 0, h = 0;
    glBindTexture(GL_TEXTURE_2D_ARRAY, tex);
    glGetTexLevelParameteriv(GL_TEXTURE_2D_ARRAY, 0, GL_TEXTURE_WIDTH, &w);
    glGetTexLevelParameteriv(GL_TEXTURE_2D_ARRAY, 0, GL_TEXTURE_HEIGHT, &h);

    bool fresh = false;
    if (!linkCmpProg)
    {
        static const char* vs = R"(#version 140
void main()
{
    vec2 p = vec2(float((gl_VertexID & 1) * 4 - 1), float((gl_VertexID & 2) * 2 - 1));
    gl_Position = vec4(p, 0.0, 1.0);
}
)";
        static const char* fs = R"(#version 140
uniform sampler2DArray Cur;
uniform sampler2DArray Prev;
uniform int Layer;
out vec4 oColor;
void main()
{
    ivec3 p = ivec3(ivec2(gl_FragCoord.xy), Layer);
    if (texelFetch(Cur, p, 0) == texelFetch(Prev, p, 0)) discard;
    oColor = vec4(1.0);
}
)";
        if (!melonDS::OpenGL::CompileVertexFragmentProgram(linkCmpProg, vs, fs, "LinkFrameCompare", {}, {{"oColor", 0}}))
        {
            linkCmpProg = 0;
            linkCmpFailed = true;   // no comparing: every frame counts as new
        }
        else
        {
            glGenVertexArrays(1, &linkCmpVao);
            glGenFramebuffers(1, &linkCmpFb);
            glGenFramebuffers(2, linkCmpCopyFb);
            glGenRenderbuffers(1, &linkCmpRb);
            glGenTextures(1, &linkCmpPrev);
            glGenQueries(2, linkCmpQuery);
        }
    }
    if (linkCmpProg && (w != linkCmpW || h != linkCmpH))
    {
        // new size (resolution changed): new buffers, this frame counts as new
        linkCmpW = w;
        linkCmpH = h;
        glBindTexture(GL_TEXTURE_2D_ARRAY, linkCmpPrev);
        glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, w, h, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glBindRenderbuffer(GL_RENDERBUFFER, linkCmpRb);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_R8, w, h);
        glBindRenderbuffer(GL_RENDERBUFFER, 0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, linkCmpFb);
        glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, linkCmpRb);
        fresh = true;
    }

    if (linkCmpProg && w > 0 && h > 0)
    {
        if (!fresh)
        {
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, linkCmpFb);
            glViewport(0, 0, w, h);
            glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
            glUseProgram(linkCmpProg);
            glBindVertexArray(linkCmpVao);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D_ARRAY, tex);
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D_ARRAY, linkCmpPrev);
            glUniform1i(glGetUniformLocation(linkCmpProg, "Cur"), 0);
            glUniform1i(glGetUniformLocation(linkCmpProg, "Prev"), 1);
            GLint layerLoc = glGetUniformLocation(linkCmpProg, "Layer");
            for (int i = 0; i < 2; i++)
            {
                glUniform1i(layerLoc, i);
                glBeginQuery(GL_SAMPLES_PASSED, linkCmpQuery[i]);
                glDrawArrays(GL_TRIANGLES, 0, 3);
                glEndQuery(GL_SAMPLES_PASSED);
            }
            GLuint changed[2] = {1, 1};
            glGetQueryObjectuiv(linkCmpQuery[0], GL_QUERY_RESULT, &changed[0]);
            glGetQueryObjectuiv(linkCmpQuery[1], GL_QUERY_RESULT, &changed[1]);
            top = changed[0] != 0;
            bottom = changed[1] != 0;
        }
        // keep this frame for the next comparison
        for (int i = 0; i < 2; i++)
        {
            glBindFramebuffer(GL_READ_FRAMEBUFFER, linkCmpCopyFb[0]);
            glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, tex, 0, i);
            glReadBuffer(GL_COLOR_ATTACHMENT0);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, linkCmpCopyFb[1]);
            glFramebufferTextureLayer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, linkCmpPrev, 0, i);
            glDrawBuffer(GL_COLOR_ATTACHMENT0);
            glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        }
    }

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D_ARRAY, prevTex1);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D_ARRAY, prevTex0);
    glActiveTexture(prevActive);
    glBindVertexArray(prevVao);
    glUseProgram(prevProg);
    glColorMask(prevMask[0], prevMask[1], prevMask[2], prevMask[3]);
    glViewport(prevView[0], prevView[1], prevView[2], prevView[3]);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, prevRead);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prevDraw);
#endif
}

// melonDS Link: hand over the read backs the GPU has finished, oldest first. wait: also
// those still running (both slots busy)
static void linkCaptureCollect(bool wait)
{
#ifdef OGLRENDERER_ENABLED
    for (int k = 0; k < 2; k++)
    {
        int i = (linkCapNext + k) % 2;   // the older one first
        if (!linkCapFence[i]) continue;
        GLenum r = glClientWaitSync(linkCapFence[i], wait ? GL_SYNC_FLUSH_COMMANDS_BIT : 0,
                                    wait ? 50000000ull : 0);
        if (r == GL_TIMEOUT_EXPIRED) return;   // not yet (and keep the order)
        glDeleteSync(linkCapFence[i]);
        linkCapFence[i] = nullptr;
        if (r == GL_WAIT_FAILED) continue;
        GLint prevPack = 0;
        glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &prevPack);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, linkCapPbo[i]);
        const void* px = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, 256 * 192 * 4, GL_MAP_READ_BIT);
        if (px) Link::Get().SubmitBottomScreen((const uint32_t*)px, false, linkCapFrame[i]);
        glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, prevPack);
    }
#endif
}

// melonDS Link: hand the DS bottom screen (256x192) to the 3DS sender.
// Software renderer: RAM framebuffer. OpenGL renderer: scale the bottom layer
// of the output texture down to 256x192 on the GPU and read it back.
static void linkCaptureBottomScreen(melonDS::NDS* nds, uint64_t emuFrame)
{
    void* top = nullptr;
    void* bottom = nullptr;
    if (nds->GPU.GetFramebuffers(&top, &bottom))
    {
        if (bottom) Link::Get().SubmitBottomScreen((const uint32_t*)bottom, true, emuFrame);
        return;
    }
#ifdef OGLRENDERER_ENABLED
    if (!top) return;
    GLuint tex = *(GLuint*)top;

    static std::vector<uint32_t> pixels(256 * 192);
    GLuint& readFb = linkCapReadFb;
    GLuint& drawFb = linkCapDrawFb;
    GLuint& rb = linkCapRb;
    if (!readFb)
    {
        glGenFramebuffers(1, &readFb);
        glGenFramebuffers(1, &drawFb);
        glGenRenderbuffers(1, &rb);
        glBindRenderbuffer(GL_RENDERBUFFER, rb);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, 256, 192);
        glBindRenderbuffer(GL_RENDERBUFFER, 0);
        GLint prevPack = 0;
        glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &prevPack);
        glGenBuffers(2, linkCapPbo);
        for (GLuint b : linkCapPbo)
        {
            glBindBuffer(GL_PIXEL_PACK_BUFFER, b);
            glBufferData(GL_PIXEL_PACK_BUFFER, 256 * 192 * 4, nullptr, GL_STREAM_READ);
        }
        glBindBuffer(GL_PIXEL_PACK_BUFFER, prevPack);
    }
    // both slots still busy (the GPU is far behind): hand over the older one first
    if (linkCapFence[linkCapNext]) linkCaptureCollect(true);

    GLint prevRead = 0, prevDraw = 0, prevTex = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevRead);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevDraw);
    glGetIntegerv(GL_TEXTURE_BINDING_2D_ARRAY, &prevTex);

    GLint w = 256, h = 192;
    glBindTexture(GL_TEXTURE_2D_ARRAY, tex);
    glGetTexLevelParameteriv(GL_TEXTURE_2D_ARRAY, 0, GL_TEXTURE_WIDTH, &w);
    glGetTexLevelParameteriv(GL_TEXTURE_2D_ARRAY, 0, GL_TEXTURE_HEIGHT, &h);
    glBindTexture(GL_TEXTURE_2D_ARRAY, prevTex);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, readFb);
    glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, tex, 0, 1); // layer 1 = bottom
    glReadBuffer(GL_COLOR_ATTACHMENT0);

    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, drawFb);
    glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
    glDrawBuffer(GL_COLOR_ATTACHMENT0);

    if (w < 256 || h < 192 ||
        glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE ||
        glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
    {
        // not ready (e.g. right after a renderer switch): skip this frame
        glBindFramebuffer(GL_READ_FRAMEBUFFER, prevRead);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prevDraw);
        return;
    }
    glBlitFramebuffer(0, 0, w, h, 0, 0, 256, 192, GL_COLOR_BUFFER_BIT,
                      (w == 256 && h == 192) ? GL_NEAREST : GL_LINEAR);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, drawFb);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    {
        // into a pixel buffer: returns at once, the fence says when the GPU is done
        int i = linkCapNext;
        GLint prevPack = 0;
        glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &prevPack);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, linkCapPbo[i]);
        glReadPixels(0, 0, 256, 192, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, prevPack);
        linkCapFence[i] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        linkCapFrame[i] = emuFrame;
        linkCapNext = (i + 1) % 2;
        glFlush();   // so the GPU starts on it now
    }
    (void)pixels;

    glBindFramebuffer(GL_READ_FRAMEBUFFER, prevRead);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prevDraw);
    // texture row 0 is DS line 0 (same as the software path), so no flip
#endif
}

// melonDS Link: screenshot asked for on the 3DS - both DS screens at the
// resolution melonDS renders them (top above bottom), saved as PNG
static void linkScreenshot(melonDS::NDS* nds, int mode)
{
    QImage shot;
    void* top = nullptr;
    void* bottom = nullptr;
    if (nds->GPU.GetFramebuffers(&top, &bottom))
    {
        if (!top || !bottom) return;
        shot = QImage(256, 384, QImage::Format_RGB32);
        memcpy(shot.bits(), top, 256 * 192 * 4);
        memcpy(shot.bits() + 256 * 192 * 4, bottom, 256 * 192 * 4);
    }
#ifdef OGLRENDERER_ENABLED
    else if (top)
    {
        GLuint tex = *(GLuint*)top;
        GLint prevRead = 0, prevTex = 0;
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevRead);
        glGetIntegerv(GL_TEXTURE_BINDING_2D_ARRAY, &prevTex);
        GLint w = 0, h = 0;
        glBindTexture(GL_TEXTURE_2D_ARRAY, tex);
        glGetTexLevelParameteriv(GL_TEXTURE_2D_ARRAY, 0, GL_TEXTURE_WIDTH, &w);
        glGetTexLevelParameteriv(GL_TEXTURE_2D_ARRAY, 0, GL_TEXTURE_HEIGHT, &h);
        glBindTexture(GL_TEXTURE_2D_ARRAY, prevTex);
        if (w < 256 || h < 192 || w > 8192 || h > 8192) return;

        GLuint fb = 0;
        glGenFramebuffers(1, &fb);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fb);
        shot = QImage(w, h * 2, QImage::Format_RGBX8888);
        bool ok = true;
        for (int layer = 0; layer < 2 && ok; layer++)
        {
            glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, tex, 0, layer);
            glReadBuffer(GL_COLOR_ATTACHMENT0);
            if (glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { ok = false; break; }
            glPixelStorei(GL_PACK_ALIGNMENT, 4);
            glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, shot.scanLine(layer * h));
        }
        glBindFramebuffer(GL_READ_FRAMEBUFFER, prevRead);
        glDeleteFramebuffers(1, &fb);
        if (!ok) shot = QImage();
    }
#endif

    std::string game = Link::Session::Get().ActiveGame();
    std::thread([shot, game, mode]
    {
        // encoding a PNG takes a moment: not on the emulator thread
        QString dir = QString::fromStdString(Link::Session::ScreenshotDir());
        QDir().mkpath(dir);
        QString base = QString::fromStdString(game.empty() ? "melonDS" : game) + " " +
                       QDateTime::currentDateTime().toString("yyyy-MM-dd_HH-mm-ss");
        // the image holds the top screen above the bottom one
        const int h = shot.height() / 2;
        bool ok = !shot.isNull();
        QString name = base + ".png";
        if (ok && mode == LINK_SHOT_TOP) ok = shot.copy(0, 0, shot.width(), h).save(dir + "/" + name, "PNG");
        else if (ok && mode == LINK_SHOT_BOTTOM) ok = shot.copy(0, h, shot.width(), h).save(dir + "/" + name, "PNG");
        else if (ok && mode == LINK_SHOT_SEPARATE)
        {
            ok = shot.copy(0, 0, shot.width(), h).save(dir + "/" + base + " top.png", "PNG") &&
                 shot.copy(0, h, shot.width(), h).save(dir + "/" + base + " bottom.png", "PNG");
            name = base + " top/bottom.png";
        }
        else if (ok) ok = shot.save(dir + "/" + name, "PNG");
        if (ok)
        {
            Link::Get().PostMessage(Link::Tr("shot.saved").arg(name).toStdString(), Link::Note::Screenshot);
            Link::Get().SetStatusText(Link::Get().Tr3DS("shot.saved3ds"));
        }
        else
            Link::Get().PostMessage(Link::TrS("shot.failed"));
    }).detach();
}

EmuThread::EmuThread(EmuInstance* inst, QObject* parent) : QThread(parent)
{
    emuInstance = inst;

    emuStatus = emuStatus_Paused;
    emuPauseStack = emuPauseStackRunning;
    emuActive = false;
}

void EmuThread::attachWindow(MainWindow* window)
{
    connect(this, SIGNAL(windowTitleChange(QString)), window, SLOT(onTitleUpdate(QString)));
    connect(this, SIGNAL(windowEmuStart()), window, SLOT(onEmuStart()));
    connect(this, SIGNAL(windowEmuStop()), window, SLOT(onEmuStop()));
    connect(this, SIGNAL(windowEmuPause(bool)), window, SLOT(onEmuPause(bool)));
    connect(this, SIGNAL(windowEmuReset()), window, SLOT(onEmuReset()));
    connect(this, SIGNAL(autoScreenSizingChange(int)), window->panel, SLOT(onAutoScreenSizingChanged(int)));
    connect(this, SIGNAL(windowFullscreenToggle()), window, SLOT(onFullscreenToggled()));
    connect(this, SIGNAL(screenEmphasisToggle()), window, SLOT(onScreenEmphasisToggled()));

    if (window->winHasMenu())
    {
        connect(this, SIGNAL(windowLimitFPSChange()), window->actLimitFramerate, SLOT(trigger()));
        connect(this, SIGNAL(swapScreensToggle()), window->actScreenSwap, SLOT(trigger()));
    }
}

void EmuThread::detachWindow(MainWindow* window)
{
    disconnect(this, SIGNAL(windowTitleChange(QString)), window, SLOT(onTitleUpdate(QString)));
    disconnect(this, SIGNAL(windowEmuStart()), window, SLOT(onEmuStart()));
    disconnect(this, SIGNAL(windowEmuStop()), window, SLOT(onEmuStop()));
    disconnect(this, SIGNAL(windowEmuPause(bool)), window, SLOT(onEmuPause(bool)));
    disconnect(this, SIGNAL(windowEmuReset()), window, SLOT(onEmuReset()));
    disconnect(this, SIGNAL(autoScreenSizingChange(int)), window->panel, SLOT(onAutoScreenSizingChanged(int)));
    disconnect(this, SIGNAL(windowFullscreenToggle()), window, SLOT(onFullscreenToggled()));
    disconnect(this, SIGNAL(screenEmphasisToggle()), window, SLOT(onScreenEmphasisToggled()));

    if (window->winHasMenu())
    {
        disconnect(this, SIGNAL(windowLimitFPSChange()), window->actLimitFramerate, SLOT(trigger()));
        disconnect(this, SIGNAL(swapScreensToggle()), window->actScreenSwap, SLOT(trigger()));
    }
}

void EmuThread::run()
{
    Config::Table& globalCfg = emuInstance->getGlobalConfig();
    u32 mainScreenPos[3];

    //emuInstance->updateConsole();
    // No carts are inserted when melonDS first boots

    mainScreenPos[0] = 0;
    mainScreenPos[1] = 0;
    mainScreenPos[2] = 0;
    autoScreenSizing = 0;

    //videoSettingsDirty = false;

    if (emuInstance->usesOpenGL())
    {
        emuInstance->initOpenGL(0);

        useOpenGL = true;
        videoRenderer = globalCfg.GetInt("3D.Renderer");
    }
    else
    {
        useOpenGL = false;
        videoRenderer = 0;
    }

    //updateRenderer();
    videoSettingsDirty = true;

    u32 nframes = 0;
    double perfCountsSec = 1.0 / SDL_GetPerformanceFrequency();
    double lastTime = SDL_GetPerformanceCounter() * perfCountsSec;
    double frameLimitError = 0.0;
    double lastMeasureTime = lastTime;

    u32 winUpdateCount = 0, winUpdateFreq = 1;
    u8 dsiVolumeLevel = 0x1F;

    char melontitle[100];

    // melonDS Link: how long each part of a frame takes (for the log, see LinkPacing.h)
    auto linkMs = [perfCountsSec] { return SDL_GetPerformanceCounter() * perfCountsSec * 1000.0; };
    uint64_t linkEmuFrame = 0;     // emulated frames (time stamps of the 3DS's pictures)
    double linkSettingsAt = 0;

    bool fastforward = false;
    bool slowmo = false;
    emuInstance->fastForwardToggled = false;
    emuInstance->slowmoToggled = false;

    double linkPrevMsgMs = 0;      // handleMessages at the end of the last loop
    double linkLastPresent = 0;
    while (emuStatus != emuStatus_Exit)
    {
        const double linkLoopStart = linkMs();
        if (emuInstance->instanceID == 0)
            MPInterface::Get().Process();
        const double linkMpMs = linkMs() - linkLoopStart;

        double linkIn = linkMs();
        emuInstance->inputProcess();
        const double linkInputMs = linkMs() - linkIn;

        if (emuInstance->instanceID == 0)
        {
            std::string linkMsg;
            while (Link::Get().PopMessage(linkMsg))
                emuInstance->osdAddMessage(0, "%s", linkMsg.c_str());
        }

        if (emuInstance->hotkeyPressed(HK_FrameLimitToggle)) emit windowLimitFPSChange();

        if (emuInstance->hotkeyPressed(HK_Pause)) emuTogglePause();
        if (emuInstance->hotkeyPressed(HK_Reset)) emuReset();
        if (emuInstance->hotkeyPressed(HK_FrameStep)) emuFrameStep();

        if (emuInstance->hotkeyPressed(HK_FullscreenToggle)) emit windowFullscreenToggle();

        if (emuInstance->hotkeyPressed(HK_SwapScreens)) emit swapScreensToggle();
        if (emuInstance->hotkeyPressed(HK_SwapScreenEmphasis)) emit screenEmphasisToggle();

        if (emuStatus == emuStatus_Running || emuStatus == emuStatus_FrameStep)
        {
            if (emuStatus == emuStatus_FrameStep) emuStatus = emuStatus_Paused;
            Link::Pacing::FrameTimes linkTimes;
            const double linkStart = linkMs();
            linkTimes.input = linkInputMs + linkMpMs;
            linkTimes.messages = linkPrevMsgMs;

            if (emuInstance->hotkeyPressed(HK_SolarSensorDecrease))
            {
                int level = emuInstance->nds->GBACartSlot.SetInput(GBACart::Input_SolarSensorDown, true);
                if (level != -1)
                {
                    emuInstance->osdAddMessage(0, "Solar sensor level: %d", level);
                }
            }
            if (emuInstance->hotkeyPressed(HK_SolarSensorIncrease))
            {
                int level = emuInstance->nds->GBACartSlot.SetInput(GBACart::Input_SolarSensorUp, true);
                if (level != -1)
                {
                    emuInstance->osdAddMessage(0, "Solar sensor level: %d", level);
                }
            }

            if (emuInstance->nds->ConsoleType == 1)
            {
                DSi* dsi = static_cast<DSi*>(emuInstance->nds);
                double currentTime = SDL_GetPerformanceCounter() * perfCountsSec;

                // Handle power button
                if (emuInstance->hotkeyDown(HK_PowerButton))
                {
                    dsi->I2C.GetBPTWL()->SetPowerButtonHeld(currentTime);
                }
                else if (emuInstance->hotkeyReleased(HK_PowerButton))
                {
                    dsi->I2C.GetBPTWL()->SetPowerButtonReleased(currentTime);
                }

                // Handle volume buttons
                if (emuInstance->hotkeyDown(HK_VolumeUp))
                {
                    dsi->I2C.GetBPTWL()->SetVolumeSwitchHeld(DSi_BPTWL::volumeKey_Up);
                }
                else if (emuInstance->hotkeyReleased(HK_VolumeUp))
                {
                    dsi->I2C.GetBPTWL()->SetVolumeSwitchReleased(DSi_BPTWL::volumeKey_Up);
                }

                if (emuInstance->hotkeyDown(HK_VolumeDown))
                {
                    dsi->I2C.GetBPTWL()->SetVolumeSwitchHeld(DSi_BPTWL::volumeKey_Down);
                }
                else if (emuInstance->hotkeyReleased(HK_VolumeDown))
                {
                    dsi->I2C.GetBPTWL()->SetVolumeSwitchReleased(DSi_BPTWL::volumeKey_Down);
                }

                dsi->I2C.GetBPTWL()->ProcessVolumeSwitchInput(currentTime);
            }

            {
                double t = linkMs();
                if (useOpenGL)
                    emuInstance->makeCurrentGL();
                linkTimes.glCurrent = linkMs() - t;
            }

            // update render settings if needed
            if (videoSettingsDirty)
            {
                emuInstance->renderLock.lock();
                if (useOpenGL)
                {
                    emuInstance->setVSyncGL(true);
                    videoRenderer = globalCfg.GetInt("3D.Renderer");
                }
#ifdef OGLRENDERER_ENABLED
                else
#endif
                {
                    videoRenderer = 0;
                }

                updateRenderer();

                videoSettingsDirty = false;
                emuInstance->renderLock.unlock();
            }

            // process input and hotkeys
            emuInstance->nds->SetKeyMask(emuInstance->inputMask);

            if (emuInstance->linkTouching)
                emuInstance->nds->TouchScreen(emuInstance->linkTouchX, emuInstance->linkTouchY);
            else if (emuInstance->isTouching)
                emuInstance->nds->TouchScreen(emuInstance->touchX, emuInstance->touchY);
            else
                emuInstance->nds->ReleaseScreen();

            if (emuInstance->hotkeyPressed(HK_Lid))
            {
                bool lid = !emuInstance->nds->IsLidClosed();
                emuInstance->nds->SetLidClosed(lid);
                emuInstance->osdAddMessage(0, lid ? "Lid closed" : "Lid opened");
            }

            // auto screen layout
            {
                mainScreenPos[2] = mainScreenPos[1];
                mainScreenPos[1] = mainScreenPos[0];
                mainScreenPos[0] = emuInstance->nds->PowerControl9 >> 15;

                int guess;
                if (mainScreenPos[0] == mainScreenPos[2] &&
                    mainScreenPos[0] != mainScreenPos[1])
                {
                    // constant flickering, likely displaying 3D on both screens
                    // TODO: when both screens are used for 2D only...???
                    guess = screenSizing_Even;
                }
                else
                {
                    if (mainScreenPos[0] == 1)
                        guess = screenSizing_EmphTop;
                    else
                        guess = screenSizing_EmphBot;
                }

                if (guess != autoScreenSizing)
                {
                    autoScreenSizing = guess;
                    emit autoScreenSizingChange(autoScreenSizing);
                }
            }

            // RTC sync
            emuInstance->syncRTC();


            // emulate
            u32 nlines;
            double linkT = linkMs();
            if (emuInstance->nds->GPU.GetRenderer().NeedsShaderCompile())
            {
                compileShaders();
                nlines = 1;
            }
            else
            {
                nlines = emuInstance->nds->RunFrame();
            }
            linkEmuFrame++;
            linkTimes.emu = linkMs() - linkT;
            linkTimes.lines = nlines;
            linkT = linkMs();

            int shotMode;
            if (emuInstance->instanceID == 0 && Link::Get().TakeScreenshotRequest(shotMode))
                linkScreenshot(emuInstance->nds, shotMode);

            // melonDS Link: send the bottom screen to the 3DS - every frame (the sender picks
            // the steady rate by when they were made); fast forward: at most ~120 a second
            if (emuInstance->instanceID == 0 && Link::Get().WantsVideo())
            {
                static auto lastLinkFrame = std::chrono::steady_clock::time_point();
                auto now = std::chrono::steady_clock::now();
                if (now - lastLinkFrame >= std::chrono::microseconds(8000))
                {
                    lastLinkFrame = now;
                    linkCaptureBottomScreen(emuInstance->nds, linkEmuFrame);
                }
            }
            linkTimes.capture = linkMs() - linkT;

            // melonDS Link: which screens show a new picture; with the setting, an unchanged
            // frame is not shown again (frame generation then gets no doubles) - at least
            // every 0.5 s one goes out anyway (messages on the screen, a resized window)
            bool linkPresent = true;
            if (emuInstance->instanceID == 0 &&
                (Link::Pacing::SkipDuplicates() || Link::Get().GetState() == Link::State::Connected))
            {
                linkT = linkMs();
                bool newTop = true, newBottom = true;
                linkFrameChanged(emuInstance->nds, newTop, newBottom);
                linkTimes.compare = linkMs() - linkT;
                linkPresent = !Link::Pacing::SkipDuplicates() || newTop || newBottom ||
                              linkT - linkLastPresent >= 500;
                Link::Pacing::Pictures(newTop, newBottom, linkPresent);
            }

            linkT = linkMs();
            if (emuInstance->ndsSave)
                emuInstance->ndsSave->CheckFlush();

            if (emuInstance->gbaSave)
                emuInstance->gbaSave->CheckFlush();

            if (emuInstance->firmwareSave)
                emuInstance->firmwareSave->CheckFlush();
            linkTimes.saves = linkMs() - linkT;

            linkT = linkMs();
            if (linkPresent)
            {
                emuInstance->drawScreen();
                linkLastPresent = linkT;
            }
            linkTimes.draw = linkMs() - linkT;
            if (useOpenGL && emuInstance->instanceID == 0) linkCaptureCollect(false);   // ready by now, usually

#ifdef MELONCAP
            MelonCap::Update();
#endif // MELONCAP

            winUpdateCount++;
            if (winUpdateCount >= winUpdateFreq && !useOpenGL && linkPresent)
            {
                emit windowUpdate();
                winUpdateCount = 0;
            }
            
            if (emuInstance->hotkeyPressed(HK_FastForwardToggle)) emuInstance->fastForwardToggled = !emuInstance->fastForwardToggled;
            if (emuInstance->hotkeyPressed(HK_SlowMoToggle)) emuInstance->slowmoToggled = !emuInstance->slowmoToggled;

            if (emuInstance->hotkeyPressed(HK_AudioMuteToggle)) emuInstance->toggleAudioMute();

            bool enablefastforward = emuInstance->hotkeyDown(HK_FastForward) | emuInstance->fastForwardToggled;
            bool enableslowmo = emuInstance->hotkeyDown(HK_SlowMo) | emuInstance->slowmoToggled;

            if (useOpenGL)
            {
                // when using OpenGL: when toggling fast-forward or slowmo, change the vsync interval
                if ((enablefastforward || enableslowmo) && !(fastforward || slowmo))
                {
                    emuInstance->setVSyncGL(false);
                }
                else if (!(enablefastforward || enableslowmo) && (fastforward || slowmo))
                {
                    emuInstance->setVSyncGL(true);
                }
            }

            fastforward = enablefastforward;
            slowmo = enableslowmo;
            emuInstance->updateFastForwardMute(fastforward);

            if (slowmo) emuInstance->curFPS = emuInstance->slowmoFPS;
            else if (fastforward) emuInstance->curFPS = emuInstance->fastForwardFPS;
            else if (!emuInstance->doLimitFPS && !emuInstance->doAudioSync) emuInstance->curFPS = 1000.0;
            else emuInstance->curFPS = emuInstance->targetFPS;

            if (emuInstance->audioDSiVolumeSync && emuInstance->nds->ConsoleType == 1)
            {
                DSi* dsi = static_cast<DSi*>(emuInstance->nds);
                u8 volumeLevel = dsi->I2C.GetBPTWL()->GetVolumeLevel();
                if (volumeLevel != dsiVolumeLevel)
                {
                    dsiVolumeLevel = volumeLevel;
                    emit syncVolumeLevel();
                }

                emuInstance->audioVolume = volumeLevel * (256.0 / 31.0);
            }

            linkT = linkMs();
            if (emuInstance->doAudioSync && !(fastforward || slowmo))
                emuInstance->audioSync();
            linkTimes.audio = linkMs() - linkT;

            double frametimeStep = nlines / (emuInstance->curFPS * 263.0);

            if (frametimeStep < 0.001) frametimeStep = 0.001;

            linkT = linkMs();
            if (emuInstance->doLimitFPS)
            {
                double curtime = SDL_GetPerformanceCounter() * perfCountsSec;

                frameLimitError += frametimeStep - (curtime - lastTime);
                if (frameLimitError < -frametimeStep)
                    frameLimitError = -frametimeStep;
                if (frameLimitError > frametimeStep)
                    frameLimitError = frametimeStep;

                if (round(frameLimitError * 1000.0) > 0.0)
                {
                    SDL_Delay(round(frameLimitError * 1000.0));
                    double timeBeforeSleep = curtime;
                    curtime = SDL_GetPerformanceCounter() * perfCountsSec;
                    frameLimitError -= curtime - timeBeforeSleep;
                }

                lastTime = curtime;
            }
            linkTimes.limiter = linkMs() - linkT;
            if (useOpenGL && emuInstance->instanceID == 0) linkCaptureCollect(false);

            nframes++;
            if (nframes >= 30)
            {
                double time = SDL_GetPerformanceCounter() * perfCountsSec;
                double dt = time - lastMeasureTime;
                lastMeasureTime = time;

                u32 fps = round(nframes / dt);
                nframes = 0;

                float fpstarget = 1.0/frametimeStep;

                winUpdateFreq = fps / (u32)round(fpstarget);
                if (winUpdateFreq < 1)
                    winUpdateFreq = 1;
                    
                double actualfps = (59.8261 * 263.0) / nlines;
                snprintf(melontitle, sizeof(melontitle), "[%d/%.0f] melonDS " MELONDS_VERSION, fps, actualfps);
                changeWindowTitle(melontitle);
            }

            if (emuInstance->instanceID == 0)
            {
                linkTimes.fast = fastforward || slowmo;
                linkTimes.total = linkMs() - linkStart + (linkStart - linkLoopStart) + linkPrevMsgMs;
                Link::Pacing::Frame(linkTimes);
                // the settings that matter for the timing (logged when they change)
                if (linkStart - linkSettingsAt > 2000)
                {
                    linkSettingsAt = linkStart;
                    static const char* renderers[] = {"software", "OpenGL", "OpenGL compute"};
                    int r = globalCfg.GetInt("3D.Renderer");
                    char line[600];
                    snprintf(line, sizeof(line),
                             "renderer %s, %dx resolution, threaded 3D %d, OpenGL display %d, VSync %d "
                             "(interval %d), limit framerate %d, audio sync %d, target %.2f fps, JIT %d, "
                             "console %s, audio interpolation %d",
                             r >= 0 && r < 3 ? renderers[r] : "?", globalCfg.GetInt("3D.GL.ScaleFactor"),
                             globalCfg.GetBool("3D.Soft.Threaded"), globalCfg.GetBool("Screen.UseGL"),
                             globalCfg.GetBool("Screen.VSync"), globalCfg.GetInt("Screen.VSyncInterval"),
                             emuInstance->doLimitFPS, emuInstance->doAudioSync, emuInstance->targetFPS,
                             globalCfg.GetBool("JIT.Enable"),
                             emuInstance->nds->ConsoleType == 1 ? "DSi" : "DS",
                             globalCfg.GetInt("Audio.Interpolation"));
                    Link::Pacing::Settings(line);
                }
            }
        }
        else
        {
            // paused
            nframes = 0;
            if (emuInstance->instanceID == 0) Link::Pacing::Paused();
            lastTime = SDL_GetPerformanceCounter() * perfCountsSec;
            lastMeasureTime = lastTime;

            emit windowUpdate();

            snprintf(melontitle, sizeof(melontitle), "melonDS " MELONDS_VERSION);
            changeWindowTitle(melontitle);

            SDL_Delay(75);

            emuInstance->drawScreen();
        }

        double linkMsgStart = linkMs();
        handleMessages();
        linkPrevMsgMs = linkMs() - linkMsgStart;
    }
}

void EmuThread::sendMessage(Message msg)
{
    msgMutex.lock();
    msgQueue.enqueue(msg);
    msgMutex.unlock();
}

void EmuThread::waitMessage(int num)
{
    if (QThread::currentThread() == this) return;
    msgSemaphore.acquire(num);
}

void EmuThread::waitAllMessages()
{
    if (QThread::currentThread() == this) return;
    while (!msgQueue.empty())
        msgSemaphore.acquire();
}

void EmuThread::handleMessages()
{
    bool glborrow = false;

    msgMutex.lock();
    while (!msgQueue.empty())
    {
        Message msg = msgQueue.dequeue();
        switch (msg.type)
        {
        case msg_Exit:
            emuStatus = emuStatus_Exit;
            emuPauseStack = emuPauseStackRunning;

            emuInstance->audioDisable();
            MPInterface::Get().End(emuInstance->instanceID);
            break;

        case msg_EmuRun:
            emuStatus = emuStatus_Running;
            emuPauseStack = emuPauseStackRunning;
            emuActive = true;

            emuInstance->audioEnable();
            emit windowEmuStart();
            break;

        case msg_EmuPause:
            emuPauseStack++;
            if (emuPauseStack > emuPauseStackPauseThreshold) break;

            prevEmuStatus = emuStatus;
            emuStatus = emuStatus_Paused;

            if (prevEmuStatus != emuStatus_Paused)
            {
                emuInstance->audioDisable();
                emit windowEmuPause(true);
                emuInstance->osdAddMessage(0, "Paused");
            }
            break;

        case msg_EmuUnpause:
            if (emuPauseStack < emuPauseStackPauseThreshold) break;

            emuPauseStack--;
            if (emuPauseStack >= emuPauseStackPauseThreshold) break;

            emuStatus = prevEmuStatus;

            if (emuStatus != emuStatus_Paused)
            {
                emuInstance->audioEnable();
                emit windowEmuPause(false);
                emuInstance->osdAddMessage(0, "Resumed");
            }
            break;

        case msg_EmuStop:
            if (msg.param.value<bool>())
                emuInstance->nds->Stop();
            emuStatus = emuStatus_Paused;
            emuActive = false;

            emuInstance->audioDisable();
            emit windowEmuStop();
            break;

        case msg_EmuFrameStep:
            emuStatus = emuStatus_FrameStep;
            break;

        case msg_EmuReset:
            emuInstance->reset();

            emuStatus = emuStatus_Running;
            emuPauseStack = emuPauseStackRunning;
            emuActive = true;

            emuInstance->audioEnable();
            emit windowEmuReset();
            emuInstance->osdAddMessage(0, "Reset");
            break;

        case msg_InitGL:
            if (msg.param.value<int>() == 0) linkCaptureReset(false);
            emuInstance->initOpenGL(msg.param.value<int>());
            useOpenGL = true;
            break;

        case msg_DeInitGL:
            if (msg.param.value<int>() == 0) linkCaptureReset(false);
            emuInstance->deinitOpenGL(msg.param.value<int>());
            if (msg.param.value<int>() == 0)
                useOpenGL = false;
            break;

        case msg_BorrowGL:
            emuInstance->releaseGL();
            glborrow = true;
            break;

        case msg_BootROM:
            msgResult = 0;
            if (!emuInstance->loadROM(msg.param.value<QStringList>(), true, msgError))
                break;

            assert(emuInstance->nds != nullptr);
            emuInstance->nds->Start();
            msgResult = 1;
            break;

        case msg_BootFirmware:
            msgResult = 0;
            if (!emuInstance->bootToMenu(msgError))
                break;

            assert(emuInstance->nds != nullptr);
            emuInstance->nds->Start();
            msgResult = 1;
            break;

        case msg_InsertCart:
            msgResult = 0;
            if (!emuInstance->loadROM(msg.param.value<QStringList>(), false, msgError))
                break;

            msgResult = 1;
            break;

        case msg_EjectCart:
            emuInstance->ejectCart();
            break;

        case msg_InsertGBACart:
            msgResult = 0;
            if (!emuInstance->loadGBAROM(msg.param.value<QStringList>(), msgError))
                break;

            msgResult = 1;
            break;

        case msg_InsertGBAAddon:
            msgResult = 0;
            emuInstance->loadGBAAddon(msg.param.value<int>(), msgError);
            msgResult = 1;
            break;

        case msg_EjectGBACart:
            emuInstance->ejectGBACart();
            break;

        case msg_SaveState:
            msgResult = emuInstance->saveState(msg.param.value<QString>().toStdString());
            break;

        case msg_LoadState:
            msgResult = emuInstance->loadState(msg.param.value<QString>().toStdString());
            break;

        case msg_UndoStateLoad:
            emuInstance->undoStateLoad();
            msgResult = 1;
            break;

        case msg_ImportSavefile:
            {
                msgResult = 0;
                auto f = Platform::OpenFile(msg.param.value<QString>().toStdString(), Platform::FileMode::Read);
                if (!f) break;

                u32 len = FileLength(f);

                std::unique_ptr<u8[]> data = std::make_unique<u8[]>(len);
                Platform::FileRewind(f);
                Platform::FileRead(data.get(), len, 1, f);

                assert(emuInstance->nds != nullptr);
                emuInstance->nds->SetNDSSave(data.get(), len);

                CloseFile(f);
                msgResult = 1;
            }
            break;

        case msg_EnableCheats:
            emuInstance->enableCheats(msg.param.value<bool>());
            break;

        case msg_LinkShiftClock:
            emuInstance->linkShiftClock(msg.param.value<qint64>());
            break;
        }

        msgSemaphore.release();
    }
    msgMutex.unlock();

    if (glborrow)
    {
        glBorrowMutex.lock();
        glBorrowCond.wait(&glBorrowMutex);
        glBorrowMutex.unlock();
    }
}

void EmuThread::changeWindowTitle(char* title)
{
    emit windowTitleChange(QString(title));
}

void EmuThread::initContext(int win)
{
    sendMessage({.type = msg_InitGL, .param = win});
    waitMessage();
}

void EmuThread::deinitContext(int win)
{
    sendMessage({.type = msg_DeInitGL, .param = win});
    waitMessage();
}

void EmuThread::borrowGL()
{
    sendMessage(msg_BorrowGL);
    waitMessage();
}

void EmuThread::returnGL()
{
    glBorrowMutex.lock();
    glBorrowCond.wakeAll();
    glBorrowMutex.unlock();
}

void EmuThread::emuRun()
{
    sendMessage(msg_EmuRun);
    waitMessage();
}

void EmuThread::emuPause(bool broadcast)
{
    sendMessage(msg_EmuPause);
    waitMessage();

    if (broadcast)
        emuInstance->broadcastCommand(InstCmd_Pause);
}

void EmuThread::emuUnpause(bool broadcast)
{
    sendMessage(msg_EmuUnpause);
    waitMessage();

    if (broadcast)
        emuInstance->broadcastCommand(InstCmd_Unpause);
}

void EmuThread::emuTogglePause(bool broadcast)
{
    if (emuStatus == emuStatus_Paused)
        emuUnpause(broadcast);
    else
        emuPause(broadcast);
}

void EmuThread::emuStop(bool external)
{
    sendMessage({.type = msg_EmuStop, .param = external});
    waitMessage();
}

void EmuThread::emuExit()
{
    sendMessage(msg_Exit);
    waitAllMessages();
}

void EmuThread::emuFrameStep()
{
    if (emuPauseStack < emuPauseStackPauseThreshold)
        sendMessage(msg_EmuPause);
    sendMessage(msg_EmuFrameStep);
    waitAllMessages();
}

void EmuThread::emuReset()
{
    sendMessage(msg_EmuReset);
    waitMessage();
}

bool EmuThread::emuIsRunning()
{
    return emuStatus == emuStatus_Running;
}

bool EmuThread::emuIsActive()
{
    return emuActive;
}

int EmuThread::bootROM(const QStringList& filename, QString& errorstr)
{
    sendMessage({.type = msg_BootROM, .param = filename});
    waitMessage();
    if (!msgResult)
    {
        errorstr = msgError;
        return msgResult;
    }

    sendMessage(msg_EmuRun);
    waitMessage();
    errorstr = "";
    return msgResult;
}

int EmuThread::bootFirmware(QString& errorstr)
{
    sendMessage(msg_BootFirmware);
    waitMessage();
    if (!msgResult)
    {
        errorstr = msgError;
        return msgResult;
    }

    sendMessage(msg_EmuRun);
    waitMessage();
    errorstr = "";
    return msgResult;
}

int EmuThread::insertCart(const QStringList& filename, bool gba, QString& errorstr)
{
    MessageType msgtype = gba ? msg_InsertGBACart : msg_InsertCart;

    sendMessage({.type = msgtype, .param = filename});
    waitMessage();
    errorstr = msgResult ? "" : msgError;
    return msgResult;
}

void EmuThread::ejectCart(bool gba)
{
    sendMessage(gba ? msg_EjectGBACart : msg_EjectCart);
    waitMessage();
}

int EmuThread::insertGBAAddon(int type, QString& errorstr)
{
    sendMessage({.type = msg_InsertGBAAddon, .param = type});
    waitMessage();
    errorstr = msgResult ? "" : msgError;
    return msgResult;
}

int EmuThread::saveState(const QString& filename)
{
    sendMessage({.type = msg_SaveState, .param = filename});
    waitMessage();
    return msgResult;
}

int EmuThread::loadState(const QString& filename)
{
    sendMessage({.type = msg_LoadState, .param = filename});
    waitMessage();
    return msgResult;
}

int EmuThread::undoStateLoad()
{
    sendMessage(msg_UndoStateLoad);
    waitMessage();
    return msgResult;
}

int EmuThread::importSavefile(const QString& filename)
{
    sendMessage(msg_EmuReset);
    sendMessage({.type = msg_ImportSavefile, .param = filename});
    waitMessage(2);
    return msgResult;
}

void EmuThread::linkShiftClock(qint64 seconds)
{
    sendMessage({.type = msg_LinkShiftClock, .param = seconds});
    waitMessage();
}

void EmuThread::enableCheats(bool enable)
{
    sendMessage({.type = msg_EnableCheats, .param = enable});
    waitMessage();
}

void EmuThread::updateRenderer()
{
    auto nds = emuInstance->nds;

    if (videoRenderer != lastVideoRenderer)
    {
        // same context, but start the capture from scratch with the new renderer
        if (useOpenGL) linkCaptureReset(true);
        switch (videoRenderer)
        {
            case renderer3D_Software:
                nds->SetRenderer(std::make_unique<SoftRenderer>(*nds));
                break;
            case renderer3D_OpenGL:
                nds->SetRenderer(std::make_unique<GLRenderer>(*nds, false));
                break;
            case renderer3D_OpenGLCompute:
                nds->SetRenderer(std::make_unique<GLRenderer>(*nds, true));
                break;
            default: __builtin_unreachable();
        }
    }
    lastVideoRenderer = videoRenderer;

    auto& cfg = emuInstance->getGlobalConfig();
    melonDS::RendererSettings settings = {
        .ScaleFactor = cfg.GetInt("3D.GL.ScaleFactor"),
        .Threaded = cfg.GetBool("3D.Soft.Threaded"),
        .HiresCoordinates = cfg.GetBool("3D.GL.HiresCoordinates"),
        .BetterPolygons = cfg.GetBool("3D.GL.BetterPolygons"),
        .TextureFilter = cfg.GetInt("3D.GL.TextureFilter"),
        .TextureFilterScale = cfg.GetInt("3D.GL.TextureFilterScale") > 0 ? cfg.GetInt("3D.GL.TextureFilterScale") : 4,
        .TextureFilterObjectsOnly = cfg.GetBool("3D.GL.TextureFilterObjectsOnly"),
    };

    nds->GetRenderer().SetRenderSettings(settings);
}

void EmuThread::compileShaders()
{
    auto& renderer = emuInstance->nds->GPU.GetRenderer();
    int currentShader, shadersCount;
    u64 startTime = SDL_GetPerformanceCounter();
    // kind of hacky to look at the wallclock, though it is easier than
    // than disabling vsync
    do
    {
        renderer.ShaderCompileStep(currentShader, shadersCount);
    }
    while (renderer.NeedsShaderCompile() &&
             (SDL_GetPerformanceCounter() - startTime) * perfCountsSec < 1.0 / 6.0);
    emuInstance->osdAddMessage(0, "Compiling shader %d/%d", currentShader+1, shadersCount);
}
