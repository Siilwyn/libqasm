/**
 * \dir
 * Contains the non-generated source files and private header files for libqasm.
 *
 * \file
 * Implementation for \ref include/v3x/cqasm.hpp "v3x/cqasm.hpp".
 */

#include "cqasm-version.hpp"
#include "v3x/cqasm.hpp"
#include "v3x/cqasm-parse-helper.hpp"

#include <stdexcept>  // runtime_error


namespace cqasm::v3x {

/**
 * Parses and analyzes the given file path with the default analyzer,
 * dumping error messages to stderr and throwing an analyzer::AnalysisFailed on failure.
 */
tree::One<cqasm::v3x::semantic::Program> analyze_file(
    const std::string &file_path,
    const std::string &api_version
) {
    return cqasm::v3x::default_analyzer(api_version).analyze(
        [&file_path]() { return version::parse_file(file_path); },
        [&file_path]() { return cqasm::v3x::parser::parse_file(file_path, std::nullopt); }
    ).unwrap();
}

/**
 * Parses and analyzes the given string with the default analyzer,
 * dumping error messages to stderr and throwing an analyzer::AnalysisFailed on failure.
 * The optional file_name is only used for error messages.
 */
tree::One<cqasm::v3x::semantic::Program> analyze_string(
    const std::string &data,
    const std::optional<std::string> &file_name,
    const std::string &api_version
) {
    return cqasm::v3x::default_analyzer(api_version).analyze(
        [&data, &file_name]() { return version::parse_string(data, file_name); },
        [&data, &file_name]() { return cqasm::v3x::parser::parse_string(data, file_name); }
    ).unwrap();
}

/**
 * Constructs an Analyzer object with the defaults for cQASM 3.0 already loaded into it.
 */
analyzer::Analyzer default_analyzer(const std::string &api_version) {
    analyzer::Analyzer analyzer{ api_version };

    analyzer.register_default_mappings();
    analyzer.register_default_functions();

    analyzer.register_instruction("cnot", "QQ");
    analyzer.register_instruction("cnot", "QV");
    analyzer.register_instruction("cnot", "VQ");
    analyzer.register_instruction("cnot", "VV");
    analyzer.register_instruction("cr", "QQf");
    analyzer.register_instruction("cr", "QVf");
    analyzer.register_instruction("cr", "VQf");
    analyzer.register_instruction("cr", "VVf");
    analyzer.register_instruction("crk", "QQi");
    analyzer.register_instruction("crk", "QVi");
    analyzer.register_instruction("crk", "VQi");
    analyzer.register_instruction("crk", "VVi");
    analyzer.register_instruction("cz", "QQ");
    analyzer.register_instruction("cz", "QV");
    analyzer.register_instruction("cz", "VQ");
    analyzer.register_instruction("cz", "VV");
    analyzer.register_instruction("h", "Q");
    analyzer.register_instruction("h", "V");
    analyzer.register_instruction("i", "Q");
    analyzer.register_instruction("measure", "BQ");
    analyzer.register_instruction("measure", "WV");
    analyzer.register_instruction("measure", "BV");
    analyzer.register_instruction("measure", "WQ");
    analyzer.register_instruction("mx90", "Q");
    analyzer.register_instruction("mx90", "V");
    analyzer.register_instruction("my90", "Q");
    analyzer.register_instruction("my90", "V");
    analyzer.register_instruction("rx", "Qf");
    analyzer.register_instruction("rx", "Vf");
    analyzer.register_instruction("ry", "Qf");
    analyzer.register_instruction("ry", "Vf");
    analyzer.register_instruction("rz", "Qf");
    analyzer.register_instruction("rz", "Vf");
    analyzer.register_instruction("s", "Q");
    analyzer.register_instruction("s", "V");
    analyzer.register_instruction("sdag", "Q");
    analyzer.register_instruction("sdag", "V");
    analyzer.register_instruction("x", "Q");
    analyzer.register_instruction("x", "V");
    analyzer.register_instruction("x90", "Q");
    analyzer.register_instruction("x90", "V");
    analyzer.register_instruction("y", "Q");
    analyzer.register_instruction("y", "V");
    analyzer.register_instruction("y90", "Q");
    analyzer.register_instruction("y90", "V");
    analyzer.register_instruction("z", "Q");
    analyzer.register_instruction("z", "V");

    return analyzer;
}

} // namespace cqasm::v3x
