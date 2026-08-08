#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ae_compression/ae_compression.hpp"
#include "ae_compression/experimental/effective_compressor.hpp"
#include "ae_compression/experimental/grammar_lz_layout.hpp"

namespace {

using Bytes = std::vector<std::uint8_t>;
using Clock = std::chrono::steady_clock;
using ae::compression::Model;
using ae::compression::experimental::GrammarDictionaryLayout;
using ae::compression::experimental::GrammarLzFrame;
using ae::compression::experimental::GrammarLzOptions;
using ae::compression::experimental::OverlappingGrammarLzOptimizer;

Bytes ReadFile(std::filesystem::path const& path) {
  auto file = std::ifstream{path, std::ios::binary};
  if (!file) {
    throw std::runtime_error{"failed to open matrix input"};
  }
  return {std::istreambuf_iterator<char>{file},
          std::istreambuf_iterator<char>{}};
}

struct BuiltModel {
  Model model;
  double milliseconds = 0.0;
};

BuiltModel BuildBaseline(Bytes const& input) {
  auto const begin = Clock::now();
  auto model = ae::compression::Compress(input);
  auto const end = Clock::now();
  if (ae::compression::Decompress(model) != input) {
    throw std::runtime_error{"baseline grammar roundtrip failed"};
  }
  return BuiltModel{
      std::move(model),
      std::chrono::duration<double, std::milli>(end - begin).count(),
  };
}

BuiltModel BuildEffective(Bytes const& input) {
  auto options =
      ae::compression::experimental::EffectiveCompressionOptions{};
  options.effective_finalize = false;
  auto const begin = Clock::now();
  auto model =
      ae::compression::experimental::CompressEffective(input, options);
  auto const end = Clock::now();
  if (ae::compression::Decompress(model) != input) {
    throw std::runtime_error{"effective grammar roundtrip failed"};
  }
  return BuiltModel{
      std::move(model),
      std::chrono::duration<double, std::milli>(end - begin).count(),
  };
}

GrammarLzFrame BestGrammarLz(Model const& model, Bytes const& input,
                             GrammarLzOptions const& options) {
  auto optimizer = OverlappingGrammarLzOptimizer{model, options};
  auto containment =
      optimizer.PackBest(GrammarDictionaryLayout::kContainment);
  auto superstring =
      optimizer.PackBest(GrammarDictionaryLayout::kGreedySuperstring);
  auto best = containment.bytes.size() <= superstring.bytes.size()
                  ? std::move(containment)
                  : std::move(superstring);
  if (ae::compression::experimental::DecodeGrammarLz(best.bytes) != input) {
    throw std::runtime_error{"grammar-LZ roundtrip failed"};
  }
  return best;
}

struct ModelResult {
  std::size_t rules = 0;
  std::size_t grammar_symbols = 0;
  std::size_t aec_payload = 0;
  std::size_t aec_frame = 0;
  std::size_t aegl_payload = 0;
  std::size_t aegl_frame = 0;
  std::size_t aegl_prefix = 0;
  std::size_t aegl_selected = 0;
  double build_ms = 0.0;
  double pack_ms = 0.0;
};

ModelResult MeasureModel(BuiltModel const& built, Bytes const& input,
                         GrammarLzOptions const& options) {
  auto const begin = Clock::now();
  auto aegl = BestGrammarLz(built.model, input, options);
  auto const end = Clock::now();
  auto aec = ae::compression::PackArithmeticDictionary(built.model);
  if (ae::compression::Decode(aec) != input) {
    throw std::runtime_error{"AEC arithmetic roundtrip failed"};
  }
  auto analysis = ae::compression::Analyze(built.model, input.size());
  return ModelResult{
      built.model.rules.size(),
      analysis.total_symbols,
      ae::compression::arithmetic::EncodeModelPayload(built.model).size(),
      aec.size(),
      aegl.stats.command_bytes,
      aegl.bytes.size(),
      aegl.stats.prefix_bytes,
      aegl.stats.selected_rules,
      built.milliseconds,
      std::chrono::duration<double, std::milli>(end - begin).count(),
  };
}

void PrintHeader() {
  std::cout
      << "dataset,input_bytes,plain_aegl_payload,plain_aegl_frame,"
         "baseline_rules,baseline_grammar_symbols,baseline_aec_payload,"
         "baseline_aec_frame,baseline_aegl_payload,baseline_aegl_frame,"
         "baseline_aegl_prefix,baseline_aegl_selected,baseline_build_ms,"
         "baseline_pack_ms,effective_rules,effective_grammar_symbols,"
         "effective_aec_payload,effective_aec_frame,effective_aegl_payload,"
         "effective_aegl_frame,effective_aegl_prefix,"
         "effective_aegl_selected,effective_build_ms,effective_pack_ms\n";
}

void Print(std::string const& dataset, Bytes const& input,
           GrammarLzFrame const& plain, ModelResult const& baseline,
           ModelResult const& effective) {
  std::cout << dataset << ',' << input.size() << ','
            << plain.stats.command_bytes << ',' << plain.bytes.size() << ','
            << baseline.rules << ',' << baseline.grammar_symbols << ','
            << baseline.aec_payload << ',' << baseline.aec_frame << ','
            << baseline.aegl_payload << ',' << baseline.aegl_frame << ','
            << baseline.aegl_prefix << ',' << baseline.aegl_selected << ','
            << std::fixed << std::setprecision(3) << baseline.build_ms << ','
            << baseline.pack_ms << ',' << effective.rules << ','
            << effective.grammar_symbols << ',' << effective.aec_payload << ','
            << effective.aec_frame << ',' << effective.aegl_payload << ','
            << effective.aegl_frame << ',' << effective.aegl_prefix << ','
            << effective.aegl_selected << ',' << effective.build_ms << ','
            << effective.pack_ms << '\n';
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "usage: compression-matrix-aegl <file> [file...]\n";
    return 2;
  }

  auto options = GrammarLzOptions{};
  options.max_candidates = 48;
  options.max_chain = 96;
  options.max_match = 4096;

  PrintHeader();
  for (int index = 1; index < argc; ++index) {
    auto const path = std::filesystem::path{argv[index]};
    auto input = ReadFile(path);

    auto plain = ae::compression::experimental::PackPlainLz(input, options);
    if (ae::compression::experimental::DecodeGrammarLz(plain.bytes) != input) {
      throw std::runtime_error{"plain AEGL roundtrip failed"};
    }

    auto baseline_model = BuildBaseline(input);
    auto baseline = MeasureModel(baseline_model, input, options);
    auto effective_model = BuildEffective(input);
    auto effective = MeasureModel(effective_model, input, options);
    Print(path.stem().string(), input, plain, baseline, effective);
  }
  return 0;
}
