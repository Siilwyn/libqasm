/** \file
 * This file contains the \ref cqasm::v1x::analyzer::Analyzer "Analyzer" class and
 * support classes, used to manage semantic analysis.
 *
 * While the Analyzer class itself only manages the semantic analysis phase,
 * it also has some convenience methods that drive lexical analysis and parsing
 * in addition.
 */

#pragma once

#include "cqasm-analysis-result.hpp"
#include "cqasm-ast.hpp"
#include "cqasm-parse-helper.hpp"
#include "cqasm-resolver.hpp"
#include "cqasm-semantic.hpp"

#include <functional>
#include <optional>
#include <string>


/**
 * Namespace for the \ref cqasm::analyzer::Analyzer "Analyzer" class and
 * support classes.
 */
namespace cqasm::v1x::analyzer {

/**
 * Main class used for analyzing cQASM files.
 *
 * Construction of this class is the entry point for libqasm whenever you need
 * to modify the default instruction set, have a different set of supported
 * error models, or want to add additional initial mappings, operators, or
 * functions. The process is simple:
 *
 *  - Construct an Analyzer object with the default constructor.
 *  - Use zero or more of the various `register_*()` methods to configure the
 *    Analyzer.
 *  - Use one or more of the `analyze*()` methods to analyze cQASM files or
 *    string representations thereof.
 *
 * Note that the only state maintained by the Analyzer object is its
 * configuration, and the `analyze*()` functions never change this state
 * (hence they are const).
 */
class Analyzer {
private:
    friend class AnalyzerHelper;

    /**
     * The maximum cQASM version that this analyzer supports.
     */
    primitives::Version api_version;

    /**
     * The set of "mappings" that the parser starts out with (map statements in
     * the cQASM code mutate a local copy of this).
     */
    resolver::MappingTable mappings;

    /**
     * The supported set of classical functions and operators. Functions have a
     * name (either a case-insensitively matched function name using the usual
     * function notation, or one of the supported operators), a signature
     * for the types of arguments it expects, and a C++ function that takes
     * value nodes of those expected types and returns the resulting value.
     * Note that, once runtime expressions are implemented, the resulting value
     * can be some expression of the incoming values.
     */
    resolver::FunctionTable functions;

    /**
     * The supported set of quantum/classical/mixed instructions, appearing in
     * the cQASM file as assembly-like commands. Instructions have a
     * case-insensitively matched name, a signature for the types of parameters
     * it expects, and some flags indicating how (much) error checking is to
     * be done. You can also add your own metadata through the Annotatable
     * interface.
     */
    resolver::InstructionTable instruction_set;

    /**
     * When set, instruction resolution is disabled. That is, instruction_set
     * is unused, no type promotion is (or can be) performed for instruction
     * parameters, and the instruction field of the semantic::Instruction nodes
     * is left uninitialized.
     */
    bool resolve_instructions;

    /**
     * The supported set of error models. Zero or one of these can be specified
     * in the cQASM file using the special "error_model" instruction. Error
     * models have a name and a signature for the types of parameters it
     * expects. You can also add your own metadata through the Annotatable
     * interface.
     */
    resolver::ErrorModelTable error_models;

    /**
     * When set, error model resolution is disabled. That is, error_models
     * is unused, no type promotion is (or can be) performed for instruction
     * parameters, and the model field of the semantic::ErrorModel node is left
     * uninitialized.
     */
    bool resolve_error_model;

public:
    /**
     * Creates a new semantic analyzer.
     */
    explicit Analyzer(const primitives::Version &api_version = "1.0");

    /**
     * Registers a function, usable within expressions.
     *
     * values::check_const() can be used in the function implementation
     * to assert that the values must be constant when the function can only be used during constant propagation.
     * When the function also (or only) supports dynamic evaluation,
     * the implementation will have to check
     * whether the inputs are const manually (for instance using `as_constant()`)
     * to determine when to return a dynamic values::Function node instead.
     */
    void register_function(
        const std::string &name,
        const types::Types &param_types,
        const resolver::FunctionImpl &impl
    );

    /**
     * Convenience method for registering a function.
     * The param_types are specified as a string,
     * converted to types::Types for the other overload using types::from_spec.
     */
    void register_function(
        const std::string &name,
        const std::string &param_types,
        const resolver::FunctionImpl &impl
    );

    /**
     * Registers an initial mapping from the given name to the given value.
     */
    void register_mapping(const std::string &name, const values::Value &value);

    /**
     * Registers a number of default functions and mappings, such as the operator functions,
     * the usual trigonometric functions, mappings for pi, eu (aka e, 2.718...), im (imaginary unit) and so on.
     */
    void register_default_functions_and_mappings();

    /**
     * Registers an instruction type. If you never call this, instructions are
     * not resolved (i.e. anything goes name- and operand type-wise). Once you
     * do, only instructions with signatures as added are legal, so anything
     * that doesn't match returns an error.
     */
    void register_instruction(const instruction::Instruction &instruction);

    /**
     * Convenience method for registering an instruction type. The arguments
     * are passed straight to instruction::Instruction's constructor.
     */
    void register_instruction(
        const std::string &name,
        const std::string &param_types = "",
        bool allow_conditional = true,
        bool allow_parallel = true,
        bool allow_reused_qubits = false,
        bool allow_different_index_sizes = false
    );

    /**
     * Convenience method for registering an instruction type with a single
     * user-specified annotation. The arguments are passed straight to
     * instruction::Instruction's constructor and set_annotation.
     */
    template <typename T>
    void register_instruction_with_annotation(
        T &&annotation,
        const std::string &name,
        const std::string &param_types = "",
        bool allow_conditional = true,
        bool allow_parallel = true,
        bool allow_reused_qubits = false,
        bool allow_different_index_sizes = false
    ) {
        instruction::Instruction insn {
            name,
            param_types,
            allow_conditional,
            allow_parallel,
            allow_reused_qubits,
            allow_different_index_sizes
        };
        insn.set_annotation<T>(std::forward<T>(annotation));
        register_instruction(insn);
    }

    /**
     * Registers an error model. If you never call this, error models are not
     * resolved (i.e. anything goes name- and operand type-wise). Once you
     * do, only error models with signatures as added are legal, so anything
     * that doesn't match returns an error.
     */
    void register_error_model(const error_model::ErrorModel &error_model);

    /**
     * Convenience method for registering an error model. The arguments
     * are passed straight to error_model::ErrorModel's constructor.
     */
    void register_error_model(
        const std::string &name,
        const std::string &param_types = ""
    );

    /**
     * Convenience method for registering an error model with a single
     * user-specified annotation. The arguments are passed straight to
     * instruction::Instruction's constructor and set_annotation.
     */
    template <typename T>
    void register_error_model_with_annotation(
        T &&annotation,
        const std::string &name,
        const std::string &param_types = ""
    ) {
        error_model::ErrorModel model {
            name,
            param_types
        };
        model.set_annotation<T>(std::forward<T>(annotation));
        register_error_model(model);
    }

    /**
     * Analyzes the given program AST node.
     */
    AnalysisResult analyze(ast::Program &program);

    /**
     * Analyzes the given parse result.
     * If there are parse errors, they are moved into the AnalysisResult error list, and
     * the root node will be empty.
     */
    AnalysisResult analyze(parser::ParseResult &&parse_result);

    /**
     * Parses and analyzes using the given version and parser closures.
     */
    AnalysisResult analyze(
        const std::function<version::Version()> &version_parser,
        const std::function<parser::ParseResult()> &parser);

    /**
     * Parses and analyzes the given file.
     */
    AnalysisResult analyze_file(const std::string &file_name);

    /**
     * Parses and analyzes the given file pointer.
     * The optional file_name argument will be used only for error messages.
     */
    AnalysisResult analyze_file(FILE *file, const std::optional<std::string> &file_name);

    /**
     * Parses and analyzes the given string.
     * The optional file_name argument will be used only for error messages.
     */
    AnalysisResult analyze_string(const std::string &data, const std::optional<std::string> &file_name);
};

} // namespace cqasm::v1x::analyzer
