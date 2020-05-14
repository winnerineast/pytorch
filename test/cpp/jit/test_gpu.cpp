#if defined(USE_CUDA)
#include <test/cpp/jit/test_base.h>

#include <torch/csrc/jit/codegen/cuda/arith.h>
#include <torch/csrc/jit/codegen/cuda/fusion.h>
#include <torch/csrc/jit/codegen/cuda/ir_all_nodes.h>
#include <torch/csrc/jit/codegen/cuda/ir_iostream.h>
#include <torch/csrc/jit/codegen/cuda/kernel.h>
#include <torch/csrc/jit/codegen/cuda/lower2device.h>
#include <torch/csrc/jit/codegen/cuda/mutator.h>
#include <torch/csrc/jit/codegen/cuda/tensor_meta.h>
#include <torch/csrc/jit/codegen/cuda/transform_replay.h>

// fuser and IR parser
#include <torch/csrc/jit/codegen/cuda/parser.h>
#include "torch/csrc/jit/ir/irparser.h"

#include <iostream>

// Tests go in torch::jit
namespace torch {
namespace jit {

using namespace torch::jit::fuser;

TensorView* makeDummyTensor(int nDims, DataType dtype = DataType::Float) {
  std::vector<IterDomain*> dom;
  for (int i = 0; i < nDims; i++)
    dom.push_back(new IterDomain(new Int(0), new Int()));

  return new TensorView(new TensorDomain(dom), dtype);
}

// 1. Test cases are void() functions.
// 2. They start with the prefix `test`

void testGPU_FusionDispatch() {
  Fusion fusion;
  FusionGuard fg(&fusion);

  Float* f = new Float{2.f};
  std::stringstream ss1, ss2, ss3;
  ss1 << f;
  ss2 << static_cast<Val*>(f);
  ss3 << static_cast<Statement*>(f);
  TORCH_CHECK(
      ss1.str().compare(ss2.str()) == 0 && ss1.str().compare(ss3.str()) == 0,
      "Error with dispatch system where results differ by passing Float* vs Val* vs Statement*.");
}

void testGPU_FusionSimpleArith() {
  std::stringstream ss1, ss2;

  Fusion fusion;
  FusionGuard fg(&fusion);

  Float* f1 = new Float(1.f);
  Float* f2 = new Float{2.f};
  Float* f3 = new Float();

  // Disrupt the fusion to make sure guard works well
  {
    Fusion fusion2;
    FusionGuard fg(&fusion2);

    Float* f1 = new Float(1.f);
    Float* f2 = new Float(2.f);
    auto f3 = add(f1, f2);
    ss2 << fusion2;
  }

  new BinaryOp(BinaryOpType::Add, f3, f1, f2);
  ss1 << fusion;

  TORCH_CHECK(
      ss1.str().compare(ss2.str()) == 0,
      "Error where explicit add nodes don't match implicit add nodes.");
}

void testGPU_FusionSimpleTypePromote() {
  Fusion fusion;
  FusionGuard fg(&fusion);

  Float* f4 = new Float{4.f};
  Int* i1 = new Int{3};
  auto f5 = add(f4, i1);

  TORCH_CHECK(f5->getDataType() == DataType::Float);
}

class ZeroMutator : public OptOutMutator {
 public:
  Statement* mutate(Float* f) {
    if (f->isConst() && *(f->value()) == 1.0)
      return new Float(0.0);
    return f;
  }
  void mutate(Fusion* f) {
    OptOutMutator::mutate(f);
  }
};

void testGPU_FusionMutator() {
  Fusion fusion;
  FusionGuard fg(&fusion);

  Float* f4 = new Float{1.f};
  Int* i1 = new Int{3};
  Val* f5 = add(f4, i1);
  ZeroMutator mutator;
  mutator.mutate(&fusion);
  Val* lhs = static_cast<BinaryOp*>(fusion.origin(f5))->lhs();
  TORCH_CHECK(
      lhs->getValType().value() == ValType::Scalar &&
      lhs->getDataType().value() == DataType::Float);
  Float* flhs = static_cast<Float*>(lhs);

  TORCH_CHECK(flhs->value().value() == 0.f);
}

void testGPU_FusionRegister() {
  Fusion fusion;
  FusionGuard fg(&fusion);
  Float* v1 = new Float{1.f};
  Float* v2 = new Float{2.f};
  Val* v3 = binaryOp(BinaryOpType::Add, v1, v2);
  Val* v4 = binaryOp(BinaryOpType::Add, v1, v2);
  TORCH_CHECK(v1->name() + 1 == v2->name());
  TORCH_CHECK(v2->name() + 1 == v3->name());
  TORCH_CHECK(v3->name() + 1 == v4->name());
  TORCH_CHECK(fusion.origin(v3)->name() + 1 == fusion.origin(v4)->name());
}

// dummy expr with 2 outputs only for toposort test.
struct DummyExpr : public Expr {
  ~DummyExpr() = default;
  DummyExpr(Val* _outlhs, Val* _outrhs, Val* _lhs, Val* _rhs)
      : Expr(ExprType::BinaryOp) // Not terribly safe...
  {
    addOutput(_outlhs);
    addOutput(_outrhs);
    addInput(_lhs);
    addInput(_rhs);
    this->name_ = FusionGuard::getCurFusion()->registerExpr(this);
  }
  DummyExpr(const DummyExpr& other) = delete;
  DummyExpr& operator=(const DummyExpr& other) = delete;
  DummyExpr(DummyExpr&& other) = delete;
  DummyExpr& operator=(DummyExpr&& other) = delete;
};

void testGPU_FusionTopoSort() {
  Fusion fusion;
  FusionGuard fg(&fusion);

  // e0: v3, v2 = dummy(v1, v0)
  // e1: v4     =   add(v3, v2)
  // e2: v5     =   add(v2, v4)
  // e3: v6     =   add(v5, v5)
  Float* v0 = new Float{1.f};
  Float* v1 = new Float{2.f};
  Float* v2 = new Float();
  Float* v3 = new Float();
  Float* v4 = new Float();
  Float* v5 = new Float();
  Float* v6 = new Float();

  Expr* e0 = new DummyExpr(v3, v2, v1, v0);
  Expr* e1 = new BinaryOp(BinaryOpType::Add, v4, v3, v2);
  Expr* e2 = new BinaryOp(BinaryOpType::Add, v5, v2, v4);
  Expr* e3 = new BinaryOp(BinaryOpType::Add, v6, v5, v5);

  std::vector<Expr*> exprs = fusion.exprs();

  TORCH_CHECK(exprs.size() == 4);
  TORCH_CHECK(exprs[0] == e0);
  TORCH_CHECK(exprs[1] == e1);
  TORCH_CHECK(exprs[2] == e2);
  TORCH_CHECK(exprs[3] == e3);

  fusion.addOutput(v2);
  exprs = fusion.exprs(true);
  TORCH_CHECK(exprs.size() == 1);
  TORCH_CHECK(exprs[0] == e0);

  fusion.addOutput(v5);
  exprs = fusion.exprs(true);
  TORCH_CHECK(exprs[0] == e0);
  TORCH_CHECK(exprs[1] == e1);
  TORCH_CHECK(exprs[2] == e2);

  fusion.addOutput(v4);
  exprs = fusion.exprs(true);
  TORCH_CHECK(exprs[0] == e0);
  TORCH_CHECK(exprs[1] == e1);
  TORCH_CHECK(exprs[2] == e2);

  fusion.addOutput(v3);
  exprs = fusion.exprs(true);
  TORCH_CHECK(exprs[0] == e0);
  TORCH_CHECK(exprs[1] == e1);
  TORCH_CHECK(exprs[2] == e2);

  fusion.addOutput(v6);
  exprs = fusion.exprs(true);
  TORCH_CHECK(exprs.size() == 4);
  TORCH_CHECK(exprs[0] == e0);
  TORCH_CHECK(exprs[1] == e1);
  TORCH_CHECK(exprs[2] == e2);
  TORCH_CHECK(exprs[3] == e3);

  TORCH_CHECK(fusion.origin(v2)->name() == 0);
  TORCH_CHECK(fusion.origin(v3)->name() == 0);
  TORCH_CHECK(fusion.origin(v4)->name() == 1);
  TORCH_CHECK(fusion.origin(v5)->name() == 2);
  TORCH_CHECK(fusion.origin(v6)->name() == 3);
}

void testGPU_FusionTensor() {
  auto options = at::TensorOptions().dtype(at::kFloat).device(at::kCUDA, 0);

  auto tensor = at::randn({2, 3, 4, 5}, options);
  auto sizes = tensor.sizes().vec();
  auto tensor_type = TensorType::create(tensor);

  Fusion fusion;
  FusionGuard fg(&fusion);

  auto fuser_tensor = new TensorView(tensor_type);
  TORCH_CHECK(fuser_tensor->getDataType().value() == DataType::Float);
  TORCH_CHECK(fuser_tensor->domain() != nullptr);
}

void testGPU_FusionTensorContiguity() {
  {
    // NCHW memory layout
    auto tensor = at::randn({2, 3, 4, 5});
    auto sizes = tensor.sizes().vec();
    auto strides = tensor.strides().vec();
    TensorContiguity t_c(sizes, strides);
    TORCH_CHECK(t_c.rank() == 4);
    TORCH_CHECK(t_c.getBroadcastDims().size() == 0);
    for (int i = 0; i < 4; i++) {
      TORCH_CHECK(!t_c.isBroadcastDim(i));
      if (i < 3) {
        TORCH_CHECK(t_c.canCollapseToHigher(i));
      }
    }
  }

  {
    // NHWC memory layout
    TensorContiguity t_c({2, 3, 4, 5}, {60, 1, 15, 3});
    TORCH_CHECK(t_c.rank() == 4);
    TORCH_CHECK(t_c.getBroadcastDims().size() == 0);
    for (int i = 0; i < 4; i++) {
      TORCH_CHECK(!t_c.isBroadcastDim(i));
      if (i < 3) {
        TORCH_CHECK((t_c.canCollapseToHigher(i) ^ (i != 2)));
      }
    }
  }

  {
    // NHWC memory layout with broadcast
    TensorContiguity t_c({2, 3, 4, 5}, {120, 0, 30, 3});
    TORCH_CHECK(t_c.rank() == 4);
    auto b_dims = t_c.getBroadcastDims();
    TORCH_CHECK(b_dims.size() == 1 && b_dims[0] == 1);
    for (int i = 0; i < 4; i++) {
      TORCH_CHECK(!(t_c.isBroadcastDim(i)) ^ (i == 1));
      if (i < 3) {
        TORCH_CHECK(!(t_c.canCollapseToHigher(i)));
      }
    }
  }

  {
    // contiguity across size-1 dimension
    auto tensor = at::randn({4, 1, 4});
    auto sizes = tensor.sizes().vec();
    auto strides = tensor.strides().vec();
    auto dim = sizes.size();
    TensorContiguity t_c(sizes, strides);
    TORCH_CHECK(t_c.rank() == sizes.size());
    auto b_dims = t_c.getBroadcastDims();
    TORCH_CHECK(b_dims.size() == 0);
    TORCH_CHECK(t_c.getFCD() == 2);
    TORCH_CHECK(t_c.hasContiguousFCD());
    for (int i = 0; i < dim; i++) {
      TORCH_CHECK(!t_c.isBroadcastDim(i));
      if (i < dim - 1) {
        TORCH_CHECK(t_c.canCollapseToHigher(i));
      }
    }
  }

  {
    // no contiguity across size-1 dimension
    auto tensor = at::randn({4, 4, 4}).split(1, 1)[0];
    auto sizes = tensor.sizes().vec();
    auto strides = tensor.strides().vec();
    TensorContiguity t_c(sizes, strides);
    TORCH_CHECK(!(t_c.canCollapseToHigher(0)));
    TORCH_CHECK((t_c.canCollapseToHigher(1)));
  }

  {
    // no contiguity across size-1 dimension
    auto tensor = at::randn({4, 1, 8}).split(4, 2)[0];
    auto sizes = tensor.sizes().vec();
    auto strides = tensor.strides().vec();
    TensorContiguity t_c(sizes, strides);
    TORCH_CHECK((t_c.canCollapseToHigher(0)));
    TORCH_CHECK((!t_c.canCollapseToHigher(1)));
  }

  {
    // no contiguity across size-1 dimension
    auto tensor = at::randn({8, 1, 4}).split(4, 0)[0];
    auto sizes = tensor.sizes().vec();
    auto strides = tensor.strides().vec();
    TensorContiguity t_c(sizes, strides);
    TORCH_CHECK((t_c.canCollapseToHigher(0)));
    TORCH_CHECK((t_c.canCollapseToHigher(1)));
  }

  {
    // test merge
    TensorContiguity t_c_l({4, 4, 4}, {16, 4, 1});
    TensorContiguity t_c_r({4, 4, 4}, {16, 4, 1});
    t_c_l.merge(t_c_r);
    TORCH_CHECK((t_c_l.isIdentical(t_c_r)));
  }

  {
    TensorContiguity t_c_l({4, 4, 4, 4}, {16, 0, 4, 1});
    TensorContiguity t_c_r({4, 4, 4, 4}, {64, 16, 4, 1});
    t_c_l.merge(t_c_r);
    TORCH_CHECK(t_c_l.getFCD() == 3);
    TORCH_CHECK(t_c_l.getAxisByStride(0) == 0);
  }

  {
    // NHWC + NCHW
    TensorContiguity t_c_l({4, 4, 4, 4}, {64, 16, 4, 1});
    TensorContiguity t_c_r({4, 4, 4, 4}, {64, 1, 16, 4});
    t_c_l.merge(t_c_r);
    TORCH_CHECK(!t_c_l.hasContiguousFCD());
    TORCH_CHECK(t_c_l.getFCD() == -1);
    TORCH_CHECK(t_c_l.getAxisByStride(0) == 0);
    TORCH_CHECK(t_c_l.getAxisByStride(1) == -1);
    TORCH_CHECK(t_c_l.getAxisByStride(2) == -1);
    TORCH_CHECK(t_c_l.getAxisByStride(3) == -1);
  }

  {
    // NCHW + NCHW with broadcasting
    TensorContiguity t_c_l({4, 4, 4, 4}, {4, 1, 4, 0});
    TensorContiguity t_c_r({4, 4, 4, 4}, {64, 1, 16, 4});
    t_c_l.merge(t_c_r);
    TORCH_CHECK(t_c_l.getFCD() == 1);
    TORCH_CHECK(t_c_l.getAxisByStride(0) == 0);
  }
}

void testGPU_FusionTVSplit() {
  Fusion fusion;
  FusionGuard fg(&fusion);

  TensorView* tv = makeDummyTensor(3);

  tv = tv->split(2, 2);
  TORCH_CHECK(tv->nDims() == 4);
  Expr* outer = tv->axis(2)->extent()->getOrigin();

  TORCH_CHECK(
      outer->getExprType().value() == ExprType::BinaryOp &&
      static_cast<BinaryOp*>(outer)->getBinaryOpType() ==
          BinaryOpType::CeilDiv &&
      static_cast<BinaryOp*>(outer)->lhs()->sameAs(
          tv->getRootDomain()->axis(2)->extent()) &&
      static_cast<Int*>(static_cast<BinaryOp*>(outer)->rhs())
          ->sameAs(new Int(2)));

  IterDomain* inner = static_cast<IterDomain*>(tv->axis(3));
  TORCH_CHECK(
      inner->extent()->isScalar() &&
      static_cast<Int*>(inner->extent())->isConst() &&
      static_cast<Int*>(inner->extent())->value().value() == 2);
}

void testGPU_FusionTVMerge() {
  Fusion fusion;
  FusionGuard fg(&fusion);

  TensorView* tv = makeDummyTensor(3);

  tv = tv->merge(1);
  Expr* axisOp = tv->axis(1)->extent()->getOrigin();

  TORCH_CHECK(
      tv->nDims() == 2 && axisOp->getExprType() == ExprType::BinaryOp &&
      static_cast<BinaryOp*>(axisOp)->getBinaryOpType() == BinaryOpType::Mul &&
      static_cast<BinaryOp*>(axisOp)->lhs() ==
          tv->getRootDomain()->axis(1)->extent() &&
      static_cast<BinaryOp*>(axisOp)->rhs() ==
          tv->getRootDomain()->axis(2)->extent());
}

void testGPU_FusionTVReorder() {
  Fusion fusion;
  FusionGuard fg(&fusion);

  TensorView* dummyTensor = makeDummyTensor(3);

  std::unordered_map<int, int> shift_right{{-1, 0}};

  std::unordered_map<int, int> shift_left{{0, -1}};

  std::unordered_map<int, int> shift_left_2{{0, -1}, {1, 0}, {2, 1}};

  std::unordered_map<int, int> swap{{0, 2}, {2, 0}};
  TensorView* ref = dummyTensor->clone();
  TensorView* tv = dummyTensor->clone();

  TensorView* s_leftl = tv->reorder(shift_left);
  for (int i = 0; i < tv->nDims(); i++)
    TORCH_CHECK(ref->axis(i) == s_leftl->axis(i - 1));

  tv = dummyTensor->clone();
  TensorView* s_left2 = tv->reorder(shift_left);
  for (int i = 0; i < tv->nDims(); i++)
    TORCH_CHECK(ref->axis(i) == s_left2->axis(i - 1));

  tv = dummyTensor->clone();
  TensorView* s_right = tv->reorder(shift_right);
  for (int i = 0; i < tv->nDims(); i++)
    TORCH_CHECK(ref->axis(i - 1) == s_right->axis(i));

  tv = dummyTensor->clone();
  TensorView* rswap = tv->reorder(swap);
  TORCH_CHECK(ref->axis(0) == rswap->axis(2));
  TORCH_CHECK(ref->axis(2) == rswap->axis(0));
  TORCH_CHECK(ref->axis(1) == rswap->axis(1));
}

void testGPU_FusionEquality() {
  Fusion fusion;
  FusionGuard fg(&fusion);

  Float* fval1 = new Float();
  Float* fval1_copy = fval1;
  Float* fval2 = new Float();
  Float* fone = new Float(1.0);

  TORCH_CHECK(fval1->sameAs(fval1_copy));
  TORCH_CHECK(!fval1->sameAs(fval2));
  TORCH_CHECK(!fone->sameAs(fval1));
  TORCH_CHECK(fone->sameAs(new Float(1.0)));

  Int* ival1 = new Int();
  Int* ival1_copy = ival1;
  Int* ival2 = new Int();
  Int* ione = new Int(1);

  TORCH_CHECK(ival1->sameAs(ival1_copy));
  TORCH_CHECK(!ival1->sameAs(ival2));
  TORCH_CHECK(!ione->sameAs(ival1));
  TORCH_CHECK(ione->sameAs(new Int(1)));

  BinaryOp* add1 = new BinaryOp(BinaryOpType::Add, new Float(), fval1, ival1);
  BinaryOp* add1_copy =
      new BinaryOp(BinaryOpType::Add, new Float(), fval1, ival1);
  BinaryOp* sub1 = new BinaryOp(BinaryOpType::Sub, new Float(), fval1, ival1);

  UnaryOp* neg1 = new UnaryOp(UnaryOpType::Neg, new Float(), fval1);
  UnaryOp* neg2 = new UnaryOp(UnaryOpType::Neg, new Float(), fval2);
  UnaryOp* neg1_copy = new UnaryOp(UnaryOpType::Neg, new Float(), fval1);

  TORCH_CHECK(add1->sameAs(add1_copy));
  TORCH_CHECK(!add1->sameAs(sub1));

  TORCH_CHECK(neg1->sameAs(neg1_copy));
  TORCH_CHECK(!static_cast<Expr*>(neg1)->sameAs(add1));
  TORCH_CHECK(!neg1->sameAs(neg2));
}

void testGPU_FusionReplaceAll() {
  Fusion fusion;
  FusionGuard fg(&fusion);

  Float* f0 = new Float();
  Float* f1 = new Float{1.f};
  Float* f2 = new Float{2.f};
  Float* f3 = new Float();
  Float* f4 = static_cast<Float*>(add(f1, f0));

  // replace the output f4 with f3
  ReplaceAll::instancesOf(f4, f3);
  // f3 should now have an origin function
  TORCH_CHECK(fusion.origin(f3) != nullptr);

  // Should have removed f4 completely so we shouldn't have any other expr than
  // f3 construction
  TORCH_CHECK(fusion.exprs().size() == 1);

  // Replace constant Float's of value 1.f with 2.f
  ReplaceAll::instancesOf(f1, f2);
  BinaryOp* bop = static_cast<BinaryOp*>(fusion.origin(f3));
  // make sure the binary op (origin of f3) actually changed to 2.f
  TORCH_CHECK(static_cast<Float*>(bop->lhs())->sameAs(new Float{2.f}));
}

void testGPU_FusionParser() {
  auto g = std::make_shared<Graph>();
  const auto graph0_string = R"IR(
    graph(%0 : Float(2:1),
          %1 : Float(2:1)):
      %c0 : Float(2:1) = aten::mul(%0, %1)
      %d0 : Float(2:1) = aten::mul(%c0, %0)
      return (%d0))IR";
  torch::jit::parseIR(graph0_string, g.get());

  // strides are not yet supported in the irparser.
  for (auto val : g->block()->inputs()) {
    if (val->isCompleteTensor())
      val->setType(val->type()->cast<TensorType>()->contiguous());
  }
  for (auto node : g->block()->nodes()) {
    for (auto val : node->outputs()) {
      if (val->isCompleteTensor())
        val->setType(val->type()->cast<TensorType>()->contiguous());
    }
  }

  Fusion fusion;
  FusionGuard fg(&fusion);
  torch::jit::fuser::cuda::CudaKernel prog;
  // These can be set to anything as there are no bindings!
  // All CTAS and threads execute the same thing.
  prog.grid(4);
  prog.block(32);
  prog.device_ = 0;
  fuser::cuda::parseJitIR(g, fusion, &prog);

  std::stringstream ref;
  ref << "__global__ void CUDAGeneratedKernel(Tensor<float, 1> T0, Tensor<float, 1> T1, Tensor<float, 1> T3){\n"
      << "  float T2[4];\n"
      << "  if ( ( ( ( ( ( blockIdx.x * 4 ) + ( 4 - 1 ) ) * 128 ) + threadIdx.x ) < T1.size[0] ) ) { \n"
      << "    for(size_t i64 = 0; i64 < 4; ++i64 ) {\n"
      << "      T2[ i64 ]\n"
      << "         = T0[ ( ( ( ( ( blockIdx.x * 4 ) + i64 ) * 128 ) + threadIdx.x ) * T0.stride[0] ) ]\n"
      << "         * T1[ ( ( ( ( ( blockIdx.x * 4 ) + i64 ) * 128 ) + threadIdx.x ) * T1.stride[0] ) ];\n"
      << "    }\n"
      << "  } else { \n"
      << "    for(size_t i64 = 0; i64 < 4; ++i64 ) {\n"
      << "      if ( ( ( ( ( ( blockIdx.x * 4 ) + i64 ) * 128 ) + threadIdx.x ) < T1.size[0] ) ) { \n"
      << "        T2[ i64 ]\n"
      << "           = T0[ ( ( ( ( ( blockIdx.x * 4 ) + i64 ) * 128 ) + threadIdx.x ) * T0.stride[0] ) ]\n"
      << "           * T1[ ( ( ( ( ( blockIdx.x * 4 ) + i64 ) * 128 ) + threadIdx.x ) * T1.stride[0] ) ];\n"
      << "      }\n"
      << "    }\n"
      << "  }\n"
      << "  if ( ( ( ( ( ( blockIdx.x * 4 ) + ( 4 - 1 ) ) * 128 ) + threadIdx.x ) < T3.size[0] ) ) { \n"
      << "    for(size_t i65 = 0; i65 < 4; ++i65 ) {\n"
      << "      T3[ ( ( ( ( ( blockIdx.x * 4 ) + i65 ) * 128 ) + threadIdx.x ) * T3.stride[0] ) ]\n"
      << "         = T2[ i65 ]\n"
      << "         * T0[ ( ( ( ( ( blockIdx.x * 4 ) + i65 ) * 128 ) + threadIdx.x ) * T0.stride[0] ) ];\n"
      << "    }\n"
      << "  } else { \n"
      << "    for(size_t i65 = 0; i65 < 4; ++i65 ) {\n"
      << "      if ( ( ( ( ( ( blockIdx.x * 4 ) + i65 ) * 128 ) + threadIdx.x ) < T3.size[0] ) ) { \n"
      << "        T3[ ( ( ( ( ( blockIdx.x * 4 ) + i65 ) * 128 ) + threadIdx.x ) * T3.stride[0] ) ]\n"
      << "           = T2[ i65 ]\n"
      << "           * T0[ ( ( ( ( ( blockIdx.x * 4 ) + i65 ) * 128 ) + threadIdx.x ) * T0.stride[0] ) ];\n"
      << "      }\n"
      << "    }\n"
      << "  }\n"
      << "}\n";

  GPULower gpulw(&fusion);
  std::stringstream cdg;
  gpulw.printKernel(cdg);
  if (ref.str().size() != cdg.str().size() ||
      ref.str().compare(cdg.str()) != 0) {
    std::cerr
        << " Codegen mismatch, codegen possibly changed, or is incorrect.\n"
        << " Length REF: " << ref.str().size() << "\n"
        << " Length RESULT: " << cdg.str().size()
        << " \n ========= REF ========= \n"
        << ref.str() << "\n========= RESULT ========== \n"
        << cdg.str() << "\n=================" << std::endl;
    TORCH_CHECK(false);
  }
}

void testGPU_FusionDependency() {
  Fusion fusion;
  FusionGuard fg(&fusion);

  Float* f0 = new Float(0.f);
  Float* f1 = new Float(1.f);
  auto f2 = add(f0, f1);

  auto f3 = add(f2, f2);

  Float* f4 = new Float(4.f);
  Float* f5 = new Float(5.f);
  auto f6 = add(f4, f5);

  Float* f7 = new Float(7.f);
  Float* f8 = new Float(8.f);
  auto f9 = add(f7, f8);

  auto f10 = add(f6, f9);

  auto f11 = add(f3, f10);

  TORCH_CHECK(DependencyCheck::isDependencyOf(f0, f11));
  TORCH_CHECK(DependencyCheck::isDependencyOf(f1, f11));
  TORCH_CHECK(DependencyCheck::isDependencyOf(f2, f11));
  TORCH_CHECK(DependencyCheck::isDependencyOf(f3, f11));
  TORCH_CHECK(DependencyCheck::isDependencyOf(f6, f11));
  TORCH_CHECK(DependencyCheck::isDependencyOf(f9, f11));
  TORCH_CHECK(DependencyCheck::isDependencyOf(f0, f2));
  TORCH_CHECK(DependencyCheck::isDependencyOf(f2, f3));
  TORCH_CHECK(DependencyCheck::isDependencyOf(f4, f6));
  TORCH_CHECK(DependencyCheck::isDependencyOf(f8, f10));

  TORCH_CHECK(!DependencyCheck::isDependencyOf(f11, f0));
  TORCH_CHECK(!DependencyCheck::isDependencyOf(f11, f1));
  TORCH_CHECK(!DependencyCheck::isDependencyOf(f11, f2));
  TORCH_CHECK(!DependencyCheck::isDependencyOf(f11, f3));
  TORCH_CHECK(!DependencyCheck::isDependencyOf(f11, f4));
  TORCH_CHECK(!DependencyCheck::isDependencyOf(f11, f5));
  TORCH_CHECK(!DependencyCheck::isDependencyOf(f2, f0));
  TORCH_CHECK(!DependencyCheck::isDependencyOf(f3, f2));
  TORCH_CHECK(!DependencyCheck::isDependencyOf(f6, f4));
  TORCH_CHECK(!DependencyCheck::isDependencyOf(f10, f8));

  std::stack<Val*> dep_chain = DependencyCheck::getDependencyChain(f0, f11);
  TORCH_CHECK(dep_chain.top() == f11);
  dep_chain.pop();
  TORCH_CHECK(dep_chain.top() == f3);
  dep_chain.pop();
  TORCH_CHECK(dep_chain.top() == f2);
  dep_chain.pop();

  dep_chain = DependencyCheck::getDependencyChain(f6, f11);
  TORCH_CHECK(dep_chain.top() == f11);
  dep_chain.pop();
  TORCH_CHECK(dep_chain.top() == f10);
  dep_chain.pop();

  dep_chain = DependencyCheck::getDependencyChain(f4, f11);
  TORCH_CHECK(dep_chain.top() == f11);
  dep_chain.pop();
  TORCH_CHECK(dep_chain.top() == f10);
  dep_chain.pop();
  TORCH_CHECK(dep_chain.top() == f6);
  dep_chain.pop();

  dep_chain = DependencyCheck::getDependencyChain(f11, f2);
  TORCH_CHECK(dep_chain.empty());
}

void testGPU_FusionCodeGen() {
  Fusion fusion;
  FusionGuard fg(&fusion);

  TensorView* tv0 = makeDummyTensor(3);

  new BinaryOp(BinaryOpType::Add, tv0, new Float(0.0), new Float(1.0));
  TensorView* tv1 = static_cast<TensorView*>(add(tv0, new Float(2.0)));
  TensorView* tv2 = static_cast<TensorView*>(add(tv1, new Float(3.0)));

  //[I0, I1, I2]
  tv2 = tv2->split(0, 4);
  //[I0o, I0i{4}, I1, I2]
  tv2 = tv2->merge(1);
  //[I0o, I0i{4}*I1, I2]
  tv2 = tv2->split(-1, 2);
  //[I0o, I0i{4}*I1, I2o, I2i{2}]
  tv2 = tv2->reorder({{0, 1}, {1, 0}, {3, 2}});
  //[I0i{4}*I1, I0o, I2i{2}, I2o]
  fusion.addOutput(tv2);

  tv0->computeAt(tv2, -1);

  std::stringstream ref;
  ref << "__global__ void CUDAGeneratedKernel(Tensor<float, 3> T2){\n"
      << "  for(size_t i82 = 0; i82 < ( 4 * T2.size[1] ); ++i82 ) {\n"
      << "    for(size_t i83 = 0; i83 < ( ceilDiv(T2.size[0], 4) ); ++i83 ) {\n"
      << "      for(size_t i84 = 0; i84 < 2; ++i84 ) {\n"
      << "        for(size_t i85 = 0; i85 < ( ceilDiv(T2.size[2], 2) ); ++i85 ) {\n"
      << "          float T0[1];\n"
      << "          if ( ( ( ( ( ( i83 * 4 ) + ( i82 / T2.size[1] ) ) < T2.size[0] ) && ( ( i82 % T2.size[1] ) < T2.size[1] ) ) && ( ( ( i85 * 2 ) + i84 ) < T2.size[2] ) ) ) { \n"
      << "            T0[ 0 ]\n"
      << "               = float(0)\n"
      << "               + float(1);\n"
      << "          }\n"
      << "          float T1[1];\n"
      << "          if ( ( ( ( ( ( i83 * 4 ) + ( i82 / T2.size[1] ) ) < T2.size[0] ) && ( ( i82 % T2.size[1] ) < T2.size[1] ) ) && ( ( ( i85 * 2 ) + i84 ) < T2.size[2] ) ) ) { \n"
      << "            T1[ 0 ]\n"
      << "               = T0[ 0 ]\n"
      << "               + float(2);\n"
      << "          }\n"
      << "          if ( ( ( ( ( ( i83 * 4 ) + ( i82 / T2.size[1] ) ) < T2.size[0] ) && ( ( i82 % T2.size[1] ) < T2.size[1] ) ) && ( ( ( i85 * 2 ) + i84 ) < T2.size[2] ) ) ) { \n"
      << "            T2[ ( ( ( i83 * 4 ) + ( i82 / T2.size[1] ) ) * T2.stride[0] ) + ( ( i82 % T2.size[1] ) * T2.stride[1] ) + ( ( ( i85 * 2 ) + i84 ) * T2.stride[2] ) ]\n"
      << "               = T1[ 0 ]\n"
      << "               + float(3);\n"
      << "          }\n"
      << "        }\n"
      << "      }\n"
      << "    }\n"
      << "  }\n"
      << "}\n";

  GPULower gpulw(&fusion);
  std::stringstream cdg;
  gpulw.printKernel(cdg);

  if (ref.str().size() != cdg.str().size() ||
      ref.str().compare(cdg.str()) != 0) {
    std::cerr
        << " Codegen mismatch, codegen possibly changed, or is incorrect. "
        << " \n ========= REF ========= \n"
        << ref.str() << "\n========= RESULT ========== \n"
        << cdg.str() << "\n=================" << std::endl;
    TORCH_CHECK(false);
  }

  torch::jit::fuser::cuda::CudaKernel prog;
  prog.device_ = 0;
  // These can be set to anything as there are no bindings!
  // All CTAS and threads execute the same thing.
  prog.grid(4);
  prog.block(32);

  auto options = at::TensorOptions().dtype(at::kFloat).device(at::kCUDA, 0);

  at::Tensor output = at::empty({16, 8, 8}, options);
  std::vector<at::Tensor> outputs{{output}};

  torch::jit::fuser::cuda::compileKernel(fusion, &prog);
  torch::jit::fuser::cuda::runTestKernel(&prog, {}, outputs);

  at::Tensor output_ref = at::zeros_like(output, options);
  output_ref = output_ref + 0.0 + 1.0 + 2.0 + 3.0;

  TORCH_CHECK(output_ref.equal(output));
}

void testGPU_FusionCodeGen2() {
  Fusion fusion;
  FusionGuard fg(&fusion);

  TensorView* tv0 = makeDummyTensor(3);
  TensorView* tv1 = makeDummyTensor(3);
  TensorView* tv2 = static_cast<TensorView*>(add(tv1, new Float(2.0)));
  TensorView* tv3 = static_cast<TensorView*>(add(tv0, tv2));

  fusion.addInput(tv0);
  fusion.addInput(tv1);
  fusion.addOutput(tv3);

  //[I0, I1, I2]
  tv3->reorder({{0, 2}, {2, 0}});
  //[I2, I1, I0]
  tv3->split(-1, 4);
  //[I2, I1, I0o, I0i{4}]
  tv3->reorder({{2, 0}, {3, 1}, {0, 3}});
  // I0o, I0i{4}, I1, I2]

  tv0->computeAt(tv3, -1);
  tv1->computeAt(tv3, -1);

  tv3->axis(0)->parallelize(ParallelType::BIDx);
  tv3->axis(-1)->parallelize(ParallelType::TIDx);

  std::stringstream ref;
  ref << "__global__ void CUDAGeneratedKernel(Tensor<float, 3> T0, Tensor<float, 3> T1, Tensor<float, 3> T3){\n"
      << "  for(size_t i33 = 0; i33 < 4; ++i33 ) {\n"
      << "    for(size_t i34 = 0; i34 < T3.size[1]; ++i34 ) {\n"
      << "      float T2[1];\n"
      << "      if ( ( ( ( blockIdx.x * 4 ) + i33 ) < T3.size[0] ) ) { \n"
      << "        T2[ 0 ]\n"
      << "           = T1[ ( ( ( blockIdx.x * 4 ) + i33 ) * T1.stride[0] ) + ( i34 * T1.stride[1] ) + ( threadIdx.x * T1.stride[2] ) ]\n"
      << "           + float(2);\n"
      << "      }\n"
      << "      if ( ( ( ( blockIdx.x * 4 ) + i33 ) < T3.size[0] ) ) { \n"
      << "        T3[ ( ( ( blockIdx.x * 4 ) + i33 ) * T3.stride[0] ) + ( i34 * T3.stride[1] ) + ( threadIdx.x * T3.stride[2] ) ]\n"
      << "           = T0[ ( ( ( blockIdx.x * 4 ) + i33 ) * T0.stride[0] ) + ( i34 * T0.stride[1] ) + ( threadIdx.x * T0.stride[2] ) ]\n"
      << "           + T2[ 0 ];\n"
      << "      }\n"
      << "    }\n"
      << "  }\n"
      << "}\n";

  GPULower gpulw(&fusion);
  std::stringstream cdg;
  gpulw.printKernel(cdg);

  if (ref.str().size() != cdg.str().size() ||
      ref.str().compare(cdg.str()) != 0) {
    std::cerr
        << " Codegen mismatch, codegen possibly changed, or is incorrect. "
        << " \n ========= REF ========= \n"
        << ref.str() << "\n========= RESULT ========== \n"
        << cdg.str() << "\n=================" << std::endl;
    TORCH_CHECK(false);
  }

  torch::jit::fuser::cuda::CudaKernel prog;
  prog.device_ = 0;
  prog.grid(4);
  prog.block(8);

  auto options = at::TensorOptions().dtype(at::kFloat).device(at::kCUDA, 0);

  at::Tensor input1 = at::randn({16, 8, 8}, options);
  at::Tensor input2 = at::randn_like(input1);

  at::Tensor output = at::empty_like(input1);
  std::array<IValue, 2> inputs = {input1, input2};
  const at::ArrayRef<IValue> input_ivalues(inputs);
  std::vector<at::Tensor> outputs{{output}};

  torch::jit::fuser::cuda::compileKernel(fusion, &prog);
  torch::jit::fuser::cuda::runTestKernel(&prog, input_ivalues, outputs);

  at::Tensor tv2_ref = input2 + 2.0;
  at::Tensor output_ref = input1 + tv2_ref;

  TORCH_CHECK(output_ref.equal(output));
}

void testGPU_FusionCodeGen3() {
  Fusion fusion;
  FusionGuard fg(&fusion);

  TensorView* tv0 = makeDummyTensor(3);
  TensorView* tv1 = makeDummyTensor(3);
  TensorView* tv2 = makeDummyTensor(3);
  TensorView* tv3 = static_cast<TensorView*>(add(tv0, new Float(2.0)));
  TensorView* tv4 = static_cast<TensorView*>(add(tv3, tv1));
  TensorView* tv5 = static_cast<TensorView*>(add(tv3, tv2));

  fusion.addInput(tv0);
  fusion.addInput(tv1);
  // Don't forget to add tv2 as an input.
  fusion.addInput(tv2);
  fusion.addOutput(tv4);
  fusion.addOutput(tv5);

  // Transform and compute at *before* calling parallelize. Compute at can
  // completely change thread bindings of intermediate stages.

  tv4->merge(0);
  tv4->merge(0);
  tv4->split(0, 128);
  tv4->split(0, 4);

  tv5->merge(0);
  tv5->merge(0);
  tv5->split(0, 128);
  tv5->split(0, 4);

  tv3->computeAt(tv4, 1);
  tv3->computeAt(tv5, 1);

  tv4->axis(0)->parallelize(ParallelType::BIDx);
  tv4->axis(1)->parallelize(ParallelType::Unroll);
  tv4->axis(-1)->parallelize(ParallelType::TIDx);

  tv5->axis(0)->parallelize(ParallelType::BIDx);
  tv5->axis(1)->parallelize(ParallelType::Unroll);
  tv5->axis(-1)->parallelize(ParallelType::TIDx);

  tv3->axis(-2)->parallelize(ParallelType::Unroll);
  tv3->axis(-1)->parallelize(ParallelType::TIDx);

  GPULower gpulw(&fusion);
  std::stringstream cdg;
  gpulw.printKernel(cdg);
}

void testGPU_FusionCodeGen4() {
  Fusion fusion;
  FusionGuard fg(&fusion);

  TensorView* tv0 = makeDummyTensor(3);
  TensorView* tv1 = makeDummyTensor(3);
  TensorView* tv2 = makeDummyTensor(3);
  TensorView* tv3 = static_cast<TensorView*>(add(tv0, new Float(2.0)));
  // These two lines are the only place where I changed from CodeGen3.
  TensorView* tv5 = static_cast<TensorView*>(add(tv3, tv2));
  TensorView* tv4 = static_cast<TensorView*>(add(tv3, tv1));

  fusion.addInput(tv0);
  fusion.addInput(tv1);
  // Don't forget to add tv2 as an input.
  fusion.addInput(tv2);
  fusion.addOutput(tv4);
  fusion.addOutput(tv5);

  // Transform and compute at *before* calling parallelize. Compute at can
  // completely change thread bindings of intermediate stages.

  tv4->merge(0);
  tv4->merge(0);
  tv4->split(0, 128);
  tv4->split(0, 4);

  tv5->merge(0);
  tv5->merge(0);
  tv5->split(0, 128);
  tv5->split(0, 4);

  tv3->computeAt(tv4, 1);
  tv3->computeAt(tv5, 1);

  tv4->axis(0)->parallelize(ParallelType::BIDx);
  tv4->axis(1)->parallelize(ParallelType::Unroll);
  tv4->axis(-1)->parallelize(ParallelType::TIDx);

  tv5->axis(0)->parallelize(ParallelType::BIDx);
  tv5->axis(1)->parallelize(ParallelType::Unroll);
  tv5->axis(-1)->parallelize(ParallelType::TIDx);

  tv3->axis(-2)->parallelize(ParallelType::Unroll);
  tv3->axis(-1)->parallelize(ParallelType::TIDx);

  std::cout << "start code gen" << std::endl;

  GPULower gpulw(&fusion);
  std::stringstream cdg;
  gpulw.printKernel(cdg);
  std::cout << cdg.str() << std::endl;
}

void testGPU_FusionSimplePWise() {
  Fusion fusion;
  FusionGuard fg(&fusion);
  // dimensionality of the problem
  int nDims = 3;

  // Set up your input tensor views
  TensorView* tv0 = makeDummyTensor(nDims);
  TensorView* tv1 = makeDummyTensor(nDims);

  // Register your inputs
  fusion.addInput(tv0);
  fusion.addInput(tv1);

  // Do math with it, it returns a `Val*` but can be static_casted back to
  // TensorView
  TensorView* tv2 = static_cast<TensorView*>(add(tv1, new Float(2.0)));
  TensorView* tv3 = static_cast<TensorView*>(add(tv0, tv2));

  // Register your outputs
  fusion.addOutput(tv3);

  // Do transformations, remember, transformations are outputs to inputs
  // This doesn't have to be in this order
  tv3->merge(1);
  tv3->merge(0);

  // Split by n_threads
  tv3->split(-1, 128 * 2);
  tv3->split(-1, 128);

  // For all inputs, computeAt the output inline, temporaries should be squeezed
  // between them
  tv0->computeAt(tv3, -1);
  tv1->computeAt(tv3, -1);

  // Parallelize TV3
  tv3->axis(0)->parallelize(ParallelType::BIDx);
  tv3->axis(-2)->parallelize(ParallelType::TIDy);
  tv3->axis(-1)->parallelize(ParallelType::TIDx);

  torch::jit::fuser::cuda::CudaKernel prog;
  prog.device_ = 0;
  prog.grid(64); //   1 CTA
  prog.block(128, 2); // 256 Threads

  auto options = at::TensorOptions().dtype(at::kFloat).device(at::kCUDA, 0);

  at::Tensor input1 = at::randn({64, 2, 128}, options);
  at::Tensor input2 = at::randn_like(input1);

  at::Tensor output = at::empty_like(input1);
  std::array<IValue, 2> inputs = {input1, input2};
  const at::ArrayRef<IValue> input_ivalues(inputs);
  std::vector<at::Tensor> outputs{{output}};

  torch::jit::fuser::cuda::compileKernel(fusion, &prog);
  torch::jit::fuser::cuda::runTestKernel(&prog, input_ivalues, outputs);

  at::Tensor tv2_ref = input2 + 2.0;
  at::Tensor output_ref = input1 + tv2_ref;

  TORCH_CHECK(output_ref.equal(output));
}

void testGPU_FusionExecKernel() {
  Fusion fusion;
  FusionGuard fg(&fusion);

  // Set up your input tensor views
  TensorView* tv0 = makeDummyTensor(2);
  TensorView* tv1 = makeDummyTensor(2);

  // Register your inputs
  fusion.addInput(tv0);
  fusion.addInput(tv1);

  // Do math with it, it returns a `Val*` but can be static_casted back to
  // TensorView
  TensorView* tv2 = static_cast<TensorView*>(add(tv1, new Float(2.0)));
  TensorView* tv3 = static_cast<TensorView*>(add(tv0, tv2));

  // Register your outputs
  fusion.addOutput(tv3);

  tv3->split(0, 4);

  // For all inputs, computeAt the output inline, temporaries should be squeezed
  // between them
  tv0->computeAt(tv3, 1);
  tv1->computeAt(tv3, 1);

  // Parallelize TV3
  tv3->axis(0)->parallelize(ParallelType::BIDx);
  tv2->axis(1)->parallelize(ParallelType::Unroll);
  tv3->axis(1)->parallelize(ParallelType::Unroll);
  tv2->axis(-1)->parallelize(ParallelType::TIDx);
  tv3->axis(-1)->parallelize(ParallelType::TIDx);

  torch::jit::fuser::cuda::CudaKernel prog;
  prog.device_ = 0;
  prog.grid(1); // 1 CTA
  prog.block(128); // 128 Threads

  auto options = at::TensorOptions().dtype(at::kFloat).device(at::kCUDA, 0);

  at::Tensor input1 = at::ones({1, 128}, options);
  at::Tensor input2 = at::ones_like(input1);

  at::Tensor output = at::empty_like(input1);
  std::array<IValue, 2> inputs = {input1, input2};
  const at::ArrayRef<IValue> input_ivalues(inputs);
  std::vector<at::Tensor> outputs{{output}};

  torch::jit::fuser::cuda::compileKernel(fusion, &prog);
  torch::jit::fuser::cuda::runTestKernel(&prog, input_ivalues, outputs);

  at::Tensor check = at::full({1, 128}, 4, options);
  ;
  TORCH_CHECK(output.equal(check));
}

void testGPU_FusionForLoop() {
  Fusion fusion;
  FusionGuard fg(&fusion);

  const auto TV0 = new TensorView(
      new TensorDomain({new IterDomain(new Int(0), new Int(16))}),
      DataType::Float);
  const auto TV1 = new TensorView(
      new TensorDomain({new IterDomain(new Int(0), new Int(16))}),
      DataType::Float);

  fusion.addInput(TV0);
  fusion.addInput(TV1);

  auto ID0 = new IterDomain(new Int(0), new Int(8));

  TensorView* TV2 = static_cast<TensorView*>(add(TV0, TV1));
  BinaryOp* op = static_cast<BinaryOp*>(TV2->getOrigin());
  fusion.addOutput(TV2);

  ForLoop* fl = new ForLoop(new Int(), ID0, {op});
  std::stringstream result;
  std::stringstream ref;
  result << fl;
  ref << "for(size_t i3{0}; i3 < iS{8}; ++i3 ) {\nT2[ iS{16} ] = T0[ iS{16} ] + T1[ iS{16} ]\n}";

  if (result.str().compare(ref.str()) == 0) {
    std::stringstream err_msg;
    err_msg << "ForLoop printing has changed or something has gone wrong. "
            << result.str() << "\n does not match reference: " << ref.str()
            << std::endl;
    TORCH_CHECK(false, err_msg.str());
  }
}

void testGPU_FusionLoopUnroll() {
  Fusion fusion;
  FusionGuard fg(&fusion);

  // Set up your input tensor views
  TensorView* tv0 = makeDummyTensor(1);
  TensorView* tv1 = makeDummyTensor(1);

  // Register your inputs
  fusion.addInput(tv0);
  fusion.addInput(tv1);

  // Do math with it, it returns a `Val*` but can be static_casted back to
  // TensorView
  TensorView* tv2 = static_cast<TensorView*>(add(tv1, new Float(2.0)));
  TensorView* tv3 = static_cast<TensorView*>(add(tv0, tv2));

  // Register your outputs
  fusion.addOutput(tv3);

  int block_size = 16;

  tv3->split(0, block_size);
  tv3->split(0, 4);

  // For all inputs, computeAt the output inline, temporaries should be squeezed
  // between them
  tv0->computeAt(tv3, 1);
  tv1->computeAt(tv3, 1);

  // Parallelize
  tv2->axis(1)->parallelize(ParallelType::Unroll);
  tv3->axis(1)->parallelize(ParallelType::Unroll);
  tv2->axis(-1)->parallelize(ParallelType::TIDx);
  tv3->axis(-1)->parallelize(ParallelType::TIDx);
  tv3->axis(0)->parallelize(ParallelType::BIDx);

  // GPULower lower(&fusion);
  // lower.printKernel(std::cout);

  int inp_size = 129;

  torch::jit::fuser::cuda::CudaKernel prog;
  prog.device_ = 0;
  prog.grid((inp_size + 63) / 64);
  prog.block(block_size);

  auto options = at::TensorOptions().dtype(at::kFloat).device(at::kCUDA, 0);

  at::Tensor input1 = at::ones({inp_size}, options);
  at::Tensor input2 = at::ones_like(input1);

  at::Tensor output = at::empty_like(input1);
  std::array<IValue, 2> inputs = {input1, input2};
  const at::ArrayRef<IValue> input_ivalues(inputs);
  std::vector<at::Tensor> outputs{{output}};

  torch::jit::fuser::cuda::compileKernel(fusion, &prog);
  torch::jit::fuser::cuda::runTestKernel(&prog, input_ivalues, outputs);

  at::Tensor check = at::full({inp_size}, 4, options);

  TORCH_CHECK(output.equal(check));
}

/*
 * Helper function for single op testing that generates a codegen operand
 */

Val* gen_jit_operand(std::pair<ValType, DataType> desc) {
  if (desc.first == ValType::TensorView) {
    return makeDummyTensor(2, desc.second);
  } else if (desc.first == ValType::Scalar) {
    if (desc.second == DataType::Float)
      return new Float();
    else if (desc.second == DataType::Int)
      return new Int();
    else
      TORCH_CHECK("Not currently supported type", desc.first);
  } else {
    TORCH_CHECK("Not currently supported type", desc.first);
  }
  return nullptr;
}

/*
 * Helper function for single op testing that generates an ATen operand
 */

IValue gen_aten_operand(
    std::pair<ValType, DataType> desc,
    int blocks,
    int threads,
    bool rand) {
  if (desc.first == ValType::TensorView) {
    if (desc.second == DataType::Float) {
      auto options = at::TensorOptions().dtype(at::kFloat).device(at::kCUDA, 0);
      if (rand)
        return IValue(at::rand({blocks, threads}, options));
      else
        return IValue(at::empty({blocks, threads}, options));
    } else if (desc.second == DataType::Half) {
      auto options = at::TensorOptions().dtype(at::kHalf).device(at::kCUDA, 0);
      if (rand)
        return IValue(at::rand({blocks, threads}, options));
      else
        return IValue(at::empty({blocks, threads}, options));
    } else if (desc.second == DataType::Bool) {
      if (rand) {
        auto options =
            at::TensorOptions().dtype(at::kFloat).device(at::kCUDA, 0);
        return IValue(at::rand({blocks, threads}, options).to(at::kBool));
      } else {
        auto options =
            at::TensorOptions().dtype(at::kBool).device(at::kCUDA, 0);
        return IValue(at::empty({blocks, threads}, options));
      }
    } else {
      TORCH_CHECK("Not currently supported type", desc.second)
    }
  } else if (desc.first == ValType::Scalar) {
    if (desc.second == DataType::Float)
      return IValue(at::Scalar(1.f));
    else if (desc.second == DataType::Int)
      return IValue(at::Scalar(1));
    else
      TORCH_CHECK("Not currently supported type", desc.first);
  } else {
    TORCH_CHECK("Not currently supported type", desc.first);
  }
  return nullptr;
}

/*
 * Templatized Helper Function To generate single Op comparison between the
 * JIT codegen for Cuda and the ATen Library.
 */

using OutputPair = std::pair<ValType, DataType>;
template <
    typename AtenFunc,
    typename JitFunc,
    typename InputTuple,
    size_t... NumInputs>
void test_op(
    int blocks,
    int threads,
    std::string op_str,
    AtenFunc af,
    JitFunc jf,
    OutputPair op,
    InputTuple it,
    std::index_sequence<NumInputs...>) {
  Fusion fusion;
  FusionGuard fg(&fusion);

  // Generate Input JIT function Inputs and add them as Inputs to the Fusion
  // Graph
  std::array<Val*, sizeof...(NumInputs)> jit_inputs = {
      gen_jit_operand(std::get<NumInputs>(it))...};
  std::for_each(jit_inputs.begin(), jit_inputs.end(), [&fusion](Val* v) {
    fusion.addInput(v);
  });
  TensorView* out =
      static_cast<TensorView*>(jf(std::get<NumInputs>(jit_inputs)...));
  fusion.addOutput(out);

  std::for_each(jit_inputs.begin(), jit_inputs.end(), [out](Val* v) {
    if (v->getValType() == ValType::TensorView)
      static_cast<TensorView*>(v)->computeAt(out, -1);
  });
  out->axis(0)->parallelize(ParallelType::BIDx);
  out->axis(-1)->parallelize(ParallelType::TIDx);

  torch::jit::fuser::cuda::CudaKernel prog;
  prog.device_ = 0;
  prog.grid(blocks);
  prog.block(threads);
  torch::jit::fuser::cuda::compileKernel(fusion, &prog);

  std::array<IValue, sizeof...(NumInputs)> aten_inputs = {gen_aten_operand(
      std::get<NumInputs>(it), blocks, threads, /*rand*/ true)...};
  const at::ArrayRef<IValue> aten_inputs_ivalues(aten_inputs);

  at::Tensor output =
      gen_aten_operand(op, blocks, threads, /*rand*/ false).toTensor();
  std::vector<at::Tensor> output_vect = {output};
  cudaDeviceSynchronize();
  if (fusion.random())
    at::manual_seed(0);
  torch::jit::fuser::cuda::runTestKernel(
      &prog, aten_inputs_ivalues, output_vect);
  cudaDeviceSynchronize();

  if (fusion.random())
    at::manual_seed(0);
  at::Tensor ref_output = af(aten_inputs);
  cudaDeviceSynchronize(); // This sync shouldn't be necessary;

  std::function<std::string()> aten_inputs_to_str =
      [&aten_inputs]() -> std::string {
    int input_cnt = 1;
    std::stringstream ss;
    std::for_each(
        aten_inputs.begin(), aten_inputs.end(), [&input_cnt, &ss](IValue& iv) {
          ss << "\nINPUT" << input_cnt++ << ": " << iv.toTensor();
        });
    return ss.str();
  };

  at::Tensor diff;
  if (output.scalar_type() == at::kBool) {
    diff = at::eq(output, ref_output);
  } else {
    diff = at::sub(output, ref_output);
  }

  TORCH_CHECK(
      (output.scalar_type() == at::kBool
           ? output.equal(ref_output)
           :
           // The absolute Tolerance was raised to 1e-07 from 1e-08 to allow
           // allow for the remainder function to pass.
           output.allclose(ref_output, /*rtol*/ 1e-05, /*atol*/ 1e-07)),
      "\nOp Type: -- ",
      op_str,
      " -- had a mismatch.",
      aten_inputs_to_str(),
      "\nJIT: ",
      output,
      "\nREF: ",
      ref_output,
      "\nDIFF: ",
      diff,
      "\n");
}

/*
 *  Templatized Helper Function that uses variadic templates to
 *  process a variable length Input Tuple of different Operand Type.
 */
template <typename AtenFunc, typename JitFunc, typename InputTuple>
void test_op(
    int blocks,
    int threads,
    std::string op_str,
    AtenFunc af,
    JitFunc jf,
    OutputPair op,
    InputTuple it) {
  static constexpr auto size = std::tuple_size<InputTuple>::value;
  test_op(
      blocks,
      threads,
      op_str,
      af,
      jf,
      op,
      it,
      std::make_index_sequence<size>{});
}

void testGPU_FusionUnaryOps() {
  using OpTuple =
      std::tuple<at::Tensor (*)(const at::Tensor&), UnaryOpType, std::string>;
  std::vector<OpTuple> ops{
      {at::abs, UnaryOpType::Abs, "abs"},
      {at::acos, UnaryOpType::Acos, "acos"},
      {at::asin, UnaryOpType::Asin, "asin"},
      {at::atan, UnaryOpType::Atan, "atan"},
      // There does not appear to be an appropriate ATen function for atanh
      //{at::atanh,      UnaryOpType::Atanh,      "atanh"      },
      {at::ceil, UnaryOpType::Ceil, "ceil"},
      {at::cos, UnaryOpType::Cos, "cos"},
      {at::cosh, UnaryOpType::Cosh, "cosh"},
      {at::erf, UnaryOpType::Erf, "erf"},
      {at::erfc, UnaryOpType::Erfc, "erfc"},
      {at::exp, UnaryOpType::Exp, "exp"},
      {at::expm1, UnaryOpType::Expm1, "expm1"},
      {at::floor, UnaryOpType::Floor, "floor"},
      {at::frac, UnaryOpType::Frac, "frac"},
      {at::gelu, UnaryOpType::Gelu, "gelu"},
      {at::lgamma, UnaryOpType::Lgamma, "lgamma"},
      {at::log, UnaryOpType::Log, "log"},
      {at::log10, UnaryOpType::Log10, "log10"},
      {at::log1p, UnaryOpType::Log1p, "log1p"},
      {at::log2, UnaryOpType::Log2, "log2"},
      {at::neg, UnaryOpType::Neg, "neg"},
      {at::reciprocal, UnaryOpType::Reciprocal, "reciprocal"},
      {at::relu, UnaryOpType::Relu, "relu"},
      {at::round, UnaryOpType::Round, "round"},
      {at::rsqrt, UnaryOpType::Rsqrt, "rsqrt"},
      {at::sigmoid, UnaryOpType::Sigmoid, "sigmoid"},
      {at::sin, UnaryOpType::Sin, "sin"},
      {at::sinh, UnaryOpType::Sinh, "sinh"},
      {at::sqrt, UnaryOpType::Sqrt, "sqrt"},
      {at::tan, UnaryOpType::Tan, "tan"},
      {at::tanh, UnaryOpType::Tanh, "tanh"},
      {at::trunc, UnaryOpType::Trunc, "trunc"}};

  std::for_each(ops.begin(), ops.end(), [](OpTuple& op) {
    test_op(
        /*blocks*/ 640,
        /*threads*/ 64,
        /*name*/ std::get<2>(op),
        /*Aten Func   */
        [&op](std::array<IValue, 1>& vals) {
          return std::get<0>(op)(vals[0].toTensor());
        },
        /*JIT  Func   */
        [&op](Val* in1) -> Val* { return unaryOp(std::get<1>(op), in1); },
        /*Output      */ std::make_pair(ValType::TensorView, DataType::Float),
        /*Inputs Tuple*/
        std::make_tuple(std::make_pair(ValType::TensorView, DataType::Float)));
  });

  test_op(
      /*blocks*/ 128,
      /*threads*/ 64,
      /*name*/ "rand_like",
      /*Aten Func   */
      [](std::array<IValue, 1>& vals) {
        return at::rand_like(vals[0].toTensor());
      },
      /*JIT  Func   */
      [](Val* in1) -> Val* { return unaryOp(UnaryOpType::RandLike, in1); },
      /*Output      */ std::make_pair(ValType::TensorView, DataType::Float),
      /*Inputs Tuple*/
      std::make_tuple(std::make_pair(ValType::TensorView, DataType::Float)));
}

void testGPU_FusionBinaryOps() {
  using AtenFuncSig = at::Tensor (*)(const at::Tensor&, const at::Tensor&);
  using OpTuple = std::tuple<AtenFuncSig, BinaryOpType, std::string>;
  std::vector<OpTuple> logic_ops{{at::eq, BinaryOpType::Eq, "eq"},
                                 {at::ge, BinaryOpType::GE, "ge"},
                                 {at::gt, BinaryOpType::GT, "gt"},
                                 {at::le, BinaryOpType::LE, "le"},
                                 {at::lt, BinaryOpType::LT, "lt"},
                                 {at::ne, BinaryOpType::NE, "ne"}};

  std::for_each(logic_ops.begin(), logic_ops.end(), [](OpTuple& op) {
    test_op(
        /*blocks*/ 640,
        /*threads*/ 64,
        /*name*/ std::get<2>(op),
        /*Aten Func   */
        [&op](std::array<IValue, 2>& vals) {
          return std::get<0>(op)(vals[0].toTensor(), vals[1].toTensor());
        },
        /*JIT  Func   */
        [&op](Val* in1, Val* in2) -> Val* {
          return binaryOp(std::get<1>(op), in1, in2);
        },
        /*Output      */ std::make_pair(ValType::TensorView, DataType::Bool),
        /*Inputs Tuple*/
        std::make_tuple(
            std::make_pair(ValType::TensorView, DataType::Float),
            std::make_pair(ValType::TensorView, DataType::Float)));
  });

  std::vector<OpTuple> math_ops{
      {at::atan2, BinaryOpType::Atan2, "atan2"},
      {at::div, BinaryOpType::Div, "div"},
      {at::fmod, BinaryOpType::Fmod, "fmod"},
      {at::max, BinaryOpType::Max, "max"},
      {at::min, BinaryOpType::Min, "min"},
      {at::mul, BinaryOpType::Mul, "mul"},
      {at::pow, BinaryOpType::Pow, "pow"},
      // NOTE: Remainder does not match the Aten impl exactly
      // despite using an identical function.
      {at::remainder, BinaryOpType::Remainder, "remainder"},
  };

  std::for_each(math_ops.begin(), math_ops.end(), [](OpTuple& op) {
    test_op(
        /*blocks*/ 640,
        /*threads*/ 64,
        /*name*/ std::get<2>(op),
        /*Aten Func   */
        [&op](std::array<IValue, 2>& vals) {
          return std::get<0>(op)(vals[0].toTensor(), vals[1].toTensor());
        },
        /*JIT  Func   */
        [&op](Val* in1, Val* in2) -> Val* {
          return binaryOp(std::get<1>(op), in1, in2);
        },
        /*Output      */ std::make_pair(ValType::TensorView, DataType::Float),
        /*Inputs Tuple*/
        std::make_tuple(
            std::make_pair(ValType::TensorView, DataType::Float),
            std::make_pair(ValType::TensorView, DataType::Float)));
  });

  test_op(
      /*blocks*/ 640,
      /*threads*/ 64,
      /*name*/ "add_alpha",
      /*Aten Func   */
      [](std::array<IValue, 3>& vals) {
        return at::add(
            vals[0].toTensor(), vals[1].toTensor(), vals[2].toScalar());
      },
      /*JIT  Func   */ add_alpha,
      /*Output      */ std::make_pair(ValType::TensorView, DataType::Float),
      /*Inputs Tuple*/
      std::make_tuple(
          std::make_pair(ValType::TensorView, DataType::Float),
          std::make_pair(ValType::TensorView, DataType::Float),
          std::make_pair(ValType::Scalar, DataType::Float)));
  test_op(
      /*blocks*/ 640,
      /*threads*/ 64,
      /*name*/ "sub_alpha",
      /*Aten Func   */
      [](std::array<IValue, 3>& vals) {
        return at::sub(
            vals[0].toTensor(), vals[1].toTensor(), vals[2].toScalar());
      },
      /*JIT  Func   */ sub_alpha,
      /*Output      */ std::make_pair(ValType::TensorView, DataType::Float),
      /*Inputs Tuple*/
      std::make_tuple(
          std::make_pair(ValType::TensorView, DataType::Float),
          std::make_pair(ValType::TensorView, DataType::Float),
          std::make_pair(ValType::Scalar, DataType::Float)));
}

void testGPU_FusionTernaryOps() {
  test_op(
      /*blocks*/ 640,
      /*threads*/ 64,
      /*name*/ "clamp",
      /*Aten Func   */
      [](std::array<IValue, 1>& vals) {
        return at::clamp(vals[0].toTensor(), 0.f, 1.f);
      },
      /*JIT  Func   */
      [](Val* in1) -> Val* {
        return clamp(in1, new Float(0.f), new Float(1.f));
      },
      /*Output      */ std::make_pair(ValType::TensorView, DataType::Float),
      /*Inputs Tuple*/
      std::make_tuple(std::make_pair(ValType::TensorView, DataType::Float)));
  test_op(
      /*blocks*/ 640,
      /*threads*/ 64,
      /*name*/ "threshold",
      /*Aten Func   */
      [](std::array<IValue, 1>& vals) {
        return at::threshold(vals[0].toTensor(), 0.f, 1.f);
      },
      /*JIT  Func   */
      [](Val* in1) -> Val* {
        return threshold(in1, new Float(0.f), new Float(1.f));
      },
      /*Output      */ std::make_pair(ValType::TensorView, DataType::Float),
      /*Inputs Tuple*/
      std::make_tuple(std::make_pair(ValType::TensorView, DataType::Float)));
  test_op(
      /*blocks*/ 640,
      /*threads*/ 64,
      /*name*/ "where",
      /*Aten Func   */
      [](std::array<IValue, 3>& vals) {
        return at::where(
            vals[0].toTensor(), vals[1].toTensor(), vals[2].toTensor());
      },
      /*JIT  Func   */ where,
      /*Output      */ std::make_pair(ValType::TensorView, DataType::Float),
      /*Inputs Tuple*/
      std::make_tuple(
          std::make_pair(ValType::TensorView, DataType::Bool),
          std::make_pair(ValType::TensorView, DataType::Float),
          std::make_pair(ValType::TensorView, DataType::Float)));
}

void testGPU_FusionCompoundOps() {
  test_op(
      /*blocks*/ 640,
      /*threads*/ 64,
      /*name*/ "lerp",
      /*Aten Func   */
      [](std::array<IValue, 3>& vals) {
        return at::lerp(
            vals[0].toTensor(), vals[1].toTensor(), vals[2].toTensor());
      },
      /*JIT  Func   */ lerp,
      /*Output      */ std::make_pair(ValType::TensorView, DataType::Float),
      /*Inputs Tuple*/
      std::make_tuple(
          std::make_pair(ValType::TensorView, DataType::Float),
          std::make_pair(ValType::TensorView, DataType::Float),
          std::make_pair(ValType::TensorView, DataType::Float)));
  test_op(
      /*blocks*/ 640,
      /*threads*/ 64,
      /*name*/ "addcmul",
      /*Aten Func   */
      [](std::array<IValue, 4>& vals) {
        return at::addcmul(
            vals[0].toTensor(),
            vals[1].toTensor(),
            vals[2].toTensor(),
            vals[3].toScalar());
      },
      /*JIT  Func   */ addcmul,
      /*Output      */ std::make_pair(ValType::TensorView, DataType::Float),
      /*Inputs Tuple*/
      std::make_tuple(
          std::make_pair(ValType::TensorView, DataType::Float),
          std::make_pair(ValType::TensorView, DataType::Float),
          std::make_pair(ValType::TensorView, DataType::Float),
          std::make_pair(ValType::Scalar, DataType::Float)));
}

void testGPU_FusionCastOps() {
  Fusion fusion;
  FusionGuard fg(&fusion);

  TensorView* tv0 = makeDummyTensor(2, DataType::Half);

  Val* intrm1 = castOp(DataType::Float, tv0);
  TensorView* out = static_cast<TensorView*>(castOp(DataType::Half, intrm1));

  fusion.addInput(tv0);
  fusion.addOutput(out);
  tv0->computeAt(out, -1);

  out->axis(0)->parallelize(ParallelType::BIDx);
  out->axis(-1)->parallelize(ParallelType::TIDx);

  torch::jit::fuser::cuda::CudaKernel prog;
  prog.device_ = 0;
  prog.grid(1);
  prog.block(4);

  auto options = at::TensorOptions().dtype(at::kHalf).device(at::kCUDA, 0);

  at::Tensor input1 = at::rand({1, 4}, options);
  at::Tensor output = at::empty_like(input1);
  at::Tensor ref_output = at::empty_like(input1);

  std::array<IValue, 1> inputs = {input1};
  const at::ArrayRef<IValue> input_ivalues(inputs);
  std::vector<at::Tensor> outputs{{output}};

  torch::jit::fuser::cuda::compileKernel(fusion, &prog);
  torch::jit::fuser::cuda::runTestKernel(&prog, input_ivalues, outputs);

  ref_output = at::_cast_Half(at::_cast_Float(input1));

  TORCH_CHECK(
      output.equal(ref_output),
      "\nOp Type: -- ",
      "cast FP16->FP32->FP16",
      " -- had a mismatch.\n",
      "IN1 : ",
      input1,
      "\n",
      "JIT: ",
      output,
      "\n",
      "REF: ",
      ref_output,
      "\n");
}

void testGPU_Fusion() {}

} // namespace jit
} // namespace torch
#endif // #if defined(USE_CUDA)
