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
*/

#ifndef MELONDS_LINKPROTO_H
#define MELONDS_LINKPROTO_H

#include <stdint.h>

#define LINK_MAGIC          0x4B4C444Du /* "MDLK" */
#define LINK_VERSION        1u
#define LINK_TCP_PORT       6543
#define LINK_UDP_PORT       6544

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

/* The DS bottom screen (256x192) is shown 1:1, centered, on the 3DS
   bottom screen (320x240). Touch coordinates are sent raw (3DS space). */
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
    uint32_t reserved[2];
} LinkInputPacket;

#if defined(_MSC_VER)
#pragma pack(pop)
#endif

#ifdef __cplusplus
static_assert(sizeof(LinkHeader) == 16, "LinkHeader size");
static_assert(sizeof(LinkInputPacket) == 32, "LinkInputPacket size");
#endif

#endif
