// Copyright melonDS-Link contributors
// Licensed under GPLv3 or any later version
// Refer to the LICENSE.txt file included.

#include <cstring>
#include "LinkH264.h"

#ifdef LINK_HAVE_OPENH264
#include <wels/codec_api.h>
#endif

namespace Link {

#ifdef LINK_HAVE_OPENH264

struct H264Encoder::Impl {
    ISVCEncoder* enc = nullptr;
    int width = 0, height = 0;
};

H264Encoder::H264Encoder() : impl(std::make_unique<Impl>()) {}

H264Encoder::~H264Encoder() {
    Close();
}

bool H264Encoder::Available() {
    return true;
}

bool H264Encoder::Open(int width, int height, int fps, int bitrate) {
    Close();
    if (WelsCreateSVCEncoder(&impl->enc) != 0 || !impl->enc) {
        impl->enc = nullptr;
        return false;
    }
    SEncParamExt p;
    impl->enc->GetDefaultParams(&p);
    // camera mode: the screen-content mode makes a key frame at every bigger change
    // (loading, fades, new menus), which floods the WiFi and stalls the picture
    p.iUsageType = CAMERA_VIDEO_REAL_TIME;
    p.iPicWidth = width;
    p.iPicHeight = height;
    p.iTargetBitrate = bitrate;
    p.iMaxBitrate = UNSPECIFIED_BIT_RATE;
    p.iRCMode = RC_BITRATE_MODE;
    p.fMaxFrameRate = static_cast<float>(fps);
    p.iTemporalLayerNum = 1;
    p.iSpatialLayerNum = 1;
    p.uiIntraPeriod = 0; // key frames only when asked (start, lost packets)
    p.bEnableFrameSkip = true;
    p.iMultipleThreadIdc = 1;
    p.iEntropyCodingModeFlag = 0; // CAVLC (baseline)
    p.bEnableDenoise = false;
    p.bEnableBackgroundDetection = true;
    p.bEnableAdaptiveQuant = false;
    p.bEnableLongTermReference = false;
    p.bEnableSceneChangeDetect = false; // a fade or a new screen is no reason for a big key frame
    p.iNumRefFrame = 1;
    p.eSpsPpsIdStrategy = CONSTANT_ID;
    p.bPrefixNalAddingCtrl = false;
    p.iComplexityMode = MEDIUM_COMPLEXITY;
    p.iMinQp = 8;
    p.iMaxQp = 26; // fine detail (text) stays sharp: never coarser than this
    SSpatialLayerConfig& l = p.sSpatialLayers[0];
    l.iVideoWidth = width;
    l.iVideoHeight = height;
    l.fFrameRate = static_cast<float>(fps);
    l.iSpatialBitrate = bitrate;
    l.iMaxSpatialBitrate = UNSPECIFIED_BIT_RATE;
    l.uiProfileIdc = PRO_BASELINE;
    l.sSliceArgument.uiSliceMode = SM_SINGLE_SLICE;
    // TV range: the 3DS hardware decoder always converts with 1.164 (Y - 16) ... (measured
    // with LINK_CMD_VIDEOPROBE; full range came out ~16 % too bright)
    l.bVideoSignalTypePresent = true;
    l.uiVideoFormat = 5; // unspecified
    l.bFullRange = false; // TV range: what the 3DS decoder expects (see ToRotatedI420)
    l.bColorDescriptionPresent = true;
    l.uiColorPrimaries = 6;          // smpte170m (BT.601)
    l.uiTransferCharacteristics = 6; // smpte170m
    l.uiColorMatrix = 6;             // smpte170m
    if (impl->enc->InitializeExt(&p) != 0) {
        Close();
        return false;
    }
    int format = videoFormatI420;
    impl->enc->SetOption(ENCODER_OPTION_DATAFORMAT, &format);
    impl->width = width;
    impl->height = height;
    return true;
}

void H264Encoder::Close() {
    if (impl->enc) {
        impl->enc->Uninitialize();
        WelsDestroySVCEncoder(impl->enc);
        impl->enc = nullptr;
    }
}

bool H264Encoder::IsOpen() const {
    return impl->enc != nullptr;
}

void H264Encoder::SetBitrate(int bitrate) {
    if (!impl->enc) {
        return;
    }
    SBitrateInfo info{};
    info.iLayer = SPATIAL_LAYER_ALL;
    info.iBitrate = bitrate;
    impl->enc->SetOption(ENCODER_OPTION_BITRATE, &info);
}

bool H264Encoder::Encode(const uint8_t* i420, bool force_key, int64_t time_ms,
                         std::vector<uint8_t>& out, bool& is_key) {
    out.clear();
    is_key = false;
    if (!impl->enc) {
        return false;
    }
    if (force_key) {
        impl->enc->ForceIntraFrame(true);
    }
    const int w = impl->width, h = impl->height;
    SSourcePicture pic{};
    pic.iColorFormat = videoFormatI420;
    pic.iPicWidth = w;
    pic.iPicHeight = h;
    pic.iStride[0] = w;
    pic.iStride[1] = pic.iStride[2] = w / 2;
    pic.pData[0] = const_cast<uint8_t*>(i420);
    pic.pData[1] = pic.pData[0] + w * h;
    pic.pData[2] = pic.pData[1] + (w / 2) * (h / 2);
    pic.uiTimeStamp = time_ms;
    SFrameBSInfo info{};
    if (impl->enc->EncodeFrame(&pic, &info) != cmResultSuccess) {
        return false;
    }
    if (info.eFrameType == videoFrameTypeSkip || info.eFrameType == videoFrameTypeInvalid) {
        return true;
    }
    for (int i = 0; i < info.iLayerNum; i++) {
        const SLayerBSInfo& layer = info.sLayerInfo[i];
        int size = 0;
        for (int n = 0; n < layer.iNalCount; n++) {
            size += layer.pNalLengthInByte[n];
        }
        out.insert(out.end(), layer.pBsBuf, layer.pBsBuf + size);
    }
    is_key = info.eFrameType == videoFrameTypeIDR;
    return true;
}

#else // no OpenH264 in this build

struct H264Encoder::Impl {};
H264Encoder::H264Encoder() : impl(std::make_unique<Impl>()) {}
H264Encoder::~H264Encoder() = default;
bool H264Encoder::Available() {
    return false;
}
bool H264Encoder::Open(int, int, int, int) {
    return false;
}
void H264Encoder::Close() {}
bool H264Encoder::IsOpen() const {
    return false;
}
void H264Encoder::SetBitrate(int) {}
bool H264Encoder::Encode(const uint8_t*, bool, int64_t, std::vector<uint8_t>& out, bool& is_key) {
    out.clear();
    is_key = false;
    return false;
}

#endif

} // namespace Link
