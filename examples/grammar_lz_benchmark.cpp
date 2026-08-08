#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <zlib.h>

#include "ae_compression/ae_compression.hpp"
#include "ae_compression/experimental/effective_compressor.hpp"
#include "ae_compression/experimental/grammar_lz.hpp"

namespace {

using Bytes = std::vector<std::uint8_t>;
using Clock = std::chrono::steady_clock;
using ae::compression::Model;
using ae::compression::experimental::GrammarLzFrame;
using ae::compression::experimental::GrammarLzOptimizer;
using ae::compression::experimental::GrammarLzOptions;

Bytes ToBytes(std::string const& text) {
  return {text.begin(), text.end()};
}

Bytes MakeCounterexample() {
  auto text = std::string{"ABCDEFGHIJABCDEFGHIJ"};
  for (int i = 0; i < 1000; ++i) {
    text += "ABCDE";
  }
  return ToBytes(text);
}

Bytes MakeNestedOnly() {
  auto text = std::string{};
  text.reserve(10000);
  for (int i = 0; i < 1000; ++i) {
    text += "ABCDEFGHIJ";
  }
  return ToBytes(text);
}

Bytes MakeRepeatingText(std::size_t size) {
  constexpr auto pattern =
      std::string_view{"sensor.temperature=21.25;sensor.humidity=40;"
                       "device=alpha;state=online;"};
  auto data = Bytes{};
  data.reserve(size);
  while (data.size() < size) {
    for (auto ch : pattern) {
      if (data.size() == size) {
        break;
      }
      data.push_back(static_cast<std::uint8_t>(ch));
    }
  }
  return data;
}

Bytes MakeFibonacci(std::size_t size) {
  auto previous = std::string{"a"};
  auto current = std::string{"ab"};
  while (current.size() < size) {
    auto next = current + previous;
    previous = std::move(current);
    current = std::move(next);
  }
  current.resize(size);
  return ToBytes(current);
}

Bytes MakeIotJson() {
  auto text = std::string{};
  for (int i = 0; i < 256; ++i) {
    auto const device = i % 16;
    auto const temperature_whole = 18 + (i * 7) % 9;
    auto const temperature_fraction = (i * 37) % 100;
    auto const humidity = 30 + (i * 13) % 45;
    auto const battery = 2900 + (i * 17) % 350;
    auto const sequence = 100000 + i;
    text += "{\"device\":\"sensor-" + std::to_string(device) +
            "\",\"temperature\":" +
            std::to_string(temperature_whole) + ".";
    if (temperature_fraction < 10) {
      text += '0';
    }
    text += std::to_string(temperature_fraction) +
            ",\"humidity\":" + std::to_string(humidity) +
            ",\"battery_mv\":" + std::to_string(battery) +
            ",\"sequence\":" + std::to_string(sequence) + "}\n";
  }
  return ToBytes(text);
}

Bytes ReadPrefix(std::filesystem::path const& path, std::size_t limit) {
  auto file = std::ifstream{path, std::ios::binary};
  if (!file) {
    throw std::runtime_error{"failed to open benchmark input"};
  }
  auto data = Bytes(limit);
  file.read(reinterpret_cast<char*>(data.data()),
            static_cast<std::streamsize>(data.size()));
  data.resize(static_cast<std::size_t>(file.gcount()));
  return data;
}

void WriteData(std::filesystem::path const& directory,
               std::string const& name, Bytes const& data) {
  std::filesystem::create_directories(directory);
  auto file = std::ofstream{directory / (name + ".bin"), std::ios::binary};
  file.write(reinterpret_cast<char const*>(data.data()),
             static_cast<std::streamsize>(data.size()));
}

Bytes RawDeflate(std::span<std::uint8_t const> input) {
  auto stream = z_stream{};
  if (deflateInit2(&stream, Z_BEST_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 9,
                   Z_DEFAULT_STRATEGY) != Z_OK) {
    throw std::runtime_error{"deflateInit2 failed"};
  }

  auto output = Bytes{};
  output.resize(compressBound(static_cast<uLong>(input.size())) + 64);
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
  auto output = Bytes(output_size);
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

void PrintHeader() {
  std::cout
      << "dataset,input_bytes,grammar,scheme,grammar_rules,candidates,"
         "selected,prefix_bytes,history_bytes,commands,literal_commands,"
         "copy_commands,literal_bytes,copied_bytes,payload_bytes,frame_bytes,"
         "model_ms,encode_ms\n";
}

void PrintFrame(std::string const& dataset, std::size_t input_size,
                std::string const& grammar, std::string const& scheme,
                GrammarLzFrame const& frame, double model_ms,
                double encode_ms) {
  auto const& stats = frame.stats;
  std::cout << dataset << ',' << input_size << ',' << grammar << ','
            << scheme << ',' << stats.grammar_rules << ','
            << stats.candidate_rules << ',' << stats.selected_rules << ','
            << stats.prefix_bytes << ',' << stats.history_bytes << ','
            << stats.command_count << ',' << stats.literal_commands << ','
            << stats.copy_commands << ',' << stats.literal_bytes << ','
            << stats.copied_bytes << ',' << stats.command_bytes << ','
            << stats.frame_bytes << ',' << std::fixed << std::setprecision(3)
            << model_ms << ',' << encode_ms << '\n';
}

void PrintSimple(std::string const& dataset, std::size_t input_size,
                 std::string const& grammar, std::string const& scheme,
                 std::size_t grammar_rules, std::size_t candidates,
                 std::size_t selected, std::size_t prefix_bytes,
                 std::size_t history_bytes, std::size_t payload_bytes,
                 std::size_t frame_bytes, double model_ms,
                 double encode_ms) {
  std::cout << dataset << ',' << input_size << ',' << grammar << ','
            << scheme << ',' << grammar_rules << ',' << candidates << ','
            << selected << ',' << prefix_bytes << ',' << history_bytes
            << ",0,0,0,0,0," << payload_bytes << ',' << frame_bytes << ','
            << std::fixed << std::setprecision(3) << model_ms << ','
            << encode_ms << '\n';
}

struct DeflatePrefixResult {
  std::size_t limit = 0;
  std::size_t prefix_bytes = 0;
  std::size_t payload_bytes = 0;
  std::size_t frame_bytes = 0;
  double milliseconds = 0.0;
};

DeflatePrefixResult BestDeflatePrefix(GrammarLzOptimizer const& optimizer,
                                      Bytes const& input) {
  auto best = DeflatePrefixResult{};
  bool initialized = false;
  for (auto limit : optimizer.TrialRuleLimits()) {
    auto prefix = optimizer.BuildPrefixForRuleLimit(limit);
    auto combined = prefix;
    combined.insert(combined.end(), input.begin(), input.end());

    auto const begin = Clock::now();
    auto compressed = RawDeflate(combined);
    auto const end = Clock::now();
    auto decoded = RawInflate(compressed, combined.size());
    if (decoded != combined) {
      throw std::runtime_error{"grammar-prefix deflate roundtrip failed"};
    }

    auto const header_bytes =
        std::size_t{4} +
        ae::compression::experimental::grammar_lz_detail::UvarintSize(
            prefix.size()) +
        ae::compression::experimental::grammar_lz_detail::UvarintSize(
            input.size());
    auto const frame_bytes = header_bytes + compressed.size();
    if (!initialized || frame_bytes < best.frame_bytes ||
        (frame_bytes == best.frame_bytes && prefix.size() < best.prefix_bytes)) {
      best = DeflatePrefixResult{
          limit,
          prefix.size(),
          compressed.size(),
          frame_bytes,
          std::chrono::duration<double, std::milli>(end - begin).count(),
      };
      initialized = true;
    }
  }
  return best;
}

void BenchmarkGrammar(std::string const& dataset, Bytes const& input,
                      std::string const& grammar_name,
                      BuiltModel const& built,
                      GrammarLzOptions const& options) {
  auto optimizer = GrammarLzOptimizer{built.model, options};

  auto const all_begin = Clock::now();
  auto all = optimizer.PackAllCandidates();
  auto const all_end = Clock::now();
  if (ae::compression::experimental::DecodeGrammarLz(all.bytes) != input) {
    throw std::runtime_error{"all-rule grammar-lz roundtrip failed"};
  }
  PrintFrame(dataset, input.size(), grammar_name, "grammar_lz_all", all,
             built.milliseconds,
             std::chrono::duration<double, std::milli>(all_end - all_begin)
                 .count());

  auto const best_begin = Clock::now();
  auto best = optimizer.PackBest();
  auto const best_end = Clock::now();
  if (ae::compression::experimental::DecodeGrammarLz(best.bytes) != input) {
    throw std::runtime_error{"best grammar-lz roundtrip failed"};
  }
  PrintFrame(dataset, input.size(), grammar_name, "grammar_lz_best", best,
             built.milliseconds,
             std::chrono::duration<double, std::milli>(best_end - best_begin)
                 .count());

  auto deflate = BestDeflatePrefix(optimizer, input);
  PrintSimple(dataset, input.size(), grammar_name,
              "grammar_prefix_raw_deflate_best", built.model.rules.size(),
              optimizer.CandidateCount(), deflate.limit,
              deflate.prefix_bytes, deflate.prefix_bytes + input.size(),
              deflate.payload_bytes, deflate.frame_bytes, built.milliseconds,
              deflate.milliseconds);

  auto arithmetic_payload =
      ae::compression::arithmetic::EncodeModelPayload(built.model);
  auto arithmetic_frame =
      ae::compression::PackArithmeticDictionary(built.model);
  PrintSimple(dataset, input.size(), grammar_name, "aec_arithmetic",
              built.model.rules.size(), 0, 0, 0, input.size(),
              arithmetic_payload.size(), arithmetic_frame.size(),
              built.milliseconds, 0.0);
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

  PrintHeader();
  auto const output_dir = std::filesystem::path{"benchmark-data"};
  for (auto const& [name, input] : datasets) {
    WriteData(output_dir, name, input);

    auto const plain_begin = Clock::now();
    auto plain = ae::compression::experimental::PackPlainLz(input, options);
    auto const plain_end = Clock::now();
    if (ae::compression::experimental::DecodeGrammarLz(plain.bytes) != input) {
      throw std::runtime_error{"plain lz roundtrip failed"};
    }
    PrintFrame(name, input.size(), "none", "plain_lz", plain, 0.0,
               std::chrono::duration<double, std::milli>(plain_end - plain_begin)
                   .count());

    auto const deflate_begin = Clock::now();
    auto deflate = RawDeflate(input);
    auto const deflate_end = Clock::now();
    if (RawInflate(deflate, input.size()) != input) {
      throw std::runtime_error{"raw deflate roundtrip failed"};
    }
    PrintSimple(
        name, input.size(), "none", "raw_deflate_9", 0, 0, 0, 0,
        input.size(), deflate.size(), deflate.size(), 0.0,
        std::chrono::duration<double, std::milli>(deflate_end - deflate_begin)
            .count());

    auto baseline = BuildBaseline(input);
    BenchmarkGrammar(name, input, "baseline", baseline, options);
    auto effective = BuildEffective(input);
    BenchmarkGrammar(name, input, "effective", effective, options);
  }
}
