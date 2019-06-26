#pragma once

#ifdef USE_FBGEMM
#include "fbgemm/Fbgemm.h"
#include "fbgemm/QuantUtils.h"

// The struct for the packed weight matrix (PackBMatrix) and the corresponding
// column offsets used for the fully connect layer, which are both prepared in
// the prepacking step to save the computations in the inference. Note the
// column offsets include the sum of the B columns as well as the scalar term
// B_zero_point * K, whereas the row offsets created by
// PackAWithQuantRowOffset/PackAWithIm2Col/PackAWithRowOffset are only the sum
// of the A rows. The column offsets are needed for the asymmetric quantization
// (affine quantization) of input matrix.
// Note that in JIT mode we can think of a way to fuse col_offsets with bias.
struct FBGEMM_API PackedLinearWeight {
  std::unique_ptr<fbgemm::PackBMatrix<int8_t>> w;
  std::vector<int32_t> col_offsets;
  double w_scale;
  int64_t w_zp;
};

struct FBGEMM_API PackedConvWeight {
  std::unique_ptr<fbgemm::PackBMatrix<int8_t>> w;
  std::vector<int32_t> col_offsets;
  std::vector<int64_t> kernel;
  double w_scale;
  int64_t w_zp;
};

// PackWeight: Convert the weight from uint8 to int8.
static void convert_uint8_int8(
    int len,
    const uint8_t* src_uint8,
    int8_t* dst_int8) {
  for (int i = 0; i < len; ++i) {
    dst_int8[i] = static_cast<int8_t>(static_cast<int32_t>(src_uint8[i]) - 128);
  }
}

// UnpackWeight: Convert the weight from int8 to uint8.
static void convert_int8_uint8(
    int len,
    const int8_t* src_int8,
    uint8_t* dst_uint8) {
  for (int i = 0; i < len; ++i) {
    dst_uint8[i] =
        static_cast<uint8_t>(static_cast<int32_t>(src_int8[i]) + 128);
  }
}

#endif // USE_FBGEMM
