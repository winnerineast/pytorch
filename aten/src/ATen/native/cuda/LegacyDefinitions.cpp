#include <ATen/ATen.h>
#include <ATen/NativeFunctions.h>
#include <ATen/LegacyTHFunctionsCUDA.h>

namespace at { namespace native {

// Methods

Tensor & masked_fill__cuda(Tensor& self, const Tensor & mask, Scalar value) {
  // As we dispatch on self and TH is type-checked, we need different definitions.
  // This can be fixed by moving to ATen.
  if (mask.dtype() == at::ScalarType::Byte) {
    return legacy::cuda::_th_masked_fill_(self, mask, value);
  } else {
    return legacy::cuda::_th_masked_fill_bool_(self, mask, value);
  }
}

Tensor & masked_fill__cuda(Tensor& self, const Tensor & mask, const Tensor & value) {
  // As we dispatch on self and TH is type-checked, we need different definitions.
  // This can be fixed by moving to ATen.
  if (mask.dtype() == at::ScalarType::Byte) {
    return legacy::cuda::_th_masked_fill_(self, mask, value);
  } else {
    return legacy::cuda::_th_masked_fill_bool_(self, mask, value);
  }
}

Tensor & masked_scatter__cuda(Tensor& self, const Tensor & mask, const Tensor & source) {
  // As we dispatch on self and TH is type-checked, we need different definitions.
  // This can be fixed by moving to ATen.
  if (mask.dtype() == at::ScalarType::Byte) {
    return legacy::cuda::_th_masked_scatter_(self, mask, source);
  } else {
    return legacy::cuda::_th_masked_scatter_bool_(self, mask, source);
  }
}

Tensor masked_select_cuda(const Tensor & self, const Tensor & mask) {
  if (mask.dtype() == at::ScalarType::Byte) {
    return legacy::cuda::_th_masked_select(self, mask);
  } else {
    return legacy::cuda::_th_masked_select_bool(self, mask);
  }
}

Tensor & gather_out_cuda(Tensor & result, const Tensor & self, int64_t dim, const Tensor & index, bool sparse_grad) {
  return legacy::cuda::_th_gather_out(result, self, dim, index);
}

Tensor gather_cuda(const Tensor & self, int64_t dim, const Tensor & index, bool sparse_grad) {
  return legacy::cuda::_th_gather(self, dim, index);
}

}} // namespace at::native
