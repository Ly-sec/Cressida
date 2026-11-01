#pragma once

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

namespace media {
  // Legacy JPEG formats have the same planes as their range-independent equivalents.
  inline AVPixelFormat scalingFormat(AVPixelFormat format) {
    switch (format) {
    case AV_PIX_FMT_YUVJ420P:
      return AV_PIX_FMT_YUV420P;
    case AV_PIX_FMT_YUVJ422P:
      return AV_PIX_FMT_YUV422P;
    case AV_PIX_FMT_YUVJ444P:
      return AV_PIX_FMT_YUV444P;
    case AV_PIX_FMT_YUVJ440P:
      return AV_PIX_FMT_YUV440P;
    case AV_PIX_FMT_YUVJ411P:
      return AV_PIX_FMT_YUV411P;
    default:
      return format;
    }
  }

  inline bool rgbaColorspace(SwsContext* context, const AVFrame& frame) {
    const auto format = static_cast<AVPixelFormat>(frame.format);
    const auto* description = av_pix_fmt_desc_get(format);
    const bool fullRange = frame.color_range == AVCOL_RANGE_JPEG
        || scalingFormat(format) != format
        || (description && (description->flags & AV_PIX_FMT_FLAG_RGB));
    int matrix = SWS_CS_DEFAULT;
    switch (frame.colorspace) {
    case AVCOL_SPC_BT709:
      matrix = SWS_CS_ITU709;
      break;
    case AVCOL_SPC_FCC:
      matrix = SWS_CS_FCC;
      break;
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M:
      matrix = SWS_CS_ITU601;
      break;
    case AVCOL_SPC_SMPTE240M:
      matrix = SWS_CS_SMPTE240M;
      break;
    case AVCOL_SPC_BT2020_NCL:
    case AVCOL_SPC_BT2020_CL:
      matrix = SWS_CS_BT2020;
      break;
    default:
      break;
    }
    const auto* coefficients = sws_getCoefficients(matrix);
    constexpr int fixedPointUnity = 1 << 16;
    return context
        && sws_setColorspaceDetails(
               context, coefficients, fullRange, coefficients, 1, 0, fixedPointUnity, fixedPointUnity
           )
        >= 0;
  }
} // namespace media
