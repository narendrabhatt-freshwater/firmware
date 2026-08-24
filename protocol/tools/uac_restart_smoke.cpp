#include "cardlink/audio/sample_bulk.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

int main(int argc, char **argv)
{
  const unsigned cycles = argc > 1
      ? static_cast<unsigned>(std::strtoul(argv[1], nullptr, 10))
      : 3u;
  const unsigned run_ms = argc > 2
      ? static_cast<unsigned>(std::strtoul(argv[2], nullptr, 10))
      : 1000u;
  if (cycles == 0u || run_ms == 0u) {
    std::cerr << "cycles and run_ms must be non-zero\n";
    return EXIT_FAILURE;
  }

  cardlink::audio::SampleBulkOut bulk;
  for (unsigned cycle = 0u; cycle < cycles; ++cycle) {
    std::string err;
    if (!bulk.Start(err)) {
      std::cerr << "UAC open " << (cycle + 1u) << '/' << cycles
                << " failed: " << err << '\n';
      return EXIT_FAILURE;
    }
    std::cout << "UAC open " << (cycle + 1u) << '/' << cycles
              << ": 10ch 48k signed-int16\n";
    std::this_thread::sleep_for(std::chrono::milliseconds(run_ms));
    bulk.Stop();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  std::cout << "ok: UAC completed " << cycles << " open/stream/close cycles\n";
  return EXIT_SUCCESS;
}
