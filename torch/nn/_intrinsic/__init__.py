from torch._ops import ops

fq_per_tensor_affine_forward = ops.quantized.fake_quantize_per_tensor_affine_forward
fq_per_tensor_affine_backward = ops.quantized.fake_quantize_per_tensor_affine_backward
