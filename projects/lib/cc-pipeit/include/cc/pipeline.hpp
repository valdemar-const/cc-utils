#pragma once

// Pipeline orchestrator — pimpl, no Boost, no plugin contract types leak here.
//
// Analogue of a graphics pipeline object: pipeline_builder "compiles + links"
// the chosen (frontend, backend) stages (dlopen, resolve, version-check) into an
// immutable pipeline, which is then only run. Construct the pipeline once, run
// it many times.

#include "cc-pipeit_export.hpp"

#include <expected>
#include <memory>
#include <string>
#include <string_view>

namespace cc::pipeit {

class pipeline_builder;

// Immutable, "compiled" pipeline: a resolved (front, back) pair, ready to run.
// Constructed only by pipeline_builder::build (private ctor + friend).
class pipeline {
 public:
  CC_PIPEIT_API pipeline(pipeline&&) noexcept;
  CC_PIPEIT_API pipeline& operator=(pipeline&&) noexcept;
  CC_PIPEIT_API ~pipeline();

  pipeline(pipeline const&) = delete;
  pipeline& operator=(pipeline const&) = delete;

  // front.compile(source) -> ir::module -> back.emit(out_path).
  CC_PIPEIT_API bool run(std::string_view source, std::string_view out_path) const;

 private:
  friend class pipeline_builder;
  struct impl;
  std::unique_ptr<impl> pimpl_;
  pipeline();
};

// Compiles a pipeline from the named frontend + backend plugins. Fluent:
//   auto p = pipeline_builder{}.front("tl").back("x86_64").build();
// build() dlopens both plugins, checks api versions and either returns a ready
// pipeline or an error string.
class pipeline_builder {
 public:
  pipeline_builder& front(std::string_view name) {
    front_ = std::string{name};
    return *this;
  }
  pipeline_builder& back(std::string_view name) {
    back_ = std::string{name};
    return *this;
  }

  [[nodiscard]] CC_PIPEIT_API std::expected<pipeline, std::string> build() const;

 private:
  std::string front_;
  std::string back_;
};

}  // namespace cc::pipeit
