#include <torch/extension.h>

#include <ATen/core/ATenDispatch.h>

using namespace at;

static int test_int;

Tensor get_dtype_tensor(caffe2::TypeMeta dtype) {
  auto tensor_impl = c10::make_intrusive<TensorImpl, UndefinedTensorImpl>(
      Storage(
          dtype, 0, at::DataPtr(nullptr, Device(DeviceType::MSNPU, 0)), nullptr, false),
      MSNPUTensorId());
  return Tensor(std::move(tensor_impl));
}

Tensor empty_override(IntArrayRef size, const TensorOptions & options) {
  test_int = 0;
  return get_dtype_tensor(options.dtype());
}

Tensor add_override(const Tensor & a, const Tensor & b , Scalar c) {
  test_int = 1;
  return get_dtype_tensor(a.dtype());
}

void init_msnpu_extension() {
  globalATenDispatch().registerOp(
    Backend::MSNPU,
    "aten::empty(int[] size, *, ScalarType? dtype=None, Layout? layout=None, Device? device=None, bool? pin_memory=None, MemoryFormat? memory_format=None) -> Tensor",
    &empty_override);
  globalATenDispatch().registerOp(
    Backend::MSNPU,
    "aten::add(Tensor self, Tensor other, *, Scalar alpha=1) -> Tensor",
    &add_override);
}

// TODO: Extend this to exercise multi-device setting.  In that case,
// we need to add a thread local variable to track the current device.
struct MSNPUGuardImpl final : public c10::impl::DeviceGuardImplInterface {
  static constexpr DeviceType static_type = DeviceType::MSNPU;
  MSNPUGuardImpl() {}
  MSNPUGuardImpl(DeviceType t) {
    AT_ASSERT(t == DeviceType::MSNPU);
  }
  DeviceType type() const override {
    return DeviceType::MSNPU;
  }
  Device exchangeDevice(Device d) const override {
    AT_ASSERT(d.type() == DeviceType::MSNPU);
    AT_ASSERT(d.index() == 0);
    return d;
  }
  Device getDevice() const override {
    return Device(DeviceType::MSNPU, 0);
  }
  void setDevice(Device d) const override {
    AT_ASSERT(d.type() == DeviceType::MSNPU);
    AT_ASSERT(d.index() == 0);
  }
  void uncheckedSetDevice(Device d) const noexcept override {
  }
  Stream getStream(Device d) const noexcept override {
    return Stream(Stream::DEFAULT, Device(DeviceType::MSNPU, 0));
  }
  Stream exchangeStream(Stream s) const noexcept override {
    return Stream(Stream::DEFAULT, Device(DeviceType::MSNPU, 0));
  }
  DeviceIndex deviceCount() const noexcept override {
    return 1;
  }
};

constexpr DeviceType MSNPUGuardImpl::static_type;
C10_REGISTER_GUARD_IMPL(MSNPU, MSNPUGuardImpl);

int get_test_int() {
  return test_int;
}

PYBIND11_MODULE(TORCH_EXTENSION_NAME, m) {
  m.def("init_msnpu_extension", &init_msnpu_extension);
  m.def("get_test_int", &get_test_int);
}
