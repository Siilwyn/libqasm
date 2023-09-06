/**
 * \file
 * Main include file for parsing v3x files.
 */

#pragma once

#include "cqasm-parse-helper.hpp"


/**
 * Toplevel namespace with entry points for the new API.
 */
namespace cqasm {

/**
 * Namespace for the "new" cQASM 3.x API. Its contents are pulled into the main
 * cQASM namespace when you include "cqasm.hpp" for compatibility.
 */
namespace v3x {

/**
 * Parses and analyzes the given file with the default analyzer, dumping error
 * messages to stderr and throwing an analyzer::AnalysisFailed on failure.
 */
void analyze(
    const std::string &filename,
    const std::string &api_version = "3.0"
);

/**
 * Parses and analyzes the given file pointer with the default analyzer, dumping
 * error messages to stderr and throwing an analyzer::AnalysisFailed on failure.
 * The optional filename is only used for error messages.
 */
void analyze(
    FILE *file,
    const std::string &filename = "<unknown>",
    const std::string &api_version = "3.0"
);

/**
 * Parses and analyzes the given string with the default analyzer, dumping
 * error messages to stderr and throwing an analyzer::AnalysisFailed on failure.
 * The optional filename is only used for error messages.
 */
void analyze_string(
    const std::string &data,
    const std::string &filename = "<unknown>",
    const std::string &api_version = "3.0"
);

} // namespace v3x
} // namespace cqasm