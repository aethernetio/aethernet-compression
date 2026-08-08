#define main grammar_lz_legacy_main
#include "grammar_lz_benchmark.cpp"
#undef main

#include "ae_compression/experimental/grammar_lz_layout.hpp"

namespace {

using ae::compression::Rule;
using ae::compression::Symbol;
using ae::compression::kFirstRuleSymbol;
using ae::compression::experimental::GrammarDictionaryLayout;
using ae::compression::experimental::OverlappingGrammarLzOptimizer;

std::vector<Symbol> LayoutSymbols(std::string const& value) {
  auto result = std::vector<Symbol>{};
  result.reserve(value.size());
  for (auto ch : value) {
    result.push_back(static_cast<unsigned char>(ch));
  }
  return result;
}

Model ManualLayoutModel() {
  auto model = Model{};
  model.rules.push_back(Rule{LayoutSymbols("abc")});
  model.rules.push_back(
      Rule{{kFirstRuleSymbol, static_cast<Symbol>('d')}});
  model.stream = {kFirstRuleSymbol, kFirstRuleSymbol + 1,
                  kFirstRuleSymbol + 1};
  return model;
}

struct PrefixDeflateResult {
  std::size_t selected = 0;
  std::size_t fragments = 0;
  std::size_t prefix_bytes = 0;
  std::size_t payload_bytes = 0;
  std::size_t frame_bytes = 0;
};

PrefixDeflateResult BestPrefixDeflate(
    OverlappingGrammarLzOptimizer const& optimizer, Bytes const& input,
    GrammarDictionaryLayout layout) {
  auto best = PrefixDeflateResult{};
  bool initialized = false;
  for (auto limit : optimizer.TrialRuleLimits()) {
    auto dictionary =
        optimizer.BuildPrefixLayoutForRuleLimit(limit, layout);
    auto combined = dictionary.bytes;
    combined.insert(combined.end(), input.begin(), input.end());
    auto payload = RawDeflate(combined);
    if (RawInflate(payload, combined.size()) != combined) {
      throw std::runtime_error{"layout prefix deflate roundtrip failed"};
    }
    auto const header_bytes =
        std::size_t{4} +
        ae::compression::experimental::grammar_lz_detail::UvarintSize(
            dictionary.bytes.size()) +
        ae::compression::experimental::grammar_lz_detail::UvarintSize(
            input.size());
    auto const frame_bytes = header_bytes + payload.size();
    if (!initialized || frame_bytes < best.frame_bytes ||
        (frame_bytes == best.frame_bytes &&
         dictionary.bytes.size() < best.prefix_bytes)) {
      best = PrefixDeflateResult{
          limit, dictionary.fragments, dictionary.bytes.size(),
          payload.size(), frame_bytes};
      initialized = true;
    }
  }
  return best;
}

void PrintLayoutHeader() {
  std::cout
      << "dataset,input_bytes,grammar,layout,scheme,grammar_rules,candidates,"
         "selected,fragments,prefix_bytes,history_bytes,payload_bytes,"
         "frame_bytes,model_ms\n";
}

void PrintLayoutFrame(
    std::string const& dataset, Bytes const& input,
    std::string const& grammar, GrammarDictionaryLayout layout,
    std::string const& scheme,
    OverlappingGrammarLzOptimizer const& optimizer,
    ae::compression::experimental::GrammarLzFrame const& frame,
    double model_ms) {
  auto dictionary = optimizer.BuildPrefixLayoutForRuleLimit(
      frame.stats.selected_rules, layout);
  std::cout
      << dataset << ',' << input.size() << ',' << grammar << ','
      << ae::compression::experimental::GrammarDictionaryLayoutName(layout)
      << ',' << scheme << ',' << frame.stats.grammar_rules << ','
      << frame.stats.candidate_rules << ',' << frame.stats.selected_rules
      << ',' << dictionary.fragments << ',' << frame.stats.prefix_bytes
      << ',' << frame.stats.history_bytes << ',' << frame.stats.command_bytes
      << ',' << frame.stats.frame_bytes << ',' << std::fixed
      << std::setprecision(3) << model_ms << '\n';
}

void PrintPrefixDeflate(
    std::string const& dataset, Bytes const& input,
    std::string const& grammar, GrammarDictionaryLayout layout,
    OverlappingGrammarLzOptimizer const& optimizer,
    PrefixDeflateResult const& result, double model_ms) {
  std::cout
      << dataset << ',' << input.size() << ',' << grammar << ','
      << ae::compression::experimental::GrammarDictionaryLayoutName(layout)
      << ",prefix_raw_deflate_best," << 0 << ','
      << optimizer.CandidateCount() << ',' << result.selected << ','
      << result.fragments << ',' << result.prefix_bytes << ','
      << result.prefix_bytes + input.size() << ',' << result.payload_bytes
      << ',' << result.frame_bytes << ',' << std::fixed
      << std::setprecision(3) << model_ms << '\n';
}

void BenchmarkLayoutModel(std::string const& dataset, Bytes const& input,
                          std::string const& grammar,
                          BuiltModel const& built,
                          GrammarLzOptions const& options) {
  auto optimizer = OverlappingGrammarLzOptimizer{built.model, options};
  for (auto layout : {GrammarDictionaryLayout::kContainment,
                      GrammarDictionaryLayout::kGreedySuperstring}) {
    auto all = optimizer.PackForRuleLimit(
        optimizer.CandidateCount(), layout);
    auto best = optimizer.PackBest(layout);
    if (ae::compression::experimental::DecodeGrammarLz(all.bytes) != input ||
        ae::compression::experimental::DecodeGrammarLz(best.bytes) != input) {
      throw std::runtime_error{"layout grammar-lz roundtrip failed"};
    }
    PrintLayoutFrame(dataset, input, grammar, layout,
                     "overlapping_lz_all", optimizer, all,
                     built.milliseconds);
    PrintLayoutFrame(dataset, input, grammar, layout,
                     "overlapping_lz_best", optimizer, best,
                     built.milliseconds);
    PrintPrefixDeflate(dataset, input, grammar, layout, optimizer,
                       BestPrefixDeflate(optimizer, input, layout),
                       built.milliseconds);
  }
}

void BenchmarkManualLayout(GrammarLzOptions const& options) {
  auto model = ManualLayoutModel();
  auto input = ToBytes("abcabcdabcd");
  auto optimizer = OverlappingGrammarLzOptimizer{model, options};
  for (auto layout : {GrammarDictionaryLayout::kContainment,
                      GrammarDictionaryLayout::kGreedySuperstring}) {
    auto frame = optimizer.PackForRuleLimit(2, layout);
    if (ae::compression::experimental::DecodeGrammarLz(frame.bytes) != input) {
      throw std::runtime_error{"manual layout roundtrip failed"};
    }
    PrintLayoutFrame("abc_manual", input, "manual", layout,
                     "overlapping_lz_all", optimizer, frame, 0.0);
    PrintPrefixDeflate("abc_manual", input, "manual", layout, optimizer,
                       BestPrefixDeflate(optimizer, input, layout), 0.0);
  }
}

}  // namespace

int main(int argc, char** argv) {
  auto datasets =
      std::vector<std::pair<std::string, Bytes>>{
          {"abc_example", ToBytes("abcabcdabcd")},
          {"counterexample", MakeCounterexample()},
          {"nested_only", MakeNestedOnly()},
          {"repeating_text_4k", MakeRepeatingText(4096)},
          {"fibonacci_8k", MakeFibonacci(8192)},
          {"iot_json_256", MakeIotJson()},
      };
  if (argc >= 2) {
    datasets.push_back({"source_prefix_8k", ReadPrefix(argv[1], 8192)});
  }
  if (argc >= 3) {
    datasets.push_back({"binary_prefix_8k", ReadPrefix(argv[2], 8192)});
  }

  auto options = GrammarLzOptions{};
  options.max_candidates = 32;
  options.max_chain = 64;
  options.max_match = 4096;

  PrintLayoutHeader();
  BenchmarkManualLayout(options);
  for (auto const& [name, input] : datasets) {
    auto baseline = BuildBaseline(input);
    BenchmarkLayoutModel(name, input, "baseline", baseline, options);
    auto effective = BuildEffective(input);
    BenchmarkLayoutModel(name, input, "effective", effective, options);
  }
}
