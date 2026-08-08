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
#include <vector>

#include "ae_compression/ae_compression.hpp"
#include "ae_compression/experimental/effective_compressor.hpp"

namespace {

using Bytes = std::vector<std::uint8_t>;
using Clock = std::chrono::steady_clock;

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
  auto data = Bytes{};
  data.resize(limit);
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

struct Result {
  std::size_t rules = 0;
  std::size_t top_symbols = 0;
  std::size_t total_symbols = 0;
  std::size_t payload_bytes = 0;
  std::size_t frame_bytes = 0;
  double elapsed_ms = 0.0;
};

Result MeasureBaseline(Bytes const& input) {
  auto const begin = Clock::now();
  auto model = ae::compression::Compress(input);
  auto const end = Clock::now();
  if (ae::compression::Decompress(model) != input) {
    throw std::runtime_error{"baseline model roundtrip failed"};
  }
  auto frame = ae::compression::PackArithmeticDictionary(model);
  if (ae::compression::Decode(frame) != input) {
    throw std::runtime_error{"baseline frame roundtrip failed"};
  }
  auto stats = ae::compression::Analyze(model, input.size());
  return Result{
      model.rules.size(),
      model.stream.size(),
      stats.total_symbols,
      ae::compression::arithmetic::EncodeModelPayload(model).size(),
      frame.size(),
      std::chrono::duration<double, std::milli>(end - begin).count(),
  };
}

Result MeasureEffective(Bytes const& input, bool effective_finalize) {
  auto options =
      ae::compression::experimental::EffectiveCompressionOptions{};
  options.effective_finalize = effective_finalize;
  auto const begin = Clock::now();
  auto model =
      ae::compression::experimental::CompressEffective(input, options);
  auto const end = Clock::now();
  if (ae::compression::Decompress(model) != input) {
    throw std::runtime_error{"effective model roundtrip failed"};
  }
  auto frame = ae::compression::PackArithmeticDictionary(model);
  if (ae::compression::Decode(frame) != input) {
    throw std::runtime_error{"effective frame roundtrip failed"};
  }
  auto stats = ae::compression::Analyze(model, input.size());
  return Result{
      model.rules.size(),
      model.stream.size(),
      stats.total_symbols,
      ae::compression::arithmetic::EncodeModelPayload(model).size(),
      frame.size(),
      std::chrono::duration<double, std::milli>(end - begin).count(),
  };
}

void Print(std::string const& dataset, std::size_t input_size,
           std::string const& variant, Result const& result) {
  std::cout << dataset << ',' << input_size << ',' << variant << ','
            << result.rules << ',' << result.top_symbols << ','
            << result.total_symbols << ',' << result.payload_bytes << ','
            << result.frame_bytes << ',' << std::fixed << std::setprecision(3)
            << result.elapsed_ms << '\n';
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

  auto const output_dir = std::filesystem::path{"benchmark-data"};
  std::cout << "dataset,input_bytes,variant,rules,top_symbols,total_symbols,"
               "payload_bytes,frame_bytes,compress_ms\n";

  for (auto const& [name, data] : datasets) {
    WriteData(output_dir, name, data);
    Print(name, data.size(), "baseline", MeasureBaseline(data));
    Print(name, data.size(), "effective_structural_finalize",
          MeasureEffective(data, false));
    Print(name, data.size(), "effective_finalize",
          MeasureEffective(data, true));
  }
}
