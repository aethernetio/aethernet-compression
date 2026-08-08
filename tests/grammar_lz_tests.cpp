#include <cassert>
#include <cstdint>
#include <string>
#include <vector>

#include "ae_compression/ae_compression.hpp"
#include "ae_compression/experimental/effective_compressor.hpp"
#include "ae_compression/experimental/grammar_lz.hpp"

namespace {

using ae::compression::Byte;
using ae::compression::Model;
using ae::compression::Rule;
using ae::compression::Symbol;
using ae::compression::kFirstRuleSymbol;
using ae::compression::experimental::GrammarLzOptimizer;
using ae::compression::experimental::GrammarLzOptions;

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

void ExpectFrame(std::vector<Byte> const& frame,
                 std::vector<Byte> const& expected) {
  auto decoded = ae::compression::experimental::DecodeGrammarLz(frame);
  assert(decoded == expected);
}

void TestManualHierarchicalDictionary() {
  auto model = Model{};
  model.rules.push_back(Rule{Symbols("abc")});
  model.rules.push_back(
      Rule{{kFirstRuleSymbol, static_cast<Symbol>('d')}});
  model.stream = {kFirstRuleSymbol, kFirstRuleSymbol + 1,
                  kFirstRuleSymbol + 1};

  auto const expected = Bytes("abcabcdabcd");
  auto options = GrammarLzOptions{};
  options.max_candidates = 8;
  auto optimizer = GrammarLzOptimizer{model, options};
  assert(optimizer.CandidateCount() == 2);

  auto all = optimizer.PackForRuleLimit(2);
  assert(all.stats.selected_rules == 2);
  assert(all.stats.prefix_bytes == 7);
  assert(all.stats.output_bytes == expected.size());
  assert(all.stats.literal_bytes <= 5);
  assert(all.stats.copy_commands >= 2);
  ExpectFrame(all.bytes, expected);

  auto plain = optimizer.PackPlain();
  ExpectFrame(plain.bytes, expected);
  auto best = optimizer.PackBest();
  ExpectFrame(best.bytes, expected);
  assert(best.bytes.size() <= plain.bytes.size());
  assert(best.bytes.size() <= all.bytes.size());
}

void TestGeneratedModels() {
  auto samples = std::vector<std::vector<Byte>>{};
  samples.push_back({});
  samples.push_back(Bytes("xy"));
  samples.push_back(Bytes("abcabcdabcd"));
  samples.push_back(Bytes("abcabcabcabcabcabc"));
  samples.push_back(Bytes("hello hello hello hello"));

  auto repeating = std::string{};
  for (int i = 0; i < 256; ++i) {
    repeating += "sensor.temperature=21.25;sensor.humidity=40;";
  }
  samples.push_back(Bytes(repeating));

  auto options = GrammarLzOptions{};
  options.max_candidates = 24;
  options.max_chain = 64;

  for (auto const& input : samples) {
    auto baseline = ae::compression::Compress(input);
    assert(ae::compression::Decompress(baseline) == input);

    auto effective_options =
        ae::compression::experimental::EffectiveCompressionOptions{};
    effective_options.effective_finalize = false;
    auto effective = ae::compression::experimental::CompressEffective(
        input, effective_options);
    assert(ae::compression::Decompress(effective) == input);

    for (auto const* model : {&baseline, &effective}) {
      auto optimizer = GrammarLzOptimizer{*model, options};
      auto plain = optimizer.PackPlain();
      auto all = optimizer.PackAllCandidates();
      auto best = optimizer.PackBest();

      ExpectFrame(plain.bytes, input);
      ExpectFrame(all.bytes, input);
      ExpectFrame(best.bytes, input);
      assert(best.bytes.size() <= plain.bytes.size());
      assert(best.bytes.size() <= all.bytes.size());
      assert(best.stats.history_bytes ==
             best.stats.prefix_bytes + best.stats.output_bytes);
      assert(best.stats.frame_bytes == best.bytes.size());
    }
  }
}

}  // namespace

int main() {
  TestManualHierarchicalDictionary();
  TestGeneratedModels();
}
