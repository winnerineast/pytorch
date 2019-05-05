#include <ATen/core/dispatch/OperatorEntry.h>

namespace c10 {
namespace impl {

OperatorEntry::OperatorEntry(FunctionSchema&& schema)
: schema_(std::move(schema))
, dispatchTable_(schema_)
, kernels_() {}

void OperatorEntry::prepareForDeregistration() {
  return dispatchTable_.read([&] (const DispatchTable& dispatchTable) {
    if (!dispatchTable.isEmpty()) {
      std::ostringstream str;
      str << schema_;
      AT_ERROR("Tried to deregister op schema for an operator that still has kernels registered. The operator schema is ", str.str());
    }
  });
  AT_ASSERTM(kernels_.size() == 0, "If the dispatch table is empty, then the invariant says there can't be any kernels");
}

RegistrationHandleRAII OperatorEntry::registerKernel(TensorTypeId dispatch_key, DispatchTableEntry kernel) {
  std::unique_lock<std::mutex> lock(kernelsMutex_);

  // Add the kernel to the kernels list,
  // possibly creating the list if this is the first kernel.
  auto& k = kernels_[dispatch_key];
  k.push_front(kernel);
  std::list<DispatchTableEntry>::iterator inserted = k.begin();
  // update the dispatch table, i.e. re-establish the invariant
  // that the dispatch table points to the newest kernel
  updateDispatchTable_(dispatch_key);

  return RegistrationHandleRAII([this, dispatch_key, inserted] {
    // list iterators stay valid even if the list changes,
    // so we can use the iterator to remove the kernel from the list
    deregisterKernel_(dispatch_key, inserted);
  });
}

void OperatorEntry::updateDispatchTable_(TensorTypeId dispatch_key) {
  // precondition: kernelsMutex_ is locked

  auto k = kernels_.find(dispatch_key);

  if (k == kernels_.end()) {
    dispatchTable_.write([&] (DispatchTable& dispatchTable) {
      dispatchTable.removeKernelIfExists(dispatch_key);
    });
  } else {
    dispatchTable_.write([&] (DispatchTable& dispatchTable) {
      dispatchTable.setKernel(dispatch_key, k->second.front());
    });
  }
}

void OperatorEntry::deregisterKernel_(TensorTypeId dispatch_key, std::list<DispatchTableEntry>::iterator kernel) {
  std::unique_lock<std::mutex> lock(kernelsMutex_);

  auto found = kernels_.find(dispatch_key);
  AT_ASSERTM(found != kernels_.end(), "Tried to deregister a kernel but there are no kernels registered for this dispatch key.");
  auto& k = found->second;
  k.erase(kernel);
  if (k.empty()) {
    // the invariant says we don't want empty lists but instead remove the list from the map
    kernels_.erase(found);
  }

  updateDispatchTable_(dispatch_key);
}

RegistrationHandleRAII OperatorEntry::registerFallbackKernel(DispatchTableEntry kernel) {
  return registerKernel(TensorTypeIds::undefined(), std::move(kernel));
}

}
}
