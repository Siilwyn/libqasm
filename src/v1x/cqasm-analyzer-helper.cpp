#include "cqasm-utils.hpp"
#include "v1x/cqasm-analyzer-helper.hpp"
#include "v1x/cqasm-values.hpp"

#include <fmt/format.h>
#include <map>
#include <unordered_set>


namespace cqasm::v1x::analyzer {

/**
 * Analyzes the given AST using the given analyzer.
 */
AnalyzerHelper::AnalyzerHelper(const Analyzer &analyzer, const ast::Program &ast)
: analyzer(analyzer)
, result()
, scope_stack( { Scope(analyzer.mappings, analyzer.functions, analyzer.instruction_set) } )
{
    try {
        // Construct the program node.
        result.root.set(tree::make<semantic::Program>());
        result.root->copy_annotation<parser::SourceLocation>(ast);
        result.root->api_version = analyzer.api_version;

        // Check and set the version.
        analyze_version(*ast.version);

        // Handle the qubits statement.
        // Qubit variables can be used instead of the qubits keyword,
        // in which case num_qubits is set to 0 to indicate that it's not being used.
        if (!ast.num_qubits.empty()) {
            analyze_qubits(*ast.num_qubits);
        } else if (ast.version->items < "1.1") {
            throw error::AnalysisError{ "missing qubits statement (required until version 1.1)" };
        } else {
            result.root->num_qubits = 0;
        }

        // Read the statements.
        analyze_statements(*ast.statements);

        // Resolve goto targets.
        if (ast.version->items >= "1.2") {

            // Figure out all the subcircuit names and check for duplicates.
            std::map<std::string, tree::Maybe<semantic::Subcircuit>> subcircuits;
            for (const auto &subcircuit : result.root->subcircuits) {
                try {
                    auto insert_result = subcircuits.insert({subcircuit->name, subcircuit});
                    if (!insert_result.second) {
                        std::ostringstream ss;
                        ss << "duplicate subcircuit name \"" << subcircuit->name << "\"";
                        if (auto loc = insert_result.first->second->get_annotation_ptr<parser::SourceLocation>()) {
                            ss << "; previous definition was at " << *loc;
                        }
                        throw error::AnalysisError{ ss.str() };
                    }
                } catch (error::AnalysisError &err) {
                    err.context(*subcircuit);
                    result.errors.push_back(std::move(err));
                }
            }

            // Resolve the goto instruction targets.
            for (const auto &it : gotos) {
                try {
                    auto it2 = subcircuits.find(it.second);
                    if (it2 == subcircuits.end()) {
                        throw error::AnalysisError{
                            fmt::format("failed to resolve subcircuit \"{}\"", it.second) };
                    }
                    it.first->target = it2->second;
                } catch (error::AnalysisError &err) {
                    err.context(*it.first);
                    result.errors.push_back(std::move(err));
                }
            }

        }

        // Save the list of final mappings.
        for (const auto &it : get_current_scope().mappings.get_table()) {
            const auto &name = it.first;
            const auto &value = it.second.first;
            const auto &ast_node = it.second.second;

            // Ignore predefined and implicit mappings.
            if (ast_node.empty()) {
                continue;
            }

            // Analyze any annotations attached to the mapping.
            auto annotations = analyze_annotations(ast_node->annotations);

            // Construct the mapping object and copy the source location.
            auto mapping = tree::make<semantic::Mapping>(
                name, value,
                analyze_annotations(ast_node->annotations)
            );
            mapping->copy_annotation<parser::SourceLocation>(*ast_node);
            result.root->mappings.add(mapping);

        }

        // The iteration order over the mapping table is undefined,
        // because it's backed by an unordered_map.
        // To get a deterministic tree, sort by source location.
        std::sort(
            result.root->mappings.begin(), result.root->mappings.end(),
            [](const tree::One<semantic::Mapping> &lhs, const tree::One<semantic::Mapping> &rhs) -> bool {
                auto lhs_source_location = lhs->get_annotation_ptr<parser::SourceLocation>();
                auto rhs_source_location = rhs->get_annotation_ptr<parser::SourceLocation>();
                return lhs_source_location && rhs_source_location && *lhs_source_location < *rhs_source_location;
            });

    } catch (error::AnalysisError &err) {
        result.errors.push_back(std::move(err));
    }
}

/**
 * Checks the AST version node and puts it into the semantic tree.
 */
void AnalyzerHelper::analyze_version(const ast::Version &ast) {
    try {
        // Default to API version in case the version in the AST is broken.
        result.root->version = tree::make<semantic::Version>();
        result.root->version->items = analyzer.api_version;

        // Check API version.
        for (const auto &item : ast.items) {
            if (item < 0) {
                throw error::AnalysisError{ "invalid version component" };
            }
        }
        if (ast.items > analyzer.api_version) {
            throw error::AnalysisError{ fmt::format(
                "the maximum cQASM version supported is {}, but the cQASM file is version {}",
                analyzer.api_version, ast.items) };
        }

        // Save the file version.
        result.root->version->items = ast.items;

    } catch (error::AnalysisError &err) {
        err.context(ast);
        result.errors.push_back(std::move(err));
    }
    result.root->version->copy_annotation<parser::SourceLocation>(ast);
}

/**
 * Checks the qubits statement and updates the scope accordingly.
 * Any semantic errors encountered are pushed into the result error vector.
 */
void AnalyzerHelper::analyze_qubits(const ast::Expression &count) {
    try {
        // Default to 0 qubits in case we get an exception or no qubit count is defined.
        result.root->num_qubits = 0;

        // Try to load the number of qubits from the expression.
        result.root->num_qubits = analyze_as_const_int(count);
        if (result.root->num_qubits < 1) {
            // Number of qubits must be positive if specified.
            throw error::AnalysisError{ "invalid number of qubits" };
        }

        // Construct the special q and b mappings, that map to the whole qubit and measurement register respectively.
        tree::Many<values::ConstInt> all_qubits;
        for (primitives::Int i = 0; i < result.root->num_qubits; i++) {
            auto vi = tree::make<values::ConstInt>(i);
            vi->copy_annotation<parser::SourceLocation>(count);
            all_qubits.add(vi);
        }
        get_current_scope().mappings.add("q", tree::make<values::QubitRefs>(all_qubits));
        get_current_scope().mappings.add("b", tree::make<values::BitRefs>(all_qubits));

    } catch (error::AnalysisError &err) {
        err.context(count);
        result.errors.push_back(std::move(err));
    }
}

/**
 * Returns a reference to the subcircuit that's currently being built.
 * If there is no subcircuit yet, a default one is created,
 * using the source location annotation on the source node.
 */
tree::Maybe<semantic::Subcircuit> AnalyzerHelper::get_current_subcircuit(const tree::Annotatable &source) {
    // If we don't have a subcircuit yet, add a default one.
    // Note that the original libqasm always had this default subcircuit (even if it was empty)
    // and used the name "default" vs. the otherwise invalid empty string.
    if (result.root->subcircuits.empty()) {
        auto subcircuit_node = tree::make<semantic::Subcircuit>("", 1);
        subcircuit_node->copy_annotation<parser::SourceLocation>(source);
        if (analyzer.api_version >= "1.2") {
            subcircuit_node->body = tree::make<semantic::Block>();
        }
        result.root->subcircuits.add(subcircuit_node);
    }

    // Add the node to the last subcircuit.
    return result.root->subcircuits.back();
}

/**
 * Returns a reference to the current scope.
 */
Scope &AnalyzerHelper::get_current_scope() {
    return scope_stack.back();
}

/**
 * Returns a reference to the global scope.
 */
Scope &AnalyzerHelper::get_global_scope() {
    return scope_stack.front();
}

/**
 * Returns a reference to the block that's currently being built.
 */
tree::Maybe<semantic::Block> AnalyzerHelper::get_current_block(
    const tree::Annotatable &source
) {
    // If we're in a local scope/block, return that block.
    const auto &scope = get_current_scope();
    if (!scope.block.empty()) {
        return scope.block;
    }

    // Otherwise return the block belonging to the current subcircuit.
    return get_current_subcircuit(source)->body;
}

/**
 * Adds an analyzed statement to the current block (1.2+).
 */
void AnalyzerHelper::add_to_current_block(const tree::Maybe<semantic::Statement> &statement) {
    // Add the statement to the current block.
    auto block = get_current_block(*statement);
    block->statements.add(statement);

    // Expand the source location annotation of the block to include the statement.
    if (auto statement_loc = statement->get_annotation_ptr<parser::SourceLocation>()) {
        if (auto block_loc = block->get_annotation_ptr<parser::SourceLocation>()) {
            block_loc->expand_to_include(statement_loc->range.first);
            block_loc->expand_to_include(statement_loc->range.last);
        } else {
            block->set_annotation<parser::SourceLocation>(*statement_loc);
        }
    }
}

/**
 * Analyzes the given statement list,
 * adding the analyzed statements to the current subcircuit (API 1.0/1.1) or block (API 1.2+).
 */
void AnalyzerHelper::analyze_statements(const ast::StatementList &statements) {
    for (const auto &statement : statements.items) {
        try {
            if (auto bundle = statement->as_bundle()) {
                if (analyzer.api_version >= "1.2") {
                    analyze_bundle_ext(*bundle);
                } else {
                    analyze_bundle(*bundle);
                }
            } else if (auto mapping = statement->as_mapping()) {
                analyze_mapping(*mapping);
            } else if (auto variables = statement->as_variables()) {
                analyze_variables(*variables);
            } else if (auto subcircuit = statement->as_subcircuit()) {
                analyze_subcircuit(*subcircuit);
            } else if (auto structured = statement->as_structured()) {
                if (result.root->version->items < "1.2") {
                    throw error::AnalysisError{ "structured control-flow is not supported (need version 1.2+)" };
                }
                analyze_structured(*structured);
            } else {
                throw std::runtime_error("unexpected statement node");
            }
        } catch (error::AnalysisError &err) {
            err.context(*statement);
            result.errors.push_back(std::move(err));
        }
    }
}

/**
 * Analyzes a statement list corresponding to a structured control-flow subblock (1.2+).
 * Handles the requisite scoping, then defers to analyze_statements().
 */
tree::Maybe<semantic::Block> AnalyzerHelper::analyze_subblock(
    const ast::StatementList &statements,
    bool is_loop
) {
    // Create the block.
    tree::Maybe<semantic::Block> block;
    block.emplace();

    // Create a scope for the block.
    scope_stack.emplace_back(get_current_scope());
    get_current_scope().block = block;
    get_current_scope().within_loop |= is_loop;

    // Analyze the statements within the block.
    // The statements will be added to the current scope, which we just updated.
    analyze_statements(statements);

    // Pop the scope from the stack.
    scope_stack.pop_back();

    return block;
}

/**
 * Analyzes the given bundle and, if valid, adds it to the current subcircuit using API version 1.0/1.1.
 * If an error occurs, the message is added to the result error vector, and nothing is added to the subcircuit.
 */
void AnalyzerHelper::analyze_bundle(const ast::Bundle &bundle) {
    try {
        // The error model statement from the original cQASM grammar is a bit of a pain,
        // because it conflicts with gates/instructions, so we have to special-case it here.
        // Technically we could also have made it a keyword,
        // but the less random keywords there are, the better.
        if (bundle.items.size() == 1) {
            if (utils::equal_case_insensitive(bundle.items[0]->name->name, "error_model")) {
                analyze_error_model(*bundle.items[0]);
                return;
            }
        }

        // Analyze and add the instructions.
        auto node = tree::make<semantic::Bundle>();
        for (const auto &insn : bundle.items) {
            node->items.add(analyze_instruction(*insn));
        }

        // If we have more than two instructions, ensure that all instructions are parallelizable.
        if (node->items.size() > 1) {
            for (const auto &insn : node->items) {
                try {
                    if (!insn->instruction.empty()) {
                        if (!insn->instruction->allow_parallel) {
                            std::ostringstream ss;
                            ss << "instruction ";
                            ss << insn->instruction->name;
                            ss << " with parameter pack ";
                            ss << insn->instruction->param_types;
                            ss << " is not parallelizable, but is bundled with ";
                            ss << (node->items.size() - 1);
                            ss << " other instruction";
                            if (node->items.size() != 2) {
                                ss << "s";
                            }
                            throw error::AnalysisError{ ss.str() };
                        }
                    }
                } catch (error::AnalysisError &err) {
                    err.context(*insn);
                    result.errors.push_back(std::move(err));
                }
            }
        }

        // It's possible that no instructions end up being added,
        // due to all condition codes resolving to constant false.
        // In that case the entire bundle is removed.
        if (node->items.empty()) {
            return;
        }

        // Copy annotation data.
        node->annotations = analyze_annotations(bundle.annotations);
        node->copy_annotation<parser::SourceLocation>(bundle);

        // Add the node to the last subcircuit.
        get_current_subcircuit(bundle)->bundles.add(node);

    } catch (error::AnalysisError &err) {
        err.context(bundle);
        result.errors.push_back(std::move(err));
    }
}

/**
 * Analyzes the given bundle and, if valid, adds it to the current
 * subcircuit using API version 1.2+. If an error occurs, the message is
 * added to the result error vector, and nothing is added to the subcircuit.
 */
void AnalyzerHelper::analyze_bundle_ext(const ast::Bundle &bundle) {
    try {
        // The error model statement from the original cQASM grammar is a bit of a pain,
        // because it conflicts with gates/instructions, so we have to special-case it here.
        // Technically we could also have made it a keyword,
        // but the less random keywords there are, the better.
        if (bundle.items.size() == 1) {
            if (utils::equal_case_insensitive(bundle.items[0]->name->name, "error_model")) {
                analyze_error_model(*bundle.items[0]);
                return;
            }
        }

        // Analyze and add the instructions
        auto node = tree::make<semantic::BundleExt>();
        for (const auto &insn : bundle.items) {
            if (utils::equal_case_insensitive(insn->name->name, "set")) {
                node->items.add(analyze_set_instruction(*insn));
            } else if (utils::equal_case_insensitive(insn->name->name, "goto")) {
                node->items.add(analyze_goto_instruction(*insn));
            } else {
                node->items.add(analyze_instruction(*insn));
            }
        }

        // If we have more than two instructions, ensure that all instructions can be executed in parallel
        if (node->items.size() > 1) {
            for (const auto &insn_base : node->items) {
                try {
                    if (auto insn = insn_base->as_instruction()) {
                        if (!insn->instruction.empty()) {
                            if (!insn->instruction->allow_parallel) {
                                std::ostringstream ss;
                                ss << "instruction ";
                                ss << insn->instruction->name;
                                ss << " with parameter pack ";
                                ss << insn->instruction->param_types;
                                ss << " is not parallelizable, but is bundled with ";
                                ss << (node->items.size() - 1);
                                ss << " other instruction";
                                if (node->items.size() != 2) {
                                    ss << "s";
                                }
                                throw error::AnalysisError{ ss.str() };
                            }
                        }
                    }
                } catch (error::AnalysisError &err) {
                    err.context(*insn_base);
                    result.errors.push_back(std::move(err));
                }
            }
        }

        // It's possible that no instructions end up being added,
        // due to all condition codes resolving to constant false.
        // In that case the entire bundle is removed.
        if (node->items.empty()) {
            return;
        }

        // Copy annotation data.
        node->annotations = analyze_annotations(bundle.annotations);
        node->copy_annotation<parser::SourceLocation>(bundle);

        // Add the node to the last subcircuit.
        add_to_current_block(node.as<semantic::Statement>());

    } catch (error::AnalysisError &err) {
        err.context(bundle);
        result.errors.push_back(std::move(err));
    }
}

/**
 * Analyzes the given instruction.
 * If an error occurs, the message is added to the result error vector, and an empty Maybe is returned.
 * It's also possible that an empty Maybe is returned without an error,
 * when the condition code statically resolves to false.
 */
tree::Maybe<semantic::Instruction> AnalyzerHelper::analyze_instruction(const ast::Instruction &insn) {
    try {
        // Figure out the operand list.
        auto operands = values::Values();
        for (const auto &operand_expr : insn.operands->items) {
            operands.add(analyze_expression(*operand_expr));
        }

        // Resolve the instruction and/or make the instruction node.
        tree::Maybe<semantic::Instruction> node;
        if (analyzer.resolve_instructions) {
            node.set(get_current_scope().instruction_set.resolve(insn.name->name, operands));
        } else {
            node.set(tree::make<semantic::Instruction>(
                tree::Maybe<instruction::Instruction>(),
                insn.name->name, values::Value(), operands,
                tree::Any<semantic::AnnotationData>()));
        }

        // Resolve the condition code.
        if (!insn.condition.empty()) {
            if (!node->instruction.empty() && !node->instruction->allow_conditional) {
                throw error::AnalysisError{
                    "conditional execution is not supported for this instruction" };
            }
            auto condition_val = analyze_expression(*insn.condition);
            node->condition = values::promote(condition_val, tree::make<types::Bool>());
            if (node->condition.empty()) {
                throw error::AnalysisError{ "condition must be a boolean" };
            }

            // If the condition is constant false, optimize the instruction
            // away.
            if (auto x = node->condition->as_const_bool()) {
                if (!x->value) {
                    return {};
                }
            }

        } else {
            node->condition.set(tree::make<values::ConstBool>(true));
        }

        // Enforce qubit uniqueness if the instruction requires us to.
        if (!node->instruction.empty() && !node->instruction->allow_reused_qubits) {
            std::unordered_set<primitives::Int> qubits_used;
            for (const auto &operand : operands) {
                if (auto x = operand->as_qubit_refs()) {
                    for (const auto &index : x->index) {
                        if (!qubits_used.insert(index->value).second) {
                            throw error::AnalysisError{
                                fmt::format("qubit with index {} is used more than once", index->value) };
                        }
                    }
                }
            }
        }

        // Enforce that all qubit and bit references have the same length if
        // the instruction requires us to. Note that historically the condition
        // is NOT split across the resulting parallel instructions but is
        // instead copied and reduced using boolean and at runtime, so its
        // length does NOT have to match.
        if (!node->instruction.empty() && !node->instruction->allow_different_index_sizes) {
            size_t num_refs = 0;
            const parser::SourceLocation *num_refs_loc = nullptr;
            for (const auto &operand : operands) {
                const tree::Many<values::ConstInt> *indices = nullptr;
                if (auto qr = operand->as_qubit_refs()) {
                    indices = &qr->index;
                } else if (auto br = operand->as_bit_refs()) {
                    indices = &br->index;
                }
                if (indices) {
                    if (!num_refs) {
                        num_refs = indices->size();
                    } else if (num_refs != indices->size()) {
                        std::ostringstream ss;
                        ss << "the number of indices (" << indices->size() << ") ";
                        ss << "doesn't match previously found number of indices ";
                        ss << "(" << num_refs << ")";
                        if (num_refs_loc) {
                            ss << " at " << *num_refs_loc;
                        }
                        throw error::AnalysisError{ ss.str(), &*operand };
                    }
                    if (!num_refs_loc) {
                        num_refs_loc = operand->get_annotation_ptr<parser::SourceLocation>();
                    }
                }
            }
        }

        // Copy annotation data.
        node->annotations = analyze_annotations(insn.annotations);
        node->copy_annotation<parser::SourceLocation>(insn);

        return node;
    } catch (error::AnalysisError &err) {
        err.context(insn);
        result.errors.push_back(std::move(err));
    }
    return {};
}

/**
 * Analyzes the given cQASM 1.2+ set instruction.
 * If an error occurs, the message is added to the result error vector,
 * and an empty Maybe is returned.
 */
tree::Maybe<semantic::SetInstruction> AnalyzerHelper::analyze_set_instruction(
    const ast::Instruction &insn
) {
    try {
        // Figure out the operand list.
        if (insn.operands->items.size() != 2) {
            throw error::AnalysisError{ "set instruction must have two operands" };
        }

        // Analyze the operands.
        auto node = analyze_set_instruction_operands(
            *insn.operands->items[0],
            *insn.operands->items[1]
        );

        // Resolve the condition code.
        if (!insn.condition.empty()) {
            auto condition_val = analyze_expression(*insn.condition);
            node->condition = values::promote(condition_val, tree::make<types::Bool>());
            if (node->condition.empty()) {
                throw error::AnalysisError{ "condition must be a boolean" };
            }

            // If the condition is constant false, optimize the instruction
            // away.
            if (auto x = node->condition->as_const_bool()) {
                if (!x->value) {
                    return {};
                }
            }
        } else {
            node->condition.set(tree::make<values::ConstBool>(true));
        }

        // Copy annotation data.
        node->annotations = analyze_annotations(insn.annotations);
        node->copy_annotation<parser::SourceLocation>(insn);

        return node;
    } catch (error::AnalysisError &err) {
        err.context(insn);
        result.errors.push_back(std::move(err));
    }
    return {};
}

/**
 * Analyzes the given two operands as lhs and rhs of a set instruction.
 * Used for the actual set instruction as well as the assignments in the header of a C-style for loop.
 */
tree::Maybe<semantic::SetInstruction> AnalyzerHelper::analyze_set_instruction_operands(
    const ast::Expression &lhs_expr,
    const ast::Expression &rhs_expr
) {
    // Analyze the expressions.
    auto lhs = analyze_expression(lhs_expr);
    auto rhs = analyze_expression(rhs_expr);

    // Check assignability of the left-hand side.
    bool assignable = lhs->as_reference();
    if (auto fn = lhs->as_function()) {
        assignable |= fn->return_type->as_type_base()->assignable;
    }
    if (!assignable) {
        throw error::AnalysisError{ "left-hand side of assignment statement must be assignable" };
    }

    // Type-check/promote the right-hand side.
    auto target_type = values::type_of(lhs).clone();
    target_type->assignable = false;
    auto rhs_promoted = values::promote(rhs, target_type);
    if (rhs_promoted.empty()) {
        throw error::AnalysisError{ fmt::format(
            "type of right-hand side ({}) could not be coerced to left-hand side ({})",
            values::type_of(rhs), values::type_of(lhs)) };
    }

    // Create the node.
    tree::Maybe<semantic::SetInstruction> node;
    node.emplace<semantic::SetInstruction>(lhs, rhs_promoted);
    return node;
}

/**
 * Analyzes the given cQASM 1.2+ goto instruction.
 * If an error occurs, the message is added to the result error vector,
 * and an empty Maybe is returned.
 */
tree::Maybe<semantic::GotoInstruction> AnalyzerHelper::analyze_goto_instruction(
    const ast::Instruction &insn
) {
    try {
        // Parse the operands.
        if (insn.operands->items.size() != 1) {
            throw error::AnalysisError{ "goto instruction must have a single operand" };
        }
        std::string target;
        if (auto identifier = insn.operands->items[0]->as_identifier()) {
            target = identifier->name;
        } else {
            throw error::AnalysisError{ "goto instruction operand must be a subcircuit identifier" };
        }

        // Create the node.
        tree::Maybe<semantic::GotoInstruction> node;
        node.set(tree::make<semantic::GotoInstruction>());

        // We can't resolve the target subcircuit yet, because goto instructions
        // may refer forward. Instead, we maintain a list of yet-to-be resolved
        // goto instructions.
        gotos.emplace_back(node, target);

        // Resolve the condition code.
        if (!insn.condition.empty()) {
            auto condition_val = analyze_expression(*insn.condition);
            node->condition = values::promote(condition_val, tree::make<types::Bool>());
            if (node->condition.empty()) {
                throw error::AnalysisError{ "condition must be a boolean" };
            }

            // If the condition is constant false, optimize the instruction
            // away.
            if (auto x = node->condition->as_const_bool()) {
                if (!x->value) {
                    return {};
                }
            }
        } else {
            node->condition.set(tree::make<values::ConstBool>(true));
        }

        // Copy annotation data.
        node->annotations = analyze_annotations(insn.annotations);
        node->copy_annotation<parser::SourceLocation>(insn);

        return node;
    } catch (error::AnalysisError &err) {
        err.context(insn);
        result.errors.push_back(std::move(err));
    }
    return {};
}

/**
 * Analyzes the error model meta-instruction and, if valid, adds it to the analysis result.
 * If an error occurs, the message is added to the result error vector, and nothing is added.
 */
void AnalyzerHelper::analyze_error_model(const ast::Instruction &insn) {
    try {
        // Only one error model should be specified, so throw an error
        // if we already have one.
        if (!result.root->error_model.empty()) {
            auto ss = std::ostringstream();
            ss << "error model can only be specified once";
            if (auto loc = result.root->error_model->get_annotation_ptr<parser::SourceLocation>()) {
                ss << ", previous specification was at " << *loc;
            }
            throw error::AnalysisError{ ss.str() };
        }

        // Figure out the name of the error model.
        const auto &arg_exprs = insn.operands->items;
        if (arg_exprs.empty()) {
            throw error::AnalysisError{ "missing error model name" };
        }
        std::string name;
        if (auto name_ident = arg_exprs[0]->as_identifier()) {
            name = name_ident->name;
        } else {
            throw error::AnalysisError{ "first argument of an error model must be its name as an identifier" };
        }

        // Figure out the argument list.
        auto arg_values = values::Values();
        for (auto arg_expr_it = arg_exprs.begin() + 1; arg_expr_it < arg_exprs.end(); arg_expr_it++) {
            arg_values.add(analyze_expression(**arg_expr_it));
        }

        // Resolve the error model to one of the known models if
        // requested. If resolving is disabled, just make a node with
        // the name and values directly (without promotion/implicit
        // casts).
        if (analyzer.resolve_error_model) {
            result.root->error_model.set(
                analyzer.error_models.resolve(
                    name, arg_values));
        } else {
            result.root->error_model.set(
                tree::make<semantic::ErrorModel>(
                    tree::Maybe<error_model::ErrorModel>(),
                    name, arg_values,
                    tree::Any<semantic::AnnotationData>()));
        }

        // Copy annotation data.
        result.root->error_model->annotations = analyze_annotations(insn.annotations);
        result.root->error_model->copy_annotation<parser::SourceLocation>(insn);

    } catch (error::AnalysisError &err) {
        err.context(insn);
        result.errors.push_back(std::move(err));
    }
}

/**
 * Analyzes the given mapping and, if valid, adds it to the current scope.
 * If an error occurs, the message is added to the result error vector, and nothing is added to the scope.
 */
void AnalyzerHelper::analyze_mapping(const ast::Mapping &mapping) {
    try {
        get_current_scope().mappings.add(
            mapping.alias->name,
            analyze_expression(*mapping.expr),
            tree::make<ast::Mapping>(mapping)
        );
    } catch (error::AnalysisError &err) {
        err.context(mapping);
        result.errors.push_back(std::move(err));
    }
}

/**
 * Analyzes the given declaration of one or more variables and, if valid, adds them to the current scope.
 * If an error occurs, the message is added to the result error vector, and nothing is added to the scope.
 */
void AnalyzerHelper::analyze_variables(const ast::Variables &variables) {
    try {
        // Check version compatibility.
        if (result.root->version->items < "1.1") {
            throw error::AnalysisError{ "variables are only supported from cQASM 1.1 onwards" };
        }

        // Figure out what type the variables should have.
        auto type_name = utils::to_lowercase(variables.typ->name);
        types::Type type{};
        if (type_name == types::qubit_type_name) {
            type = tree::make<types::Qubit>();
        } else if (type_name == types::bit_type_name || type_name == types::bool_type_name) {
            type = tree::make<types::Bool>();
        } else if (type_name == types::integer_type_name) {
            type = tree::make<types::Int>();
        } else if (type_name == types::real_type_name) {
            type = tree::make<types::Real>();
        } else if (type_name == types::complex_type_name) {
            type = tree::make<types::Complex>();
        } else {
            throw error::AnalysisError{ fmt::format("unknown type \"{}\"", type_name) };
        }
        type->assignable = true;

        // Construct the variables and add mappings for them.
        for (const auto &identifier : variables.names) {
            // Construct variable.
            // Use the location tag of the identifier to record where the variable was defined.
            auto var = tree::make<semantic::Variable>(identifier->name, type.clone());
            var->copy_annotation<parser::SourceLocation>(*identifier);
            var->annotations = analyze_annotations(variables.annotations);
            result.root->variables.add(var);

            // Add a mapping for the variable.
            get_current_scope().mappings.add(
                identifier->name,
                tree::make<values::VariableRef>(var),
                tree::Maybe<ast::Mapping>()
            );
        }

    } catch (error::AnalysisError &err) {
        err.context(variables);
        result.errors.push_back(std::move(err));
    }
}

/**
 * Analyzes the given subcircuit header and, if valid, adds it to the subcircuit list.
 * If an error occurs, the message is added to the result error vector, and nothing is added to the result.
 */
void AnalyzerHelper::analyze_subcircuit(const ast::Subcircuit &subcircuit) {
    try {
        if (scope_stack.size() > 1) {
            throw error::AnalysisError{ "cannot open subcircuit within subblock" };
        }
        primitives::Int iterations = 1;
        if (!subcircuit.iterations.empty()) {
            iterations = analyze_as_const_int(*subcircuit.iterations);
            if (iterations < 1) {
                throw error::AnalysisError{
                    fmt::format("subcircuit iteration count must be positive, but is {}", iterations),
                    &*subcircuit.iterations };
            }
        }
        auto node = tree::make<semantic::Subcircuit>(
            subcircuit.name->name,
            iterations,
            tree::Any<semantic::Bundle>(),
            analyze_annotations(subcircuit.annotations));
        node->copy_annotation<parser::SourceLocation>(subcircuit);
        if (analyzer.api_version >= "1.2") {
            node->body = tree::make<semantic::Block>();
            node->body->copy_annotation<parser::SourceLocation>(subcircuit);
        }
        result.root->subcircuits.add(node);
    } catch (error::AnalysisError &err) {
        err.context(subcircuit);
        result.errors.push_back(std::move(err));
    }
}

/**
 * Analyzes the given structured control-flow statement and,
 * if valid, adds it to the current scope/block using API version 1.2+.
 * If an error occurs, the message is added to the result error vector,
 * and nothing is added to the block.
 */
void AnalyzerHelper::analyze_structured(const ast::Structured &structured) {
    try {
        tree::Maybe<semantic::Structured> node;

        // Switch based on statement type.
        if (auto if_else = structured.as_if_else()) {
            node = analyze_if_else(*if_else);
        } else if (auto for_loop = structured.as_for_loop()) {
            node = analyze_for_loop(*for_loop);
        } else if (auto foreach_loop = structured.as_foreach_loop()) {
            node = analyze_foreach_loop(*foreach_loop);
        } else if (auto while_loop = structured.as_while_loop()) {
            node = analyze_while_loop(*while_loop);
        } else if (auto repeat_until_loop = structured.as_repeat_until_loop()) {
            node = analyze_repeat_until_loop(*repeat_until_loop);
        } else if (structured.as_break_statement()) {
            // Handle break statement.
            if (!get_current_scope().within_loop) {
                throw error::AnalysisError{ "cannot use break outside of a structured loop" };
            }
            node.emplace<semantic::BreakStatement>();
        } else if (structured.as_continue_statement()) {
            // Handle continue statement.
            if (!get_current_scope().within_loop) {
                throw error::AnalysisError{ "cannot use continue outside of a structured loop" };
            }
            node.emplace<semantic::ContinueStatement>();
        } else {
            throw std::runtime_error("unexpected statement node");
        }

        // Stop if the node was optimized away.
        if (node.empty()) {
            return;
        }

        // Copy annotation data.
        node->annotations = analyze_annotations(structured.annotations);
        node->copy_annotation<parser::SourceLocation>(structured);

        // Add the node to the current block.
        add_to_current_block(node.as<semantic::Statement>());

    } catch (error::AnalysisError &err) {
        err.context(structured);
        result.errors.push_back(std::move(err));
    }
}

/**
 * Analyzes the given if-else chain.
 * Only intended for use as a helper function within analyze_structured().
 */
tree::Maybe<semantic::IfElse> AnalyzerHelper::analyze_if_else(
    const ast::IfElse &if_else
) {
    // Create the if-else node.
    tree::Maybe<semantic::IfElse> node;
    node.emplace();

    // Analyze the branches.
    for (const auto &branch : if_else.branches) {
        // Analyze the condition.
        auto condition = analyze_expression(*branch->condition);
        condition = values::promote(condition, tree::make<types::Bool>());
        if (condition.empty()) {
            throw error::AnalysisError{ "if/else condition must be a boolean" };
        }

        // Analyze the block.
        auto body = analyze_subblock(*branch->body, false);

        // Add the branch.
        node->branches.emplace(condition, body);
    }

    // Analyze the otherwise block, if any.
    if (!if_else.otherwise.empty()) {
        node->otherwise = analyze_subblock(*if_else.otherwise, false);
    }

    // Remove branches that are never taken due to constant-propagated
    // conditions.
    for (size_t idx = 0; idx < node->branches.size(); ) {
        if (auto val = node->branches[idx]->condition->as_const_bool()) {
            if (val->value) {

                // Constant true: optimize away all subsequent branches and
                // replace the otherwise block with this one.
                node->otherwise = node->branches[idx]->body;
                while (node->branches.size() > idx) {
                    node->branches.remove();
                }
            } else {
                // Constant false: remove this condition/block.
                node->branches.remove(static_cast<tree::signed_size_t>(idx));
            }
        } else {
            idx++;
        }
    }

    // If no branches remain, optimize the entire statement away.
    if (node->branches.empty()) {
        if (!node->otherwise.empty()) {
            for (const auto &statement : node->otherwise->statements) {
                add_to_current_block(statement);
            }
        }
        return {};
    }

    return node;
}

/**
 * Analyzes the given C-style for loop.
 * Only intended for use as a helper function within analyze_structured().
 */
tree::Maybe<semantic::ForLoop> AnalyzerHelper::analyze_for_loop(
    const ast::ForLoop &for_loop
) {
    // Create the for-loop node.
    tree::Maybe<semantic::ForLoop> node;
    node.emplace();

    // Analyze the initialization assignment.
    if (!for_loop.initialize.empty()) {
        node->initialize = analyze_set_instruction_operands(
            *for_loop.initialize->lhs,
            *for_loop.initialize->rhs
        );
        node->initialize->condition.emplace<values::ConstBool>(true);
    }

    // Analyze the condition.
    auto condition = analyze_expression(*for_loop.condition);
    node->condition = values::promote(condition, tree::make<types::Bool>());
    if (node->condition.empty()) {
        throw error::AnalysisError{ "loop condition must be a boolean" };
    }

    // Analyze the update assignment.
    if (!for_loop.update.empty()) {
        node->update = analyze_set_instruction_operands(
            *for_loop.update->lhs,
            *for_loop.update->rhs
        );
        node->update->condition.emplace<values::ConstBool>(true);
    }

    // Analyze the body.
    node->body = analyze_subblock(*for_loop.body, true);

    return node;
}

/**
 * Analyzes the given static for loop.
 * Only intended for use as a helper function within analyze_structured().
 */
tree::Maybe<semantic::ForeachLoop> AnalyzerHelper::analyze_foreach_loop(
    const ast::ForeachLoop &foreach_loop
) {
    // Create the foreach loop node.
    tree::Maybe<semantic::ForeachLoop> node;
    node.emplace();

    // Analyze the loop variable.
    node->lhs = values::promote(analyze_expression(*foreach_loop.lhs), tree::make<types::Int>(true));
    if (node->lhs.empty()) {
        throw error::AnalysisError{ "foreach loop variable must be an assignable integer" };
    }

    // Analyze the boundaries.
    node->frm = analyze_as_const_int(*foreach_loop.frm);
    node->to = analyze_as_const_int(*foreach_loop.to);

    // Analyze the body.
    node->body = analyze_subblock(*foreach_loop.body, true);

    return node;
}

/**
 * Analyzes the given while loop.
 * Only intended for use as a helper function within analyze_structured().
 */
tree::Maybe<semantic::WhileLoop> AnalyzerHelper::analyze_while_loop(
    const ast::WhileLoop &while_loop
) {
    // Create the while-loop node.
    tree::Maybe<semantic::WhileLoop> node;
    node.emplace();

    // Analyze the condition.
    auto condition = analyze_expression(*while_loop.condition);
    node->condition = values::promote(condition, tree::make<types::Bool>());
    if (node->condition.empty()) {
        throw error::AnalysisError{ "loop condition must be a boolean" };
    }

    // Analyze the body.
    node->body = analyze_subblock(*while_loop.body, true);

    // If the condition is constant false, optimize away.
    if (auto cond = node->condition->as_const_bool(); cond && !cond->value) {
        return {};
    }

    return node;
}

/**
 * Analyzes the given repeat-until loop.
 * Only intended for use as a helper function within analyze_structured().
 */
tree::Maybe<semantic::RepeatUntilLoop> AnalyzerHelper::analyze_repeat_until_loop(
    const ast::RepeatUntilLoop &repeat_until_loop
) {
    // Create the repeat-until-loop node.
    tree::Maybe<semantic::RepeatUntilLoop> node;
    node.emplace();

    // Analyze the body.
    node->body = analyze_subblock(*repeat_until_loop.body, true);

    // Analyze the condition.
    auto condition = analyze_expression(*repeat_until_loop.condition);
    node->condition = values::promote(condition, tree::make<types::Bool>());
    if (node->condition.empty()) {
        throw error::AnalysisError{ "loop condition must be a boolean" };
    }

    // If the condition is constant true, optimize away.
    if (auto cond = node->condition->as_const_bool()) {
        if (cond->value) {
            for (const auto &statement : node->body->statements) {
                add_to_current_block(statement);
            }
            return {};
        }
    }

    return node;
}

/**
 * Analyzes the given list of annotations. Any errors found result in the
 * annotation being skipped and an error being appended to the result error
 * vector.
 */
tree::Any<semantic::AnnotationData> AnalyzerHelper::analyze_annotations(
    const tree::Any<ast::AnnotationData> &annotations
) {
    auto retval = tree::Any<semantic::AnnotationData>();
    for (const auto &annotation_ast : annotations) {
        try {
            auto annotation = tree::make<semantic::AnnotationData>();
            annotation->interface = annotation_ast->interface->name;
            annotation->operation = annotation_ast->operation->name;
            for (const auto &expression_ast : annotation_ast->operands->items) {
                try {
                    annotation->operands.add(analyze_expression(*expression_ast));
                } catch (error::AnalysisError &err) {
                    err.context(*annotation_ast);
                    result.errors.push_back(std::move(err));
                }
            }
            annotation->copy_annotation<parser::SourceLocation>(*annotation_ast);
            retval.add(annotation);
        } catch (error::AnalysisError &err) {
            err.context(*annotation_ast);
            result.errors.push_back(std::move(err));
        }
    }
    return retval;
}

/**
 * Parses any kind of expression. Always returns a filled value or throws
 * an exception.
 */
values::Value AnalyzerHelper::analyze_expression(const ast::Expression &expression) {
    values::Value retval;
    try {
        if (auto int_lit = expression.as_integer_literal()) {
            retval.set(tree::make<values::ConstInt>(int_lit->value));
        } else if (auto float_lit = expression.as_float_literal()) {
            retval.set(tree::make<values::ConstReal>(float_lit->value));
        } else if (auto string_lit = expression.as_string_literal()) {
            retval.set(tree::make<values::ConstString>(string_lit->value));
        } else if (auto json_lit = expression.as_json_literal()) {
            retval.set(tree::make<values::ConstJson>(json_lit->value));
        } else if (auto matrix_lit = expression.as_matrix_literal()) {
            retval.set(analyze_matrix(*matrix_lit));
        } else if (auto ident = expression.as_identifier()) {
            retval.set(get_current_scope().mappings.resolve(ident->name));
        } else if (auto index = expression.as_index()) {
            retval.set(analyze_index(*index));
        } else if (auto func = expression.as_function_call()) {
            retval.set(analyze_function(func->name->name, *func->arguments));
        } else if (auto negate = expression.as_negate()) {
            retval.set(analyze_operator("-", negate->expr));
        } else if (auto bit_not = expression.as_bitwise_not()) {
            retval.set(analyze_operator("~", bit_not->expr));
        } else if (auto log_not = expression.as_logical_not()) {
            retval.set(analyze_operator("!", log_not->expr));
        } else if (auto power = expression.as_power()) {
            retval.set(analyze_operator("**", power->lhs, power->rhs));
        } else if (auto mult = expression.as_multiply()) {
            retval.set(analyze_operator("*", mult->lhs, mult->rhs));
        } else if (auto div = expression.as_divide()) {
            retval.set(analyze_operator("/", div->lhs, div->rhs));
        } else if (auto idiv = expression.as_int_divide()) {
            retval.set(analyze_operator("//", idiv->lhs, idiv->rhs));
        } else if (auto mod = expression.as_modulo()) {
            retval.set(analyze_operator("%", mod->lhs, mod->rhs));
        } else if (auto add = expression.as_add()) {
            retval.set(analyze_operator("+", add->lhs, add->rhs));
        } else if (auto sub = expression.as_subtract()) {
            retval.set(analyze_operator("-", sub->lhs, sub->rhs));
        } else if (auto shl = expression.as_shift_left()) {
            retval.set(analyze_operator("<<", shl->lhs, shl->rhs));
        } else if (auto sra = expression.as_shift_right_arith()) {
            retval.set(analyze_operator(">>", sra->lhs, sra->rhs));
        } else if (auto srl = expression.as_shift_right_logic()) {
            retval.set(analyze_operator(">>>", srl->lhs, srl->rhs));
        } else if (auto cmpeq = expression.as_cmp_eq()) {
            retval.set(analyze_operator("==", cmpeq->lhs, cmpeq->rhs));
        } else if (auto cmpne = expression.as_cmp_ne()) {
            retval.set(analyze_operator("!=", cmpne->lhs, cmpne->rhs));
        } else if (auto cmpgt = expression.as_cmp_gt()) {
            retval.set(analyze_operator(">", cmpgt->lhs, cmpgt->rhs));
        } else if (auto cmpge = expression.as_cmp_ge()) {
            retval.set(analyze_operator(">=", cmpge->lhs, cmpge->rhs));
        } else if (auto cmplt = expression.as_cmp_lt()) {
            retval.set(analyze_operator("<", cmplt->lhs, cmplt->rhs));
        } else if (auto cmple = expression.as_cmp_le()) {
            retval.set(analyze_operator("<=", cmple->lhs, cmple->rhs));
        } else if (auto band = expression.as_bitwise_and()) {
            retval.set(analyze_operator("&", band->lhs, band->rhs));
        } else if (auto bxor = expression.as_bitwise_xor()) {
            retval.set(analyze_operator("^", bxor->lhs, bxor->rhs));
        } else if (auto bor = expression.as_bitwise_or()) {
            retval.set(analyze_operator("|", bor->lhs, bor->rhs));
        } else if (auto land = expression.as_logical_and()) {
            retval.set(analyze_operator("&&", land->lhs, land->rhs));
        } else if (auto lxor = expression.as_logical_xor()) {
            retval.set(analyze_operator("^^", lxor->lhs, lxor->rhs));
        } else if (auto lor = expression.as_logical_or()) {
            retval.set(analyze_operator("||", lor->lhs, lor->rhs));
        } else if (auto tcond = expression.as_ternary_cond()) {
            retval.set(analyze_operator("?:", tcond->cond, tcond->if_true, tcond->if_false));
        } else {
            throw std::runtime_error("unexpected expression node");
        }
        if ((analyzer.api_version < "1.1") &&
            !retval.empty() &&
            (retval->as_function() || retval->as_variable_ref())) {
            throw error::AnalysisError{ "dynamic expressions are only supported from cQASM 1.1 onwards" };
        }
    } catch (error::AnalysisError &err) {
        err.context(expression);
        throw;
    }
    if (retval.empty()) {
        throw std::runtime_error(
            "analyze_expression returned nonsense, this should never happen");
    }
    retval->copy_annotation<parser::SourceLocation>(expression);
    return retval;
}

/**
 * Shorthand for parsing an expression to a constant integer.
 */
primitives::Int AnalyzerHelper::analyze_as_const_int(const ast::Expression &expression) {
    try {
        auto value = analyze_as<types::Int>(expression);
        if (value.empty()) {
            throw error::AnalysisError{ "expected an integer" };
        }
        if (auto int_value = value->as_const_int()) {
            return int_value->value;
        } else {
            throw error::AnalysisError{ "integer must be constant" };
        }
    } catch (error::AnalysisError &err) {
        err.context(expression);
        throw;
    }
}

/**
 * Parses a matrix. Always returns a filled value or throws an exception.
 */
values::Value AnalyzerHelper::analyze_matrix(const ast::MatrixLiteral &matrix_lit) {
    // Figure out the size of the matrix and parse the subexpressions.
    // Note that the number of rows is always at least 1 (Many vs Any) so the number of cols line is well-behaved.
    size_t num_rows = matrix_lit.rows.size();
    size_t num_cols = matrix_lit.rows[0]->items.size();
    for (const auto &row : matrix_lit.rows) {
        if (row->items.size() != num_cols) {
            throw error::AnalysisError{ "matrix is not rectangular" };
        }
    }
    std::vector<values::Value> vals;
    for (size_t row = 0; row < num_rows; row++) {
        for (size_t col = 0; col < num_cols; col++) {
            vals.push_back(analyze_expression(*matrix_lit.rows[row]->items[col]));
        }
    }

    // Try building a matrix of constant real numbers.
    auto value = analyze_matrix_helper<
        types::Real, values::ConstReal,
        primitives::RMatrix, values::ConstRealMatrix
    >(num_rows, num_cols, vals);
    if (!value.empty()) {
        return value;
    }

    // Try building a matrix of constant complex numbers.
    value = analyze_matrix_helper<
        types::Complex, values::ConstComplex,
        primitives::CMatrix, values::ConstComplexMatrix
    >(num_rows, num_cols, vals);
    if (!value.empty()) {
        return value;
    }

    // Only real and complex are supported right now. If more is to be
    // added in the future, this should probably be written a little
    // neater.
    throw error::AnalysisError{
        "only matrices of constant real or complex numbers are currently supported" };
}

/**
 * Parses an index operator. Always returns a filled value or throws an error.
 */
values::Value AnalyzerHelper::analyze_index(const ast::Index &index) {
    auto expr = analyze_expression(*index.expr);
    if (auto qubit_refs = expr->as_qubit_refs()) {

        // Qubit refs.
        auto indices = analyze_index_list(*index.indices, qubit_refs->index.size());
        for (const auto &idx : indices) {
            idx->value = qubit_refs->index[idx->value]->value;
        }
        return tree::make<values::QubitRefs>(indices);

    } else if (auto bit_refs = expr->as_bit_refs()) {

        // Measurement bit refs.
        auto indices = analyze_index_list(*index.indices, bit_refs->index.size());
        for (const auto &idx : indices) {
            idx->value = bit_refs->index[idx->value]->value;
        }
        return tree::make<values::BitRefs>(indices);

    } else {

        // While matrices could conceivably be indexed, this is not supported right now.
        throw error::AnalysisError{ fmt::format(
            "indexation is not supported for value of type {}", values::type_of(expr)) };
    }
}

/**
 * Parses an index list.
 */
tree::Many<values::ConstInt> AnalyzerHelper::analyze_index_list(const ast::IndexList &index_list, size_t size) {
    tree::Many<values::ConstInt> retval;
    for (const auto &entry : index_list.items) {
        if (auto item = entry->as_index_item()) {

            // Single index.
            auto index = analyze_as_const_int(*item->index);
            if (index < 0 || static_cast<unsigned long>(index) >= size) {
                throw error::AnalysisError{
                    fmt::format("index {} out of range (size {})", index, size), item };
            }
            auto index_val = tree::make<values::ConstInt>(index);
            index_val->copy_annotation<parser::SourceLocation>(*item);
            retval.add(index_val);

        } else if (auto range = entry->as_index_range()) {

            // Range notation.
            auto first = analyze_as_const_int(*range->first);
            if (first < 0 || static_cast<unsigned long>(first) >= size) {
                throw error::AnalysisError{
                    fmt::format("index {} out of range (size {})", first, size), &*range->first };
            }
            auto last = analyze_as_const_int(*range->last);
            if (last < 0 || static_cast<unsigned long>(last) >= size) {
                throw error::AnalysisError{
                        fmt::format("index {} out of range (size {})", last, size), &*range->first };
            }
            if (first > last) {
                throw error::AnalysisError{ "last index is lower than first index", range };
            }
            for (auto index = first; index <= last; index++) {
                auto index_val = tree::make<values::ConstInt>(index);
                index_val->copy_annotation<parser::SourceLocation>(*range);
                retval.add(index_val);
            }

        } else {
            throw std::runtime_error{ "unknown IndexEntry AST node" };
        }
    }
    return retval;
}

/**
 * Parses a function. Always returns a filled value or throws an exception.
 */
values::Value AnalyzerHelper::analyze_function(const ast::Identifier &name, const ast::ExpressionList &args) {
    auto arg_values = values::Values();
    for (const auto &arg : args.items) {
        arg_values.add(analyze_expression(*arg));
    }
    auto retval = get_current_scope().functions.call(name.name, arg_values);
    if (retval.empty()) {
        throw std::runtime_error("function implementation returned empty value");
    }
    return retval;
}

/**
 * Parses an operator. Always returns a filled value or throws an exception.
 */
values::Value AnalyzerHelper::analyze_operator(
    const std::string &name,
    const tree::One<ast::Expression> &a,
    const tree::One<ast::Expression> &b,
    const tree::One<ast::Expression> &c
) {
    auto identifier = ast::Identifier("operator" + name);
    auto args = ast::ExpressionList();
    args.items.add(a);
    args.items.add(b);
    args.items.add(c);
    return analyze_function(identifier, args);
}

} // namespace cqasm::v1x::analyzer
