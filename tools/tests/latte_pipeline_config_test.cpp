#include "../../runtime/src/host/uwp/latte_pipeline_config.h"
#include <cassert>
#include <chrono>
#include <string_view>

int main() {
  const auto directory =
      std::filesystem::temp_directory_path() /
      ("latte-config-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directory(directory);
  const auto file = directory / "latte-pipeline.txt";
  auto mode = [&] {
    return std::string_view(uwp::ReadLattePipelineMode(file));
  };
  assert(mode() == "2");
  for (const auto *selected : {"0", "1", "2", "3"}) {
    {
      std::ofstream output(file);
      output << " \t" << selected << "\r\n";
    }
    assert(mode() == selected);
  }
  for (const auto *invalid : {"", "\n", "invalid", "30"}) {
    {
      std::ofstream output(file);
      output << invalid;
    }
    assert(mode() == "1");
  }
  std::filesystem::remove_all(directory);
}
