#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <zlib.h>

#include "ae_compression/ae_compression.hpp"
#include "ae_compression/experimental/effective_compressor.hpp"
#include "ae_compression/experimental/grammar_lz_layout.hpp"

namespace {

using Bytes = std::vector<std::uint8_t>;
using Clock = std::chrono::steady_clock;
using ae::compression::Model;
using ae::compression::Rule;
using ae::compression::Symbol;
using ae::compression::kFirstRuleSymbol;
using ae::compression::experimental::GrammarDictionaryLayout;
using ae::compression::experimental::GrammarLzFrame;
using ae::compression::experimental::GrammarLzOptions;
using ae::compression::experimental::OverlappingGrammarLzOptimizer;

Bytes ReadFile(std::filesystem::path const& path) {
  auto file = std::ifstream{path, std::ios::binary};
  if (!file) {
    throw std::runtime_error{"failed to open layout benchmark input"};
  }
  return {std::istreambuf_iterator<char>{file},
          std::istreambuf_iterator<char>{}};
}

std::vector<Symbol> Symbols(std::string const& value) {
  auto result = std::vector<Symbol>{};
  for (auto ch : value) {
    result.push_back(static_cast<unsigned char>(ch));
  }
  return result;
}

Model ManualModel() {
  auto model = Model{};
  model.rules.push_back(Rule{Symbols("abc")});
  model.rules.push_back(
      Rule{{kFirstRuleSymbol, static_cast<Symbol>('d')}});
  model.stream = {kFirstRuleSymbol, kFirstRuleSymbol + 1,
                  kFirstRuleSymbol + 1};
  return model;
}

Bytes RawDeflate(std::span<std::uint8_t const> input) {
  auto stream = z_stream{};
  if (deflateInit2(&stream, Z_BEST_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 9,
                   Z_DEFAULT_STRATEGY) != Z_OK) {
    throw std::runtime_error{"deflateInit2 failed"};
  }
  auto output = Bytes(compressBound(static_cast<uLong>(input.size())) + 64);
  stream.next_in = const_cast<Bytef*>(
      reinterpret_cast<Bytef const*>(input.data()));
  stream.avail_in = static_cast<uInt>(input.size());
  stream.next_out = reinterpret_cast<Bytef*>(output.data());
  stream.avail_out = static_cast<uInt>(output.size());
  auto const result = deflate(&stream, Z_FINISH);
  if (result != Z_STREAM_END) {
    deflateEnd(&stream);
    throw std::runtime_error{"raw deflate failed"};
  }
  output.resize(stream.total_out);
  deflateEnd(&stream);
  return output;
}

Bytes RawInflate(std::span<std::uint8_t const> input,
                 std::size_t output_size) {
  auto stream = z_stream{};
  if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) {
    throw std::runtime_error{"inflateInit2 failed"};
  }
  auto output = Bytes(std::max<std::size_t>(output_size, 1));
  stream.next_in = const_cast<Bytef*>(
      reinterpret_cast<Bytef const*>(input.data()));
  stream.avail_in = static_cast<uInt>(input.size());
  stream.next_out = reinterpret_cast<Bytef*>(output.data());
  stream.avail_out = static_cast<uInt>(output.size());
  auto const result = inflate(&stream, Z_FINISH);
  if (result != Z_STREAM_END || stream.total_out != output_size) {
    inflateEnd(&stream);
    throw std::runtime_error{"raw inflate failed"};
  }
  output.resize(output_size);
  inflateEnd(&stream);
  return output;
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
    throw std::runtime_error{"baseline layout grammar roundtrip failed"};
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
    throw std::runtime_error{"effective layout grammar roundtrip failed"};
  }
  return BuiltModel{
      std::move(model),
      std::chrono::duration<double, std::milli>(end - begin).count(),
  };
}

void PrintHeader() {
  std::cout
      << "dataset,input_bytes,grammar,layout,scheme,grammar_rules,candidates,"
         "selected,fragments,prefix_bytes,history_bytes,payload_bytes,"
         "frame_bytes,model_ms,encode_ms\n";
}

void PrintFrame(std::string const& dataset, std::size_t input_size,
                std::string const& grammar,
                GrammarDictionaryLayout layout, std::string const& scheme,
                OverlappingGrammarLzOptimizer const& optimizer,
                GrammarLzFrame const& frame, double model_ms,
                double encode_ms) {
  auto dictionary = optimizer.BuildPrefixLayoutForRuleLimit(
      frame.stats.selected_rules, layout);
  std::cout << dataset << ',' << input_size << ',' << grammar << ','
            << ae::compression::experimental::GrammarDictionaryLayoutName(
                   layout)
            << ',' << scheme << ',' << frame.stats.grammar_rules << ','
            << frame.stats.candidate_rules << ','
            << frame.stats.selected_rules << ',' << dictionary.fragments
            << ',' << frame.stats.prefix_bytes << ','
            << frame.stats.history_bytes << ',' << frame.stats.command_bytes
            << ',' << frame.stats.frame_bytes << ',' << std::fixed
            << std::setprecision(3) << model_ms << ',' << encode_ms << '\n';
}

struct DeflatePrefixResult {
  std::size_t limit = 0;
  std::size_t fragments = 0;
  std::size_t prefix_bytes = 0;
  std::size_t payload_bytes = 0;
  std::size_t frame_bytes = 0;
  double milliseconds = 0.0;
};

DeflatePrefixResult BestDeflatePrefix(
    OverlappingGrammarLzOptimizer const& optimizer, Bytes const& input,
    GrammarDictionaryLayout layout) {
  auto best = DeflatePrefixResult{};
  bool initialized = false;
  for (auto limit : optimizer.TrialRuleLimits()) {
    auto dictionary =
        optimizer.BuildPrefixLayoutForRuleLimit(limit, layout);
    auto combined = dictionary.bytes;
    combined.insert(combined.end(), input.begin(), input.end());

    auto const begin = Clock::now();
    auto compressed = RawDeflate(combined);
    auto const end = Clock::now();
    if (RawInflate(compressed, combined.size()) != combined) {
      throw std::runtime_error{"overlapping prefix deflate roundtrip failed"};
    }
    auto const header_bytes =
        std::size_t{4} +
        ae::compression::experimental::grammar_lz_detail::UvarintSize(
            dictionary.bytes.size()) +
        ae::compression::experimental::grammar_lz_detail::UvarintSize(
            input.size());
    auto const frame_bytes = header_bytes + compressed.size();
    if (!initialized || frame_bytes < best.frame_bytes ||
        (frame_bytes == best.frame_bytes &&
         dictionary.bytes.size() < best.prefix_bytes)) {
      best = DeflatePrefixResult{
          limit,
          dictionary.fragments,
          dictionary.bytes.size(),
          compressed.size(),
          frame_bytes,
          std::chrono::duration<double, std::milli>(end - begin).count(),
      };
      initialized = true;
    }
  }
  return best;
}

void PrintDeflate(std::string const& dataset, std::size_t input_size,
                  std::string const& grammar,
                  GrammarDictionaryLayout layout,
                  OverlappingGrammarLzOptimizer const& optimizer,
                  DeflatePrefixResult const& result, double model_ms) {
  std::cout << dataset << ',' << input_size << ',' << grammar << ','
            << ae::compression::experimental::GrammarDictionaryLayoutName(
                   layout)
            << ",prefix_raw_deflate_best," << 0 << ','
            << optimizer.CandidateCount() << ',' << result.limit << ','
            << result.fragments << ',' << result.prefix_bytes << ','
            << result.prefix_bytes + input_size << ','
            << result.payload_bytes << ',' << result.frame_bytes << ','
            << std::fixed << std::setprecision(3) << model_ms << ','
            << result.milliseconds << '\n';
}

void BenchmarkModel(std::string const& dataset, Bytes const& input,
                    std::string const& grammar, BuiltModel const& built,
                    GrammarLzOptions const& options) {
  auto optimizer = OverlappingGrammarLzOptimizer{built.model, options};
  for (auto layout : {GrammarDictionaryLayout::kContainment,
                      GrammarDictionaryLayout::kGreedySuperstring}) {
    auto const all_begin = Clock::now();
    auto all = optimizer.PackForRuleLimit(
        optimizer.CandidateCount(), layout);
    auto const all_end = Clock::now();
    if (ae::compression::experimental::DecodeGrammarLz(all.bytes) != input) {
      throw std::runtime_error{"overlapping all-rule roundtrip failed"};
    }
    PrintFrame(
        dataset, input.size(), grammar, layout, "overlapping_lz_all",
        optimizer, all, built.milliseconds,
        std::chrono::duration<double, std::milli>(all_end - all_begin)
            .count());

    auto const best_begin = Clock::now();
    auto best = optimizer.PackBest(layout);
    auto const best_end = Clock::now();
    if (ae::compression::experimental::DecodeGrammarLz(best.bytes) != input) {
      throw std::runtime_error{"overlapping best-rule roundtrip failed"};
    }
    PrintFrame(
        dataset, input.size(), grammar, layout, "overlapping_lz_best",
        optimizer, best, built.milliseconds,
        std::chrono::duration<double, std::milli>(best_end - best_begin)
            .count());

    auto deflate = BestDeflatePrefix(optimizer, input, layout);
    PrintDeflate(dataset, input.size(), grammar, layout, optimizer, deflate,
                 built.milliseconds);
  }
}

void BenchmarkManual(GrammarLzOptions const& options) {
  auto model = ManualModel();
  auto input = Bytes{'a', 'b', 'c', 'a', 'b', 'c', 'd', 'a', 'b', 'c', 'd'};
  auto optimizer = OverlappingGrammarLzOptimizer{model, options};
  auto built = BuiltModel{std::move(model), 0.0};
  (void)built;
  for (auto layout : {GrammarDictionaryLayout::kContainment,
                      GrammarDictionaryLayout::kGreedySuperstring}) {
    auto frame = optimizer.PackForRuleLimit(2, layout);
    if (ae::compression::experimental::DecodeGrammarLz(frame.bytes) != input) {
      throw std::runtime_error{"manual overlapping grammar roundtrip failed"};
    }
    PrintFrame("abc_manual", input.size(), "manual", layout,
               "overlapping_lz_all", optimizer, frame, 0.0, 0.0);
    auto deflate = BestDeflatePrefix(optimizer, input, layout);
    PrintDeflate("abc_manual", input.size(), "manual", layout, optimizer,
                 deflate, 0.0);
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "usage: grammar-lz-layout-benchmark <file> [file...]\n";
    return 2;
  }

  auto options = GrammarLzOptions{};
  options.max_candidates = 32;
  options.max_chain = 64;
  options.max_match = 4096;

  PrintHeader();
  BenchmarkManual(options);
  for (int i = 1; i < argc; ++i) {
    auto const path = std::filesystem::path{argv[i]};
    auto const dataset = path.stem().string();
    auto input = ReadFile(path);
    auto baseline = BuildBaseline(input);
    BenchmarkModel(dataset, input, "baseline", baseline, options);
    auto effective = BuildEffective(input);
    BenchmarkModel(dataset, input, "effective", effective, options);
  }
}
