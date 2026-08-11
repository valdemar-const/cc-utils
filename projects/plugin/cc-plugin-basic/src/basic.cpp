// cc-plugin-basic — basic node plugin (no UI).
//
// Registers:
//   wire types: text (std::string), path (std::filesystem::path), int (long)
//   nodes:
//     - text.from_file: reads file content from a "path" property → outputs text
//     - view:           debug tap, accepts any value
//     - exec:           runs an executable, returns ret_code/cout/cerr
//
// exec is implemented via boost::process v2 + boost::asio so it is
// cross-platform (Linux/macOS/Windows). std::filesystem::path is used for
// every filesystem location — no platform-specific path encoding.

#include "cc/any_value.hpp"
#include "cc/host.hpp"
#include "cc/node.hpp"
#include "cc/node_factory.hpp"
#include "cc/plugin_entry.hpp"

#include <boost/asio/buffer.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/readable_pipe.hpp>
#include <boost/process/v2/process.hpp>
#include <boost/process/v2/stdio.hpp>
#include <boost/system/error_code.hpp>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>       // std::remove
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef _WIN32
#  include <process.h>  // _getpid
#  define CC_GET_PID()  _getpid()
#else
#  include <unistd.h>   // getpid
#  include <sys/wait.h>
#  define CC_GET_PID()  ::getpid()
#endif

namespace cc::basic {

namespace {

// Generate a fresh unique instance id for a node of the given type.
auto fresh_instance_id(std::string_view type_id) -> std::string {
  static std::atomic<unsigned> counter{0};
  unsigned n = counter.fetch_add(1, std::memory_order_relaxed) + 1;
  return std::string{type_id} + "#" + std::to_string(n);
}

// Minimal property bag for plugin-local nodes.
class props final : public node_properties {
 public:
  auto get(std::string_view key) const -> std::string_view override {
    auto it = m_.find(std::string{key});
    return it == m_.end() ? std::string_view{} : it->second;
  }
  auto set(std::string_view key, std::string_view value) -> void override {
    m_[std::string{key}] = std::string{value};
  }
 private:
  std::unordered_map<std::string, std::string> m_;
};

// ---------------------------------------------------------------------------
// Slots — reused across nodes where the wire type matches.
// ---------------------------------------------------------------------------
class text_out_slot final : public slot {
 public:
  auto id()   const -> std::string_view  override { return "out"; }
  auto type() const -> type_descriptor_t override { return descriptor_of<std::string>; }
  auto dir()  const -> slot_dir          override { return slot_dir::out; }
  auto card() const -> slot_card         override { return slot_card::single; }
};

class any_in_slot final : public slot {
 public:
  auto id()   const -> std::string_view  override { return "in"; }
  auto type() const -> type_descriptor_t override { return type_descriptor_t{}; }  // wildcard
  auto dir()  const -> slot_dir          override { return slot_dir::in; }
  auto card() const -> slot_card         override { return slot_card::multi; }
};

// ---------------------------------------------------------------------------
// text.from_file node — reads a file from the "path" property.
// ---------------------------------------------------------------------------
class from_file_node final : public node {
 public:
  auto type_id()     const -> std::string_view override { return "basic.text.from_file"; }
  auto instance_id() const -> std::string_view override { return id_; }

  auto slots() const -> std::span<slot const* const> override {
    static const text_out_slot s_out{};
    static constexpr slot const* arr[] = {&s_out};
    return arr;
  }

  auto properties() -> node_properties& override { return props_; }

  auto activate(std::span<const input_pair>,
                std::span<output_pair>      outputs) -> activate_result override {
    std::string raw{props_.get("path")};
    if (raw.empty()) {
      return std::unexpected(failure{"'path' not set"});
    }
    std::filesystem::path path(raw);
    log("from_file[" + id_ + "]: opening " + path.string());
    std::ifstream in(path);
    if (!in) {
      log("from_file[" + id_ + "]: cannot open");
      return std::unexpected(failure{"cannot open '" + path.string() + "'"});
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    auto content = ss.str();
    log("from_file[" + id_ + "]: read " + std::to_string(content.size()) + " bytes");

    for (auto& [slot_id, out] : outputs) {
      if (slot_id == "out") {
        *out = std::move(content);
        return {};
      }
    }
    return std::unexpected(failure{"no 'out' slot on " + id_});
  }

 private:
  std::string id_{fresh_instance_id("basic.text.from_file")};
  props       props_;
};

class from_file_factory final : public node_factory {
 public:
  auto type_id()      const -> std::string_view override { return "basic.text.from_file"; }
  auto display_name() const -> std::string_view override { return "Source File"; }
  auto category()     const -> std::string_view override { return "Basic"; }

  auto property_schema() const -> std::span<const property_desc> override {
    static constexpr property_desc schema[] = {
      {"path", "File Path", property_kind::path, ""},
    };
    return schema;
  }

  auto create() const -> std::unique_ptr<node> override {
    auto n = std::make_unique<from_file_node>();
    for (auto const& d : property_schema()) {
      n->properties().set(d.key, d.default_value);
    }
    return n;
  }
};

// ---------------------------------------------------------------------------
// text.constant node — emits a literal string entered in the property editor
// (multiline). Useful for args / inline source / hardcoded values without a
// round-trip through a file on disk.
// ---------------------------------------------------------------------------
class constant_node final : public node {
 public:
  auto type_id()     const -> std::string_view override { return "basic.text.constant"; }
  auto instance_id() const -> std::string_view override { return id_; }

  auto slots() const -> std::span<slot const* const> override {
    static const text_out_slot s_out{};
    static constexpr slot const* arr[] = {&s_out};
    return arr;
  }

  auto properties() -> node_properties& override { return props_; }

  auto activate(std::span<const input_pair>,
                std::span<output_pair> outputs) -> activate_result override {
    std::string value{props_.get("value")};
    log("text.constant[" + id_ + "]: emitting " + std::to_string(value.size()) + " bytes");
    for (auto& [slot_id, out] : outputs) {
      if (slot_id == "out") {
        *out = std::move(value);
        return {};
      }
    }
    return std::unexpected(failure{"no 'out' slot on " + id_});
  }

 private:
  std::string id_{fresh_instance_id("basic.text.constant")};
  props       props_;
};

class constant_factory final : public node_factory {
 public:
  auto type_id()      const -> std::string_view override { return "basic.text.constant"; }
  auto display_name() const -> std::string_view override { return "Text Constant"; }
  auto category()     const -> std::string_view override { return "Basic"; }

  auto property_schema() const -> std::span<const property_desc> override {
    static constexpr property_desc schema[] = {
      {"value", "Value", property_kind::multiline, ""},
    };
    return schema;
  }

  auto create() const -> std::unique_ptr<node> override {
    auto n = std::make_unique<constant_node>();
    for (auto const& d : property_schema()) {
      n->properties().set(d.key, d.default_value);
    }
    return n;
  }
};

// ---------------------------------------------------------------------------
// view node — debug tap, accepts any type.
// ---------------------------------------------------------------------------
class view_node final : public node {
 public:
  auto type_id()     const -> std::string_view override { return "basic.view"; }
  auto instance_id() const -> std::string_view override { return id_; }

  auto slots() const -> std::span<slot const* const> override {
    static const any_in_slot s_in{};
    static constexpr slot const* arr[] = {&s_in};
    return arr;
  }

  auto properties() -> node_properties& override { return props_; }

  // Sinks; the View panel pulls upstream value directly via the runner.
  auto activate(std::span<const input_pair>,
                std::span<output_pair>) -> activate_result override {
    return {};
  }

 private:
  std::string id_{fresh_instance_id("basic.view")};
  props       props_;
};

class view_factory final : public node_factory {
 public:
  auto type_id()      const -> std::string_view override { return "basic.view"; }
  auto display_name() const -> std::string_view override { return "View"; }
  auto category()     const -> std::string_view override { return "Basic"; }

  auto property_schema() const -> std::span<const property_desc> override {
    static constexpr property_desc schema[] = {
      {"name", "Name", property_kind::text, "view"},
    };
    return schema;
  }

  auto create() const -> std::unique_ptr<node> override {
    auto n = std::make_unique<view_node>();
    for (auto const& d : property_schema()) {
      n->properties().set(d.key, d.default_value);
    }
    return n;
  }
};

// ---------------------------------------------------------------------------
// exec node — runs an executable, exposes ret_code / cout / cerr as wires.
//
//   inputs:  exe (path), args (text, optional)
//   outputs: ret_code (long), cout (text), cerr (text)
//   property: merge_stderr (bool, default false) — analogous to `2>&1`
//
// Implementation uses popen()/pclose() with a temp file for stderr — simple,
// sync, deadlock-free, and cross-platform (POSIX popen / Windows _popen).
// We deliberately avoid boost::process v2 here because its asio pipe loop
// hangs on Linux when the child is a small static binary that exits quickly
// (pipe EOF race in v2's process_stdio pipe teardown).
// ---------------------------------------------------------------------------
class path_in_exe_slot final : public slot {
 public:
  auto id()   const -> std::string_view  override { return "exe"; }
  auto type() const -> type_descriptor_t override { return descriptor_of<std::filesystem::path>; }
  auto dir()  const -> slot_dir          override { return slot_dir::in; }
  auto card() const -> slot_card         override { return slot_card::single; }
};

class text_in_args_slot final : public slot {
 public:
  auto id()   const -> std::string_view  override { return "args"; }
  auto type() const -> type_descriptor_t override { return descriptor_of<std::string>; }
  auto dir()  const -> slot_dir          override { return slot_dir::in; }
  auto card() const -> slot_card         override { return slot_card::single; }
  // Optional — exec without args is legitimate (the process just runs plainly).
  auto is_required() const -> bool       override { return false; }
};

class long_out_slot final : public slot {
 public:
  auto id()   const -> std::string_view  override { return "ret_code"; }
  auto type() const -> type_descriptor_t override { return descriptor_of<long>; }
  auto dir()  const -> slot_dir          override { return slot_dir::out; }
  auto card() const -> slot_card         override { return slot_card::single; }
};

class text_out_cout_slot final : public slot {
 public:
  auto id()   const -> std::string_view  override { return "cout"; }
  auto type() const -> type_descriptor_t override { return descriptor_of<std::string>; }
  auto dir()  const -> slot_dir          override { return slot_dir::out; }
  auto card() const -> slot_card         override { return slot_card::single; }
};

class text_out_cerr_slot final : public slot {
 public:
  auto id()   const -> std::string_view  override { return "cerr"; }
  auto type() const -> type_descriptor_t override { return descriptor_of<std::string>; }
  auto dir()  const -> slot_dir          override { return slot_dir::out; }
  auto card() const -> slot_card         override { return slot_card::single; }
};

class exec_node final : public node {
 public:
  auto type_id()     const -> std::string_view override { return "basic.exec"; }
  auto instance_id() const -> std::string_view override { return id_; }

  auto slots() const -> std::span<slot const* const> override {
    static const path_in_exe_slot   s_exe{};
    static const text_in_args_slot  s_args{};
    static const long_out_slot      s_ret{};
    static const text_out_cout_slot s_cout{};
    static const text_out_cerr_slot s_cerr{};
    static constexpr slot const* arr[] = {&s_exe, &s_args, &s_ret, &s_cout, &s_cerr};
    return arr;
  }

  auto properties() -> node_properties& override { return props_; }

  auto activate(std::span<const input_pair>  inputs,
                std::span<output_pair>       outputs) -> activate_result override {
    const std::filesystem::path* exe_p = nullptr;
    const std::string* args_s = nullptr;
    for (auto [slot_id, value] : inputs) {
      if (slot_id == "exe"  && value) exe_p  = aa::any_cast<std::filesystem::path>(value);
      if (slot_id == "args" && value) args_s = aa::any_cast<std::string>(value);
    }
    if (!exe_p || exe_p->empty()) {
      return std::unexpected(failure{"'exe' input not connected or empty"});
    }

    // Build argv directly from exe + whitespace-split args. We bypass
    // /bin/sh intentionally: no shell quoting surprises, no shell startup
    // overhead, no shell-injection surface. exe is run as-is, args is split
    // by whitespace into separate argv entries.
    std::vector<std::string> argv_stored;
    argv_stored.push_back(exe_p->string());
    if (args_s && !args_s->empty()) {
      std::istringstream iss(*args_s);
      std::string tok;
      while (iss >> tok) argv_stored.push_back(tok);
    }
    std::vector<std::string_view> argv;
    argv.reserve(argv_stored.size());
    for (auto const& a : argv_stored) argv.push_back(a);

    const bool merge = props_.get("merge_stderr") == "true"
                       || props_.get("merge_stderr") == "1";

    log("exec[" + id_ + "]: spawn " + exe_p->string() +
        " (argc=" + std::to_string(argv.size()) + ")");

    namespace proc = boost::process::v2;
    namespace asio = boost::asio;

    asio::io_context ctx;
    asio::readable_pipe out_pipe(ctx);
    asio::readable_pipe err_pipe(ctx);

    // v2 in boost 1.90: process_stdio{ in, out, err }. nullptr → /dev/null
    // (POSIX) / NUL (Windows). Each output binding creates a pipe internally;
    // the *child* end (write fd) lives inside the binding until the binding
    // is destroyed. We MUST destroy stdio right after spawn so the parent's
    // copy of the write end is closed — otherwise the readable_pipe never
    // sees EOF and read_some blocks forever.
    boost::system::error_code ec;
    proc::process child = [&] {
      proc::process_stdio s = [&] {
        if (merge) return proc::process_stdio{ nullptr, out_pipe, out_pipe };
        return     proc::process_stdio{ nullptr, out_pipe, err_pipe };
      }();
      return proc::default_process_launcher()(
          ctx, ec, *exe_p, argv_stored, s);
      // s destroyed here → child-end write fds closed in parent.
    }();
    if (ec) {
      log("exec[" + id_ + "]: spawn FAILED: " + ec.message());
      return std::unexpected(failure{"spawn failed: " + ec.message()});
    }
    log("exec[" + id_ + "]: pid=" + std::to_string(child.id()));

    // Drain each pipe in its own thread via blocking read_some. Concurrent
    // reads are essential — a single-threaded loop that reads stdout-then-
    // stderr would deadlock on a child that fills the stderr pipe while we
    // wait on stdout. Each thread loops until EOF (child closed its end).
    auto drain = [](asio::readable_pipe& pipe) {
      std::string out;
      char buf[4096];
      while (true) {
        boost::system::error_code rec;
        std::size_t n = pipe.read_some(asio::buffer(buf), rec);
        if (n > 0) out.append(buf, n);
        if (rec) break;  // eof or error
      }
      return out;
    };

    std::string out, err;
    std::thread out_t([&] { out = drain(out_pipe); });
    std::thread err_t([&] { err = merge ? std::string{} : drain(err_pipe); });
    out_t.join();
    err_t.join();

    child.wait();
    long code = child.exit_code();
    log("exec[" + id_ + "]: exit_code = " + std::to_string(code) +
        ", cout=" + std::to_string(out.size()) + "B" +
        ", cerr=" + std::to_string(err.size()) + "B");

    for (auto& [slot_id, out_v] : outputs) {
      if (slot_id == "ret_code") *out_v = code;
      else if (slot_id == "cout") *out_v = out;
      else if (slot_id == "cerr") *out_v = err;
    }
    return {};
  }

 private:
  std::string id_{fresh_instance_id("basic.exec")};
  props       props_;
};

class exec_factory final : public node_factory {
 public:
  auto type_id()      const -> std::string_view override { return "basic.exec"; }
  auto display_name() const -> std::string_view override { return "Exec"; }
  auto category()     const -> std::string_view override { return "Basic"; }

  auto property_schema() const -> std::span<const property_desc> override {
    static constexpr property_desc schema[] = {
      {"merge_stderr", "Merge stderr into stdout (2>&1)", property_kind::boolean, "false"},
    };
    return schema;
  }

  auto create() const -> std::unique_ptr<node> override {
    auto n = std::make_unique<exec_node>();
    for (auto const& d : property_schema()) {
      n->properties().set(d.key, d.default_value);
    }
    return n;
  }
};

}  // namespace

}  // namespace cc::basic

// ===========================================================================
// Plugin entry points
// ===========================================================================

extern "C" cc::plugin_info cc_plugin_load() {
  return {cc::plugin_api_version, "basic", "basic"};
}

extern "C" void cc_plugin_register(cc::host_registry& r) {
  r.types().register_value_type<std::string>("text");
  r.types().register_value_type<std::filesystem::path>("path");
  r.types().register_value_type<long>("int");
  r.register_node_factory(std::make_unique<cc::basic::from_file_factory>());
  r.register_node_factory(std::make_unique<cc::basic::constant_factory>());
  r.register_node_factory(std::make_unique<cc::basic::view_factory>());
  r.register_node_factory(std::make_unique<cc::basic::exec_factory>());
}
