#include "Python.h"
#include "VariableType.h"

// ${generated_comment}

#include "torch/csrc/autograd/variable.h"
#include "torch/csrc/autograd/function.h"
#include "torch/csrc/autograd/grad_mode.h"
#include "torch/csrc/autograd/saved_variable.h"
#include "torch/csrc/autograd/generated/Functions.h"
#include "torch/csrc/autograd/functions/tensor.h"
#include "torch/csrc/autograd/functions/basic_ops.h"
#include "torch/csrc/jit/tracer.h"

#include <initializer_list>
#include <iostream>
#include <functional>

#ifdef _MSC_VER
#ifdef Type
#undef Type
#endif
#endif

using namespace at;
using namespace torch::autograd::generated;

namespace torch { namespace autograd {

// Helper methods for working with Attributes (torch/csrc/jit/attributes.h)

// The overloaded accessors are convenient for the generated code (since we
// don't want to make the codegen do the dispatch manually)
static void setattr(jit::Node* n, jit::Symbol name, int64_t v)             { n->i_(name, v); }
static void setattr(jit::Node* n, jit::Symbol name, const at::Scalar& v)   { n->t_(name, v.toTensor()); }
static void setattr(jit::Node* n, jit::Symbol name, SparseTensor s)        { n->t_(name, s.tref); }
static void setattr(jit::Node* n, jit::Symbol name, const at::IntList& v)  { n->is_(name, v); }
static void setattr(jit::Node* n, jit::Symbol name, bool v)                { n->i_(name, v); }
static void setattr(jit::Node* n, jit::Symbol name, double v)              { n->f_(name, v); }
template<unsigned long N>
static void setattr(jit::Node* n, jit::Symbol name, std::array<bool, N> v) { n->is_(name, std::vector<int64_t>(v.begin(), v.end())); }

VariableType::VariableType(Context* context, Type* baseType)
  : Type(context)
  , baseType(baseType) {
  str = std::string("Variable[") + baseType->toString() + "]";
}

ScalarType VariableType::scalarType() const {
  return baseType->scalarType();
}
Backend VariableType::backend() const {
  return baseType->backend();
}
bool VariableType::is_cuda() const { return baseType->is_cuda(); }
bool VariableType::is_sparse() const { return baseType->is_sparse(); }
bool VariableType::is_distributed() const { return baseType->is_distributed(); }

std::unique_ptr<Storage> VariableType::storage() const {
  return baseType->storage();
}
std::unique_ptr<Storage> VariableType::storage(size_t size) const {
  return baseType->storage(size);
}
std::unique_ptr<Storage> VariableType::storageFromBlob(void * data, int64_t size, const std::function<void(void*)> & deleter) const {
  return baseType->storageFromBlob(data, size, deleter);
}
std::unique_ptr<Storage> VariableType::unsafeStorageFromTH(void * th_pointer, bool retain) const {
  return baseType->unsafeStorageFromTH(th_pointer, retain);
}
std::unique_ptr<Storage> VariableType::storageWithAllocator(int64_t size, std::unique_ptr<Allocator> allocator) const {
  return baseType->storageWithAllocator(size, std::move(allocator));
}
Tensor VariableType::unsafeTensorFromTH(void * th_pointer, bool retain) const {
  return make_variable(baseType->unsafeTensorFromTH(th_pointer, retain), false);
}
std::unique_ptr<Generator> VariableType::generator() const {
  return baseType->generator();
}

const char * VariableType::toString() const {
  return str.c_str();
}
size_t VariableType::elementSizeInBytes() const {
  return baseType->elementSizeInBytes();
}
Type & VariableType::toBackend(Backend b) const {
  return *VariableImpl::getType(baseType->toBackend(b));
}
Type & VariableType::toScalarType(ScalarType s) const {
  return *VariableImpl::getType(baseType->toScalarType(s));
}
TypeID VariableType::ID() const {
  throw std::runtime_error("VariableType::ID() not implemented");
}

const char * VariableType::typeString() {
  return "VariableType";
}

Variable & VariableType::checked_cast(const Type & type, const Tensor & t, const char * name, int pos) {
  if(!t.defined()) {
    runtime_error("Expected a Tensor of type %s but found an undefined Tensor for argument #%d '%s'",
        type.toString(), pos, name);
  }
  if (&t.type() != &type && &t.type() != &type.toBackend(toSparse(t.type().backend()))) {
    runtime_error("Expected object of type %s but found type %s for argument #%d '%s'",
        type.toString(), t.type().toString(), pos, name);
  }
  return static_cast<Variable&>(const_cast<Tensor&>(t));
}

Tensor & VariableType::unpack(const Tensor & t, const char * name, int pos) const {
  return checked_cast(*this, t, name, pos).data();
}

SparseTensor VariableType::unpack(SparseTensor t, const char * name, int pos) const {
  auto backend = is_cuda() ? kSparseCUDA : kSparseCPU;
  return SparseTensor(checked_cast(this->toBackend(backend), t.tref, name, pos).data());
}

Tensor & VariableType::unpack_long(const Tensor & t, const char * name, int pos) const {
  auto& type = *VariableImpl::getType(baseType->toScalarType(kLong));
  return checked_cast(type, t, name, pos).data();
}

Tensor & VariableType::unpack_byte(const Tensor & t, const char * name, int pos) const {
  auto& type = *VariableImpl::getType(baseType->toScalarType(kByte));
  return checked_cast(type, t, name, pos).data();
}

Tensor & VariableType::unpack_any(const Tensor & t, const char * name, int pos) const {
  if (!t.defined()) {
    runtime_error("Expected a Tensor of type Variable but found an undefined Tensor for argument #%d '%s'",
        pos, name);
  }
  auto scalarType = t.type().scalarType();
  auto backend = t.type().backend();
  auto& type = *VariableImpl::getType(baseType->toScalarType(scalarType).toBackend(backend));
  return checked_cast(type, t, name, pos).data();
}

Tensor VariableType::unpack_opt(const Tensor & t, const char * name, int pos) const {
  if(!t.defined()) {
    return Tensor();
  }
  return unpack(t, name, pos);
}

std::vector<at::Tensor> VariableType::unpack(at::TensorList tl, const char *name, int pos) const {
  std::vector<at::Tensor> ret(tl.size());
  for (size_t i = 0; i < tl.size(); ++i) {
    const auto &t = tl[i];
    if (!t.defined()) {
      runtime_error("Expected a Tensor of type %s but found an undefined Tensor at position #%d "
                    "for iterable argument #%d '%s'",
                    toString(), i, pos, name);
    }
    if (&t.type() == this) {
      ret[i] = static_cast<const Variable&>(t).data();
    } else {
      runtime_error("Expected object of type %s but found type %s at position #%d "
                    "for iterable argument #%d '%s'",
                    toString(),t.type().toString(), i, pos, name);
    }
  }
  return ret;
}

std::vector<at::Tensor> VariableType::unpack_idxs(at::TensorList tl, const char *name, int pos) const {
  auto& longType = *VariableImpl::getType(baseType->toScalarType(kLong));
  auto& byteType = *VariableImpl::getType(baseType->toScalarType(kByte));
  std::vector<at::Tensor> ret(tl.size());
  for (size_t i = 0; i < tl.size(); ++i) {
    const auto &t = tl[i];
    if (!t.defined()) {
      continue;
    } else if (!(t.type() == longType || t.type() == byteType)) {
      runtime_error("Expected object of type %s or %s but found type %s at position #%d "
                    "for iterable argument #%d '%s'",
                    longType.toString(), byteType.toString(), t.type().toString(),
                    i, pos, name);
    } else  {
      ret[i] = static_cast<const Variable&>(t).data();
    }
  }
  return ret;
}

Variable VariableType::as_variable(Tensor tensor) const {
  return make_variable(std::move(tensor));
}

std::tuple<Variable, Variable>
VariableType::as_variable(std::tuple<Tensor, Tensor> tensors) const {
  return std::make_tuple<>(
      make_variable(std::move(std::get<0>(tensors))),
      make_variable(std::move(std::get<1>(tensors))));
}

std::tuple<Variable, Variable, Variable>
VariableType::as_variable(std::tuple<Tensor, Tensor, Tensor> tensors) const {
  return std::make_tuple<>(
      make_variable(std::move(std::get<0>(tensors))),
      make_variable(std::move(std::get<1>(tensors))),
      make_variable(std::move(std::get<2>(tensors))));
}

std::tuple<Variable, Variable, Variable, Variable>
VariableType::as_variable(std::tuple<Tensor, Tensor, Tensor, Tensor> tensors) const {
  return std::make_tuple<>(
      make_variable(std::move(std::get<0>(tensors))),
      make_variable(std::move(std::get<1>(tensors))),
      make_variable(std::move(std::get<2>(tensors))),
      make_variable(std::move(std::get<3>(tensors))));
}

std::vector<Variable> VariableType::as_variable(TensorList tl) const {
  std::vector<Variable> variables;
  for (auto& t : tl) {
    variables.emplace_back(make_variable(std::move(t)));
  }
  return variables;
}

static Variable as_view(Variable base, Tensor tensor) {
  if (base.is_view()) {
    base = base.base();
  }
  return make_variable_view(std::move(base), std::move(tensor));
}

static void ensure_no_aten_scalars(Tensor & data) {
  if (data.defined() && data.dim() == 0) {
    data.as_strided_({1}, {1});
  }
}

template<typename T>
static bool computes_grad_tmpl(T tensors) {
  if (!GradMode::is_enabled()) {
    return false;
  }
  for (const Tensor& tensor : tensors) {
    auto& var = static_cast<const Variable&>(tensor);
    if (var.defined() && var.requires_grad()) {
      return true;
    }
  }
  return false;
}

using TensorRef = std::reference_wrapper<const Tensor>;
using TensorRefList = std::initializer_list<TensorRef>;

// ArrayRef is not covariant, which means there is no
// implicit conversion between TensorList (aka ArrayRef<Tensor>)
// and ArrayRef<Variable>.  What we do instead is manually
// construct a variable_list, which itself is implicitly convertible
// into an ArrayRef<Variable> (but don't return an ArrayRef<Variable>;
// ArrayRef is non-owning!)
static variable_list cast_tensor_list(const TensorList& tensors) {
  // TODO: Eliminate the intermediate vector allocation
  return variable_list(tensors.begin(), tensors.end());
}

static bool compute_requires_grad(const TensorRefList& tensors) {
  return computes_grad_tmpl(tensors);
}

static bool compute_requires_grad(TensorList tensors) {
  return computes_grad_tmpl(tensors);
}

static void check_no_requires_grad(const Tensor& tensor, const char* name) {
  auto& var = static_cast<const Variable&>(tensor);
  if (var.defined() && var.requires_grad()) {
    std::string msg = "the derivative for '";
    msg += name;
    msg += "' is not implemented";
    throw std::runtime_error(msg);
  }
}

static function_list compute_next_functions(const std::initializer_list<Tensor>& tensors) {
  return Function::flags(tensors).next_functions;
}

static function_list compute_next_functions(TensorList tensors) {
  return Function::flags(tensors).next_functions;
}

static void check_inplace(const Tensor& tensor) {
  auto& var = static_cast<const Variable&>(tensor);
  if (var.requires_grad() && var.is_leaf() && GradMode::is_enabled()) {
    at::runtime_error(
      "a leaf Variable that requires grad has been used in an in-place operation.");
  }
}

static void rebase_history(Variable& var, std::shared_ptr<Function> grad_fn, int output_nr=0) {
  if (!var.defined()) {
    return;
  }
  if (grad_fn) {
    grad_fn->num_inputs = 1;
    var.rebase_history(output_nr, std::move(grad_fn));
  }
}

// var must be the only differentiable output of the function. Use the ArrayRef
// overload for functions with multiple differentiable outputs.
static void set_history(Variable& var, std::shared_ptr<Function> grad_fn, int output_nr=0) {
  if (grad_fn) {
    grad_fn->num_inputs = 1;
    var.get()->output_nr = output_nr;
    var.get()->_grad_fn = std::move(grad_fn);
  }
}

static void set_history(at::ArrayRef<Variable> vl, std::shared_ptr<Function> grad_fn) {
  if (grad_fn) {
    grad_fn->num_inputs = vl.size();
    int64_t output_nr = 0;
    for (auto& var : vl) {
      if (!var.defined()) continue;
      // TODO: combine this with the Variable construction
      var.get()->output_nr = output_nr;
      var.get()->_grad_fn = grad_fn;
      output_nr++;
    }
  }
}

variable_list flatten(const TensorList& tensors) {
  return cast_tensor_list(tensors);
}

variable_list flatten(const Tensor& x, const TensorList& y) {
  std::vector<Variable> r;
  r.reserve(1 + y.size());
  r.emplace_back(x);
  r.insert(r.end(), y.begin(), y.end());
  return r;
}

variable_list flatten(const Tensor& x, const TensorList& y, const Tensor& z) {
  std::vector<Variable> r;
  r.reserve(2 + y.size());
  r.emplace_back(x);
  r.insert(r.end(), y.begin(), y.end());
  r.emplace_back(z);
  return r;
}

std::vector<Tensor> as_tensor_list(std::vector<Variable> &vars) {
  std::vector<Tensor> tensors;
  for (auto& v : vars) {
    tensors.emplace_back(std::move(v));
  }
  return tensors;
}

static void increment_version(const Tensor & t) {
  auto& var = static_cast<const Variable&>(t);
  var.version_counter().increment();
}

static bool isFloatingPoint(ScalarType s) {
  return s == kFloat || s == kDouble || s == kHalf;
}

Tensor & VariableType::s_copy_(Tensor & self, const Tensor & src, bool async) const {
  // TODO: once copy is exposed in Declarations.yaml we may be able to bind
  // it automatically
  auto& self_ = unpack(self, "self", 0);
  auto& src_ = unpack_any(src, "src", 1);
  check_inplace(self);
  std::shared_ptr<CopyBackwards> grad_fn;
  auto requires_grad = compute_requires_grad({ self, src });
  requires_grad &= isFloatingPoint(self.type().scalarType());
  if (requires_grad) {
    grad_fn = std::make_shared<CopyBackwards>();
    grad_fn->next_functions = compute_next_functions({ self, src });
    grad_fn->num_inputs = 1;
    grad_fn->src_type = &src.type();
    grad_fn->src_device = src.is_cuda() ? src.get_device() : -1;
  }
  baseType->s_copy_(self_, src_, async);
  increment_version(self);
  rebase_history(static_cast<Variable&>(self), std::move(grad_fn));
  return self;
}

Tensor & VariableType::resize_(Tensor & self, IntList size) const {
  auto& self_ = unpack(self, "self", 0);
  if (static_cast<Variable&>(self).requires_grad()) {
    at::runtime_error("cannot resize variables that require grad");
  }
  baseType->resize_(self_, size);
  return self;
}

Tensor & VariableType::resize_as_(Tensor & self, const Tensor & the_template) const {
  return resize_(self, the_template.sizes());
}

Tensor VariableType::contiguous(const Tensor & self) const {
  unpack(self, "self", 0);
  if (self.is_contiguous()) {
    return self;
  }
  return self.clone();
}

std::vector<int64_t> to_arg_sizes(TensorList tensors, int64_t dim) {
  std::vector<int64_t> arg_sizes(tensors.size());
  for (size_t i = 0; i < tensors.size(); ++i) {
    arg_sizes[i] = tensors[i].size(dim);
  }
  return arg_sizes;
}

${type_derived_method_definitions}

}} // namespace torch::autograd
