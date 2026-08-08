#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ae_compression/ae_compression.hpp"
#include "ae_compression/experimental/context_arithmetic.hpp"
#include "ae_compression/experimental/effective_compressor.hpp"
#include "ae_compression/experimental/prior_residual_arithmetic.hpp"

namespace {

using Bytes = std::vector<std::uint8_t>;
using Clock = std::chrono::steady_clock;
using ae::compression::Model;
using ae::compression::experimental::ContextScheme;
using ae::compression::experimental::PriorResidualMode;

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

struct BuiltModel {
  Model model;
  double elapsed_ms = 0.0;
};

BuiltModel BuildBaseline(Bytes const& input) {
  auto const begin = Clock::now();
  auto model = ae::compression::Compress(input);
  auto const end = Clock::now();
  if (ae::compression::Decompress(model) != input) {
    throw std::runtime_error{"baseline model roundtrip failed"};
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
    throw std::runtime_error{"effective model roundtrip failed"};
  }
  return BuiltModel{
      std::move(model),
      std::chrono::duration<double, std::milli>(end - begin).count(),
  };
}

void PrintCurrent(std::string const& dataset, Bytes const& input,
                  std::string const& grammar, BuiltModel const& built,
                  ContextScheme scheme) {
  auto const begin = Clock::now();
  auto frame =
      ae::compression::experimental::PackContextArithmetic(built.model, scheme);
  auto const end = Clock::now();
  auto unpacked =
      ae::compression::experimental::UnpackContextArithmetic(frame.bytes);
  if (ae::compression::Decompress(unpacked) != input) {
    throw std::runtime_error{"current context frame roundtrip failed"};
  }
  auto const model_stats = ae::compression::Analyze(built.model, input.size());
  auto const& stats = frame.stats;
  auto const global_entries =
      scheme == ContextScheme::kGlobal
          ? stats.non_zero_frequency_entries
          : std::size_t{0};
  std::cout
      << dataset << ',' << input.size() << ',' << grammar << ','
      << ae::compression::experimental::ContextSchemeName(scheme) << ','
      << built.model.rules.size() << ',' << stats.level_count << ','
      << stats.context_count << ',' << built.model.stream.size() << ','
      << model_stats.total_symbols << ',' << global_entries << ','
      << stats.non_zero_frequency_entries << ','
      << stats.non_zero_frequency_entries << ",0,"
      << stats.largest_context_alphabet << ',' << std::fixed
      << std::setprecision(2) << stats.ideal_payload_bits << ','
      << stats.payload_bytes << ',' << stats.shape_bytes << ','
      << stats.frequency_bytes << ",0," << stats.payload_size_bytes << ','
      << stats.frame_bytes << ',' << std::setprecision(3)
      << built.elapsed_ms << ','
      << std::chrono::duration<double, std::milli>(end - begin).count()
      << '\n';
}

void PrintPrior(std::string const& dataset, Bytes const& input,
                std::string const& grammar, BuiltModel const& built,
                PriorResidualMode mode) {
  auto const begin = Clock::now();
  auto frame =
      ae::compression::experimental::PackPriorResidual(built.model, mode);
  auto const end = Clock::now();
  auto unpacked =
      ae::compression::experimental::UnpackPriorResidual(frame.bytes);
  if (ae::compression::Decompress(unpacked) != input) {
    throw std::runtime_error{"prior residual frame roundtrip failed"};
  }
  auto const model_stats = ae::compression::Analyze(built.model, input.size());
  auto const& stats = frame.stats;
  std::cout
      << dataset << ',' << input.size() << ',' << grammar << ','
      << ae::compression::experimental::PriorResidualModeName(mode) << ','
      << built.model.rules.size() << ',' << stats.level_count << ','
      << stats.context_count << ',' << built.model.stream.size() << ','
      << model_stats.total_symbols << ',' << stats.global_frequency_entries
      << ',' << stats.actual_frequency_entries << ','
      << stats.model_frequency_entries << ',' << stats.residual_entries << ','
      << stats.largest_context_alphabet << ',' << std::fixed
      << std::setprecision(2) << stats.ideal_payload_bits << ','
      << stats.payload_bytes << ',' << stats.shape_bytes << ','
      << stats.prior_bytes << ',' << stats.residual_bytes << ','
      << stats.payload_size_bytes << ',' << stats.frame_bytes << ','
      << std::setprecision(3) << built.elapsed_ms << ','
      << std::chrono::duration<double, std::milli>(end - begin).count()
      << '\n';
}

void PrintAll(std::string const& dataset, Bytes const& input,
              std::string const& grammar, BuiltModel const& built) {
  PrintCurrent(dataset, input, grammar, built, ContextScheme::kGlobal);
  PrintCurrent(dataset, input, grammar, built, ContextScheme::kRuleHeight);
  PrintPrior(dataset, input, grammar, built, PriorResidualMode::kPriorOnly);
  PrintPrior(dataset, input, grammar, built,
             PriorResidualMode::kExactResidual);
}

}  // namespace

int main(int argc, char** argv) {
  auto datasets =
      std::vector<std::pair<std::string, Bytes>>{
          {"counterexample", MakeCounterexample()},
          {"nested_only", MakeNestedOnly()},
          {"repeating_text_4k", MakeRepeatingText(4096)},
          {"iot_json_256", MakeIotJson()},
      };
  if (argc >= 2) {
    datasets.push_back({"source_prefix_8k", ReadPrefix(argv[1], 8192)});
  }
  if (argc >= 3) {
    datasets.push_back({"binary_prefix_8k", ReadPrefix(argv[2], 8192)});
  }

  std::cout
      << "dataset,input_bytes,grammar,scheme,rules,levels,contexts,"
         "top_symbols,total_symbols,global_frequency_entries,"
         "actual_frequency_entries,model_frequency_entries,residual_entries,"
         "largest_context_alphabet,ideal_payload_bits,payload_bytes,"
         "shape_bytes,prior_bytes,residual_bytes,payload_size_bytes,"
         "frame_bytes,model_ms,encode_ms\n";

  for (auto const& [name, data] : datasets) {
    auto baseline = BuildBaseline(data);
    PrintAll(name, data, "baseline", baseline);
    auto effective = BuildEffective(data);
    PrintAll(name, data, "effective_structural_finalize", effective);
  }
}
