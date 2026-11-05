/* Copyright 2026 Aethernet Inc. Licensed under the Apache License, Version 2.0. */
#include <cuda_runtime.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <vector>

#include "ae_compression/aegl/pack64.hpp"

namespace {
using namespace ae::compression::aegl;
constexpr unsigned kThreads = 128;

// Exactly one independent 64-byte stream per CUDA thread. No cooperation is
// required to decode a stream. offsets has count+1 entries; it is not free
// compression metadata. Shared history occupies 2048 bytes per warp.
__global__ void DecodeBatch64(const std::uint8_t* packed, std::uint32_t packed_size,
                              const std::uint32_t* offsets, std::uint32_t count,
                              std::uint8_t* result, std::uint8_t* status) {
  __shared__ std::uint32_t history[kThreads / 32][16 * 32];
  const auto index = blockIdx.x * kThreads + threadIdx.x;
  if (index >= count) return;
  const auto begin = offsets[index];
  const auto end = offsets[index + 1];
  if (begin > end || end > packed_size) {
    status[index] = 0;
    return;
  }
  WarpOutput64 output{history[threadIdx.x / 32], threadIdx.x % 32};
  const bool ok = Decode64(packed + begin, end - begin, output);
  status[index] = ok ? 1 : 0;
  if (!ok) return;
  // Diagnostic writeback for verification, not a fused-compute benchmark.
  // A consuming computation can instead read this lane's shared history here.
  for (std::uint32_t i = 0; i < kBlockBytes; ++i)
    result[static_cast<std::size_t>(index) * kBlockBytes + i] = output.Read(i);
}

void CheckCuda(cudaError_t error) {
  if (error != cudaSuccess) {
    std::cerr << cudaGetErrorString(error) << '\n';
    std::exit(1);
  }
}

void Run(std::uint32_t count) {
  std::vector<std::uint8_t> packed;
  std::vector<std::uint8_t> expected;
  std::vector<std::uint32_t> offsets{0};
  for (std::uint32_t block = 0; block < count; ++block) {
    std::array<std::uint8_t, kBlockBytes> input;
    for (std::uint32_t i = 0; i < kBlockBytes; ++i)
      input[i] = static_cast<std::uint8_t>(
          block % 3 == 0 ? 0 : block % 3 == 1 ? i % 7 : i + block);
    const auto payload = Pack64(input);
    packed.insert(packed.end(), payload.begin(), payload.end());
    expected.insert(expected.end(), input.begin(), input.end());
    offsets.push_back(static_cast<std::uint32_t>(packed.size()));
  }
  std::uint8_t* device_packed;
  std::uint8_t* device_output;
  std::uint8_t* device_status;
  std::uint32_t* device_offsets;
  CheckCuda(cudaMalloc(reinterpret_cast<void**>(&device_packed), packed.size()));
  CheckCuda(cudaMalloc(reinterpret_cast<void**>(&device_output), expected.size()));
  CheckCuda(cudaMalloc(reinterpret_cast<void**>(&device_status), count));
  CheckCuda(cudaMalloc(reinterpret_cast<void**>(&device_offsets),
                        offsets.size() * sizeof(std::uint32_t)));
  CheckCuda(cudaMemcpy(device_packed, packed.data(), packed.size(), cudaMemcpyHostToDevice));
  CheckCuda(cudaMemcpy(device_offsets, offsets.data(),
                        offsets.size() * sizeof(std::uint32_t), cudaMemcpyHostToDevice));
  DecodeBatch64<<<(count + kThreads - 1) / kThreads, kThreads>>>(
      device_packed, static_cast<std::uint32_t>(packed.size()), device_offsets,
      count, device_output, device_status);
  CheckCuda(cudaGetLastError());
  CheckCuda(cudaDeviceSynchronize());
  std::vector<std::uint8_t> actual(expected.size());
  std::vector<std::uint8_t> status(count);
  CheckCuda(cudaMemcpy(actual.data(), device_output, actual.size(), cudaMemcpyDeviceToHost));
  CheckCuda(cudaMemcpy(status.data(), device_status, status.size(), cudaMemcpyDeviceToHost));
  if (actual != expected) {
    std::cerr << "GPU roundtrip mismatch\n";
    std::exit(1);
  }
  for (auto value : status) {
    if (value != 1) {
      std::cerr << "GPU decoder rejected a valid stream\n";
      std::exit(1);
    }
  }
  CheckCuda(cudaFree(device_offsets));
  CheckCuda(cudaFree(device_status));
  CheckCuda(cudaFree(device_output));
  CheckCuda(cudaFree(device_packed));
  std::cout << count << " blocks: PASS; payload=" << packed.size()
            << ", offset_table=" << offsets.size() * sizeof(std::uint32_t) << '\n';
}
}  // namespace

int main() {
  int devices = 0;
  if (cudaGetDeviceCount(&devices) != cudaSuccess || devices == 0) {
    std::cerr << "SKIP: no CUDA device available\n";
    return 77;
  }
  for (const auto count : {1u, 31u, 32u, 33u, 127u, 128u, 129u, 4096u}) Run(count);
}
