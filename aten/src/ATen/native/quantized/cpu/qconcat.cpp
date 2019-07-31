#include <ATen/ATen.h>
#include <ATen/core/op_registration/op_registration.h>
#include <ATen/native/TensorIterator.h>
#include <ATen/native/cpu/Loops.h>

#include <algorithm>
#include <vector>

namespace at { namespace native {
namespace {

bool is_valid_quantization_scheme(Tensor t) {
  const auto qtype = t.qscheme();
  return (qtype == kPerTensorAffine) || (qtype == kPerTensorSymmetric);
}

/* Quantized concatenation.
 *
 * Note: This function uses a dequantization.
 */
template <bool ReLUFused>
Tensor quantized_cat(const std::vector<Tensor>& qxs, int64_t axis,
                     double scale, int64_t zero_point) {
  const auto x_dtype = qxs[0].scalar_type();
  TORCH_CHECK(is_valid_quantization_scheme(qxs[0]),
              "Only per-tensor quantization is supported in 'cat'!")
  const auto x_qscheme = qxs[0].qscheme();
  std::vector<Tensor> xs;
  xs.reserve(qxs.size());
  for (const auto& qx: qxs) {
    TORCH_CHECK(x_dtype == qx.scalar_type(), "All dtypes must be the same.");
    TORCH_CHECK(x_qscheme == qx.qscheme(),
                "Quantization schemes must be the same.");
    xs.push_back(qx.dequantize());
  }
  const Tensor y = at::cat(xs, axis);
  Tensor qy;
  AT_DISPATCH_QINT_TYPES(x_dtype, "qcat", [&]() {
    qy = at::quantize_linear(y, scale, zero_point, SCALAR_TYPE);
    if (ReLUFused) {
      auto iter = TensorIterator::unary_op(qy, qy);
      cpu_kernel(iter, [&](scalar_t value) -> scalar_t {
        return scalar_t(std::max<underlying_t>(value.val_, zero_point));
      });
    }
  });
  return qy;
}

template <bool ReLUFused = false>
class QCat final : public torch::OperatorKernel {
 public:
  Tensor operator()(const std::vector<Tensor>& qxs, int64_t axis,
                    c10::optional<double> scale,
                    c10::optional<int64_t> zero_point) {
    double _scale = scale.has_value() ? scale.value() : qxs[0].q_scale();
    int64_t _zero_point = zero_point.has_value() ? zero_point.value()
                                                 : qxs[0].q_zero_point();
    return quantized_cat<ReLUFused>(qxs, axis, _scale, _zero_point);
  }
};

template <bool ReLUFused = false>
class QCatOut final : public torch::OperatorKernel {
 public:
  Tensor operator()(const std::vector<Tensor>& qxs, int64_t axis, Tensor out) {
    auto out_ = quantized_cat<ReLUFused>(qxs, axis, out.q_scale(),
                                         out.q_zero_point());
    at::native::copy_(out, out_, /*non_blocking=*/false);
    return out;
  }
};

static auto registry = torch::RegisterOperators()
.op("quantized::cat(Tensor[] qx, int axis, float? scale, int? zero_point)"
    " -> Tensor",
    torch::RegisterOperators::options()
      .kernel<QCat<false>>(QuantizedCPUTensorId()))
.op("quantized::cat_relu(Tensor[] qx, int axis, float? scale, int? zero_point)"
    " -> Tensor",
    torch::RegisterOperators::options()
      .kernel<QCat<true>>(QuantizedCPUTensorId()))
.op("quantized::cat_out(Tensor[] qx, int axis, Tensor out)"
    " -> Tensor",
    torch::RegisterOperators::options()
      .kernel<QCatOut<false>>(QuantizedCPUTensorId()))
.op("quantized::cat_relu_out(Tensor[] qx, int axis, Tensor out)"
    " -> Tensor",
    torch::RegisterOperators::options()
      .kernel<QCatOut<true>>(QuantizedCPUTensorId()));

}  // namespace
}}  // namespace at::native
