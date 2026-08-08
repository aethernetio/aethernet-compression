#include <cassert>
#include <cstdint>
#include <string>
#include <vector>

#include "ae_compression/ae_compression.hpp"
#include "ae_compression/experimental/effective_compressor.hpp"

namespace {

using ae::compression::Byte;
using ae::compression::Model;
using ae::compression::Rule;
using ae::compression::Symbol;
using ae::compression::kFirstRuleSymbol;

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

void TestEffectiveCount() {
  auto model = Model{};
  model.rules.push_back(Rule{Symbols("ABCDEFGHIJ")});
  model.stream.push_back(kFirstRuleSymbol);
  model.stream.push_back(kFirstRuleSymbol);
  auto short_pattern = Symbols("ABCDE");
  for (int i = 0; i < 1000; ++i) {
    model.stream.insert(model.stream.end(), short_pattern.begin(),
                        short_pattern.end());
  }

  auto multiplicities =
      ae::compression::experimental::RuleMultiplicities(model);
  assert(multiplicities.size() == 1);
  assert(multiplicities[0] == 2);
  assert(ae::compression::experimental::CountStructuralOccurrences(
             model, short_pattern) == 1001);
  assert(ae::compression::experimental::CountEffectiveOccurrences(
             model, short_pattern, multiplicities) == 1002);
}

void TestNestedMultiplicity() {
  auto model = Model{};
  model.rules.push_back(Rule{Symbols("ABCDE")});
  model.rules.push_back(
      Rule{{kFirstRuleSymbol, static_cast<Symbol>('F'),
            static_cast<Symbol>('G'), static_cast<Symbol>('H'),
            static_cast<Symbol>('I'), static_cast<Symbol>('J')}});
  for (int i = 0; i < 1000; ++i) {
    model.stream.push_back(kFirstRuleSymbol + 1);
  }

  auto multiplicities =
      ae::compression::experimental::RuleMultiplicities(model);
  assert(multiplicities[1] == 1000);
  assert(multiplicities[0] == 1000);
}

void TestRoundTrips() {
  auto samples = std::vector<std::vector<Byte>>{};
  samples.push_back(Bytes("abcabcab"));
  samples.push_back(Bytes("aaaaabaaaaabaaaaab"));
  samples.push_back(Bytes("hello hello hello hello"));

  auto repeated = std::string{};
  for (int i = 0; i < 1000; ++i) {
    repeated += "ABCDEFGHIJ";
  }
  samples.push_back(Bytes(repeated));

  auto counterexample = std::string{"ABCDEFGHIJABCDEFGHIJ"};
  for (int i = 0; i < 1000; ++i) {
    counterexample += "ABCDE";
  }
  samples.push_back(Bytes(counterexample));

  for (auto const& input : samples) {
    for (bool effective_finalize : {false, true}) {
      auto options =
          ae::compression::experimental::EffectiveCompressionOptions{};
      options.effective_finalize = effective_finalize;
      auto model =
          ae::compression::experimental::CompressEffective(input, options);
      assert(ae::compression::Decompress(model) == input);
      auto frame = ae::compression::PackArithmeticDictionary(model);
      assert(ae::compression::Decode(frame) == input);
    }
  }
}

}  // namespace

int main() {
  TestEffectiveCount();
  TestNestedMultiplicity();
  TestRoundTrips();
}
