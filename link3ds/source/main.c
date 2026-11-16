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
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>

#include "LinkProto.h"
#include "lz4.h"

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
        /* 64 KB per call, like ftpd: bigger sends are slower through the 3DS socket service */
        size_t piece = len > 0x10000 ? 0x10000 : len;
        ssize_t n = send(fd, p, piece, 0);
        if (n < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                /* wait until there is room again - no fixed sleep, that capped the speed */
                struct pollfd pf = { .fd = fd, .events = POLLOUT, .revents = 0 };
                if (!g_running) return false;
                poll(&pf, 1, 100);
                continue;
            }
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
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                struct pollfd pf = { .fd = fd, .events = POLLIN, .revents = 0 };
                poll(&pf, 1, 100);
                continue;
            }
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
/* game list and launch requests                                       */

#define MAX_GAMES 512
static char g_romDir[128] = "/roms/nds";
static char* g_games[MAX_GAMES];
static int g_gameCount = 0;
static volatile bool g_menuOpen = true;      /* game list shown, input not sent to the game */
static volatile bool g_gameStarted = false;
static char g_currentGame[128] = "";

/* pending launch for melonDS (guarded by g_lock) */
static bool g_haveEvent = false;
static LinkEvent g_event;
static char g_eventRom[LINK_MAX_PATH];
static char g_eventSave[LINK_MAX_PATH];

/* favourites: one file name per line in FAVS_PATH */
#define FAVS_PATH "sdmc:/3ds/melonDS-Link-favoriten.txt"
#define MAX_FAVS 128
static char* g_favs[MAX_FAVS];
static int g_favCount = 0;
static char g_lastGame[256] = "";   /* file name of the last launched game */

static bool isFav(const char* name)
{
    for (int i = 0; i < g_favCount; i++)
        if (strcasecmp(g_favs[i], name) == 0) return true;
    return false;
}

static void loadFavs(void)
{
    FILE* f = fopen(FAVS_PATH, "r");
    if (!f) return;
    char line[300];
    while (g_favCount < MAX_FAVS && fgets(line, sizeof(line), f))
    {
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] && !isFav(line)) g_favs[g_favCount++] = strdup(line);
    }
    fclose(f);
}

static void saveFavs(void)
{
    FILE* f = fopen(FAVS_PATH, "w");
    if (!f) return;
    for (int i = 0; i < g_favCount; i++) fprintf(f, "%s\n", g_favs[i]);
    fclose(f);
}

static void toggleFav(const char* name)
{
    for (int i = 0; i < g_favCount; i++)
    {
        if (strcasecmp(g_favs[i], name) == 0)
        {
            free(g_favs[i]);
            g_favs[i] = g_favs[--g_favCount];
            saveFavs();
            return;
        }
    }
    if (g_favCount < MAX_FAVS) g_favs[g_favCount++] = strdup(name);
    saveFavs();
}

/* favourites first, then alphabetical */
static int cmpNoCase(const void* a, const void* b)
{
    const char* x = *(char* const*)a;
    const char* y = *(char* const*)b;
    bool fx = isFav(x), fy = isFav(y);
    if (fx != fy) return fx ? -1 : 1;
    return strcasecmp(x, y);
}

static void sortGames(void)
{
    qsort(g_games, g_gameCount, sizeof(char*), cmpNoCase);
}

static int findGame(const char* name)
{
    for (int i = 0; i < g_gameCount; i++)
        if (strcasecmp(g_games[i], name) == 0) return i;
    return -1;
}

static bool hasNdsExt(const char* name)
{
    size_t n = strlen(name);
    return n > 4 && strcasecmp(name + n - 4, ".nds") == 0;
}

static void scanGames(void)
{
    for (int i = 0; i < g_gameCount; i++) free(g_games[i]);
    g_gameCount = 0;
    char dir[160];
    snprintf(dir, sizeof(dir), "sdmc:%s", g_romDir);
    DIR* d = opendir(dir);
    if (!d) return;
    struct dirent* e;
    while ((e = readdir(d)) != NULL && g_gameCount < MAX_GAMES)
    {
        if (e->d_type == DT_DIR || !hasNdsExt(e->d_name)) continue;
        g_games[g_gameCount++] = strdup(e->d_name);
    }
    closedir(d);
    sortGames();
}

static bool sdExists(const char* sdPath)
{
    char full[LINK_MAX_PATH + 8];
    snprintf(full, sizeof(full), "sdmc:%s", sdPath);
    struct stat st;
    return stat(full, &st) == 0;
}

/* the user picked a game: prepare the event melonDS will fetch */
static void requestLaunch(int idx)
{
    if (idx < 0 || idx >= g_gameCount) return;
    const char* name = g_games[idx];

    char rom[LINK_MAX_PATH], base[LINK_MAX_PATH], save[LINK_MAX_PATH], alt[LINK_MAX_PATH];
    snprintf(rom, sizeof(rom), "%.100s/%.150s", g_romDir, name);
    snprintf(base, sizeof(base), "%s", name);
    base[strlen(base) - 4] = '\0';                          /* drop ".nds" */
    /* TWiLight Menu++ keeps saves in "saves/" next to the ROMs (default)
       or right next to the ROM */
    snprintf(save, sizeof(save), "%.100s/saves/%.140s.sav", g_romDir, base);
    snprintf(alt, sizeof(alt), "%.100s/%.140s.sav", g_romDir, base);
    if (!sdExists(save) && sdExists(alt)) snprintf(save, sizeof(save), "%s", alt);

    char full[LINK_MAX_PATH + 8];
    snprintf(full, sizeof(full), "sdmc:%s", rom);
    struct stat st;
    u64 size = (stat(full, &st) == 0) ? (u64)st.st_size : 0;

    LightLock_Lock(&g_lock);
    memset(&g_event, 0, sizeof(g_event));
    g_event.type = LINK_EVENT_LAUNCH;
    g_event.romSize = size;
    snprintf(g_eventRom, sizeof(g_eventRom), "%s", rom);
    snprintf(g_eventSave, sizeof(g_eventSave), "%s", save);
    g_event.romPathLen = (uint16_t)strlen(g_eventRom);
    g_event.savePathLen = (uint16_t)strlen(g_eventSave);
    g_haveEvent = true;
    snprintf(g_currentGame, sizeof(g_currentGame), "%.120s", base);
    snprintf(g_lastGame, sizeof(g_lastGame), "%s", name);
    snprintf(g_statusText, sizeof(g_statusText), "Starte Spiel - warte auf melonDS...");
    LightLock_Unlock(&g_lock);

    g_gameStarted = true;
    g_menuOpen = false;
}

/* ------------------------------------------------------------------ */
/* command handlers                                                    */

static uint8_t* g_payload;   /* LINK_MAX_PAYLOAD + header space */
static uint8_t* g_outbuf;    /* LINK_MAX_PAYLOAD */

/* ------------------------------------------------------------------ */
/* streaming big files: a reader thread fills buffers from the SD card  */
/* while the server thread sends the previous one                       */

#define STREAM_BUFS 3
#define STREAM_CHUNK LINK_STREAM_BLOCK_SIZE
static u8* g_streamBuf[STREAM_BUFS];               /* raw data from the SD card */
static u8* g_streamEnc[STREAM_BUFS];               /* LZ4 output */
static LinkStreamBlock g_streamHdr[STREAM_BUFS];   /* how block i is sent (compressed mode) */
static volatile u32 g_streamLen[STREAM_BUFS];
static volatile int g_streamState[STREAM_BUFS];    /* 0 empty, 1 full, 2 end/error */
static FILE* g_streamFile;
static volatile u64 g_streamLeft;
static volatile bool g_streamStop;
static volatile bool g_streamCompress;   /* LZ4 on (switched off if it can't keep up) */
static volatile bool g_streamPack;       /* compressed protocol (fill blocks etc.) */
static volatile u32 g_senderStarved;     /* sender had to wait for the reader */
static LZ4_stream_t g_lz4State;          /* ~16 KB: must not live on a thread stack */

/* transfer info for the top screen (guarded by g_lock) */
static char g_xferInfo[64] = "";

/* one value repeated over the whole block (cartridge padding)? */
static bool isUniform(const u8* p, u32 n)
{
    const u32* w = (const u32*)p;
    u32 first = w[0];
    if ((first & 0xFF) * 0x01010101u != first) return false;
    for (u32 i = 1; i < n / 4; i++) if (w[i] != first) return false;
    for (u32 i = n & ~3u; i < n; i++) if (p[i] != p[0]) return false;
    return true;
}

static void streamReader(void* arg)
{
    (void)arg;
    int i = 0;
    while (!g_streamStop)
    {
        if (g_streamState[i] != 0) { svcSleepThread(200000); continue; }   /* wait for the sender */
        if (g_streamLeft == 0) { g_streamState[i] = 2; break; }
        u32 want = g_streamLeft > STREAM_CHUNK ? STREAM_CHUNK : (u32)g_streamLeft;
        size_t got = fread(g_streamBuf[i], 1, want, g_streamFile);
        if (got != want) { g_streamState[i] = 2; g_streamLen[i] = 0; break; }
        g_streamLen[i] = want;

        if (g_streamPack)
        {
            LinkStreamBlock* b = &g_streamHdr[i];
            memset(b, 0, sizeof(*b));
            b->rawLen = want;
            if (isUniform(g_streamBuf[i], want))
            {
                b->mode = LINK_BLOCK_FILL;
                b->fill = g_streamBuf[i][0];
            }
            else if (g_streamCompress)
            {
                int enc = LZ4_compress_fast_extState(&g_lz4State, (const char*)g_streamBuf[i], (char*)g_streamEnc[i],
                                                     (int)want, LZ4_compressBound(STREAM_CHUNK), 1);
                if (enc > 0 && (u32)enc < want - want / 32)
                {
                    b->mode = LINK_BLOCK_LZ4;
                    b->encLen = (u32)enc;
                }
                else
                {
                    b->mode = LINK_BLOCK_RAW;   /* already compressed data: send as is */
                    b->encLen = want;
                }
            }
            else
            {
                b->mode = LINK_BLOCK_RAW;       /* LZ4 switched off: the CPU was the bottleneck */
                b->encLen = want;
            }
        }

        g_streamLeft -= want;
        g_streamState[i] = 1;
        i = (i + 1) % STREAM_BUFS;
    }
}

static bool streamFile(int fd, const LinkHeader* h, const char* path, u64 offset, u64 length, bool compress)
{
    FILE* f = fopen(path, "rb");
    if (!f) return sendResponse(fd, h, LINK_ERR_NOTFOUND, NULL, 0);
    struct stat st;
    u64 size = (fstat(fileno(f), &st) == 0) ? (u64)st.st_size : 0;
    if (offset > size) offset = size;
    if (length > size - offset) length = size - offset;
    if (fseeko(f, (off_t)offset, SEEK_SET) != 0) { fclose(f); return sendResponse(fd, h, LINK_ERR_IO, NULL, 0); }
    setvbuf(f, NULL, _IONBF, 0);   /* we read big blocks ourselves */

    if (!g_streamBuf[0])
        for (int i = 0; i < STREAM_BUFS; i++) g_streamBuf[i] = (u8*)memalign(0x1000, STREAM_CHUNK);
    if (compress && !g_streamEnc[0])
        for (int i = 0; i < STREAM_BUFS; i++) g_streamEnc[i] = (u8*)memalign(0x1000, LZ4_compressBound(STREAM_CHUNK));
    if (!g_streamBuf[STREAM_BUFS - 1] || (compress && !g_streamEnc[STREAM_BUFS - 1]))
    {
        fclose(f);
        return sendResponse(fd, h, LINK_ERR_IO, NULL, 0);
    }
    for (int i = 0; i < STREAM_BUFS; i++) { g_streamState[i] = 0; g_streamLen[i] = 0; }
    g_streamFile = f;
    g_streamLeft = length;
    g_streamStop = false;
    g_streamPack = compress;
    g_streamCompress = compress;
    g_senderStarved = 0;

    u64 len64 = length;
    if (!sendResponse(fd, h, LINK_OK, &len64, sizeof(len64))) { fclose(f); return false; }

    /* on a New 3DS the reading/compressing runs on its own CPU core */
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    bool isNew = false;
    APT_CheckNew3DS(&isNew);
    Thread rd = NULL;
    if (isNew) rd = threadCreate(streamReader, NULL, 0x10000, prio, 2, false);
    if (!rd) rd = threadCreate(streamReader, NULL, 0x10000, prio, -2, false);

    bool ok = true;
    u64 sent = 0, wire = 0;
    int i = 0;
    u64 t0 = osGetTime(), lastInfo = t0;
    u64 waitTicks = 0, blocks = 0;
    while (sent < length && ok && g_running)
    {
        if (g_streamState[i] == 0)
        {
            u64 w0 = svcGetSystemTick();
            svcSleepThread(200000);
            waitTicks += svcGetSystemTick() - w0;
            continue;
        }
        if (g_streamState[i] == 2) { ok = false; break; }           /* SD read error */
        if (compress)
        {
            LinkStreamBlock* b = &g_streamHdr[i];
            ok = sendAll(fd, b, sizeof(*b));
            if (ok && b->mode == LINK_BLOCK_LZ4) ok = sendAll(fd, g_streamEnc[i], b->encLen);
            else if (ok && b->mode == LINK_BLOCK_RAW) ok = sendAll(fd, g_streamBuf[i], b->encLen);
        }
        else ok = sendAll(fd, g_streamBuf[i], g_streamLen[i]);
        sent += g_streamLen[i];
        wire += compress ? sizeof(LinkStreamBlock) + (g_streamHdr[i].mode == LINK_BLOCK_FILL ? 0 : g_streamHdr[i].encLen)
                         : g_streamLen[i];
        g_streamState[i] = 0;
        i = (i + 1) % STREAM_BUFS;
        blocks++;

        /* every 8 blocks: if we spent more than a third of the time waiting
           for the reader, packing is slower than the WiFi -> stop using LZ4 */
        if (compress && g_streamCompress && blocks % 8 == 0)
        {
            u64 now = osGetTime();
            u64 elapsedTicks = (now - lastInfo) * (u64)SYSCLOCK_ARM11 / 1000;
            if (elapsedTicks > 0 && waitTicks * 3 > elapsedTicks) g_streamCompress = false;
            waitTicks = 0;
            lastInfo = now;
        }
        u64 secs1000 = osGetTime() - t0;
        if (secs1000 > 0 && blocks % 4 == 0)
        {
            LightLock_Lock(&g_lock);
            snprintf(g_xferInfo, sizeof(g_xferInfo), "WLAN %lu KB/s, Daten %lu KB/s, LZ4 %s",
                     (unsigned long)(wire / secs1000), (unsigned long)(sent / secs1000),
                     !compress ? "-" : g_streamCompress ? "an" : "aus (CPU zu langsam)");
            LightLock_Unlock(&g_lock);
        }
    }
    g_streamStop = true;
    threadJoin(rd, U64_MAX);
    threadFree(rd);
    fclose(f);
    /* on an error mid-stream the framing is broken: closing the connection
       (returning false) makes the PC retry from where it got to */
    return ok && sent == length;
}

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

    case LINK_CMD_POLLEVENT:
    {
        LightLock_Lock(&g_lock);
        bool have = g_haveEvent;
        uint32_t used = 0;
        if (have)
        {
            memcpy(g_outbuf, &g_event, sizeof(g_event));
            used = sizeof(g_event);
            memcpy(g_outbuf + used, g_eventRom, g_event.romPathLen);
            used += g_event.romPathLen;
            memcpy(g_outbuf + used, g_eventSave, g_event.savePathLen);
            used += g_event.savePathLen;
            g_haveEvent = false;
        }
        LightLock_Unlock(&g_lock);
        if (!have) return sendResponse(fd, h, LINK_ERR_NOTFOUND, NULL, 0);
        return sendResponse(fd, h, LINK_OK, g_outbuf, used);
    }

    case LINK_CMD_READSTREAM:
    case LINK_CMD_READSTREAMZ:
    {
        if (h->len <= sizeof(LinkStreamReq)) return sendResponse(fd, h, LINK_ERR_BADARGS, NULL, 0);
        LinkStreamReq req;
        memcpy(&req, payload, sizeof(req));
        if (!makeSdPath(path, sizeof(path), (const char*)payload + sizeof(req), h->len - sizeof(req)))
            return sendResponse(fd, h, LINK_ERR_DENIED, NULL, 0);
        return streamFile(fd, h, path, req.offset, req.length, h->cmd == LINK_CMD_READSTREAMZ);
    }

    case LINK_CMD_REPLACE:
    {
        /* replace `to` with `from`, keeping the old file until the new one is in place */
        if (h->len < 2) return sendResponse(fd, h, LINK_ERR_BADARGS, NULL, 0);
        uint16_t fromLen;
        memcpy(&fromLen, payload, 2);
        if (2u + fromLen >= h->len) return sendResponse(fd, h, LINK_ERR_BADARGS, NULL, 0);
        char from[LINK_MAX_PATH + 8], to[LINK_MAX_PATH + 8], bak[LINK_MAX_PATH + 16];
        if (!makeSdPath(from, sizeof(from), (const char*)payload + 2, fromLen) ||
            !makeSdPath(to, sizeof(to), (const char*)payload + 2 + fromLen, h->len - 2 - fromLen))
            return sendResponse(fd, h, LINK_ERR_DENIED, NULL, 0);
        struct stat st;
        if (stat(from, &st) != 0) return sendResponse(fd, h, LINK_ERR_NOTFOUND, NULL, 0);
        snprintf(bak, sizeof(bak), "%s.old", to);
        bool hadOld = stat(to, &st) == 0;
        if (hadOld)
        {
            remove(bak);
            if (rename(to, bak) != 0) return sendResponse(fd, h, LINK_ERR_IO, NULL, 0);
        }
        else mkdirParents(to);
        if (rename(from, to) != 0)
        {
            if (hadOld) rename(bak, to);   /* put the old one back */
            return sendResponse(fd, h, LINK_ERR_IO, NULL, 0);
        }
        if (hadOld) remove(bak);
        return sendResponse(fd, h, LINK_OK, NULL, 0);
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
        {
            int buf = 32768;   /* what ftpd uses on the 3DS; larger was slower */
            setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buf, sizeof(buf));
            setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf, sizeof(buf));
        }

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

static u16 g_dsImage[192 * 256];          /* RGB565, row-major: what is shown */
static u16 g_dsBack[192 * 256];           /* tiles are decoded here first */
static volatile u32 g_videoFrames = 0;    /* bumps on every received packet */
static volatile u64 g_lastVideoTime = 0;
static volatile bool g_zoom = true;   /* enlarged is the default */
static u32 g_menuKeys = 0;            /* key presses for the game list (g_lock) */
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
    u16* dst = &g_dsBack[ty * 256 + tx];
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
    case LINK_TILE_QUARTER:
    {
        u16 px[4]; memcpy(px, p, 8);
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++) dst[y * 256 + x] = px[(y >> 2) * 2 + (x >> 2)];
        break;
    }
    case LINK_TILE_HALF:
    {
        u16 px[16]; memcpy(px, p, 32);
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++) dst[y * 256 + x] = px[(y >> 1) * 4 + (x >> 1)];
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
    case LINK_TILE_HALF: return 32;
    case LINK_TILE_QUARTER: return 8;
    default: return -1;
    }
}

/* show the decoded image; called when a frame is complete */
#ifdef LINK_HOST_TEST
void link_test_on_present(const u16* img);   /* only for the PC simulation */
#endif

static void presentFrame(void)
{
    memcpy(g_dsImage, g_dsBack, sizeof(g_dsImage));
    g_videoFrames++;
#ifdef LINK_HOST_TEST
    link_test_on_present(g_dsImage);
#endif
}

/* which packet seqs have arrived (ring, slot holds the seq that filled it) */
#define SEQ_RING 4096
static u32 g_seqRing[SEQ_RING];
static bool g_seqRingInit = false;

static void markReceived(u32 seq)
{
    if (!g_seqRingInit)
    {
        /* slot i can only ever hold seqs with (seq & mask) == i, so i + 1 means "empty" */
        for (int i = 0; i < SEQ_RING; i++) g_seqRing[i] = (u32)i + 1;
        g_seqRingInit = true;
    }
    g_seqRing[seq & (SEQ_RING - 1)] = seq;
}

static bool haveAll(u32 from, u32 to)
{
    if ((s32)(to - from) < 0 || to - from >= SEQ_RING) return false;
    for (u32 s = from; s != to + 1; s++)
        if (g_seqRing[s & (SEQ_RING - 1)] != s) return false;
    return true;
}

static void videoThread(void* arg)
{
    (void)arg;
    static u8 buf[2048];
    u32 curFrame = 0;
    bool frameOpen = false;          /* decoded data not yet shown */
    bool haveLast = false;
    u32 lastSeq = 0, baseSeq = 0;
    u64 lastPresent = 0;

    while (g_running)
    {
        ssize_t n = recvfrom(g_videoFd, buf, sizeof(buf), 0, NULL, NULL);
        if (n <= 0)
        {
            /* safety net: never freeze, even if consistency cannot be reached */
            if (frameOpen && osGetTime() - lastPresent > 250)
            {
                presentFrame();
                frameOpen = false;
                lastPresent = osGetTime();
            }
            svcSleepThread(500000); /* 0.5 ms */
            continue;
        }
        if ((size_t)n < sizeof(LinkFramePacket)) continue;
        LinkFramePacket fp;
        memcpy(&fp, buf, sizeof(fp));
        if (fp.magic != LINK_FRAME_MAGIC) continue;

        if (fp.frameId != curFrame)
        {
            /* a frame that never became consistent is simply not shown;
               its lost parts are repeated in a later frame */
            curFrame = fp.frameId;
            haveLast = false;
        }
        if (lastPresent == 0) lastPresent = osGetTime();
        frameOpen = true;

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
        markReceived(fp.seq);
        g_lastVideoTime = osGetTime();

        if (fp.flags & LINK_FRAME_LAST)
        {
            haveLast = true;
            lastSeq = fp.seq;
            baseSeq = fp.baseSeq;
        }
        if (haveLast && haveAll(baseSeq, lastSeq))
        {
            presentFrame();
            frameOpen = false;
            haveLast = false;
            lastPresent = osGetTime();
        }
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
    u32 prevHeld = 0;

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

        /* C-stick down opens/closes the game list */
        u32 down = held & ~prevHeld;
        prevHeld = held;
        if (down & KEY_CSTICK_DOWN)
        {
            if (!g_menuOpen) g_menuOpen = true;
            else if (g_gameStarted) g_menuOpen = false;
        }
        else if (g_menuOpen)
        {
            LightLock_Lock(&g_lock);
            g_menuKeys |= down;
            LightLock_Unlock(&g_lock);
        }
        /* while the list is open, the game gets no input */
        u32 gameKeys = g_menuOpen ? 0 : held;

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
        bool dsTouch = (gameKeys & KEY_TOUCH) && mapTouch(touch.px, touch.py, g_zoom, &dsX, &dsY);

        if (havePeer)
        {
            LinkInputPacket p;
            memset(&p, 0, sizeof(p));
            p.magic = LINK_MAGIC;
            p.id = packetId++;
            p.keys = gameKeys;
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
        /* 256x192 -> 320x240, nearest neighbour, with precomputed source rows/columns */
        static u16 srcRow[240];   /* framebuffer y (rotated) -> DS image row offset */
        static u8 srcCol[320];
        static bool init = false;
        if (!init)
        {
            for (int y = 0; y < 240; y++) srcRow[239 - y] = (u16)((y * 4 / 5) * 256);
            for (int x = 0; x < 320; x++) srcCol[x] = (u8)(x * 4 / 5);
            init = true;
        }
        if (!haveVideo)
        {
            for (int i = 0; i < 320 * 240; i++) fb[i] = empty;
        }
        else
        {
            for (int x = 0; x < 320; x++)
            {
                const u16* src = g_dsImage + srcCol[x];
                u16* col = fb + x * 240;
                for (int j = 0; j < 240; j++) col[j] = src[srcRow[j]];
            }
        }
    }
    else
    {
        fillRect(fb, 0, 0, 320, 240, bg);
        fillRect(fb, LINK_DS_OFFSET_X - 2, LINK_DS_OFFSET_Y - 2, 260, 196, frame);
        for (int x = 0; x < 256; x++)
        {
            u16* col = fb + (x + LINK_DS_OFFSET_X) * 240 + (239 - LINK_DS_OFFSET_Y);
            const u16* src = g_dsImage + x;
            for (int y = 0; y < 192; y++)
                col[-y] = haveVideo ? src[y * 256] : empty;
        }
    }

    /* without a picture, show where the finger is */
    if (!haveVideo && touching)
        fillRect(fb, tx - 2, ty - 2, 5, 5, LINK_RGB565(0xE0, 0x60, 0x60));
}

/* ------------------------------------------------------------------ */
/* top screen text: only redraw lines that changed (the console draws  */
/* in software; redrawing everything 60x a second starved the network)  */

#define CON_ROWS 31
static char g_conCache[CON_ROWS][160];

static void setLine(int row, const char* fmt, ...)
{
    if (row < 1 || row >= CON_ROWS) return;
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (strcmp(buf, g_conCache[row]) == 0) return;
    snprintf(g_conCache[row], sizeof(g_conCache[row]), "%s", buf);
    printf("\x1b[%d;1H%s\x1b[0m\x1b[K", row, buf);
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
        char dir[128];
        if (sscanf(line, "zoom=%d", &v) == 1) g_zoom = v != 0;
        else if (strncmp(line, "last=", 5) == 0)
        {
            snprintf(g_lastGame, sizeof(g_lastGame), "%s", line + 5);
            g_lastGame[strcspn(g_lastGame, "\r\n")] = '\0';
        }
        else if (sscanf(line, "romdir=%127[^\r\n]", dir) == 1 && dir[0] == '/')
        {
            size_t n = strlen(dir);
            while (n > 1 && dir[n - 1] == '/') dir[--n] = '\0';
            snprintf(g_romDir, sizeof(g_romDir), "%s", dir);
        }
    }
    fclose(f);
}

static void saveConfig(void)
{
    FILE* f = fopen(CONFIG_PATH, "w");
    if (!f) return;
    fprintf(f, "zoom=%d\n", g_zoom ? 1 : 0);
    fprintf(f, "romdir=%s\n", g_romDir);
    if (g_lastGame[0]) fprintf(f, "last=%s\n", g_lastGame);
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
    loadFavs();
    scanGames();

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
    {
        /* allow restarting the app right away (port may still be in TIME_WAIT) */
        int one = 1;
        setsockopt(g_listenFd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    }
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
    /* lower number = higher priority. The network threads mostly sleep in
       poll/recv, so they can safely go before the UI (main thread). */
    Thread inp = threadCreate(inputThread, NULL, 0x4000, prio - 2, -2, false);
    Thread vid = threadCreate(videoThread, NULL, 0x4000, prio - 1, -2, false);
    Thread srv = threadCreate(serverThread, NULL, 0x4000, prio - 1, -2, false);

    u32 lastVideoFrames = (u32)-1;
    bool lastZoom = !g_zoom, lastHaveVideo = true;
    bool savedZoom = g_zoom;
    int menuCursor = 0, menuTop = 0;
    if (g_lastGame[0]) { int i = findGame(g_lastGame); if (i >= 0) menuCursor = i; }
    const int LIST_ROWS = 14;
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

        /* game list input */
        u32 mk;
        LightLock_Lock(&g_lock);
        mk = g_menuKeys;
        g_menuKeys = 0;
        LightLock_Unlock(&g_lock);
        if (g_menuOpen && mk)
        {
            if (mk & (KEY_DUP | KEY_CPAD_UP)) menuCursor--;
            if (mk & (KEY_DDOWN | KEY_CPAD_DOWN)) menuCursor++;
            if (mk & (KEY_DLEFT | KEY_L)) menuCursor -= LIST_ROWS;
            if (mk & (KEY_DRIGHT | KEY_R)) menuCursor += LIST_ROWS;
            if (menuCursor >= g_gameCount) menuCursor = g_gameCount - 1;
            if (menuCursor < 0) menuCursor = 0;
            if ((mk & (KEY_X | KEY_Y)) && g_gameCount > 0)
            {
                char keep[256];
                snprintf(keep, sizeof(keep), "%s", g_games[menuCursor]);
                if (mk & KEY_X) { toggleFav(keep); sortGames(); }
                if (mk & KEY_Y) scanGames();
                int i = findGame(keep);
                if (i >= 0) menuCursor = i;
            }
            if ((mk & KEY_A) && g_gameCount > 0)
            {
                requestLaunch(menuCursor);
                saveConfig();      /* remembers the last game */
            }
            else if ((mk & KEY_B) && g_gameStarted) g_menuOpen = false;
        }

        /* top screen: status */
        char status[128], peer[32], game[128], xi[64];
        LightLock_Lock(&g_lock);
        snprintf(status, sizeof(status), "%s", g_statusText);
        snprintf(peer, sizeof(peer), "%s", g_peerName);
        snprintf(game, sizeof(game), "%s", g_currentGame);
        snprintf(xi, sizeof(xi), "%s", g_xferInfo);
        LightLock_Unlock(&g_lock);

        setLine(3, "IP-Adresse:  \x1b[33m%s", inet_ntoa(ip));
        setLine(4, "melonDS:     %s", g_tcpConnected ? "\x1b[32mverbunden" : "\x1b[90mwartet...");
        setLine(5, "Steuerung:   %s\x1b[0m   Bild: %s",
                in.haveUdpPeer ? "\x1b[32maktiv" : "\x1b[90m-",
                haveVideo ? "\x1b[32mok" : "\x1b[90m-");
        setLine(6, "Anzeige:     %s", zoom ? "Vergroessert" : "Original (1:1)");
        setLine(7, "\x1b[90m%.49s", xi);
        setLine(8, "\x1b[33m%.49s", status);

        /* top screen: game list or current game */
        if (g_menuOpen)
        {
            setLine(10, "\x1b[36mSpiel waehlen\x1b[0m (%d in %.30s)", g_gameCount, g_romDir);
            if (menuCursor < menuTop) menuTop = menuCursor;
            if (menuCursor >= menuTop + LIST_ROWS) menuTop = menuCursor - LIST_ROWS + 1;
            for (int r = 0; r < LIST_ROWS; r++)
            {
                int i = menuTop + r;
                if (i < g_gameCount)
                {
                    char name[64];
                    snprintf(name, sizeof(name), "%s", g_games[i]);
                    size_t n = strlen(name);
                    if (n > 4 && strcasecmp(name + n - 4, ".nds") == 0) name[n - 4] = '\0';
                    bool fav = isFav(g_games[i]);
                    if (i == menuCursor) setLine(11 + r, "\x1b[47;30m>%c%-47.47s", fav ? '*' : ' ', name);
                    else if (fav) setLine(11 + r, "\x1b[33m*\x1b[0m %-47.47s", name);
                    else setLine(11 + r, "  %-47.47s", name);
                }
                else if (r == 0 && g_gameCount == 0)
                    setLine(11 + r, "\x1b[31m  Keine .nds-Dateien gefunden.");
                else setLine(11 + r, "");
            }
            setLine(26, "\x1b[90mA starten  X Favorit  Y neu laden%s", g_gameStarted ? "  B zurueck" : "");
        }
        else
        {
            setLine(10, "\x1b[36mLaeuft:\x1b[0m %.40s", game);
            for (int r = 0; r < LIST_ROWS; r++) setLine(11 + r, "");
            setLine(26, "");
        }

        setLine(27, "\x1b[90mC-Stick runter = Spieleliste");
        setLine(28, "\x1b[90mC-Stick hoch = Original / Vergroessert");
        setLine(29, "\x1b[90mIn melonDS: 3DS Link > Mit 3DS verbinden");
        setLine(30, "\x1b[90mSELECT + START halten = Beenden");

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
