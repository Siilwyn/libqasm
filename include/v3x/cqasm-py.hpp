/** \file
 * Defines SWIG'd things for the Python interface.
 * This is just the *internal* interface to the library.
 * The user-facing API is written in pure-Python and wraps around this.
 */

#pragma once

// Don't include any libqasm headers!
// We don't want SWIG to generate Python wrappers for the entire world.
// Those headers are only included in the source file that provides the implementations.
#include <memory>
#include <optional>
#include <string>
#include <vector>


// Forward declarations for internal types.
namespace cqasm::v3x::analyzer {
    class Analyzer;
}

/**
 * Main class for parsing and analyzing cQASM files with the v3.x API.
 */
class V3xAnalyzer {
    /**
     * Reference to the actual C++ analyzer that this wraps.
     */
    std::unique_ptr<cqasm::v3x::analyzer::Analyzer> analyzer;

public:
    /**
     * Creates a new v3.x semantic analyzer.
     * When without_defaults is specified,
     * the default instruction set and error models are not loaded into the instruction and error model tables,
     * so you have to specify the entire instruction set using register_instruction() and register_error_model().
     * Otherwise, those functions only add to the defaults.
     * Unlike the C++ version of the analyzer class,
     * the initial mappings and functions are not configurable at all.
     * The defaults for these are always used.
     */
    V3xAnalyzer(const std::string &max_version = "3.0", bool without_defaults = false);

    /**
     * std::unique_ptr<T> requires T to be a complete class for the ~T operation.
     * Since we are using a forward declaration for Analyzer, we need to declare ~T in the header file,
     * and implement it in the source file.
     */
    ~V3xAnalyzer();

    /**
     * Registers an instruction type.
     * The arguments are passed straight to instruction::Instruction's constructor.
     */
    void register_instruction(const std::string &name, const std::string &param_types = "");

    /**
     * Only parses the given file.
     * The file must be in v3.x syntax.
     * No version check or conversion is performed.
     * Returns a vector of strings, of which the first is reserved for the CBOR serialization of the v3.x AST.
     * Any additional strings represent error messages.
     * Notice that the AST and error messages won't be available at the same time.
     */
    static std::vector<std::string> parse_file(const std::string &file_name);

    /**
     * Counterpart of parse_file that returns a string with a JSON representation of the ParseResult.
     */
    static std::string parse_file_to_json(const std::string &file_name);

    /**
     * Same as parse_file(), but instead receives the file contents directly.
     * The file_name, if specified, is only used when reporting errors.
     */
    static std::vector<std::string> parse_string(const std::string &data, const std::optional<std::string> &file_name);

    /**
     * Counterpart of parse_string that returns a string with a JSON representation of the ParseResult.
     */
    static std::string parse_string_to_json(const std::string &data, const std::optional<std::string> &file_name);

    /**
     * Parses and analyzes the given file.
     * If the file is written in a later file version,
     * this function may try to reduce it to the maximum v3.x API version support advertised
     * using this object's constructor.
     * Returns a vector of strings, of which the first is reserved for the CBOR serialization of the v3.x semantic tree.
     * Any additional strings represent error messages.
     * Notice that the AST and error messages won't be available at the same time.
     */
    std::vector<std::string> analyze_file(const std::string &file_name) const;

    /**
     * Counterpart of analyze_file that returns a string with a JSON representation of the AnalysisResult.
     */
    std::string analyze_file_to_json(const std::string &file_name) const;

    /**
     * Same as analyze_file(), but instead receives the file contents directly.
     * The file_name, if specified, is only used when reporting errors.
     */
    std::vector<std::string> analyze_string(const std::string &data, const std::optional<std::string> &file_name) const;

    /**
     * Counterpart of analyze_string that returns a string with a JSON representation of the AnalysisResult.
     */
    std::string analyze_string_to_json(const std::string &data, const std::optional<std::string> &file_name) const;
};
