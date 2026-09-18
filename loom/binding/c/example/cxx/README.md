# C++ source to GPU execution

`jit_amdgpu.c` is a C embedding that compiles the standalone HIP-style C++
translation unit in `kernels.cpp`, emits one HSACO, loads it through IREE HAL
and executes both kernels. Each kernel checks 128 values and surrounding
guards. The shared template helper calls the generated scalar FMA binding
through the ordinary HIP header facade.

```sh
iree-bazel-run --config=loom-importer-cxx \
  --//runtime/config/hal:drivers=amdgpu,task \
  --//loom/config/emit:enable=amdgpu \
  //loom/binding/c/example/cxx:jit_amdgpu
```

The first optional argument is a HAL device URI, defaulting to `amdgpu`.
The second supplies an external facade include root, disabling embedded
headers for the import:

```sh
iree-bazel-run --config=loom-importer-cxx \
  --//runtime/config/hal:drivers=amdgpu,task \
  --//loom/config/emit:enable=amdgpu \
  --//loom/config/import/cxx:embed_includes=false \
  //loom/binding/c/example/cxx:jit_amdgpu -- \
  amdgpu loom/src/loom/import/cxx/include
```

The driver prepares the context, compiler and target pipeline before accepting
source. Those objects can be reused across JIT submissions. Import creates a
normal module and destroys the C++ frontend before compilation; source buffers
can be released immediately. The API performs no cleanup pipeline during
import. The target pipeline owns subsequent optimization and lowering.

The kernels carry explicit workgroup dimensions. `affine` fixes both size and
count; `residual` bounds its count and gets exact values from a normal Loom
config module. Config validation rejects choices outside the source contract.
The compiler emits launch configuration bytecode, and the host evaluates that
artifact before each dispatch. Upload, dispatch and download are ordered by
semaphore dependencies, and every successful submission completes before its
buffers are released.

Only the example depends on a GPU runtime. The importer extension itself has
no target or runtime dependency and also accepts functions-only units for the
normal Loom compilation and linking APIs.
