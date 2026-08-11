#include "cc/map_properties.hpp"

namespace cc::runtime {

auto map_properties::get(std::string_view key) const -> std::string_view {
  auto it = storage_.find(std::string{key});
  return it == storage_.end() ? std::string_view{} : it->second;
}

auto map_properties::set(std::string_view key, std::string_view value) -> void {
  storage_[std::string{key}] = std::string{value};
}

}  // namespace cc::runtime
