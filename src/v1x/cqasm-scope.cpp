#include "v1x/cqasm-scope.hpp"


namespace cqasm::v1x::analyzer {

/**
 * Creates a scope from a table of mappings, functions, and an instruction set.
 */
Scope::Scope(
    resolver::MappingTable mappings,
    resolver::FunctionTable functions,
    resolver::InstructionTable instruction_set)
: mappings{ std::move(mappings) }
, functions{ std::move(functions) }
, instruction_set{ std::move(instruction_set) }
, block{}
, within_loop{ false }
{}

} // namespace cqasm::v1x::analyzer
