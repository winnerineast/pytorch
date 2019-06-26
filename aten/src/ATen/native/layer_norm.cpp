#include <ATen/NativeFunctions.h>

#include <array>
#include <functional>
#include <numeric>
#include <tuple>
#include <vector>

#include <ATen/ATen.h>
#include <ATen/AccumulateType.h>
#include <ATen/CPUApplyUtils.h>
#include <ATen/Config.h>
#include <ATen/Parallel.h>
#include <ATen/native/cpu/layer_norm_kernel.h>

namespace at {
namespace native {

std::tuple<Tensor, Tensor, Tensor> layer_norm_cpu(
    const Tensor& X,
    const Tensor& gamma /* optional */,
    const Tensor& beta /* optional */,
    int64_t M,
    int64_t N,
    double eps) {
  Tensor Y = at::native::empty_like(X);
  Tensor mean = at::empty({M}, X.options());
  Tensor rstd = at::empty({M}, X.options());
  LayerNormKernel(kCPU, X, gamma, beta, M, N, eps, &Y, &mean, &rstd);
  return std::make_tuple(Y, mean, rstd);
}

std::tuple<Tensor, Tensor, Tensor> layer_norm_backward_cpu(
    const Tensor& dY,
    const Tensor& X,
    const Tensor& mean,
    const Tensor& rstd,
    const Tensor& gamma,
    int64_t M,
    int64_t N,
    std::array<bool, 3> grad_input_mask) {
  Tensor dX;
  Tensor dgamma;
  Tensor dbeta;
  if (grad_input_mask[0]) {
    dX = at::native::empty_like(X);
  }
  if (grad_input_mask[1]) {
    dgamma = at::native::empty_like(gamma);
  }
  if (grad_input_mask[2]) {
    dbeta = at::native::empty_like(gamma);
  }
  LayerNormBackwardKernel(
      kCPU, dY, X, mean, rstd, gamma, M, N, &dX, &dgamma, &dbeta);
  return std::make_tuple(dX, dgamma, dbeta);
}

// TODO(yangxm): Change this function to Aten impl so that we can support higher
// order gradients.
std::tuple<Tensor, Tensor, Tensor> layer_norm_double_backward_cpu(
    const Tensor& ddX,
    const Tensor& ddgamma,
    const Tensor& ddbeta,
    const Tensor& dY,
    const Tensor& X,
    const Tensor& mean,
    const Tensor& rstd,
    const Tensor& gamma,
    int64_t M,
    int64_t N,
    std::array<bool, 3> grad_input_mask) {
  Tensor ddY;
  Tensor dX;
  Tensor dgamma;
  if (grad_input_mask[0]) {
    ddY = at::native::empty_like(dY);
  }
  if (grad_input_mask[1]) {
    dX = at::native::empty_like(X);
  }
  if (grad_input_mask[2]) {
    dgamma = at::native::empty_like(gamma);
  }
  LayerNormDoubleBackwardKernel(
      kCPU,
      ddX,
      ddgamma,
      ddbeta,
      dY,
      X,
      mean,
      rstd,
      gamma,
      M,
      N,
      &ddY,
      &dX,
      &dgamma);
  return std::make_tuple(ddY, dX, dgamma);
}

Tensor layer_norm(
    const Tensor& input,
    IntArrayRef normalized_shape,
    const Tensor& weight /* optional */,
    const Tensor& bias /* optional */,
    double eps,
    bool cudnn_enabled) {
  const int normalized_ndim = normalized_shape.size();
  TORCH_CHECK(
      normalized_ndim >= 1,
      "Expected normalized_shape to be at least 1-dimensional, i.e., ",
      "containing at least one element, but got normalized_shape = ",
      normalized_shape);
  TORCH_CHECK(
      !weight.defined() || weight.sizes().equals(normalized_shape),
      "Expected weight to be of same shape as normalized_shape, but got ",
      "weight of shape ",
      weight.sizes(),
      " and normalized_shape = ",
      normalized_shape);
  TORCH_CHECK(
      !bias.defined() || bias.sizes().equals(normalized_shape),
      "Expected bias to be of same shape as normalized_shape, but got ",
      "bias of shape ",
      bias.sizes(),
      " and normalized_shape = ",
      normalized_shape);

  const auto input_shape = input.sizes();
  const auto input_ndim = input.dim();

  if (input_ndim < normalized_ndim ||
      !input_shape.slice(input_ndim - normalized_ndim)
           .equals(normalized_shape)) {
    std::stringstream ss;
    ss << "Given normalized_shape=" << normalized_shape
       << ", expected input with shape [*";
    for (auto size : normalized_shape) {
      ss << ", " << size;
    }
    ss << "], but got input of size" << input_shape;
    AT_ERROR(ss.str());
  }

  const int axis = input_ndim - normalized_ndim;
  const int64_t M = std::accumulate(
      input_shape.cbegin(),
      input_shape.cbegin() + axis,
      1LL,
      std::multiplies<int64_t>());
  const int64_t N = std::accumulate(
      input_shape.cbegin() + axis,
      input_shape.cend(),
      1LL,
      std::multiplies<int64_t>());

  if (input.device().is_cpu()) {
    return std::get<0>(native_layer_norm(
        input.contiguous(), weight.contiguous(), bias.contiguous(), M, N, eps));
  }

  // Apply layer norm
  auto input_reshaped = input.contiguous().view({1, M, -1});
  auto out = at::batch_norm(
      input_reshaped, {}, {}, {}, {}, true, 0, eps, cudnn_enabled);
  out = out.view(input_shape);

  if (weight.defined() && bias.defined()) {
    return bias.addcmul(out, weight, 1);
  } else if (weight.defined()) {
    return out.mul(weight);
  } else if (bias.defined()) {
    return out.add(bias);
  } else {
    return out;
  }
}

DEFINE_DISPATCH(LayerNormKernel);
DEFINE_DISPATCH(LayerNormBackwardKernel);
DEFINE_DISPATCH(LayerNormDoubleBackwardKernel);

} // namespace native
} // namespace at
