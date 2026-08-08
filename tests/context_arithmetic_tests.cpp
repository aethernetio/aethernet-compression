#include <cassert>
#include <cstdint>
#include <string>
#include <vector>

#include "ae_compression/ae_compression.hpp"
#include "ae_compression/experimental/context_arithmetic.hpp"
#include "ae_compression/experimental/effective_compressor.hpp"

namespace {

using ae::compression::Byte;
using ae::compression::Model;
using ae::compression::Rule;
using ae::compression::Symbol;
using ae::compression::kFirstRuleSymbol;
using ae::compression::experimental::ContextScheme;

std::vector<Byte> Bytes(std::string const& value) {
  return {value.begin(), value.end()};
}

std::vector<Symbol> Symbols(std::string const& value) {
  auto result = std::vector<Symbol>{};
  result.reserve(value.size());
  for (auto ch : value) {
    result.push_back(static_cast<unsigned char>(ch));
  }
  return result;
}

void ExpectRoundTrip(Model const& model) {
  auto const expected = ae::compression::Decompress(model);
  for (auto scheme : {ContextScheme::kGlobal, ContextScheme::kRuleHeight,
                      ContextScheme::kMinimumDepth,
                      ContextScheme::kParentRule}) {
    auto frame =
        ae::compression::experimental::PackContextArithmetic(model, scheme);
    auto unpacked =
        ae::compression::experimental::UnpackContextArithmetic(frame.bytes);
    assert(ae::compression::Decompress(unpacked) == expected);
    assert(frame.stats.frame_bytes == frame.bytes.size());
    assert(frame.stats.frame_bytes ==
           frame.stats.fixed_header_bytes + frame.stats.shape_bytes +
               frame.stats.frequency_bytes +
               frame.stats.payload_size_bytes + frame.stats.payload_bytes);
  }
}

void TestEmpty() {
  auto model = Model{};
  ExpectRoundTrip(model);
}

void TestGeneratedModels() {
  auto samples = std::vector<std::vector<Byte>>{};
  samples.push_back(Bytes("abcabcab"));
  samples.push_back(Bytes("aaaaabaaaaabaaaaab"));
  samples.push_back(Bytes("hello hello hello hello"));

  auto nested = std::string{};
  for (int i = 0; i < 1000; ++i) {
    nested += "ABCDEFGHIJ";
  }
  samples.push_back(Bytes(nested));

  auto counterexample = std::string{"ABCDEFGHIJABCDEFGHIJ"};
  for (int i = 0; i < 1000; ++i) {
    counterexample += "ABCDE";
  }
  samples.push_back(Bytes(counterexample));

  for (auto const& input : samples) {
    auto baseline = ae::compression::Compress(input);
    ExpectRoundTrip(baseline);

    auto effective =
        ae::compression::experimental::CompressEffective(input);
    ExpectRoundTrip(effective);

    auto global = ae::compression::experimental::PackContextArithmetic(
        effective, ContextScheme::kGlobal);
    assert(global.stats.payload_bytes ==
           ae::compression::arithmetic::EncodeModelPayload(effective).size());
    assert(global.stats.frame_bytes ==
           ae::compression::PackArithmeticDictionary(effective).size());
  }
}

void TestSharedHierarchy() {
  auto model = Model{};
  model.rules.push_back(Rule{Symbols("AB")});
  model.rules.push_back(
      Rule{{kFirstRuleSymbol, static_cast<Symbol>('C')}});
  model.rules.push_back(
      Rule{{kFirstRuleSymbol, static_cast<Symbol>('D')}});
  model.stream = {kFirstRuleSymbol + 1, kFirstRuleSymbol + 2,
                  kFirstRuleSymbol + 1};

  ExpectRoundTrip(model);

  auto height = ae::compression::experimental::PackContextArithmetic(
      model, ContextScheme::kRuleHeight);
  auto depth = ae::compression::experimental::PackContextArithmetic(
      model, ContextScheme::kMinimumDepth);
  auto parent = ae::compression::experimental::PackContextArithmetic(
      model, ContextScheme::kParentRule);
  assert(height.stats.level_count == 2);
  assert(depth.stats.level_count == 2);
  assert(height.stats.context_count == 3);
  assert(depth.stats.context_count == 3);
  assert(parent.stats.context_count == 4);
}

}  // namespace

int main() {
  TestEmpty();
  TestGeneratedModels();
  TestSharedHierarchy();
}
