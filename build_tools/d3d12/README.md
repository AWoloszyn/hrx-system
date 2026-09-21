# D3D12 API clients

Enable D3D12 clients with `-DIREE_ENABLE_D3D12=ON` in either configure wrapper,
or pass `--//build_tools/d3d12/config:enabled=true` to Bazel. The option defaults
to off and requires a Windows target. Linux-to-Windows cross-compilation uses
the same option with the existing Windows toolchain and SDK.

For example, cross-build the native memory interop test:

```bash
iree-bazel-build --config=windows-x86_64 \
  --//libamdf/config:enabled=true \
  --//build_tools/d3d12/config:enabled=true \
  //libamdf/cts/interop/gpu/d3d12:memory_static_bin
```

Headers and import libraries come from the Windows SDK. Ordinary memory CTS
does not require a shader compiler. D3D12 API selection is independent of
Vulkan and HAL driver selection and applies equally to compute and rendering
clients.

`requirements/defs.bzl` owns `D3D12_API` for compilation and
`D3D12_DEVICE_RESOURCE` for tests needing a compatible device. Their semantic
IDs are `d3d12.api` and `d3d12.resource.device`. Component package policies apply
the build requirement to every compiled artifact, including a CTS corpus and
all its linkage binaries. Run requirements and physical-device serialization
are separate test policies.

Native libamdf interop additionally requires its AMD endpoint. The test checks
that both endpoints identify the same adapter and support the requested shared
storage representation. Selecting a Windows target or finding an SDK does not
establish execution support.
