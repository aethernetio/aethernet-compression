#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <random>
#include <vector>

#include "ae_compression/aegl/pack64.hpp"

namespace {
using namespace ae::compression::aegl;
using Block = std::array<std::uint8_t, kBlockBytes>;
void Check(bool ok) {
  if (!ok) {
    std::cerr << "AEGL-64 check failed\n";
    std::exit(1);
  }
}

// Separate full-AEGL interpreter, accepting both short and extended commands.
// Prefix is zero here; fixtures are headerless command streams.
bool Reference(const std::vector<std::uint8_t>& bytes, Block& result) {
  std::vector<std::uint8_t> history;
  std::size_t pos = 0;
  auto varint = [&](std::size_t& value) {
    value = 0;
    for (unsigned shift = 0; shift < 28 && pos < bytes.size(); shift += 7) {
      const auto byte = bytes[pos++];
      value |= static_cast<std::size_t>(byte & 127u) << shift;
      if (!(byte & 128u)) return true;
    }
    return false;
  };
  while (history.size() < 64 && pos < bytes.size()) {
    const auto token = bytes[pos++];
    std::size_t length;
    if (token <= 127) length = token + 1;
    else if (token <= 191) length = token - 128 + 3;
    else if (token <= 193) {
      if (!varint(length)) return false;
    } else return false;
    if (length == 0 || length > 64 - history.size()) return false;
    if (token <= 127 || token == 192) {
      if (length > bytes.size() - pos) return false;
      history.insert(history.end(), bytes.begin() + pos,
                     bytes.begin() + pos + length);
      pos += length;
    } else {
      std::size_t distance;
      if (length < 3 || !varint(distance) || ++distance > history.size())
        return false;
      while (length--) history.push_back(history[history.size() - distance]);
    }
  }
  if (pos != bytes.size() || history.size() != 64) return false;
  std::copy(history.begin(), history.end(), result.begin());
  return true;
}

// Independent forward shortest-path calculation (encoder uses backward DP).
std::uint32_t ReferenceCost(const Block& input) {
  std::array<std::uint32_t, 65> costs;
  costs.fill(10000);
  costs[0] = 0;
  for (std::uint32_t end = 1; end <= 64; ++end) {
    for (std::uint32_t start = 0; start < end; ++start) {
      const auto length = end - start;
      costs[end] = std::min(costs[end], costs[start] + length + 1);
      if (length < 3) continue;
      for (std::uint32_t source = 0; source < start; ++source) {
        if (std::equal(input.begin() + start, input.begin() + end,
                       input.begin() + source)) {
          costs[end] = std::min(costs[end], costs[start] + 2);
          break;
        }
      }
    }
  }
  return costs[64];
}

void RoundTrip(const Block& input, std::uint32_t lane, bool check_cost) {
  const auto packed = Pack64(input);
  Check(packed.size() <= 65);
  std::array<std::uint8_t, 66> guarded;
  guarded.fill(0xa5);
  Check(Decode64(packed.data(), static_cast<std::uint32_t>(packed.size()),
                 LinearOutput64{guarded.data() + 1}));
  Check(guarded.front() == 0xa5 && guarded.back() == 0xa5);
  Check(std::equal(input.begin(), input.end(), guarded.begin() + 1));
  Block reference;
  Check(Reference(packed, reference) && reference == input);
  if (check_cost) Check(packed.size() == ReferenceCost(input));

  std::array<std::uint32_t, 512> shared;
  shared.fill(0xdeadbeefu);
  WarpOutput64 output{shared.data(), lane};
  Check(Decode64(packed.data(), static_cast<std::uint32_t>(packed.size()), output));
  for (std::uint32_t i = 0; i < 64; ++i) Check(output.Read(i) == input[i]);
  for (std::uint32_t i = 0; i < 512; ++i)
    if (i % 32 != lane) Check(shared[i] == 0xdeadbeefu);

  for (std::uint32_t n = 0; n < packed.size(); ++n)
    Check(!Decode64(packed.data(), n, LinearOutput64{guarded.data() + 1}));
  auto trailing = packed;
  trailing.push_back(0);
  Check(!Decode64(trailing.data(), static_cast<std::uint32_t>(trailing.size()),
                  LinearOutput64{guarded.data() + 1}));
}
}  // namespace

int main() {
  Block input;
  input.fill(0);
  Check((Pack64(input) == std::vector<std::uint8_t>{0, 0, 0xbc, 0}));
  for (std::uint32_t i = 0; i < 64; ++i) input[i] = static_cast<std::uint8_t>(i);
  Check(Pack64(input).size() == 65);

  std::uint32_t checked = 0;
  for (std::uint32_t mask = 0; mask < 1024; ++mask) {
    for (std::uint32_t i = 0; i < 64; ++i)
      input[i] = i < 10 ? static_cast<std::uint8_t>((mask >> i) & 1u)
                       : static_cast<std::uint8_t>(i + 32);
    RoundTrip(input, mask % 32, true);
    ++checked;
  }
  std::mt19937 random(0x64ae2026u);
  for (std::uint32_t trial = 0; trial < 4096; ++trial) {
    for (std::uint32_t i = 0; i < 64; ++i) {
      switch (trial % 4) {
        case 0: input[i] = static_cast<std::uint8_t>(random()); break;
        case 1: input[i] = static_cast<std::uint8_t>(random() % 4); break;
        case 2: input[i] = static_cast<std::uint8_t>(i % (1 + trial % 17)); break;
        default: input[i] = i < 32 ? static_cast<std::uint8_t>(random())
                                  : input[i - 32]; break;
      }
    }
    RoundTrip(input, trial % 32, trial < 128);
    ++checked;
  }
  // All lanes coexist; writing one lane must not alter any other lane.
  std::array<std::uint32_t, 512> shared;
  for (std::uint32_t lane = 0; lane < 32; ++lane) {
    input.fill(static_cast<std::uint8_t>(lane));
    const auto packed = Pack64(input);
    Check(Decode64(packed.data(), static_cast<std::uint32_t>(packed.size()),
                   WarpOutput64{shared.data(), lane}));
  }
  for (std::uint32_t lane = 0; lane < 32; ++lane)
    for (std::uint32_t i = 0; i < 64; ++i)
      Check((WarpOutput64{shared.data(), lane}.Read(i) == lane));

  // Malformed inputs must be bounded. Successful decodes must agree with the
  // full-format interpreter. A decoder error is not a checksum guarantee.
  for (unsigned trial = 0; trial < 50000; ++trial) {
    std::vector<std::uint8_t> bytes(random() % 130);
    for (auto& byte : bytes) byte = static_cast<std::uint8_t>(random());
    std::array<std::uint8_t, 66> guarded;
    guarded.fill(0x5a);
    if (Decode64(bytes.data(), static_cast<std::uint32_t>(bytes.size()),
                  LinearOutput64{guarded.data() + 1})) {
      Block reference;
      Check(Reference(bytes, reference));
      Check(std::equal(reference.begin(), reference.end(), guarded.begin() + 1));
    }
    Check(guarded.front() == 0x5a && guarded.back() == 0x5a);
  }
  std::cout << "AEGL-64: " << checked
            << " roundtrips, 1152 independent optimal-cost checks, "
               "50000 malformed-input probes, shared layout: PASS\n";
}
