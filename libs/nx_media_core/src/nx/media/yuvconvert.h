// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include <array>
#include <vector>

#include <QtCore/qtypes.h>

static const unsigned int X_STRIDE_FOR_SSE_CONVERT_UTILS = 32;

// All the following functions use SSE on x86, but have stub implementations for ARMs, thus, can be
// compiled on any platform but should be called only on x64.

/** ATTENTION: Despite its name, this function actually converts to BGRA. */
NX_MEDIA_CORE_API void yuv444_argb32_simd_intr(
    unsigned char* dst,
    const unsigned char* py,
    const unsigned char* pu,
    const unsigned char* pv,
    const unsigned int width,
    const unsigned int height,
    const unsigned int dst_stride,
    const unsigned int y_stride,
    const unsigned int uv_stride,
    quint8 alpha);

/** ATTENTION: Despite its name, this function actually converts to BGRA. */
NX_MEDIA_CORE_API void yuv422_argb32_simd_intr(
    unsigned char* dst,
    const unsigned char* py,
    const unsigned char* pu,
    const unsigned char* pv,
    const unsigned int width,
    const unsigned int height,
    const unsigned int dst_stride,
    const unsigned int y_stride,
    const unsigned int uv_stride,
    quint8 alpha);

/** ATTENTION: Despite its name, this function actually converts to BGRA. */
NX_MEDIA_CORE_API void yuv420_argb32_simd_intr(
    unsigned char* dst,
    const unsigned char* py,
    const unsigned char* pu,
    const unsigned char* pv,
    const unsigned int width,
    const unsigned int height,
    const unsigned int dst_stride,
    const unsigned int y_stride,
    const unsigned int uv_stride,
    quint8 alpha);

NX_MEDIA_CORE_API void bgra_yuv420(
    quint8* rgba,
    quint8* yptr,
    quint8* uptr,
    quint8* vptr,
    int width,
    int height,
    bool flip);
/*!
    \param xStride Line length in \a rgba buffer. MUST be multiple of 32
    \param yStride Line length in \a y
    \param uvStride Line length in \a u and \a v buffers
*/
NX_MEDIA_CORE_API void bgra_to_yv12_simd_intr(
    const quint8* rgba,
    int xStride,
    quint8* y,
    quint8* u,
    quint8* v,
    int yStride,
    int uvStride,
    int width,
    int height,
    bool flip);
//! Converts bgra to yuv420 with alpha plane, total 20 bits per pixel (Y - 8bit, A - 8bit, UV)
/*!
    \param xStride Line length in \a rgba buffer. MUST be multiple of 32
    \param yStride Line length in \a y
    \param uvStride Line length in \a u and \a v buffers
*/
NX_MEDIA_CORE_API void bgra_to_yva12_simd_intr(
    const quint8* rgba,
    int xStride,
    quint8* y,
    quint8* u,
    quint8* v,
    quint8* a,
    int yStride,
    int uvStride,
    int aStride,
    int width,
    int height,
    bool flip);

struct Shuffle16
{
    std::vector<std::array<uint8_t, 16>> masks;
    std::vector<int> offsets;
};

struct NX_MEDIA_CORE_API YuvToRgbScaleContext
{
    int srcWidth = 0;
    int srcHeight = 0;
    int dstWidth = 0; ///< Logical destination width — drives the scale factor.
    int dstHeight = 0;
    int dstLineSize = 0; ///< Destination row pitch in PIXELS.
    int srcFormat = 0;

    std::vector<Shuffle16> xShufleY;
    std::vector<Shuffle16> xShufleUV;

    std::vector<int> lineStepY;
    std::vector<int> lineStepUV;

    /**
     * @param dstWidth Logical output width — used for the scale factor
     *     `xStep = srcWidth / dstWidth`. Need not be 16-aligned. Each
     *     row the SIMD kernels write dstWidth rounded UP to a whole
     *     16-pixel block (the last block's lanes past dstWidth are
     *     zeroed), then re-anchor to the next row by dstLineSize.
     * @param dstLineSize Destination row pitch in PIXELS. MUST be a
     *     multiple of 16 (asserted) and >= dstWidth. For an output
     *     buffer allocated by `CLVideoDecoderOutput`, pass
     *     `linesize[plane] / bytesPerPixel`. Callers that shift the
     *     destination pointers into a row (pillar-box padLeft) must
     *     keep the shift 16-aligned so the rounded-up write span stays
     *     inside the row (VMS-186). Source-side reads in the tail block
     *     are clamped against `yLineSize` / `uvLineSize` so they
     *     stay inside the source row's allocation.
     * @param srcFormat AVPixelFormat — used only to detect chroma
     *     subsampling for UV plane stepping.
     */
    void init(
        int srcWidth,
        int srcHeight,
        int dstWidth,
        int dstHeight,
        int dstLineSize,
        int yLineSize,
        int uvLineSize,
        int srcFormat);
};

using YuvToRgbPlanarFp32Fn = void (*)(
    const YuvToRgbScaleContext* context,
    const uint8_t* yp,
    const uint8_t* up,
    const uint8_t* vp,
    float* rp,
    float* gp,
    float* bp);

NX_MEDIA_CORE_API void yuv_to_rgb_planarfp32(
    const YuvToRgbScaleContext* context,
    const uint8_t* yp,
    const uint8_t* up,
    const uint8_t* vp,
    float* rp,
    float* gp,
    float* bp);

NX_MEDIA_CORE_API void yuv_to_rgb_planarfp32_sse(
    const YuvToRgbScaleContext* context,
    const uint8_t* yp,
    const uint8_t* up,
    const uint8_t* vp,
    float* rp,
    float* gp,
    float* bp);

// uint8 RGB CHW variant — 4x less output bandwidth than the float32 version
// for callers that want raw bytes (e.g. an inference model that does its own
// uint8 -> float cast at the input layer). Same Y/U/V load and color math as
// the fp32 version, but skips the widen-to-float / multiply-by-1/255 / store
// 32-byte steps. Output is 3 contiguous H*W byte planes (R, G, B).
using YuvToRgbPlanarU8Fn = void (*)(
    const YuvToRgbScaleContext* context,
    const uint8_t* yp,
    const uint8_t* up,
    const uint8_t* vp,
    uint8_t* rp,
    uint8_t* gp,
    uint8_t* bp);

NX_MEDIA_CORE_API void yuv_to_rgb_planaru8(
    const YuvToRgbScaleContext* context,
    const uint8_t* yp,
    const uint8_t* up,
    const uint8_t* vp,
    uint8_t* rp,
    uint8_t* gp,
    uint8_t* bp);

NX_MEDIA_CORE_API void yuv_to_rgb_planaru8_sse(
    const YuvToRgbScaleContext* context,
    const uint8_t* yp,
    const uint8_t* up,
    const uint8_t* vp,
    uint8_t* rp,
    uint8_t* gp,
    uint8_t* bp);
