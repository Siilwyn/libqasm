/** \file
 * Implementation for \ref include/v3x/cqasm-instruction.hpp "v3x/cqasm-instruction.hpp".
 */

#include "v3x/cqasm-instruction.hpp"
#include "v3x/cqasm-semantic.hpp"


namespace cqasm::v3x {

namespace instruction {

/**
 * Creates a new instruction.
 * param_types is a shorthand type specification string as parsed by cqasm::types::from_spec().
 * If you need more control, you can also manipulate param_types directly.
 */
Instruction::Instruction(
    const std::string &name,
    const std::string &param_types,
    bool request_qubit_and_bit_indices_have_same_size)
: name{ name }
, param_types{ types::from_spec(param_types) }
, request_qubit_and_bit_indices_have_same_size{ request_qubit_and_bit_indices_have_same_size }
{}

/**
 * Equality operator.
 */
bool Instruction::operator==(const Instruction& rhs) const {
    return name == rhs.name && param_types == rhs.param_types;
}

/**
 * Stream << overload for instructions.
 */
std::ostream &operator<<(std::ostream &os, const Instruction &insn) {
    return os << insn.name << insn.param_types;
}

/**
 * Stream << overload for instruction references.
 */
std::ostream &operator<<(std::ostream &os, const InstructionRef &insn) {
    return insn.empty()
        ? os << "unresolved"
        : os << *insn;
}

}  // namespace instruction


namespace primitives {

template <>
void serialize(const instruction::InstructionRef &obj, ::tree::cbor::MapWriter &map) {
    if (obj.empty()) {
        return;
    }
    map.append_string("n", obj->name);
    map.append_bool("i", obj->request_qubit_and_bit_indices_have_same_size);
    auto aw = map.append_array("t");
    for (const auto &t : obj->param_types) {
        aw.append_binary(::tree::base::serialize(t));
    }
    aw.close();
}

template <>
instruction::InstructionRef deserialize(const ::tree::cbor::MapReader &map) {
    if (!map.count("n")) {
        return {};
    }
    auto insn = tree::make<instruction::Instruction>(
        map.at("n").as_string(),
        "",
        map.at("f").as_bool()
    );
    auto ar = map.at("t").as_array();
    for (const auto &element : ar) {
        insn->param_types.add(::tree::base::deserialize<types::Node>(element.as_binary()));
    }
    return insn;
}

}  // namespace primitives

}  // namespace cqasm::v3x