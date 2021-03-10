/** \file
 * Defines primitive types for use in trees generated by \ref tree-gen.
 */

#pragma once

#include <string>
#include <cstdint>
#include <complex>
#include <vector>

#include "cqasm-version.hpp"

namespace cqasm {
namespace v2 {

/**
 * Namespace for the primitive types used in trees generated by \ref tree-gen.
 */
namespace primitives {

/**
 * Generates a default value for the given primitive type. This is specialized
 * for the primitives mapping to builtin types (int, bool, etc, for which the
 * "constructor" doesn't initialize the value at all) such that they actually
 * initialize with a sane default. Used in the default constructors of the
 * generated tree nodes to ensure that there's no garbage in the nodes.
 */
template <class T>
T initialize() { return T(); };

/**
 * String primitive used within the AST and semantic trees.
 */
using Str = std::string;
template <>
Str initialize<Str>();

/**
 * Boolean primitive used within the semantic trees. Defaults to false.
 */
using Bool = bool;
template <>
Bool initialize<Bool>();

/**
 * Integer primitive used within the AST and semantic trees.
 */
using Int = std::int64_t;
template <>
Int initialize<Int>();

/**
 * Real number primitive used within the AST and semantic trees.
 */
using Real = double;
template <>
Real initialize<Real>();

/**
 * Complex number primitive used within the semantic trees.
 */
using Complex = std::complex<double>;
template <>
Complex initialize<Complex>();

/**
 * Version number primitive used within the AST and semantic trees.
 */
using Version = version::Version;

} // namespace primitives
} // namespace v2
} // namespace cqasm