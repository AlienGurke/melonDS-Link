/*
    Texture upscaling filters, see GPU3D_TexFilter.h
*/

#include "GPU3D_TexFilter.h"

#include "texfilter/xbrz/xbrz.h"
extern "C"
{
#include "texfilter/mmpx/mmpx.h"
}

namespace melonDS
{

thread_local std::vector<u32> TexFilter::Padded, TexFilter::Tmp, TexFilter::Big;

namespace
{

// the texture cache stores R, G, B with 6 bits and alpha with 5 bits, one per byte
inline u32 toARGB8(u32 v)
{
    u32 r = v & 0x3F, g = (v >> 8) & 0x3F, b = (v >> 16) & 0x3F, a = (v >> 24) & 0x1F;
    r = (r << 2) | (r >> 4);
    g = (g << 2) | (g >> 4);
    b = (b << 2) | (b >> 4);
    a = (a * 255 + 15) / 31;
    return (a << 24) | (r << 16) | (g << 8) | b;
}

inline u32 fromARGB8(u32 c, bool binaryAlpha)
{
    u32 a = c >> 24, r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
    r = (r * 63 + 127) / 255;
    g = (g * 63 + 127) / 255;
    b = (b * 63 + 127) / 255;
    // textures that were either opaque or fully transparent stay like that:
    // in-between alpha would make the DS hardware blend those pixels
    a = binaryAlpha ? (a >= 128 ? 31 : 0) : (a * 31 + 127) / 255;
    return r | (g << 8) | (b << 16) | (a << 24);
}

}

u32 TexFilter::ScaleFor(const TexFilterSettings& s, u32 width, u32 height)
{
    if (s.Mode == texFilter_Off) return 1;
    u32 scale;
    if (s.Mode == texFilter_MMPX) scale = s.Scale >= 4 ? 4 : 2;   // MMPX works in steps of 2x
    else scale = s.Scale < 2 ? 2 : s.Scale > 6 ? 6 : s.Scale;
    // keep big textures in check (at most 1024x1024 texels = 4 MB each)
    while (scale > 1 && (u64)width * scale * height * scale > 1024u * 1024u)
        scale = s.Mode == texFilter_MMPX ? scale / 2 : scale - 1;
    return scale < 2 ? 1 : scale;
}

bool TexFilter::Wants(const TexFilterSettings& s, const u32* tex, u32 width, u32 height)
{
    if (s.Mode == texFilter_Off) return false;
    if (!s.ObjectsOnly) return true;
    // characters, Pokemon, trees... are drawn with transparent surroundings,
    // ground, walls and buildings mostly not
    bool clear = false, solid = false;
    for (u32 i = 0; i < width * height; i++)
    {
        if (tex[i] >> 24) solid = true;
        else clear = true;
        if (clear && solid) return true;
    }
    return false;
}

void TexFilter::Upscale(const TexFilterSettings& s, const u32* in, u32 width, u32 height, u32 scale, u32* out)
{
    const u32 n = width * height;
    bool transparent = false, binaryAlpha = true;
    for (u32 i = 0; i < n; i++)
    {
        u32 a = in[i] >> 24;
        if (a == 0) transparent = true;
        else if (a != 31) binaryAlpha = false;
    }

    // a border around the texture, so the filters see the right neighbours at
    // the edges: repeated for opaque textures (they tile, e.g. the ground),
    // continued edge pixels for textures with transparency (single objects)
    const int P = 4;
    const int pw = (int)width + 2 * P, ph = (int)height + 2 * P;
    Padded.resize((size_t)pw * ph);
    for (int y = 0; y < ph; y++)
    {
        int sy = y - P;
        if (transparent) sy = sy < 0 ? 0 : sy >= (int)height ? (int)height - 1 : sy;
        else sy = ((sy % (int)height) + (int)height) % (int)height;
        for (int x = 0; x < pw; x++)
        {
            int sx = x - P;
            if (transparent) sx = sx < 0 ? 0 : sx >= (int)width ? (int)width - 1 : sx;
            else sx = ((sx % (int)width) + (int)width) % (int)width;
            Padded[(size_t)y * pw + x] = toARGB8(in[(size_t)sy * width + sx]);
        }
    }

    const int bw = pw * (int)scale, bh = ph * (int)scale;
    Big.resize((size_t)bw * bh);
    if (s.Mode == texFilter_XBRZ)
    {
        xbrz::scale(scale, Padded.data(), Big.data(), pw, ph, xbrz::ColorFormat::ARGB);
    }
    else if (scale == 4)
    {
        Tmp.resize((size_t)pw * 2 * ph * 2);
        mmpx_scale2x(Padded.data(), Tmp.data(), pw, ph);
        mmpx_scale2x(Tmp.data(), Big.data(), pw * 2, ph * 2);
    }
    else
    {
        mmpx_scale2x(Padded.data(), Big.data(), pw, ph);
    }

    const u32 ow = width * scale, oh = height * scale;
    for (u32 y = 0; y < oh; y++)
    {
        const u32* src = &Big[(size_t)(y + P * scale) * bw + P * scale];
        u32* dst = &out[(size_t)y * ow];
        for (u32 x = 0; x < ow; x++)
            dst[x] = fromARGB8(src[x], binaryAlpha);
    }
}

}
