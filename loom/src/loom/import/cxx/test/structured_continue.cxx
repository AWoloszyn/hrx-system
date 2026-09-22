// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <hip/hip_runtime.h>

// Each arm may skip, and the final update belongs to their shared tail.
__forceinline__ unsigned continue_branches(unsigned count, unsigned choose) {
  unsigned total = 0;
  for (unsigned index = 0; index < count; ++index) {
    ++total;
    if ((index & choose) != 0u) {
      total += 2u;
      if ((index & 2u) != 0u) {
        continue;
      }
      total += 4u;
    } else {
      total += 8u;
      if ((index & 4u) != 0u) {
        continue;
      }
      total += 16u;
    }
    total += 32u + index;
  }
  return total;
}

__forceinline__ unsigned continue_for_step(unsigned count, unsigned choose) {
  unsigned index = 0;
  unsigned steps = 0;
  unsigned total = 0;
  for (; index++ < count; ++steps) {
    if ((index & choose) != 0u) {
      continue;
    }
    total += index;
  }
  return total + steps * 257u + index * 65537u;
}

__forceinline__ unsigned continue_while(unsigned count, unsigned choose) {
  unsigned index = 0;
  unsigned total = 0;
  while (index++ < count) {
    if ((index & choose) != 0u) {
      continue;
    }
    total += index;
  }
  return total + index * 257u;
}

__forceinline__ unsigned continue_do(unsigned count, unsigned choose) {
  unsigned index = 0;
  unsigned visits = 0;
  unsigned total = 0;
  do {
    ++visits;
    if ((index & choose) != 0u) {
      continue;
    }
    total += index;
  } while (++index < count);
  return total + index * 257u + visits * 65537u;
}

__forceinline__ unsigned continue_nested(unsigned count, unsigned choose) {
  unsigned total = 0;
  for (unsigned outer = 0; outer < count; ++outer) {
    for (unsigned inner = 0; inner < 5u; ++inner) {
      if (((outer + inner) & 1u) != 0u) {
        continue;
      }
      total += outer * 7u + inner;
    }
    if ((outer & choose) != 0u) {
      continue;
    }
    total += 100u;
  }
  return total;
}

__forceinline__ unsigned continue_all(unsigned count) {
  unsigned total = 0;
  for (unsigned index = 0; index < count; ++index) {
    total += index;
    if ((index & 1u) != 0u) {
      continue;
    } else {
      continue;
    }
    total += 999999u;
  }
  unsigned index = 0;
  while (index++ < count) {
    continue;
  }
  do {
    continue;
  } while (++index < count);
  return total + index * 257u;
}

__forceinline__ unsigned continue_scopes(unsigned count, unsigned choose) {
  unsigned value = 1;
  unsigned total = 0;
  for (unsigned index = 0; index < count; ++index) {
    if ((index & choose) != 0u) {
      unsigned value = index + 11u;
      if ((value & 1u) != 0u) {
        continue;
      }
      total += value;
    } else {
      unsigned value = index + 23u;
      if ((value & 2u) != 0u) {
        continue;
      }
      total += value;
    }
    total += value;
    ++value;
  }
  return total + value * 257u;
}

__forceinline__ unsigned continue_byte(unsigned count, unsigned choose) {
  unsigned char value = 250;
  for (unsigned index = 0; index < count; ++index) {
    ++value;
    if ((index & choose) != 0u) {
      continue;
    }
    value += 7u;
  }
  return value;
}

template <unsigned Unroll, unsigned Pipeline>
__forceinline__ unsigned continue_schedule(const unsigned* input,
                                           unsigned count, unsigned choose) {
  unsigned total = 0;
  [[loom::unroll(Unroll), loom::pipeline(Pipeline)]]
  for (unsigned index = 0; index < count; ++index) {
    unsigned value = input[index];
    if ((index & choose) != 0u) {
      continue;
    }
    total += value;
  }
  return total;
}

__global__ [[loom::workgroup_size(64, 1, 1), loom::workgroup_count(1, 1, 1)]]
void continue_values(unsigned* output, unsigned count, unsigned choose) {
  unsigned lane = threadIdx.x;
  unsigned length = count + (lane & 3u);
  unsigned mask = choose ^ (lane & 7u);
  output[lane * 8u] = continue_branches(length, mask);
  output[lane * 8u + 1u] = continue_for_step(length, mask);
  output[lane * 8u + 2u] = continue_while(length, mask);
  output[lane * 8u + 3u] = continue_do(length, mask);
  output[lane * 8u + 4u] = continue_nested(length, mask);
  output[lane * 8u + 5u] = continue_all(length);
  output[lane * 8u + 6u] = continue_scopes(length, mask);
  output[lane * 8u + 7u] = continue_byte(length, mask);
}

__global__ [[loom::workgroup_size(64, 1, 1), loom::workgroup_count(1, 1, 1)]]
void continue_scheduled(const unsigned* input, unsigned* output, unsigned count,
                        unsigned choose) {
  unsigned lane = threadIdx.x;
  const unsigned* row = input + lane * count;
  unsigned mask = choose ^ (lane & 7u);
  output[lane * 4u] = continue_schedule<1, 1>(row, count, mask);
  output[lane * 4u + 1u] = continue_schedule<3, 1>(row, count, mask);
  output[lane * 4u + 2u] = continue_schedule<1, 2>(row, count, mask);
  output[lane * 4u + 3u] = continue_schedule<3, 2>(row, count, mask);
}

// The renderer's sparse-copy pattern leaves every even destination untouched.
__forceinline__ void copy_odd(const unsigned* input, unsigned* output) {
  for (unsigned index = 0; index < 16u; ++index) {
    if ((index & 1u) == 0u) {
      continue;
    }
    output[index] = input[index];
  }
}

__global__ [[loom::workgroup_size(64, 1, 1), loom::workgroup_count(1, 1, 1)]]
void continue_copy(const unsigned* input, unsigned* output) {
  copy_odd(input + threadIdx.x * 16u, output + threadIdx.x * 16u);
}

__global__ [[loom::workgroup_size(64, 1, 1), loom::workgroup_count(1, 1, 1)]]
void continue_pointers(const unsigned* input, unsigned* output, unsigned length,
                       unsigned choose) {
  unsigned lane = threadIdx.x;
  const unsigned* cursor = input + lane * length;
  unsigned* result = output + lane * 34u;
  unsigned stored = 0;
  for (unsigned index = 0; index < 33u; ++index) {
    if (index >= length) {
      continue;
    }
    unsigned value = *cursor++;
    if ((value & choose) == 0u) {
      continue;
    }
    *result++ = value;
    ++stored;
  }
  output[lane * 34u + 33u] = stored;
}

using U4 = unsigned __attribute__((vector_size(16)));

__global__ [[loom::workgroup_size(1, 1, 1), loom::workgroup_count(1, 1, 1)]]
void continue_vectors(U4* output, unsigned count, unsigned choose) {
  U4 value = {1u, 2u, 3u, 4u};
  for (unsigned index = 0; index < count; ++index) {
    value += U4{index, index * 2u, index * 3u, index * 4u};
    if ((index & choose) != 0u) {
      continue;
    }
    value ^= U4{17u, 31u, 63u, 127u};
  }
  output[0u] = value;
}
