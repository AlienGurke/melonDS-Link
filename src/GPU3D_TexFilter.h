/*
    Texture upscaling filters for the OpenGL renderers (melonDS Link fork).

    The texture cache decodes every DS texture once. With a filter switched on,
    the decoded texture is magnified (MMPX or xBRZ) before it goes to the GPU.
    The renderers sample textures with normalised coordinates, so a bigger
    texture needs no shader changes: the filtered version simply shows up
    wherever the texture is drawn.
*/

#ifndef GPU3D_TEXFILTER_H
#define GPU3D_TEXFILTER_H

#include <vector>

#include "types.h"

namespace melonDS
{

enum
{
    texFilter_Off = 0,
    texFilter_MMPX,
    texFilter_XBRZ,
};

struct TexFilterSettings
{
    int Mode = texFilter_Off;
    int Scale = 4;              // 2 or 4 (xBRZ also 3, 5, 6)
    bool ObjectsOnly = false;   // only textures with transparent pixels (characters, Pokemon, trees...)

    bool operator==(const TexFilterSettings& o) const
    {
        return Mode == o.Mode && Scale == o.Scale && ObjectsOnly == o.ObjectsOnly;
    }
    bool operator!=(const TexFilterSettings& o) const { return !(*this == o); }
};

class TexFilter
{
public:
    // the factor a texture of this size gets (1 = stays as it is). Depends only
    // on the size, so all textures of one size share the same storage size.
    static u32 ScaleFor(const TexFilterSettings& s, u32 width, u32 height);

    // whether this texture (RGB6A5, as decoded by the texture cache) is filtered
    static bool Wants(const TexFilterSettings& s, const u32* tex, u32 width, u32 height);

    // magnify `in` (width x height, RGB6A5) by `scale` into `out`
    // ((width*scale) x (height*scale), RGB6A5)
    static void Upscale(const TexFilterSettings& s, const u32* in, u32 width, u32 height, u32 scale, u32* out);

private:
    static thread_local std::vector<u32> Padded, Tmp, Big;
};

}

#endif
