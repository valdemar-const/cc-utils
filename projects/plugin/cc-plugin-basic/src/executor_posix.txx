// executor_posix.txx — linux_tag specialization (boost.process v2).
//
// This file is NOT a standalone translation unit. It is included textually by
// executor.cpp under the POSIX branch and expects executor.hpp to have been
// included immediately above it.
//
// boost.process v2's process_stdio path works on Linux/macOS; it only
// misbehaves under MinGW/Windows (see executor_win32.txx), so the v2 launcher
// is the natural choice here.

#include <boost/asio/connect_pipe.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/readable_pipe.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/writable_pipe.hpp>
#include <boost/process/v2/process.hpp>
#include <boost/process/v2/stdio.hpp>
#include <boost/system/error_code.hpp>

#include <chrono>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace cc::basic
{
namespace
{

std::vector<std::string> split_args(std::string_view args)
{
    std::vector<std::string> v;
    std::istringstream       iss{std::string(args)};
    std::string              tok;
    while (iss >> tok)
        v.push_back(std::move(tok));
    return v;
}

std::string drain(boost::asio::readable_pipe &p)
{
    std::string              out;
    boost::system::error_code ec;
    boost::asio::read(p, boost::asio::dynamic_buffer(out), ec);
    return out;
}

} // namespace

std::expected<exec_result, exec_error>
basic_executor<linux_tag>::exec(std::string_view        exe,
                                std::string_view        args,
                                std::optional<unsigned> timeout_ms)
{
    namespace proc = boost::process::v2;
    namespace asio = boost::asio;

    asio::io_context     ctx;
    asio::readable_pipe  out_rp(ctx);
    asio::writable_pipe  out_wp(ctx);
    asio::readable_pipe  err_rp(ctx);
    asio::writable_pipe  err_wp(ctx);
    asio::connect_pipe(out_rp, out_wp);
    asio::connect_pipe(err_rp, err_wp);

    std::vector<std::string> argv;
    argv.emplace_back(exe);
    for (auto &a : split_args(args))
        argv.push_back(std::move(a));

    boost::system::error_code ec;
    auto child = proc::default_process_launcher()(
            ctx, ec, std::string(exe), argv,
            proc::process_stdio {nullptr, out_wp, err_wp});
    if (ec)
        return std::unexpected(exec_error {ec.message(), false});

    {
        boost::system::error_code cec;
        out_wp.close(cec);
        err_wp.close(cec);
    }

    std::string out, err;
    std::thread out_t([&] { out = drain(out_rp); });
    std::thread err_t([&] { err = drain(err_rp); });
    out_t.join();
    err_t.join();

    if (timeout_ms)
    {
        asio::steady_timer timer(ctx, std::chrono::milliseconds(*timeout_ms));
        bool timed_out = false;

        child.async_wait([&timer](boost::system::error_code ec, int /*exit_code*/) {
            if (!ec)
                timer.cancel();
        });
        timer.async_wait([&](boost::system::error_code ec) {
            if (!ec)
            {
                timed_out = true;
                boost::system::error_code tec;
                child.terminate(tec);
            }
        });
        ctx.run();

        if (timed_out)
        {
            child.wait(ec);
            return std::unexpected(exec_error {"timeout", true});
        }
    }
    else
    {
        child.wait();
    }
    return exec_result { child.exit_code(), std::move(out), std::move(err) };
}

} // namespace cc::basic
