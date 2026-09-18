# Standalone C++ kernels

These programs exercise the full source path: C++ import, ordinary Loom
bytecode linking, config specialization, AMDGPU compilation, and GPU execution.
The existing `iree-test-loom` runner compares outputs against independent
double-precision references and checks both output guards bitwise.

```sh
iree-bazel-test --config=asan --config=loom-importer-cxx \
  --//runtime/config/hal:drivers=amdgpu,task \
  --//loom/config/emit:enable=amdgpu \
  //loom/src/loom/import/cxx/test:kernels_test
```

The manifest imports each kernel through an external facade include root, so
the same test works with `--//loom/config/import/cxx:embed_includes=false`.
Launch count bounds become normal config declarations; the runner supplies
three workgroups through `--config` for the numerical kernels. The source
semantics kernels specify one workgroup. All use 64 threads per workgroup.
The numerical tests explicitly permit approximate mathematical functions.
They test correctness and source compatibility, not kernel performance or
compatibility with complete upstream libraries.

| Source | Numerical coverage | Provenance |
| --- | --- | --- |
| `flash_attention.cpp` | Online softmax, shared key/value tiles, shuffle reductions, partial query/key tiles, and large score differences. | Original standalone f32 attention implementation, head dimension 64 and 16-key tiles. |
| `llama_rms_norm.cpp` | Two 32-lane reductions, shared reduction storage, and columns of length 1, 33, and 129. | [llama.cpp norm.cu](https://github.com/ggml-org/llama.cpp/blob/972d2313bc0bf0a45f634f77d95c9fb03aeab12c/ggml/src/ggml-cuda/norm.cu), MIT. |
| `aiter_swiglu_f16.cpp` | FP16 storage with f32 arithmetic, clamp extremes, reciprocal/exponential calls, and columns of length 1, 31, 65, and 129. | [aiter activation_kernels.cu](https://github.com/ROCm/aiter/blob/df95f04b703bfd7c520f072fcf2560092ec9d5ac/csrc/kernels/activation_kernels.cu), MIT. |
| `control_flow.cpp` | Pre-test, post-test, and nested loops; final scalar values and effectful helper calls in conditions. Seven trip counts including zero are checked bitwise. | Original source-language semantics witness. |
| `scheduled_sum.cpp` | Template-selected unroll factors 1/3 and pipeline depths 1/2 with linear ordering. Exact integer sums for 0, 1, 2, 5, 17, and 33 columns cover startup, tails, and drain under all four schedules. | Original scheduling-contract witness. |

The llama.cpp extraction specializes `rms_norm_f32`, `block_reduce<SUM>` and
`warp_reduce_sum` for contiguous rows, one channel/sample, block size 64, and
no multiply/add fusion. Two shared values replace dynamic shared allocation.
The HIP PDL hooks are empty and omitted. The source states the reduction loop's
`offset < 32` invariant explicitly for subgroup index analysis.

The aiter extraction specializes `swiglu_act_and_mul_kernel` for scalar vector
width and equal input/output types. It keeps the original clamp/arithmetic
order, AMD reciprocal, and OCML exponential spellings. `_Float16` storage and
explicit casts retain the input/output rounding points. Both extractions use
unsigned flat element indices in place of row pointer adjustment; surrounding
framework dispatch and vector memory wrappers are omitted. Launch annotations
supply the Loom configuration contract. The upstream licenses are included
beside the source files.

`loom/py/loom/gen/test/cxx_kernel_cases.py` generates deterministic, quantized
inputs and reference fixtures using only Python's standard library. Attention
uses a materialized score matrix and double-precision softmax reference; RMSNorm
uses a direct row sum of squares. These references are independent of the
source kernels' reduction and staging algorithms. Python runs only to prepare
test fixtures; the importer, CLI, and public C extension are native.
