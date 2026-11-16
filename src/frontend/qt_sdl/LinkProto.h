/*
    melonDS Link protocol
    Shared between the melonDS frontend (PC) and the link3ds homebrew (3DS).

    Plain C so both sides can include it. Everything is little-endian,
    which both x86/ARM64 PCs and the 3DS are.

    Transport:
      - TCP (LINK_TCP_PORT on the 3DS): request/response control channel.
        Every message starts with LinkHeader followed by `len` payload bytes.
        Responses echo `cmd | LINK_RESP` and `seq`, and carry a LinkStatus.
      - UDP (LINK_UDP_PORT on the 3DS): input stream 3DS -> PC.
        The PC sends one LINK_UDP_HELLO datagram from the socket it wants
        to receive on; the 3DS then streams LinkInputPacket to that address.
      - UDP (LINK_VIDEO_PORT on the 3DS): DS bottom screen PC -> 3DS as
        changed 8x8 tiles (RGB565). Lost tiles heal through a rolling refresh.
*/

#ifndef MELONDS_LINKPROTO_H
#define MELONDS_LINKPROTO_H

#include <stdint.h>

#define LINK_MAGIC          0x4B4C444Du /* "MDLK" */
#define LINK_VERSION        6u
#define LINK_TCP_PORT       6543
#define LINK_UDP_PORT       6544
#define LINK_VIDEO_PORT     6545   /* UDP, PC -> 3DS: bottom screen tiles */

#define LINK_RESP           0x8000u
#define LINK_MAX_PAYLOAD    (256u * 1024u)
#define LINK_MAX_PATH       256u

/* TCP commands */
enum LinkCommand
{
    LINK_CMD_HELLO      = 1, /* in: u32 version            out: LinkHelloResp      */
    LINK_CMD_PING       = 2, /* in: -                      out: -                  */
    LINK_CMD_STAT       = 3, /* in: path                   out: LinkStatResp       */
    LINK_CMD_READ       = 4, /* in: LinkReadReq + path     out: data               */
    LINK_CMD_WRITE      = 5, /* in: LinkWriteReq + path + data   out: -            */
    LINK_CMD_SETSIZE    = 6, /* in: LinkSetSizeReq + path  out: -                  */
    LINK_CMD_LISTDIR    = 7, /* in: path                   out: LinkDirEntry list  */
    LINK_CMD_STATUSTEXT = 8, /* in: utf-8 text (shown on the 3DS)  out: -          */
    LINK_CMD_POLLEVENT  = 9, /* in: -   out: LinkEvent + paths, or LINK_ERR_NOTFOUND */
    LINK_CMD_REPLACE    = 10,/* in: u16 fromLen + from + to: atomically replace `to`
                                with `from` (to may exist)  out: -                */
};

/* Events the 3DS has for melonDS (fetched with LINK_CMD_POLLEVENT) */
enum LinkEventType
{
    LINK_EVENT_LAUNCH = 1,   /* the user picked a game on the 3DS */
};

enum LinkStatus
{
    LINK_OK              = 0,
    LINK_ERR_BADCMD      = 1,
    LINK_ERR_BADARGS     = 2,
    LINK_ERR_NOTFOUND    = 3,
    LINK_ERR_IO          = 4,
    LINK_ERR_DENIED      = 5,
    LINK_ERR_VERSION     = 6,
};

#if defined(_MSC_VER)
#pragma pack(push, 1)
#define LINK_PACKED
#else
#define LINK_PACKED __attribute__((packed))
#endif

typedef struct LINK_PACKED LinkHeader
{
    uint32_t magic;
    uint16_t cmd;
    uint16_t status;
    uint32_t seq;
    uint32_t len;
} LinkHeader;

typedef struct LINK_PACKED LinkHelloResp
{
    uint32_t version;
    uint16_t udpPort;
    uint16_t isNew3DS;
    char     name[24];
} LinkHelloResp;

typedef struct LINK_PACKED LinkStatResp
{
    uint64_t size;
    uint32_t isDir;
    uint32_t reserved;
} LinkStatResp;

typedef struct LINK_PACKED LinkReadReq
{
    uint64_t offset;
    uint32_t length;   /* <= LINK_MAX_PAYLOAD */
    /* followed by path (no terminator needed, length = header.len - 12) */
} LinkReadReq;

typedef struct LINK_PACKED LinkWriteReq
{
    uint64_t offset;
    uint16_t pathLen;
    uint16_t flags;    /* LINK_WRITE_CREATE */
    /* followed by pathLen bytes path, then data */
} LinkWriteReq;

#define LINK_WRITE_CREATE   1u

typedef struct LINK_PACKED LinkSetSizeReq
{
    uint64_t size;
    /* followed by path */
} LinkSetSizeReq;

typedef struct LINK_PACKED LinkEvent
{
    uint32_t type;
    uint32_t reserved;
    uint64_t romSize;
    uint16_t romPathLen;     /* followed by romPathLen bytes ROM path (SD card) */
    uint16_t savePathLen;    /* then savePathLen bytes save path (may not exist yet) */
} LinkEvent;

typedef struct LINK_PACKED LinkDirEntry
{
    uint64_t size;
    uint8_t  isDir;
    uint8_t  nameLen;
    /* followed by nameLen bytes name */
} LinkDirEntry;

/* UDP */
#define LINK_UDP_HELLO      0x4F4C4548u /* "HELO" */

/* 3DS HID bits we forward. The low 12 bits match the DS key layout 1:1
   (A B Select Start Right Left Up Down R L X Y). */
#define LINK_KEY_ZL         (1u << 14)
#define LINK_KEY_ZR         (1u << 15)
#define LINK_KEY_TOUCH      (1u << 20)
#define LINK_KEY_CSTICK_RIGHT (1u << 24)
#define LINK_KEY_CSTICK_LEFT  (1u << 25)
#define LINK_KEY_CSTICK_UP    (1u << 26)
#define LINK_KEY_CSTICK_DOWN  (1u << 27)
#define LINK_KEY_CPAD_RIGHT (1u << 28)
#define LINK_KEY_CPAD_LEFT  (1u << 29)
#define LINK_KEY_CPAD_UP    (1u << 30)
#define LINK_KEY_CPAD_DOWN  (1u << 31)

/* Original mode: the DS bottom screen (256x192) is shown 1:1, centered,
   on the 3DS bottom screen (320x240). Zoom mode: scaled to fill 320x240
   (same 4:3 ratio, factor 1.25). Raw touch is sent too, for reference. */
#define LINK_DS_OFFSET_X    32
#define LINK_DS_OFFSET_Y    24

typedef struct LINK_PACKED LinkInputPacket
{
    uint32_t magic;
    uint32_t id;       /* increments every packet */
    uint32_t keys;     /* hidKeysHeld() */
    int16_t  cpadX, cpadY;
    uint16_t touchX, touchY;
    int16_t  cstickX, cstickY;
    uint16_t dsTouchX, dsTouchY;   /* touch already mapped to DS space (256x192) */
    uint32_t flags;                /* LINK_INPUT_* */
    /* video receipt: highest video packet seq received, and a bitmask of
       which of the 64 seqs up to it arrived (bit n = videoAckSeq - n) */
    uint32_t videoAckSeq;
    uint32_t reserved2;
    uint64_t videoAckMask;
} LinkInputPacket;

#define LINK_INPUT_DSTOUCH  1u     /* dsTouchX/Y valid: the DS screen is being touched */
#define LINK_INPUT_ZOOM     2u     /* 3DS shows the DS screen enlarged */
#define LINK_INPUT_VIDEOACK 4u     /* videoAckSeq/Mask valid */

/* Video: the DS bottom screen is split into 32x24 tiles of 8x8 RGB565 pixels.
   Each entry starts with a u16: bits 0-9 tile index, bits 13-15 tile type.
     RAW    : 64 u16 pixels, row-major                         (128 bytes)
     SOLID  : 1 u16 colour                                     (2 bytes)
     PAL2   : 2 u16 colours + 8 bytes, 1 bit/pixel, row y = byte y,
              pixel x = bit x                                  (12 bytes)
     PAL4   : 4 u16 colours + 16 bytes, 2 bits/pixel, row y = u16 y (LE),
              pixel x = bits 2x..2x+1                          (24 bytes)
     PAL16  : 16 u16 colours + 32 bytes, 4 bits/pixel, row y = u32 y (LE),
              pixel x = bits 4x..4x+3                          (64 bytes)
     HALF   : 16 u16 pixels (4x4, row-major), each covering 2x2 pixels.
              Lossy quick preview; the exact tile follows later (32 bytes)
     QUARTER: 4 u16 pixels (2x2, row-major), each covering 4x4 pixels.
              Coarsest preview, used only when even HALF does not fit (8 bytes)

   Frames: all packets of one update share frameId. `flags` holds the packet
   index within the frame (bits 1-15) and LINK_FRAME_LAST on the final packet.
   baseSeq: all packets before it were confirmed, or were lost and their tiles
   are repeated in this frame. So once every packet from baseSeq to the end of
   this frame has arrived, the 3DS has a consistent picture and may show it. */
#define LINK_TILE_INDEX_MASK 0x03FFu
#define LINK_TILE_TYPE_SHIFT 13
#define LINK_TILE_RAW       0u
#define LINK_TILE_SOLID_T   1u
#define LINK_TILE_PAL2      2u
#define LINK_TILE_PAL4      3u
#define LINK_TILE_PAL16     4u
#define LINK_TILE_HALF      5u
#define LINK_TILE_QUARTER   6u

#define LINK_FRAME_LAST     1u
#define LINK_FRAME_INDEX_SHIFT 1
#define LINK_FRAME_MAGIC    0x52464D4Du /* "MDFR" */
#define LINK_TILES_X        32
#define LINK_TILES_Y        24
#define LINK_TILE_COUNT     (LINK_TILES_X * LINK_TILES_Y)
#define LINK_FRAME_MAX_BYTES 1400

typedef struct LINK_PACKED LinkFramePacket
{
    uint32_t magic;
    uint32_t frameId;
    uint32_t seq;          /* increments per packet, for receipts */
    uint32_t baseSeq;      /* show this frame only if every packet from baseSeq
                              up to the frame's last packet has arrived */
    uint16_t entryCount;
    uint16_t flags;
    /* followed by entries */
} LinkFramePacket;

#if defined(_MSC_VER)
#pragma pack(pop)
#endif

#ifdef __cplusplus
static_assert(sizeof(LinkHeader) == 16, "LinkHeader size");
static_assert(sizeof(LinkInputPacket) == 48, "LinkInputPacket size");
static_assert(sizeof(LinkFramePacket) == 20, "LinkFramePacket size");
#endif

#endif
