#include "cc/parseit.hpp"

#include <cctype>
#include <utility>

namespace cc::parseit
{

std::string_view
version() noexcept
{
    return "0.1.0";
}

// MVP grammar: a single `return <integer>;` statement.
std::expected<cc::ast::program, std::string>
parse(std::string_view src)
{
    auto pos = src.find("return");
    if (pos == std::string_view::npos)
    {
        return std::unexpected(std::string {"expected 'return'"});
    }
    pos += 6;

    std::int64_t v   = 0;
    bool         any = false;
    while (pos < src.size())
    {
        unsigned char c = static_cast<unsigned char>(src[pos]);
        if (c >= '0' && c <= '9')
        {
            v   = v * 10 + (c - '0');
            any = true;
        }
        else if (c == ';')
        {
            break;
        }
        else if (std::isspace(c))
        {
            // whitespace tolerated
        }
        else
        {
            return std::unexpected(std::string {"unexpected '"} + static_cast<char>(c) + "' in return value");
        }
        ++pos;
    }
    if (!any)
    {
        return std::unexpected(std::string {"expected integer literal after 'return'"});
    }

    auto lit   = std::make_unique<cc::ast::int_literal>();
    lit->value = v;
    auto ret   = std::make_unique<cc::ast::return_stmt>();
    ret->value = std::move(lit);
    cc::ast::program prog;
    prog.body.push_back(std::move(ret));
    return prog;
}

} // namespace cc::parseit
