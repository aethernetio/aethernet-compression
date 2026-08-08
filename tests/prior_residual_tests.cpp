#include <cassert>
#include <cstdint>
#include <string>
#include <vector>

#include "ae_compression/ae_compression.hpp"
#include "ae_compression/experimental/effective_compressor.hpp"
#include "ae_compression/experimental/prior_residual_arithmetic.hpp"

namespace {

using ae::compression::Byte;
using ae::compression::Model;
using ae::compression::experimental::PriorResidualMode;

std::vector<Byte> Bytes(std::string const& value) {
  return {value.begin(), value.end()};
}

void ExpectRoundTrip(Model const& model, std::vector<Byte> const& input) {
  for (auto mode : {PriorResidualMode::kPriorOnly,
                    PriorResidualMode::kExactResidual}) {
    auto frame = ae::compression::experimental::PackPriorResidual(model, mode);
    auto unpacked =
        ae::compression::experimental::UnpackPriorResidual(frame.bytes);
    assert(ae::compression::Decompress(unpacked) == input);
    assert(frame.stats.frame_bytes == frame.bytes.size());
  }
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

  auto counterexample = std::string{"ABCDEFGHIJABCDEFGHIJ"};
  for (int i = 0; i < 1000; ++i) {
    counterexample += "ABCDE";
  }
  samples.push_back(Bytes(counterexample));

  auto iot = std::string{};
  for (int i = 0; i < 64; ++i) {
    iot += "{\"device\":\"sensor-" + std::to_string(i % 8) +
           "\",\"temperature\":" + std::to_string(18 + i % 9) +
           ".25,\"humidity\":" + std::to_string(30 + i % 45) +
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

void TestExactResidualKeepsHeightPayload() {
  auto input = Bytes(
      "sensor.temperature=21.25;sensor.humidity=40;device=alpha;"
      "state=online;sensor.temperature=21.25;sensor.humidity=40;"
      "device=alpha;state=online;");
  auto model = ae::compression::Compress(input);
  auto exact = ae::compression::experimental::PackPriorResidual(
      model, PriorResidualMode::kExactResidual);
  auto height = ae::compression::experimental::PackContextArithmetic(
      model, ae::compression::experimental::ContextScheme::kRuleHeight);
  assert(exact.stats.payload_bytes == height.stats.payload_bytes);
}

}  // namespace

int main() {
  TestSamples();
  TestExactResidualKeepsHeightPayload();
}
