/** \file
 * Implementation for \ref include/v3x/cqasm-analyzer.hpp "v3x/cqasm-analyzer.hpp".
 */

#include "cqasm-error.hpp"
#include "v3x/AnalyzeTreeGenAstVisitor.hpp"
#include "v3x/cqasm-analyzer.hpp"
#include "v3x/cqasm-functions.hpp"
#include "v3x/cqasm-parse-helper.hpp"

#include <fmt/format.h>
#include <memory>  // make_unique
#include <numbers>
#include <stdexcept>  // runtime_error


namespace cqasm::v3x::analyzer {

/**
 * Creates a new semantic analyzer.
 * Creates a global scope.
 */
Analyzer::Analyzer(const primitives::Version &api_version)
: api_version{ api_version }
, scope_stack_{ Scope{} }
{
    if (api_version != "3.0") {
        throw std::invalid_argument{ "this analyzer only supports cQASM 3.0" };
    }
    global_scope().block = tree::make<semantic::Block>();
}

[[nodiscard]] Scope &Analyzer::global_scope() { return scope_stack_.back(); }
[[nodiscard]] Scope &Analyzer::current_scope() { return scope_stack_.front(); }
[[nodiscard]] tree::One<semantic::Block> Analyzer::current_block() { return current_scope().block; }
[[nodiscard]] tree::Any<semantic::Variable> &Analyzer::current_variables() { return current_scope().variables; }
[[nodiscard]] tree::Any<semantic::Function> &Analyzer::global_functions() { return global_scope().functions; }

[[nodiscard]] const Scope &Analyzer::global_scope() const { return scope_stack_.back(); }
[[nodiscard]] const Scope &Analyzer::current_scope() const { return scope_stack_.front(); }
[[nodiscard]] const tree::Any<semantic::Variable> &Analyzer::current_variables() const { return current_scope().variables; }
[[nodiscard]] const tree::Any<semantic::Function> &Analyzer::global_functions() const { return global_scope().functions; }

/**
 * Registers mappings for pi, eu (aka e, 2.718...), tau and im (imaginary unit).
 */
void Analyzer::register_default_mappings() {
    static constexpr double tau = 2 * std::numbers::pi;
    register_variable("x", tree::make<values::ConstAxis>(primitives::Axis{ 1, 0, 0 }));
    register_variable("y", tree::make<values::ConstAxis>(primitives::Axis{ 0, 1, 0 }));
    register_variable("z", tree::make<values::ConstAxis>(primitives::Axis{ 0, 0, 1 }));
    register_variable("true", tree::make<values::ConstBool>(true));
    register_variable("false", tree::make<values::ConstBool>(false));
    register_variable("pi", tree::make<values::ConstFloat>(std::numbers::pi));
    register_variable("eu", tree::make<values::ConstFloat>(std::numbers::e));
    register_variable("tau", tree::make<values::ConstFloat>(tau));
    register_variable("im", tree::make<values::ConstComplex>(primitives::Complex(0.0, 1.0)));
}

/**
 * Registers a number of default functions, such as the operator functions, and the usual trigonometric functions.
 */
void Analyzer::register_default_functions() {
    functions::register_default_function_impls_into(global_scope().function_impl_table);
}

/**
 * Analyzes the given AST.
 */
AnalysisResult Analyzer::analyze(ast::Program &ast) {
    auto analyze_visitor_up = std::make_unique<AnalyzeTreeGenAstVisitor>(*this);
    auto result = std::any_cast<AnalysisResult>(analyze_visitor_up->visit_program(ast));
    if (result.errors.empty() && !result.root.is_well_formed()) {
        std::cerr << *result.root;
        throw std::runtime_error{ "internal error: no semantic errors returned, but semantic tree is incomplete."
            " Tree was dumped." };
    }
    return result;
}

/**
 * Analyzes the given parse result.
 * If there are parse errors, they are moved into the AnalysisResult error list,
 * and the root node will be empty.
 */
AnalysisResult Analyzer::analyze(parser::ParseResult &&parse_result) {
    if (!parse_result.errors.empty()) {
        AnalysisResult result;
        result.errors = std::move(parse_result.errors);
        return result;
    }
    return analyze(*parse_result.root->as_program());
}

/**
 * Parses and analyzes using the given version and parser closures.
 */
AnalysisResult Analyzer::analyze(
    const std::function<version::Version()> &version_parser,
    const std::function<parser::ParseResult()> &parser) {

    AnalysisResult result;
    try {
        if (auto version = version_parser(); version > api_version) {
            result.errors.emplace_back(fmt::format(
                "cQASM file version is {}, but at most {} is supported here", version, api_version));
            return result;
        }
    } catch (error::AnalysisError &err) {
        result.errors.push_back(std::move(err));
        return result;
    }
    return analyze(parser());
}

/**
 * Parses and analyzes the given file.
 */
AnalysisResult Analyzer::analyze_file(const std::string &file_name) {
    return analyze(
        [=](){ return version::parse_file(file_name); },
        [=](){ return parser::parse_file(file_name, file_name); }
    );
}

/**
 * Parses and analyzes the given string.
 * The optional file_name argument will be used only for error messages.
 */
AnalysisResult Analyzer::analyze_string(const std::string &data, const std::optional<std::string> &file_name) {
    return analyze(
        [=](){ return version::parse_string(data, file_name); },
        [=](){ return parser::parse_string(data, file_name); }
    );
}

/**
 * Pushes a new empty scope to the top of the scope stack.
 */
void Analyzer::push_scope() {
    scope_stack_.emplace_front();
    current_scope().block = tree::make<semantic::Block>();
}

/**
 * Pops a scope from the top of the scope stack.
 */
void Analyzer::pop_scope() {
    scope_stack_.pop_front();
}

/**
 * Adds a statement to the current scope.
 */
void Analyzer::add_statement_to_current_scope(const tree::One<semantic::Statement> &statement) {
    if (current_block().empty()) {
        throw error::AnalysisError{ "trying to add a statement but current block is empty" };
    }

    // Add the statement to the current block
    current_block()->statements.add(statement);

    // Expand the source location annotation of the block to include the statement
    if (auto statement_sl = statement->get_annotation_ptr<parser::SourceLocation>()) {
        if (auto block_sl = current_block()->get_annotation_ptr<parser::SourceLocation>()) {
            block_sl->expand_to_include(statement_sl->range.first);
            block_sl->expand_to_include(statement_sl->range.last);
        } else {
            current_block()->set_annotation<parser::SourceLocation>(*statement_sl);
        }
    }
}

/**
 * Adds a variable to the current scope.
 */
void Analyzer::add_variable_to_current_scope(const tree::One<semantic::Variable> &variable) {
    current_variables().add(variable);
}

/**
 * Adds a function to the global scope.
 */
void Analyzer::add_function_to_global_scope(const tree::One<semantic::Function> &function) {
    global_functions().add(function);
}

/**
 * Resolves a variable.
 * Throws NameResolutionFailure if no variable by the given name exists.
 */
values::Value Analyzer::resolve_variable(const std::string &name) const {
    for (const auto &scope : scope_stack_) {
        try {
            return scope.variable_table.resolve(name);
        } catch (const error::AnalysisError &) {
            continue;
        }
    }
    throw resolver::NameResolutionFailure{ fmt::format("failed to resolve variable '{}'", name) };
}

/**
 * Registers a variable.
 */
void Analyzer::register_variable(const std::string &name, const values::Value &value) {
    current_scope().variable_table.add(name, value);
}

/**
 * Resolves a function implementation.
 * Throws NameResolutionFailure if no function by the given name exists,
 * OverloadResolutionFailure if no overload of the function exists for the given arguments,
 * or otherwise returns the value returned by the function.
 */
values::Value Analyzer::resolve_function_impl(const std::string &name, const values::Values &args) const {
    return global_scope().function_impl_table.resolve(name, args);
}

/**
 * Resolves a function.
 * Tries to call a function implementation first.
 * If it doesn't succeed, tries to call a function.
 * Throws NameResolutionFailure if no function by the given name exists,
 * OverloadResolutionFailure if no overload of the function exists for the given arguments,
 * or otherwise returns the value returned by the function.
 */
values::Value Analyzer::resolve_function(const std::string &name, const values::Values &args) const {
    try {
        return global_scope().function_impl_table.resolve(name, args);
    } catch (const error::AnalysisError &) {}
    return global_scope().function_table.resolve(name, args);
}

/**
 * Registers a function implementation, usable within expressions.
 */
void Analyzer::register_function_impl(
    const std::string &name,
    const types::Types &param_types,
    const resolver::FunctionImpl &impl) {

    global_scope().function_impl_table.add(name, param_types, impl);
}

/**
 * Convenience method for registering a function implementation.
 * The param_types are specified as a string,
 * converted to types::Types for the other overload using types::from_spec.
 */
void Analyzer::register_function_impl(
    const std::string &name,
    const std::string &param_types,
    const resolver::FunctionImpl &impl) {

    global_scope().function_impl_table.add(name, types::from_spec(param_types), impl);
}

/**
 * Convenience method for registering a function.
 */
void Analyzer::register_function(
    const std::string &name,
    const types::Types &param_types,
    const values::Value &value) {

    global_scope().function_table.add(name, param_types, value);
}

/**
 * Resolves an instruction.
 * Throws NameResolutionFailure if no instruction by the given name exists,
 * OverloadResolutionFailure if no overload exists for the given arguments,
 * or otherwise returns the resolved instruction node.
 * Annotation data, line number information, and the condition still need to be set by the caller.
 */
[[nodiscard]] tree::One<semantic::Instruction> Analyzer::resolve_instruction(
    const std::string &name, const values::Values &args) const {

    for (const auto &scope : scope_stack_) {
        try {
            return scope.instruction_table.resolve(name,  args);
        } catch (const error::AnalysisError &) {
            continue;
        }
    }
    throw resolver::ResolutionFailure{
        fmt::format("failed to resolve instruction '{}' with argument pack {}", name, values::types_of(args)) };
}

/**
 * Registers an instruction type.
 */
void Analyzer::register_instruction(const instruction::Instruction &instruction) {
    current_scope().instruction_table.add(instruction);
}

/**
 * Convenience method for registering an instruction type.
 * The arguments are passed straight to instruction::Instruction's constructor.
 */
void Analyzer::register_instruction(const std::string &name, const std::optional<std::string> &param_types) {
    register_instruction(instruction::Instruction(name, param_types));
}

} // namespace cqasm::v3x::analyzer
