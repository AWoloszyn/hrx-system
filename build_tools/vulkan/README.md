# Vulkan API clients

Vulkan API clients can build independently of the Vulkan HAL. Enable them with
`-DIREE_ENABLE_VULKAN=ON` in either configure wrapper, or pass
`--//build_tools/vulkan/config:enabled=true` to Bazel. The explicit option
defaults to off. Enabling the Vulkan HAL also makes the API available; disabling
all Vulkan clients requires both the API option and HAL driver to be off.

The existing pinned Vulkan headers are exposed through
`//third_party:vulkan_headers`. API selection does not enable a HAL driver,
shader compiler, display system or physical-device requirement. Clients that
execute Vulkan operations use the loader available in their execution
environment. The Loom profile adapter only borrows query functions from its
caller and does not link or load a Vulkan loader.

For example, the raw profile test runs on the CPU with Vulkan HAL disabled:

```bash
iree-bazel-test --config=asan \
  --//build_tools/vulkan/config:enabled=true \
  --//runtime/config/hal:drivers=task \
  --//loom/config/target:enable=spirv \
  //loom/binding/c/test/target/spirv:vulkan_test
```

`requirements/defs.bzl` owns `VULKAN_API` for compilation and
`VULKAN_DEVICE_RESOURCE` for tests needing a compatible device. Their semantic
IDs are `vulkan.api` and `vulkan.resource.device`. Component package policies
apply these requirements to their own callers. A CPU mock test needs the API
build requirement but no device run requirement. Physical-device serialization
is an explicit test policy separate from both requirements.

Tests establish the actual enabled features, queue capabilities and adapter
identity needed by their operations. A device resource label alone does not
establish external-memory compatibility with another API. A Vulkan test
importing a buffer allocated through D3D12 declares both APIs and both device
resources.
