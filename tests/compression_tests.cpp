#include <algorithm>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <span>
#include <string>
#include <vector>

#include "ae_compression/ae_compression.hpp"

namespace {

std::vector<std::uint8_t> Bytes(std::string const& s) {
  return {s.begin(), s.end()};
}

void ExpectRoundTrip(std::vector<std::uint8_t> const& input) {
  auto model = ae::compression::Compress(input);
  auto decoded_model = ae::compression::Decompress(model);
  assert(decoded_model == input);

  auto dictionary_frame = ae::compression::PackDictionary(model);
  auto unpacked = ae::compression::UnpackDictionary(dictionary_frame);
  assert(ae::compression::Decompress(unpacked) == input);

  auto frame = ae::compression::Encode(input);
  auto decoded_frame = ae::compression::Decode(frame);
  assert(decoded_frame == input);

  auto flattened = ae::compression::arithmetic::FlattenModel(model);
  if (!flattened.empty()) {
    auto frequencies = ae::compression::arithmetic::BuildFrequencyModel(
        flattened, ae::compression::arithmetic::AlphabetSize(model));
    auto arithmetic_frame =
        ae::compression::arithmetic::Encode(flattened, frequencies);
    auto decoded_symbols = ae::compression::arithmetic::Decode(
        arithmetic_frame, flattened.size(), frequencies);
    assert(decoded_symbols == flattened);
  }
}

void TestBasicRoundTrips() {
  ExpectRoundTrip({});
  ExpectRoundTrip(Bytes("abcabcab"));
  ExpectRoundTrip(Bytes("aaaaabaaaaabaaaaab"));
  ExpectRoundTrip(Bytes("hello hello hello hello"));
  ExpectRoundTrip(Bytes("temperature=21.2;humidity=40;temperature=21.2;humidity=40;"));
}

void TestBinaryRoundTrip() {
  auto input = std::vector<std::uint8_t>{};
  for (int repeat = 0; repeat < 4; ++repeat) {
    for (int i = 0; i < 64; ++i) {
      input.push_back(static_cast<std::uint8_t>(i));
    }
  }
  ExpectRoundTrip(input);
}

void TestRawFallback() {
  auto input = Bytes("xy");
  auto frame = ae::compression::Encode(input);
  auto raw = ae::compression::PackRaw(input);
  assert(frame == raw);
  assert(ae::compression::Decode(frame) == input);
}

void TestStats() {
  auto input = Bytes("sensor:abc;sensor:abc;sensor:abc;sensor:abc;");
  auto model = ae::compression::Compress(input);
  auto stats = ae::compression::Analyze(model, input.size());
  assert(stats.original_size == input.size());
  assert(stats.total_symbols > 0);
  assert(stats.rule_count == model.rules.size());
}

}  // namespace

int main() {
  TestBasicRoundTrips();
  TestBinaryRoundTrip();
  TestRawFallback();
  TestStats();
  std::cout << "ae-compression tests passed\n";
}
