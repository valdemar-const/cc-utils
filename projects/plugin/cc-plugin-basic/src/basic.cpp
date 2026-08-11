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
#include <boost/asio/steady_timer.hpp>
#include <boost/process/v2/process.hpp>
#include <boost/process/v2/shell.hpp>
#include <boost/process/v2/stdio.hpp>
#include <boost/system/error_code.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

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
//   inputs:  exe (path), args (text)
//   outputs: ret_code (long), cout (text), cerr (text)
//   property: merge_stderr (bool, default false) — analogous to `2>&1`
//
// Cross-platform via boost::process v2: spawn + std_out/std_err > pipes, drive
// async reads on an io_context, wait for exit, harvest exit code. On POSIX
// the default launcher forks+execs; on Windows it uses CreateProcess.
// ---------------------------------------------------------------------------
namespace proc = boost::process::v2;
namespace asio = boost::asio;

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

    // Build a shell command line: "<exe> <args>". proc::shell parses this
    // cross-platform (POSIX word-split / Windows CommandLineToArgvW) and
    // resolves the executable in PATH via proc::shell::exe().
    std::string cmd = exe_p->string();
    if (args_s && !args_s->empty()) {
      cmd += " ";
      cmd += *args_s;
    }
    log("exec[" + id_ + "]: cmd = \"" + cmd + "\"");
    proc::shell sh(cmd);
    if (sh.empty()) {
      log("exec[" + id_ + "]: shell parse empty");
      return std::unexpected(failure{"empty command line"});
    }
    auto exe_resolved = sh.exe();
    if (exe_resolved.empty()) {
      exe_resolved = *exe_p;  // fall back to literal path
    }
    log("exec[" + id_ + "]: resolved exe = " + exe_resolved.string());

    const bool merge = props_.get("merge_stderr") == "true"
                       || props_.get("merge_stderr") == "1";

    boost::system::error_code ec;
    asio::io_context ctx;
    asio::readable_pipe out_pipe(ctx);
    asio::readable_pipe err_pipe(ctx);

    // v2 in boost 1.90 uses a single process_stdio initializer with three
    // fields (in/out/err). For merge_stderr we route both out and err to
    // the same readable_pipe. stdin is bound to the null device so the child
    // never blocks waiting on the host's stdin.
    proc::process_stdio stdio;
    stdio.in  = nullptr;  // /dev/null (POSIX) or NUL (Windows)
    stdio.out = out_pipe;
    if (merge) {
      stdio.err = out_pipe;
    } else {
      stdio.err = err_pipe;
    }

    proc::process child(ctx, exe_resolved, sh.args(), stdio, ec);
    if (ec) {
      log("exec[" + id_ + "]: spawn FAILED: " + ec.message());
      return std::unexpected(failure{"spawn failed: " + ec.message()});
    }
    log("exec[" + id_ + "]: spawned pid " + std::to_string(child.id()));

    // Watchdog: if the child hasn't exited within the timeout, terminate it
    // so the host UI doesn't block forever on a process waiting for stdin or
    // running an infinite loop. Default 10 s, configurable via property.
    auto timeout_sec = std::chrono::seconds{10};
    {
      std::string raw{props_.get("timeout_sec")};
      if (!raw.empty()) {
        try { timeout_sec = std::chrono::seconds{std::stol(raw)}; }
        catch (...) { /* keep default */ }
      }
    }
    asio::steady_timer watchdog(ctx, timeout_sec);
    bool timed_out = false;
    watchdog.async_wait([&](boost::system::error_code te) {
      if (te) return;  // cancelled
      timed_out = true;
      log("exec[" + id_ + "]: TIMEOUT — terminating child");
      boost::system::error_code kill_ec;
      child.terminate(kill_ec);
    });

    // Drain both pipes asynchronously. ctx.run() returns when both hit EOF
    // (child exited, write ends closed).
    std::string out, err;
    char out_buf[4096], err_buf[4096];
    std::function<void()> read_out, read_err;

    read_out = [&]() {
      out_pipe.async_read_some(asio::buffer(out_buf),
        [&](boost::system::error_code r_ec, std::size_t n) {
          if (n > 0) out.append(out_buf, n);
          if (!r_ec) read_out();  // continue until EOF
        });
    };
    read_err = [&]() {
      err_pipe.async_read_some(asio::buffer(err_buf),
        [&](boost::system::error_code r_ec, std::size_t n) {
          if (n > 0) err.append(err_buf, n);
          if (!r_ec) read_err();
        });
    };

    log("exec[" + id_ + "]: ctx.run starting");
    read_out();
    if (!merge) read_err();
    ctx.run();  // returns when pipes EOF (child closed its outputs)
    log("exec[" + id_ + "]: ctx.run done");
    watchdog.cancel();
    boost::system::error_code wait_ec;
    child.wait(wait_ec);  // reap exit status (already terminated either way)
    long code = child.exit_code();
    log("exec[" + id_ + "]: exit_code = " + std::to_string(code) +
        ", cout=" + std::to_string(out.size()) + "B" +
        ", cerr=" + std::to_string(err.size()) + "B");

    if (timed_out) {
      return std::unexpected(failure{"exec timed out after " +
                                    std::to_string(timeout_sec.count()) +
                                    "s (child terminated)"});
    }

    for (auto& [slot_id, out] : outputs) {
      if (slot_id == "ret_code") *out = code;
      else if (slot_id == "cout") *out = out;
      else if (slot_id == "cerr") *out = err;
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
      {"timeout_sec",  "Timeout (seconds, 0 = no limit)", property_kind::integer, "10"},
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
