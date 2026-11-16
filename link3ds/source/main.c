/*
    melonDS Link - 3DS side

    Turns the 3DS into a controller (buttons + touch) for melonDS on the PC,
    shows the DS bottom screen streamed from the PC, and gives melonDS
    access to files on the SD card (saves, ROMs).

    C-Stick up: switch between original size (1:1) and zoom (full screen).
    Hold SELECT + START for about one second to quit.
*/

#include <3ds.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "LinkProto.h"

#define SOC_ALIGN       0x1000
#define SOC_BUFFERSIZE  0x100000

/* ------------------------------------------------------------------ */
/* shared state between main thread and server thread                  */

static LightLock g_lock;
static volatile bool g_running = true;
static volatile bool g_tcpConnected = false;
static char g_statusText[128] = "";
static char g_peerName[32] = "";

static void setStatusText(const char* s)
{
    LightLock_Lock(&g_lock);
    snprintf(g_statusText, sizeof(g_statusText), "%s", s);
    LightLock_Unlock(&g_lock);
}

/* ------------------------------------------------------------------ */
/* socket helpers                                                      */

static bool sendAll(int fd, const void* buf, size_t len)
{
    const uint8_t* p = (const uint8_t*)buf;
    while (len > 0)
    {
        ssize_t n = send(fd, p, len, 0);
        if (n < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK) { svcSleepThread(1000000); continue; }
            return false;
        }
        if (n == 0) return false;
        p += n;
        len -= (size_t)n;
    }
    return true;
}

static bool recvAll(int fd, void* buf, size_t len)
{
    uint8_t* p = (uint8_t*)buf;
    while (len > 0)
    {
        if (!g_running) return false;
        ssize_t n = recv(fd, p, len, 0);
        if (n < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK) { svcSleepThread(1000000); continue; }
            return false;
        }
        if (n == 0) return false;
        p += n;
        len -= (size_t)n;
    }
    return true;
}

static bool sendResponse(int fd, const LinkHeader* req, uint16_t status, const void* payload, uint32_t len)
{
    LinkHeader h;
    h.magic = LINK_MAGIC;
    h.cmd = (uint16_t)(req->cmd | LINK_RESP);
    h.status = status;
    h.seq = req->seq;
    h.len = len;
    if (!sendAll(fd, &h, sizeof(h))) return false;
    if (len && !sendAll(fd, payload, len)) return false;
    return true;
}

/* ------------------------------------------------------------------ */
/* path handling: everything is relative to the SD card root           */

static bool makeSdPath(char* out, size_t outSize, const char* in, size_t inLen)
{
    if (inLen == 0 || inLen >= LINK_MAX_PATH) return false;
    char tmp[LINK_MAX_PATH + 1];
    memcpy(tmp, in, inLen);
    tmp[inLen] = '\0';
    if (strlen(tmp) != inLen) return false;       /* embedded NUL */
    if (tmp[0] != '/') return false;
    if (strstr(tmp, "..")) return false;
    int n = snprintf(out, outSize, "sdmc:%s", tmp);
    return n > 0 && (size_t)n < outSize;
}

static void mkdirParents(const char* path)
{
    char tmp[LINK_MAX_PATH + 8];
    snprintf(tmp, sizeof(tmp), "%s", path);
    /* skip "sdmc:/" */
    for (char* p = tmp + 6; *p; p++)
    {
        if (*p == '/')
        {
            *p = '\0';
            mkdir(tmp, 0777);
            *p = '/';
        }
    }
}

/* ------------------------------------------------------------------ */
/* command handlers                                                    */

static uint8_t* g_payload;   /* LINK_MAX_PAYLOAD + header space */
static uint8_t* g_outbuf;    /* LINK_MAX_PAYLOAD */

static bool handleCommand(int fd, const LinkHeader* h, uint8_t* payload)
{
    char path[LINK_MAX_PATH + 8];

    switch (h->cmd)
    {
    case LINK_CMD_HELLO:
    {
        LinkHelloResp r;
        memset(&r, 0, sizeof(r));
        r.version = LINK_VERSION;
        r.udpPort = LINK_UDP_PORT;
        bool isNew = false;
        APT_CheckNew3DS(&isNew);
        r.isNew3DS = isNew ? 1 : 0;
        snprintf(r.name, sizeof(r.name), "%s", isNew ? "New 3DS" : "3DS");
        uint32_t ver = (h->len >= 4) ? *(uint32_t*)payload : 0;
        return sendResponse(fd, h, ver == LINK_VERSION ? LINK_OK : LINK_ERR_VERSION, &r, sizeof(r));
    }

    case LINK_CMD_PING:
        return sendResponse(fd, h, LINK_OK, NULL, 0);

    case LINK_CMD_STATUSTEXT:
    {
        char txt[128];
        size_t n = h->len < sizeof(txt) - 1 ? h->len : sizeof(txt) - 1;
        memcpy(txt, payload, n);
        txt[n] = '\0';
        setStatusText(txt);
        return sendResponse(fd, h, LINK_OK, NULL, 0);
    }

    case LINK_CMD_STAT:
    {
        if (!makeSdPath(path, sizeof(path), (const char*)payload, h->len))
            return sendResponse(fd, h, LINK_ERR_DENIED, NULL, 0);
        struct stat st;
        if (stat(path, &st) != 0)
            return sendResponse(fd, h, LINK_ERR_NOTFOUND, NULL, 0);
        LinkStatResp r;
        memset(&r, 0, sizeof(r));
        r.size = (uint64_t)st.st_size;
        r.isDir = S_ISDIR(st.st_mode) ? 1 : 0;
        return sendResponse(fd, h, LINK_OK, &r, sizeof(r));
    }

    case LINK_CMD_READ:
    {
        if (h->len <= sizeof(LinkReadReq))
            return sendResponse(fd, h, LINK_ERR_BADARGS, NULL, 0);
        LinkReadReq req;
        memcpy(&req, payload, sizeof(req));
        if (req.length > LINK_MAX_PAYLOAD)
            return sendResponse(fd, h, LINK_ERR_BADARGS, NULL, 0);
        if (!makeSdPath(path, sizeof(path), (const char*)payload + sizeof(req), h->len - sizeof(req)))
            return sendResponse(fd, h, LINK_ERR_DENIED, NULL, 0);
        FILE* f = fopen(path, "rb");
        if (!f) return sendResponse(fd, h, LINK_ERR_NOTFOUND, NULL, 0);
        size_t got = 0;
        if (fseeko(f, (off_t)req.offset, SEEK_SET) == 0)
            got = fread(g_outbuf, 1, req.length, f);
        fclose(f);
        return sendResponse(fd, h, LINK_OK, g_outbuf, (uint32_t)got);
    }

    case LINK_CMD_WRITE:
    {
        if (h->len < sizeof(LinkWriteReq))
            return sendResponse(fd, h, LINK_ERR_BADARGS, NULL, 0);
        LinkWriteReq req;
        memcpy(&req, payload, sizeof(req));
        if (sizeof(req) + req.pathLen > h->len)
            return sendResponse(fd, h, LINK_ERR_BADARGS, NULL, 0);
        if (!makeSdPath(path, sizeof(path), (const char*)payload + sizeof(req), req.pathLen))
            return sendResponse(fd, h, LINK_ERR_DENIED, NULL, 0);
        const uint8_t* data = payload + sizeof(req) + req.pathLen;
        size_t dataLen = h->len - sizeof(req) - req.pathLen;

        FILE* f = fopen(path, "r+b");
        if (!f && (req.flags & LINK_WRITE_CREATE))
        {
            mkdirParents(path);
            f = fopen(path, "w+b");
        }
        if (!f) return sendResponse(fd, h, LINK_ERR_NOTFOUND, NULL, 0);
        bool ok = fseeko(f, (off_t)req.offset, SEEK_SET) == 0 &&
                  fwrite(data, 1, dataLen, f) == dataLen;
        ok = (fflush(f) == 0) && ok;
        fclose(f);
        return sendResponse(fd, h, ok ? LINK_OK : LINK_ERR_IO, NULL, 0);
    }

    case LINK_CMD_SETSIZE:
    {
        if (h->len <= sizeof(LinkSetSizeReq))
            return sendResponse(fd, h, LINK_ERR_BADARGS, NULL, 0);
        LinkSetSizeReq req;
        memcpy(&req, payload, sizeof(req));
        if (!makeSdPath(path, sizeof(path), (const char*)payload + sizeof(req), h->len - sizeof(req)))
            return sendResponse(fd, h, LINK_ERR_DENIED, NULL, 0);
        bool ok = truncate(path, (off_t)req.size) == 0;
        return sendResponse(fd, h, ok ? LINK_OK : LINK_ERR_IO, NULL, 0);
    }

    case LINK_CMD_LISTDIR:
    {
        if (!makeSdPath(path, sizeof(path), (const char*)payload, h->len))
            return sendResponse(fd, h, LINK_ERR_DENIED, NULL, 0);
        DIR* d = opendir(path);
        if (!d) return sendResponse(fd, h, LINK_ERR_NOTFOUND, NULL, 0);
        uint32_t used = 0;
        struct dirent* e;
        while ((e = readdir(d)) != NULL)
        {
            size_t nameLen = strlen(e->d_name);
            if (nameLen == 0 || nameLen > 255) continue;
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            if (used + sizeof(LinkDirEntry) + nameLen > LINK_MAX_PAYLOAD) break;

            LinkDirEntry de;
            memset(&de, 0, sizeof(de));
            de.isDir = (e->d_type == DT_DIR) ? 1 : 0;
            de.nameLen = (uint8_t)nameLen;
            if (!de.isDir)
            {
                char full[LINK_MAX_PATH * 2 + 16];
                snprintf(full, sizeof(full), "%s/%s", path, e->d_name);
                struct stat st;
                if (stat(full, &st) == 0) de.size = (uint64_t)st.st_size;
            }
            memcpy(g_outbuf + used, &de, sizeof(de));
            memcpy(g_outbuf + used + sizeof(de), e->d_name, nameLen);
            used += sizeof(de) + nameLen;
        }
        closedir(d);
        return sendResponse(fd, h, LINK_OK, g_outbuf, used);
    }

    default:
        return sendResponse(fd, h, LINK_ERR_BADCMD, NULL, 0);
    }
}

/* ------------------------------------------------------------------ */
/* TCP server thread                                                   */

static int g_listenFd = -1;

static void serverThread(void* arg)
{
    (void)arg;
    while (g_running)
    {
        struct sockaddr_in peer;
        socklen_t plen = sizeof(peer);
        int fd = accept(g_listenFd, (struct sockaddr*)&peer, &plen);
        if (fd < 0)
        {
            svcSleepThread(50 * 1000000LL);
            continue;
        }

        /* non-blocking, so quitting never hangs in recv(); recvAll/sendAll poll */
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);

        LightLock_Lock(&g_lock);
        snprintf(g_peerName, sizeof(g_peerName), "%s", inet_ntoa(peer.sin_addr));
        LightLock_Unlock(&g_lock);
        g_tcpConnected = true;

        while (g_running)
        {
            LinkHeader h;
            if (!recvAll(fd, &h, sizeof(h))) break;
            if (h.magic != LINK_MAGIC || h.len > LINK_MAX_PAYLOAD + LINK_MAX_PATH + 64) break;
            if (h.len && !recvAll(fd, g_payload, h.len)) break;
            if (!handleCommand(fd, &h, g_payload)) break;
        }

        close(fd);
        g_tcpConnected = false;
        setStatusText("");
    }
}

/* ------------------------------------------------------------------ */
/* DS bottom screen image (filled by the video thread)                 */

static u16 g_dsImage[192 * 256];          /* RGB565, row-major */
static volatile u32 g_videoFrames = 0;    /* bumps on every received packet */
static volatile u64 g_lastVideoTime = 0;
static volatile bool g_zoom = true;   /* enlarged is the default */
static int g_videoFd = -1;

/* receipt of video packets, reported back to the PC in every input packet */
static LightLock g_ackLock;
static u32 g_ackSeq = 0;
static u64 g_ackMask = 0;
static bool g_ackValid = false;

static void noteVideoSeq(u32 seq)
{
    LightLock_Lock(&g_ackLock);
    /* a big jump backwards means melonDS was restarted: start over */
    if (!g_ackValid || (s32)(seq - g_ackSeq) < -4096)
    {
        g_ackSeq = seq;
        g_ackMask = 1;
        g_ackValid = true;
    }
    else if ((s32)(seq - g_ackSeq) > 0)
    {
        u32 d = seq - g_ackSeq;
        g_ackMask = (d >= 64) ? 0 : (g_ackMask << d);
        g_ackMask |= 1;
        g_ackSeq = seq;
    }
    else
    {
        u32 d = g_ackSeq - seq;
        if (d < 64) g_ackMask |= (1ULL << d);
    }
    LightLock_Unlock(&g_ackLock);
}

static void decodeTile(int type, const u8* p, int tx, int ty)
{
    u16* dst = &g_dsImage[ty * 256 + tx];
    switch (type)
    {
    case LINK_TILE_RAW:
        for (int y = 0; y < 8; y++) memcpy(dst + y * 256, p + y * 16, 16);
        break;
    case LINK_TILE_SOLID_T:
    {
        u16 c; memcpy(&c, p, 2);
        for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) dst[y * 256 + x] = c;
        break;
    }
    case LINK_TILE_PAL2:
    {
        u16 pal[2]; memcpy(pal, p, 4);
        const u8* bits = p + 4;
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++) dst[y * 256 + x] = pal[(bits[y] >> x) & 1];
        break;
    }
    case LINK_TILE_PAL4:
    {
        u16 pal[4]; memcpy(pal, p, 8);
        for (int y = 0; y < 8; y++)
        {
            u16 row; memcpy(&row, p + 8 + y * 2, 2);
            for (int x = 0; x < 8; x++) dst[y * 256 + x] = pal[(row >> (x * 2)) & 3];
        }
        break;
    }
    case LINK_TILE_PAL16:
    {
        u16 pal[16]; memcpy(pal, p, 32);
        for (int y = 0; y < 8; y++)
        {
            u32 row; memcpy(&row, p + 32 + y * 4, 4);
            for (int x = 0; x < 8; x++) dst[y * 256 + x] = pal[(row >> (x * 4)) & 15];
        }
        break;
    }
    }
}

static int tilePayloadSize(int type)
{
    switch (type)
    {
    case LINK_TILE_RAW: return 128;
    case LINK_TILE_SOLID_T: return 2;
    case LINK_TILE_PAL2: return 12;
    case LINK_TILE_PAL4: return 24;
    case LINK_TILE_PAL16: return 64;
    default: return -1;
    }
}

static void videoThread(void* arg)
{
    (void)arg;
    static u8 buf[2048];
    while (g_running)
    {
        ssize_t n = recvfrom(g_videoFd, buf, sizeof(buf), 0, NULL, NULL);
        if (n <= 0)
        {
            svcSleepThread(500000); /* 0.5 ms */
            continue;
        }
        if ((size_t)n < sizeof(LinkFramePacket)) continue;
        LinkFramePacket fp;
        memcpy(&fp, buf, sizeof(fp));
        if (fp.magic != LINK_FRAME_MAGIC) continue;

        const u8* p = buf + sizeof(fp);
        const u8* end = buf + n;
        for (u32 e = 0; e < fp.entryCount; e++)
        {
            if (p + 2 > end) break;
            u16 head;
            memcpy(&head, p, 2);
            p += 2;
            int idx = head & LINK_TILE_INDEX_MASK;
            int type = head >> LINK_TILE_TYPE_SHIFT;
            int size = tilePayloadSize(type);
            if (idx >= LINK_TILE_COUNT || size < 0 || p + size > end) break;
            decodeTile(type, p, (idx % LINK_TILES_X) * 8, (idx / LINK_TILES_X) * 8);
            p += size;
        }
        noteVideoSeq(fp.seq);
        g_videoFrames++;
        g_lastVideoTime = osGetTime();
    }
}

/* ------------------------------------------------------------------ */
/* input thread: samples HID every 4 ms and sends it straight away     */

typedef struct
{
    u32 keys;
    touchPosition touch;
    bool dsTouch;
    u16 dsX, dsY;
    bool quit;
    bool haveUdpPeer;
} InputState;

static InputState g_input;    /* snapshot for the UI, guarded by g_lock */
static int g_udpFd = -1;

static bool mapTouch(u16 px, u16 py, bool zoom, u16* dx, u16* dy)
{
    if (zoom)
    {
        /* 320x240 -> 256x192 (factor 0.8) */
        int x = px * 4 / 5, y = py * 4 / 5;
        if (x > 255) x = 255;
        if (y > 191) y = 191;
        *dx = (u16)x;
        *dy = (u16)y;
        return true;
    }
    int x = (int)px - LINK_DS_OFFSET_X, y = (int)py - LINK_DS_OFFSET_Y;
    if (x < 0 || x >= 256 || y < 0 || y >= 192) return false;
    *dx = (u16)x;
    *dy = (u16)y;
    return true;
}

static void inputThread(void* arg)
{
    (void)arg;
    struct sockaddr_in peer;
    bool havePeer = false;
    u64 lastHello = 0;
    u32 packetId = 0;
    int quitHold = 0;
    bool cstickUpWas = false;

    while (g_running)
    {
        u64 start = osGetTime();

        hidScanInput();
        u32 held = hidKeysHeld();

        /* SELECT + START for ~1 s quits */
        if ((held & KEY_SELECT) && (held & KEY_START)) quitHold++;
        else quitHold = 0;

        /* C-stick up toggles zoom */
        bool cup = (held & KEY_CSTICK_UP) != 0;
        if (cup && !cstickUpWas) g_zoom = !g_zoom;
        cstickUpWas = cup;

        /* HELO from the PC (also keep-alive) */
        for (;;)
        {
            uint32_t msg = 0;
            struct sockaddr_in from;
            socklen_t flen = sizeof(from);
            ssize_t n = recvfrom(g_udpFd, &msg, sizeof(msg), 0, (struct sockaddr*)&from, &flen);
            if (n <= 0) break;
            if (n == sizeof(msg) && msg == LINK_UDP_HELLO)
            {
                peer = from;
                havePeer = true;
                lastHello = osGetTime();
            }
        }
        if (havePeer && osGetTime() - lastHello > 5000) havePeer = false;

        touchPosition touch;
        circlePosition cpad, cstick;
        hidTouchRead(&touch);
        hidCircleRead(&cpad);
        irrstCstickRead(&cstick);

        u16 dsX = 0, dsY = 0;
        bool dsTouch = (held & KEY_TOUCH) && mapTouch(touch.px, touch.py, g_zoom, &dsX, &dsY);

        if (havePeer)
        {
            LinkInputPacket p;
            memset(&p, 0, sizeof(p));
            p.magic = LINK_MAGIC;
            p.id = packetId++;
            p.keys = held;
            p.cpadX = cpad.dx;
            p.cpadY = cpad.dy;
            p.touchX = touch.px;
            p.touchY = touch.py;
            p.cstickX = cstick.dx;
            p.cstickY = cstick.dy;
            p.dsTouchX = dsX;
            p.dsTouchY = dsY;
            p.flags = (dsTouch ? LINK_INPUT_DSTOUCH : 0) | (g_zoom ? LINK_INPUT_ZOOM : 0);
            LightLock_Lock(&g_ackLock);
            if (g_ackValid)
            {
                p.flags |= LINK_INPUT_VIDEOACK;
                p.videoAckSeq = g_ackSeq;
                p.videoAckMask = g_ackMask;
            }
            LightLock_Unlock(&g_ackLock);
            sendto(g_udpFd, &p, sizeof(p), 0, (struct sockaddr*)&peer, sizeof(peer));
        }

        LightLock_Lock(&g_lock);
        g_input.keys = held;
        g_input.touch = touch;
        g_input.dsTouch = dsTouch;
        g_input.dsX = dsX;
        g_input.dsY = dsY;
        g_input.quit = quitHold > 250;   /* 250 x 4 ms */
        g_input.haveUdpPeer = havePeer;
        LightLock_Unlock(&g_lock);

        u64 el = osGetTime() - start;
        if (el < 4) svcSleepThread((s64)(4 - el) * 1000000LL);
    }
}

/* ------------------------------------------------------------------ */
/* bottom screen drawing (framebuffer is 240x320, rotated, RGB565)     */

#define LINK_RGB565(r, g, b) (u16)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3))

static inline void putPx(u16* fb, int x, int y, u16 c)
{
    fb[x * 240 + (239 - y)] = c;
}

static void fillRect(u16* fb, int x0, int y0, int w, int h, u16 c)
{
    for (int x = x0; x < x0 + w; x++)
    {
        if (x < 0 || x >= 320) continue;
        for (int y = y0; y < y0 + h; y++)
        {
            if (y < 0 || y >= 240) continue;
            putPx(fb, x, y, c);
        }
    }
}

static void drawBottom(bool haveVideo, bool zoom, bool touching, int tx, int ty)
{
    u16* fb = (u16*)gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL);
    const u16 bg = LINK_RGB565(0x10, 0x10, 0x14);
    const u16 frame = LINK_RGB565(0x50, 0x50, 0x60);
    const u16 empty = LINK_RGB565(0x22, 0x22, 0x2A);

    if (zoom)
    {
        /* 256x192 -> 320x240, nearest neighbour; walk the rotated fb linearly */
        for (int x = 0; x < 320; x++)
        {
            int sx = x * 4 / 5;
            u16* col = fb + x * 240;
            for (int y = 0; y < 240; y++)
            {
                int sy = y * 4 / 5;
                col[239 - y] = haveVideo ? g_dsImage[sy * 256 + sx] : empty;
            }
        }
    }
    else
    {
        fillRect(fb, 0, 0, 320, 240, bg);
        fillRect(fb, LINK_DS_OFFSET_X - 2, LINK_DS_OFFSET_Y - 2, 260, 196, frame);
        for (int x = 0; x < 256; x++)
        {
            u16* col = fb + (x + LINK_DS_OFFSET_X) * 240;
            for (int y = 0; y < 192; y++)
                col[239 - (y + LINK_DS_OFFSET_Y)] = haveVideo ? g_dsImage[y * 256 + x] : empty;
        }
    }

    /* without a picture, show where the finger is */
    if (!haveVideo && touching)
        fillRect(fb, tx - 2, ty - 2, 5, 5, LINK_RGB565(0xE0, 0x60, 0x60));
}

/* ------------------------------------------------------------------ */
/* settings on the SD card                                             */

#define CONFIG_PATH "sdmc:/3ds/melonDS-Link.cfg"

static void loadConfig(void)
{
    FILE* f = fopen(CONFIG_PATH, "r");
    if (!f) return;
    char line[64];
    while (fgets(line, sizeof(line), f))
    {
        int v;
        if (sscanf(line, "zoom=%d", &v) == 1) g_zoom = v != 0;
    }
    fclose(f);
}

static void saveConfig(void)
{
    FILE* f = fopen(CONFIG_PATH, "w");
    if (!f) return;
    fprintf(f, "zoom=%d\n", g_zoom ? 1 : 0);
    fclose(f);
}

/* ------------------------------------------------------------------ */

int main(int argc, char** argv)
{
    (void)argc; (void)argv;

    gfxInitDefault();
    gfxSetScreenFormat(GFX_BOTTOM, GSP_RGB565_OES);
    gfxSetDoubleBuffering(GFX_BOTTOM, true);
    PrintConsole top;
    consoleInit(GFX_TOP, &top);
    osSetSpeedupEnable(true);
    irrstInit();   /* C-stick / ZL / ZR on New 3DS */
    LightLock_Init(&g_lock);
    LightLock_Init(&g_ackLock);
    loadConfig();

    printf("\x1b[1;1H\x1b[36mmelonDS Link\x1b[0m  (Protokoll v%u)\n\n", (unsigned)LINK_VERSION);

    u32* socBuf = (u32*)memalign(SOC_ALIGN, SOC_BUFFERSIZE);
    g_payload = (uint8_t*)malloc(LINK_MAX_PAYLOAD + LINK_MAX_PATH + 64);
    g_outbuf = (uint8_t*)malloc(LINK_MAX_PAYLOAD);
    if (!socBuf || !g_payload || !g_outbuf || R_FAILED(socInit(socBuf, SOC_BUFFERSIZE)))
    {
        printf("\x1b[31mWLAN konnte nicht gestartet werden.\x1b[0m\n");
        printf("Ist WLAN an? START zum Beenden.\n");
        while (aptMainLoop())
        {
            hidScanInput();
            if (hidKeysDown() & KEY_START) break;
            gspWaitForVBlank();
        }
        irrstExit();
        gfxExit();
        return 0;
    }

    struct in_addr ip;
    ip.s_addr = gethostid();

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    /* TCP listen socket */
    g_listenFd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    addr.sin_port = htons(LINK_TCP_PORT);
    bind(g_listenFd, (struct sockaddr*)&addr, sizeof(addr));
    listen(g_listenFd, 1);
    fcntl(g_listenFd, F_SETFL, fcntl(g_listenFd, F_GETFL, 0) | O_NONBLOCK);

    /* UDP input socket */
    g_udpFd = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    addr.sin_port = htons(LINK_UDP_PORT);
    bind(g_udpFd, (struct sockaddr*)&addr, sizeof(addr));
    fcntl(g_udpFd, F_SETFL, fcntl(g_udpFd, F_GETFL, 0) | O_NONBLOCK);

    /* UDP video socket */
    g_videoFd = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    addr.sin_port = htons(LINK_VIDEO_PORT);
    bind(g_videoFd, (struct sockaddr*)&addr, sizeof(addr));
    fcntl(g_videoFd, F_SETFL, fcntl(g_videoFd, F_GETFL, 0) | O_NONBLOCK);
    {
        int rcv = 128 * 1024;   /* room for bursts of tile packets */
        setsockopt(g_videoFd, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof(rcv));
    }

    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    Thread srv = threadCreate(serverThread, NULL, 0x4000, prio + 1, -2, false);
    Thread inp = threadCreate(inputThread, NULL, 0x4000, prio - 1, -2, false);
    Thread vid = threadCreate(videoThread, NULL, 0x4000, prio, -2, false);

    u32 lastVideoFrames = (u32)-1;
    bool lastZoom = !g_zoom, lastHaveVideo = true;
    bool savedZoom = g_zoom;
    int redrawBoth = 2;

    while (aptMainLoop() && g_running)
    {
        InputState in;
        LightLock_Lock(&g_lock);
        in = g_input;
        LightLock_Unlock(&g_lock);
        if (in.quit) break;

        bool haveVideo = g_lastVideoTime != 0 && osGetTime() - g_lastVideoTime < 3000;
        bool zoom = g_zoom;

        /* redraw when something changed; both buffers need it (double buffering) */
        if (g_videoFrames != lastVideoFrames || zoom != lastZoom || haveVideo != lastHaveVideo ||
            (!haveVideo && (in.keys & KEY_TOUCH)))
            redrawBoth = 2;
        if (zoom != savedZoom) { saveConfig(); savedZoom = zoom; }
        lastVideoFrames = g_videoFrames;
        lastZoom = zoom;
        lastHaveVideo = haveVideo;
        if (redrawBoth > 0)
        {
            drawBottom(haveVideo, zoom, (in.keys & KEY_TOUCH) != 0, in.touch.px, in.touch.py);
            redrawBoth--;
        }

        /* top screen status */
        char status[128], peer[32];
        LightLock_Lock(&g_lock);
        snprintf(status, sizeof(status), "%s", g_statusText);
        snprintf(peer, sizeof(peer), "%s", g_peerName);
        LightLock_Unlock(&g_lock);

        printf("\x1b[3;1HIP-Adresse:  \x1b[33m%s\x1b[0m          \n", inet_ntoa(ip));
        printf("\x1b[5;1HPC (Daten):   %s\x1b[K\n",
               g_tcpConnected ? "\x1b[32mverbunden\x1b[0m" : "\x1b[90mwartet...\x1b[0m");
        printf("\x1b[6;1HSteuerung:    %s\x1b[K\n",
               in.haveUdpPeer ? "\x1b[32maktiv\x1b[0m" : "\x1b[90mwartet...\x1b[0m");
        printf("\x1b[7;1HBild:         %s\x1b[K\n",
               haveVideo ? "\x1b[32mempfangen\x1b[0m" : "\x1b[90mkein Bild\x1b[0m");
        printf("\x1b[8;1HAnzeige:      %s\x1b[K\n", zoom ? "Vergroessert" : "Original (1:1)");
        if (g_tcpConnected) printf("\x1b[9;1HPC-Adresse:   %s\x1b[K\n", peer);
        else printf("\x1b[9;1H\x1b[K\n");
        printf("\x1b[11;1H%s\x1b[K\n", status);

        printf("\x1b[25;1H\x1b[90mIn melonDS: 3DS Link > Mit 3DS verbinden\n");
        printf("und diese IP eingeben.\n\n");
        printf("C-Stick hoch = Original / Vergroessert\n");
        printf("SELECT + START halten = Beenden\x1b[0m");

        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }

    g_running = false;
    close(g_listenFd);
    threadJoin(inp, U64_MAX);
    threadJoin(vid, U64_MAX);
    threadJoin(srv, U64_MAX);
    threadFree(inp);
    threadFree(vid);
    threadFree(srv);
    close(g_udpFd);
    close(g_videoFd);
    socExit();
    irrstExit();
    gfxExit();
    return 0;
}
