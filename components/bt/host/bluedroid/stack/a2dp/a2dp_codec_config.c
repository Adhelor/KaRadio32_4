#include "string.h"
#include "stack/a2d_api.h"
#include "stack/a2d_sbc.h"
#include "stack/a2dp_codec_api.h"
#include "stack/a2dp_vendor.h"
#include "stack/a2dp_vendor_aptx_hd.h"
#include "bta/bta_av_api.h"
#include "bta/bta_av_ci.h"
#include "bta/bta_av_co.h"
#include "bta/bta_av_sbc.h"
#include "bta/bta_sys.h"
#include "btc/btc_util.h"
#include "btc_a2dp.h"
#include "btc_a2dp_source.h"
#include "btc_av_co.h"
#include "common/bt_target.h"
#include "osi/mutex.h"

#include "bt_av.h"

#if (A2D_INCLUDED == TRUE)

tA2D_CODEC_TYPE A2DP_GetCodecType(const uint8_t* p_codec_info) {
  return (tA2D_CODEC_TYPE)(p_codec_info[AVDT_CODEC_TYPE_INDEX]);
}

tA2D_STATUS A2DP_ParseInfo(uint8_t* p_ie, const uint8_t* p_codec_info,
                           bool is_capability) {
  tA2D_CODEC_TYPE codec_type = A2DP_GetCodecType(p_codec_info);

  LOG_VERBOSE("%s: codec_type = 0x%x", __func__, codec_type);

  switch (codec_type) {
    case A2D_MEDIA_CT_SBC:
      // POPRAWIONO: Wywołanie natywnej, oryginalnej funkcji Broadcom z nagłówka a2d_sbc.h
      return A2D_ParsSbcInfo((tA2D_SBC_CIE*)p_ie, (UINT8*)p_codec_info, is_capability);
    case A2D_MEDIA_CT_NON_A2DP:
      return A2DP_VendorParseInfo(p_ie, p_codec_info, is_capability);
    default:
      break;
  }
  return A2D_FAIL;
}

bool A2DP_IsPeerSinkCodecValid(const uint8_t* p_codec_info) {
  tA2D_CODEC_TYPE codec_type = A2DP_GetCodecType(p_codec_info);

  LOG_VERBOSE("%s: codec_type = 0x%x", __func__, codec_type);

  switch (codec_type) {
    case A2D_MEDIA_CT_SBC:
      {
        tA2D_SBC_CIE sbc_cie;
        // POPRAWIONO: Natywny parser z 4.4.8 bez literki "e"
        return (A2D_ParsSbcInfo(&sbc_cie, (UINT8*)p_codec_info, true) == A2D_SUCCESS);
      }
    case A2D_MEDIA_CT_NON_A2DP:
      return A2DP_IsVendorPeerSinkCodecValid(p_codec_info);
    default:
      break;
  }
  return false;
}

bool A2DP_IsSinkCodecSupported(const uint8_t* p_codec_info) {
  tA2D_CODEC_TYPE codec_type = A2DP_GetCodecType(p_codec_info);

  LOG_VERBOSE("%s: codec_type = 0x%x", __func__, codec_type);

  switch (codec_type) {
    case A2D_MEDIA_CT_SBC:
      {
        tA2D_SBC_CIE sbc_cie;
        // POPRAWIONO: Natywny parser z 4.4.8 bez literki "e"
        return (A2D_ParsSbcInfo(&sbc_cie, (UINT8*)p_codec_info, true) == A2D_SUCCESS);
      }
    case A2D_MEDIA_CT_NON_A2DP:
      return A2DP_IsVendorSinkCodecSupported(p_codec_info);
    default:
      break;
  }

  LOG_ERROR("%s: unsupported codec type 0x%x", __func__, codec_type);
  return false;
}

bool A2DP_IsPeerSourceCodecSupported(const uint8_t* p_codec_info) {
  tA2D_CODEC_TYPE codec_type = A2DP_GetCodecType(p_codec_info);

  LOG_VERBOSE("%s: codec_type = 0x%x", __func__, codec_type);

  switch (codec_type) {
    case A2D_MEDIA_CT_SBC:
      {
        tA2D_SBC_CIE sbc_cie;
        // POPRAWIONO: Natywny parser z 4.4.8 bez literki "e"
        return (A2D_ParsSbcInfo(&sbc_cie, (UINT8*)p_codec_info, true) == A2D_SUCCESS);
      }
    case A2D_MEDIA_CT_NON_A2DP:
      return A2DP_IsVendorPeerSourceCodecSupported(p_codec_info);
    default:
      break;
  }

  LOG_ERROR("%s: unsupported codec type 0x%x", __func__, codec_type);
  return false;
}

const tA2DP_DECODER_INTERFACE* A2DP_GetDecoderInterface(
    const uint8_t* p_codec_info) {
  tA2D_CODEC_TYPE codec_type = A2DP_GetCodecType(p_codec_info);

  switch (codec_type) {
    case A2D_MEDIA_CT_SBC:
      {
        extern const tA2DP_DECODER_INTERFACE a2dp_decoder_interface_sbc;
        return &a2dp_decoder_interface_sbc;
      }
    case A2D_MEDIA_CT_NON_A2DP:
      return A2DP_GetVendorDecoderInterface(p_codec_info);
    default:
      break;
  }
  return NULL;
}

btav_a2dp_codec_index_t A2DP_SinkCodecIndex(const uint8_t* p_codec_info) {
  tA2D_CODEC_TYPE codec_type = A2DP_GetCodecType(p_codec_info);

  switch (codec_type) {
    case A2D_MEDIA_CT_SBC:
      return BTAV_A2DP_CODEC_INDEX_SINK_SBC;
    case A2D_MEDIA_CT_NON_A2DP:
      return A2DP_VendorSinkCodecIndex(p_codec_info);
    default:
      break;
  }
  return BTAV_A2DP_CODEC_INDEX_MAX;
}

btav_a2dp_codec_index_t A2DP_SourceCodecIndex(const uint8_t* p_codec_info) {
  tA2D_CODEC_TYPE codec_type = A2DP_GetCodecType(p_codec_info);

  switch (codec_type) {
    case A2D_MEDIA_CT_SBC:
      return BTAV_A2DP_CODEC_INDEX_SOURCE_SBC;
    case A2D_MEDIA_CT_NON_A2DP:
      return A2DP_VendorSourceCodecIndex(p_codec_info);
    default:
      break;
  }
  return BTAV_A2DP_CODEC_INDEX_MAX;
}

bool A2DP_InitCodecConfig(btav_a2dp_codec_index_t codec_index, UINT8 *p_result) {
  if (codec_index == BTAV_A2DP_CODEC_INDEX_SINK_SBC || codec_index == BTAV_A2DP_CODEC_INDEX_SOURCE_SBC) {
      return true;
  }
  if (A2DP_VendorInitCodecConfig(codec_index, p_result)) {
    return true;
  }
  return false;
}

bool A2DP_BuildCodecConfig(UINT8 *p_src_cap, UINT8 *p_result) {
  tA2D_CODEC_TYPE codec_type = A2DP_GetCodecType(p_src_cap);

  switch (codec_type) {
    case A2D_MEDIA_CT_SBC:
      {
        tA2D_SBC_CIE sbc_cie;
        // POPRAWIONO: Oryginalna Broadcomowa nazwa A2D_ParsSbcInfo
        if (A2D_ParsSbcInfo(&sbc_cie, p_src_cap, true) == A2D_SUCCESS) {
            // Funkcja bta_av_sbc_cfg_matches_cap jest już w pełni zadeklarowana w dołączonym bta_av_sbc.h!
            return (bta_av_sbc_cfg_matches_cap(p_result, &sbc_cie) == 0); 
        }
        return false;
      }
    case A2D_MEDIA_CT_NON_A2DP:
      return A2DP_VendorBuildCodecConfig(p_src_cap, p_result);
    default:
      return false;
  }
}

const char* A2DP_CodecName(const uint8_t* p_codec_info) {
  tA2D_CODEC_TYPE codec_type = A2DP_GetCodecType(p_codec_info);

  switch (codec_type) {
    case A2D_MEDIA_CT_SBC:
      return A2DP_CodecName(p_codec_info);
    case A2D_MEDIA_CT_NON_A2DP:
      return A2DP_VendorCodecName(p_codec_info);
    default:
      break;
  }
  return "UNKNOWN CODEC";
}

bool A2DP_CodecTypeEquals(const uint8_t* p_codec_info_a,
                          const uint8_t* p_codec_info_b) {
  tA2D_CODEC_TYPE codec_type_a = A2DP_GetCodecType(p_codec_info_a);
  tA2D_CODEC_TYPE codec_type_b = A2DP_GetCodecType(p_codec_info_b);

  if (codec_type_a != codec_type_b) return false;

  switch (codec_type_a) {
    case A2D_MEDIA_CT_SBC:
      return A2DP_CodecTypeEquals(p_codec_info_a, p_codec_info_b);
    case A2D_MEDIA_CT_NON_A2DP:
      return A2DP_VendorCodecTypeEquals(p_codec_info_a, p_codec_info_b);
    default:
      break;
  }

  LOG_ERROR("%s: unsupported codec type 0x%x", __func__, codec_type_a);
  return false;
}

#endif  ///A2D_INCLUDED