#include <cassert>
#include <cstdint>
#include <string>
#include <vector>

#include "ae_compression/ae_compression.hpp"
#include "ae_compression/experimental/class_prior_arithmetic.hpp"
#include "ae_compression/experimental/effective_compressor.hpp"

namespace {

using ae::compression::Byte;
using ae::compression::Model;

std::vector<Byte> Bytes(std::string const& value) {
  return {value.begin(), value.end()};
}

void ExpectRoundTrip(Model const& model, std::vector<Byte> const& input) {
  auto frame = ae::compression::experimental::PackClassPrior(model);
  auto unpacked = ae::compression::experimental::UnpackClassPrior(frame.bytes);
  assert(ae::compression::Decompress(unpacked) == input);
  assert(frame.stats.frame_bytes == frame.bytes.size());
}

void TestSamples() {
  auto samples = std::vector<std::vector<Byte>>{};
  samples.push_back({});
  samples.push_back(Bytes("xy"));
  samples.push_back(Bytes("abcabcab"));
  samples.push_back(Bytes("hello hello hello hello"));

  auto repeating = std::string{};
  for (int i = 0; i < 1000; ++i) {
    repeating += "ABCDEFGHIJ";
  }
  samples.push_back(Bytes(repeating));

  auto iot = std::string{};
  for (int i = 0; i < 128; ++i) {
    iot += "{\"device\":\"sensor-" + std::to_string(i % 16) +
           "\",\"temperature\":" + std::to_string(18 + i % 9) +
           ".25,\"humidity\":" + std::to_string(30 + i % 45) +
           ",\"battery_mv\":" + std::to_string(2900 + i % 350) +
           ",\"sequence\":" + std::to_string(100000 + i) + "}\n";
  }
  samples.push_back(Bytes(iot));

  for (auto const& input : samples) {
    auto baseline = ae::compression::Compress(input);
    ExpectRoundTrip(baseline, input);

    auto options =
        ae::compression::experimental::EffectiveCompressionOptions{};
    options.effective_finalize = false;
    auto effective =
        ae::compression::experimental::CompressEffective(input, options);
    ExpectRoundTrip(effective, input);
  }
}

}  // namespace

int main() {
  TestSamples();
}
