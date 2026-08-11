#include "cc/astq.hpp"

namespace cc::astq
{

namespace
{

    // Visitor that walks the tree and accumulates IR. Pure double-dispatch: no
    // dynamic_cast on nodes, so the node hierarchy needs no exported typeinfo.
    struct ir_emitter : cc::ast::visitor
    {
        cc::ir::module mod;
        std::int64_t   pending_imm = 0;

        void
        visit(const cc::ast::program &p) override
        {
            for (const auto &stmt : p.body)
            {
                stmt->accept(*this);
            }
        }

        void
        visit(const cc::ast::return_stmt &r) override
        {
            if (r.value)
            {
                r.value->accept(*this); // resolves pending_imm via visit(int_literal)
            }
            mod.code.push_back({cc::ir::opcode::ret, pending_imm});
        }

        void
        visit(const cc::ast::int_literal &l) override
        {
            pending_imm = l.value;
        }
    };

} // namespace

std::string_view
version() noexcept
{
    return "0.1.0";
}

cc::ir::module
lower(const cc::ast::program &root)
{
    ir_emitter e;
    root.accept(e);
    return e.mod;
}

} // namespace cc::astq
