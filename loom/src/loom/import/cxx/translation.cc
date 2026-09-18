// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/import/cxx/translation.h"

#include <cxx/ast.h>
#include <cxx/ast_interpreter.h>
#include <cxx/attributes.h>
#include <cxx/control.h>
#include <cxx/memory_layout.h>
#include <cxx/names.h>
#include <cxx/preprocessor.h>
#include <cxx/symbols.h>
#include <cxx/token.h>
#include <cxx/translation_unit.h>
#include <cxx/types.h>

#include <array>
#include <bit>
#include <cctype>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "iree/base/api.h"
#include "iree/base/internal/arena.h"
#include "loom/import/cxx/failure.h"
#include "loom/import/cxx/intrinsics.h"
#include "loom/import/cxx/launch.h"
#include "loom/import/cxx/loop_schedule.h"
#include "loom/import/cxx/mutations.h"
#include "loom/import/cxx/source.h"
#include "loom/ir/context.h"
#include "loom/ir/module.h"
#include "loom/ops/buffer/ops.h"
#include "loom/ops/func/ops.h"
#include "loom/ops/index/ops.h"
#include "loom/ops/kernel/ops.h"
#include "loom/ops/scalar/ops.h"
#include "loom/ops/scf/ops.h"
#include "loom/ops/view/ops.h"

namespace loom::cxx_import {
namespace {
bool annotated(cxx::Symbol* symbol, std::string_view spelling) {
  if (!symbol || !symbol->attributes()) {
    return false;
  }
  for (const auto& attribute : *symbol->attributes()) {
    if (attribute.attributeNamespace && attribute.name &&
        attribute.attributeNamespace->name() == "loom" &&
        attribute.name->name() == spelling) {
      return true;
    }
  }
  return false;
}

class Translator {
 public:
  Translator(cxx::TranslationUnit& unit, Diagnostics& diagnostics,
             loom_module_t* module, const loom_cxx_import_options_t& options)
      : unit_(unit),
        diagnostics_(diagnostics),
        intrinsics_(unit, diagnostics),
        launches_(unit, diagnostics),
        module_(module),
        options_(options),
        math_flags_(iree_any_bit_set(options.flags,
                                     LOOM_CXX_IMPORT_FLAG_APPROXIMATE_FUNCTIONS)
                        ? LOOM_SCALAR_FASTMATHFLAGS_AFN
                        : 0) {
    loom_builder_initialize(module_, &module_->arena,
                            loom_module_block(module_), &builder_);
  }

  void translate() {
    auto* root = cxx::ast_cast<cxx::TranslationUnitAST>(unit_.ast());
    if (!root) {
      fail(unit_.ast(), "expected an ordinary translation unit");
    }
    std::vector<cxx::FunctionSymbol*> definitions;
    collect(root->declarationList, definitions);
    if (options_.root_count) {
      std::unordered_map<std::string, std::vector<cxx::FunctionSymbol*>>
          candidates;
      for (auto* symbol : definitions) {
        candidates[qualified_name(symbol)].push_back(symbol);
      }
      for (size_t i = 0; i < options_.root_count; ++i) {
        auto spelling = cxx_import::string(options_.roots[i]);
        auto found = candidates.find(spelling);
        if (found == candidates.end()) {
          fail(root, "root has no concrete definition: " + spelling);
        }
        if (found->second.size() != 1) {
          fail(root, "ambiguous root: " + spelling);
        }
        auto* selected = found->second.front();
        exported_.insert(selected);
        declare(selected);
      }
    } else {
      for (auto* symbol : definitions) {
        bool visible = !symbol->isStatic();
        auto* visibility =
            cxx::attributeArgument(symbol->attributes(), "visibility");
        if (visibility && visibility->name() == "hidden") {
          visible = false;
        }
        for (auto* owner : symbol->enclosingSymbols()) {
          if (auto* space = cxx::symbol_cast<cxx::NamespaceSymbol>(owner)) {
            if (space->parent() && !space->name()) {
              visible = false;
            }
          }
        }
        if (visible) {
          exported_.insert(symbol);
          declare(symbol);
        }
      }
    }
    for (size_t index = 0; index < functions_.size(); ++index) {
      function(functions_[index]);
    }
  }

 private:
  void collect(cxx::List<cxx::DeclarationAST*>* declarations,
               std::vector<cxx::FunctionSymbol*>& definitions) {
    for (auto* declaration : cxx::ListView{declarations}) {
      if (auto* function =
              cxx::ast_cast<cxx::FunctionDefinitionAST>(declaration)) {
        if (!function->symbol->isTemplatePattern()) {
          intrinsics_.declaration(function->symbol, function->attributeList,
                                  function);
          launches_.declaration(function->symbol, function->attributeList);
          definitions.push_back(function->symbol);
        }
      } else if (auto* space =
                     cxx::ast_cast<cxx::NamespaceDefinitionAST>(declaration)) {
        collect(space->declarationList, definitions);
      } else if (auto* linkage =
                     cxx::ast_cast<cxx::LinkageSpecificationAST>(declaration)) {
        collect(linkage->declarationList, definitions);
      } else if (auto* simple =
                     cxx::ast_cast<cxx::SimpleDeclarationAST>(declaration)) {
        for (auto* declarator : cxx::ListView{simple->initDeclaratorList}) {
          if (auto* function =
                  cxx::symbol_cast<cxx::FunctionSymbol>(declarator->symbol)) {
            intrinsics_.declaration(function, simple->attributeList,
                                    declarator);
            launches_.declaration(function, simple->attributeList);
          }
          auto* variable =
              cxx::symbol_cast<cxx::VariableSymbol>(declarator->symbol);
          if (variable && !variable->isExtern() &&
              !(variable->isConstexpr() ||
                (unit_.typeTraits().is_const(variable->type()) &&
                 variable->constValue()))) {
            fail(declarator,
                 "global storage definitions require a global-storage "
                 "projection");
          }
        }
      }
    }
  }

  const std::string& qualified_name(cxx::FunctionSymbol* symbol) {
    auto found = qualified_names_.find(symbol);
    if (found != qualified_names_.end()) {
      return found->second;
    }
    std::string spelling = cxx::to_string(symbol->name());
    for (auto* owner : symbol->enclosingSymbols()) {
      if (auto* space = cxx::symbol_cast<cxx::NamespaceSymbol>(owner)) {
        if (space->name()) {
          spelling = cxx::to_string(space->name()) + "::" + spelling;
        }
      }
    }
    return qualified_names_.emplace(symbol, std::move(spelling)).first->second;
  }

  [[noreturn]] void fail(cxx::AST* ast, const std::string& message) {
    diagnostics_.reject(unit_, ast, message);
  }

  loom_location_id_t location(cxx::AST* ast) {
    if (auto found = locations_.find(ast); found != locations_.end()) {
      return found->second;
    }
    auto first = unit_.tokenStartPosition(ast->firstSourceLocation());
    auto last = unit_.tokenEndPosition(ast->lastSourceLocation().previous());
    // Instantiation can synthesize literals without source tokens. Keep those
    // locations unknown instead of inventing an empty filename at line zero.
    if (first.fileName.empty() || !first.line || !first.column) {
      locations_[ast] = LOOM_LOCATION_UNKNOWN;
      return LOOM_LOCATION_UNKNOWN;
    }
    if (first.line > UINT16_MAX || last.line > UINT16_MAX ||
        first.column > UINT16_MAX || last.column > UINT16_MAX) {
      fail(ast, "source range exceeds Loom's location representation");
    }
    loom_source_id_t source;
    check(loom_module_register_source(
        module_,
        iree_make_string_view(first.fileName.data(), first.fileName.size()),
        &source));
    loom_location_id_t result;
    check(loom_module_add_location(
        module_,
        loom_location_file_range(source, first.line, first.column, last.line,
                                 last.column),
        &result));
    locations_[ast] = result;
    return result;
  }

  const cxx::Type* unqualified(const cxx::Type* type) {
    return unit_.typeTraits().remove_cv(type);
  }

  loom_type_t type(const cxx::Type* input, cxx::AST* ast) {
    if (!input) {
      fail(ast, "expression has no resolved C++ type");
    }
    if (unit_.typeTraits().is_volatile(input)) {
      fail(ast, "volatile access is not supported");
    }
    switch (unqualified(input)->kind()) {
      case cxx::TypeKind::kBool:
        return loom_type_scalar(LOOM_SCALAR_TYPE_I1);
      case cxx::TypeKind::kInt:
      case cxx::TypeKind::kUnsignedInt:
        return loom_type_scalar(LOOM_SCALAR_TYPE_I32);
      case cxx::TypeKind::kChar:
      case cxx::TypeKind::kSignedChar:
      case cxx::TypeKind::kUnsignedChar:
        return loom_type_scalar(LOOM_SCALAR_TYPE_I8);
      case cxx::TypeKind::kShortInt:
      case cxx::TypeKind::kUnsignedShortInt:
        return loom_type_scalar(LOOM_SCALAR_TYPE_I16);
      case cxx::TypeKind::kLongInt:
      case cxx::TypeKind::kUnsignedLongInt:
        return loom_type_scalar(unit_.control()->memoryLayout()->sizeOfLong() ==
                                        4
                                    ? LOOM_SCALAR_TYPE_I32
                                    : LOOM_SCALAR_TYPE_I64);
      case cxx::TypeKind::kLongLongInt:
      case cxx::TypeKind::kUnsignedLongLongInt:
        return loom_type_scalar(LOOM_SCALAR_TYPE_I64);
      case cxx::TypeKind::kFloat:
        return loom_type_scalar(LOOM_SCALAR_TYPE_F32);
      case cxx::TypeKind::kDouble:
        return loom_type_scalar(LOOM_SCALAR_TYPE_F64);
      case cxx::TypeKind::kFloat16:
        return loom_type_scalar(LOOM_SCALAR_TYPE_F16);
      case cxx::TypeKind::kBoundedArray: {
        auto* array = cxx::type_cast<cxx::BoundedArrayType>(unqualified(input));
        auto element = type(array->elementType(), ast);
        if (loom_type_kind(element) != LOOM_TYPE_SCALAR ||
            loom_type_element_type(element) == LOOM_SCALAR_TYPE_I1) {
          fail(ast, "arrays require a supported non-boolean scalar element");
        }
        return loom_type_buffer();
      }
      case cxx::TypeKind::kPointer: {
        auto* pointer = cxx::type_cast<cxx::PointerType>(unqualified(input));
        auto element = type(pointer->elementType(), ast);
        if (loom_type_kind(element) != LOOM_TYPE_SCALAR ||
            loom_type_element_type(element) == LOOM_SCALAR_TYPE_I1) {
          fail(ast, "pointers require a supported non-boolean scalar element");
        }
        return loom_type_buffer();
      }
      default:
        fail(ast, "unsupported C++ type: " + cxx::to_string(input));
    }
  }

  bool is_unsigned(const cxx::Type* type) {
    return unit_.typeTraits().is_unsigned(type);
  }

  bool is_float(const cxx::Type* input) {
    auto kind = unqualified(input)->kind();
    return kind == cxx::TypeKind::kFloat || kind == cxx::TypeKind::kFloat16 ||
           kind == cxx::TypeKind::kDouble;
  }

  loom_value_id_t convert(cxx::ExpressionAST* input_ast,
                          const cxx::Type* output_type, cxx::AST* owner) {
    return convert_value(expression(input_ast), input_ast->type, output_type,
                         owner);
  }

  loom_value_id_t convert_value(loom_value_id_t value,
                                const cxx::Type* input_type,
                                const cxx::Type* output_type, cxx::AST* owner) {
    auto input = type(input_type, owner);
    auto output = type(output_type, owner);
    if (loom_type_equal(input, output)) {
      return value;
    }
    if (loom_type_kind(input) != LOOM_TYPE_SCALAR ||
        loom_type_kind(output) != LOOM_TYPE_SCALAR) {
      fail(owner, "conversion must preserve pointer/array representation");
    }
    loom_op_t* op;
    if (loom_type_element_type(output) == LOOM_SCALAR_TYPE_I1) {
      loom_op_t* zero;
      check(loom_scalar_constant_build(
          &builder_,
          is_float(input_type) ? loom_attr_f64(0.0) : loom_attr_i64(0), input,
          location(owner), &zero));
      if (is_float(input_type)) {
        check(loom_scalar_cmpf_build(&builder_, 0,
                                     LOOM_SCALAR_CMPF_PREDICATE_UNE, value,
                                     result(zero), location(owner), &op));
      } else {
        check(loom_scalar_cmpi_build(&builder_, LOOM_SCALAR_CMPI_PREDICATE_NE,
                                     value, result(zero), location(owner),
                                     &op));
      }
      return result(op);
    }
    bool unsigned_input = is_unsigned(input_type) ||
                          loom_type_element_type(input) == LOOM_SCALAR_TYPE_I1;
    auto* layout = unit_.control()->memoryLayout();
    bool narrows = layout->sizeOf(input_type) > layout->sizeOf(output_type);
    auto build =
        is_float(output_type)
            ? (is_float(input_type)
                   ? (narrows ? loom_scalar_fptrunc_build
                              : loom_scalar_extf_build)
                   : (unsigned_input ? loom_scalar_uitofp_build
                                     : loom_scalar_sitofp_build))
            : (is_float(input_type)
                   ? (is_unsigned(output_type) ? loom_scalar_fptoui_build
                                               : loom_scalar_fptosi_build)
                   : (narrows ? loom_scalar_trunci_build
                              : (unsigned_input ? loom_scalar_extui_build
                                                : loom_scalar_extsi_build)));
    check(build(&builder_, value, input, output, location(owner), &op));
    return result(op);
  }

  loom_value_id_t binary_value(cxx::TokenKind token, loom_value_id_t left,
                               loom_value_id_t right,
                               const cxx::Type* input_type,
                               const cxx::Type* output_type, cxx::AST* ast) {
    auto source = location(ast);
    bool floating = is_float(input_type);
    bool unsigned_input = is_unsigned(input_type);
    loom_op_t* op;
    int predicate = -1;
    switch (token) {
      case cxx::TokenKind::T_EQUAL_EQUAL:
        predicate = 0;
        break;
      case cxx::TokenKind::T_EXCLAIM_EQUAL:
        predicate = 1;
        break;
      case cxx::TokenKind::T_LESS:
        predicate = 2;
        break;
      case cxx::TokenKind::T_LESS_EQUAL:
        predicate = 3;
        break;
      case cxx::TokenKind::T_GREATER:
        predicate = 4;
        break;
      case cxx::TokenKind::T_GREATER_EQUAL:
        predicate = 5;
        break;
      default:
        break;
    }
    if (predicate >= 0) {
      if (floating) {
        const loom_scalar_cmpf_predicate_t predicates[] = {
            LOOM_SCALAR_CMPF_PREDICATE_OEQ, LOOM_SCALAR_CMPF_PREDICATE_UNE,
            LOOM_SCALAR_CMPF_PREDICATE_OLT, LOOM_SCALAR_CMPF_PREDICATE_OLE,
            LOOM_SCALAR_CMPF_PREDICATE_OGT, LOOM_SCALAR_CMPF_PREDICATE_OGE};
        check(loom_scalar_cmpf_build(&builder_, 0, predicates[predicate], left,
                                     right, source, &op));
      } else {
        auto selected = static_cast<loom_scalar_cmpi_predicate_t>(
            predicate >= 2 && unsigned_input ? predicate + 4 : predicate);
        check(loom_scalar_cmpi_build(&builder_, selected, left, right, source,
                                     &op));
      }
      return result(op);
    }
    if (!floating) {
      auto build = token == cxx::TokenKind::T_SLASH
                       ? (unsigned_input ? loom_scalar_divui_build
                                         : loom_scalar_divsi_build)
                   : token == cxx::TokenKind::T_PERCENT
                       ? (unsigned_input ? loom_scalar_remui_build
                                         : loom_scalar_remsi_build)
                   : token == cxx::TokenKind::T_AMP   ? loom_scalar_andi_build
                   : token == cxx::TokenKind::T_BAR   ? loom_scalar_ori_build
                   : token == cxx::TokenKind::T_CARET ? loom_scalar_xori_build
                   : token == cxx::TokenKind::T_GREATER_GREATER
                       ? (unsigned_input ? loom_scalar_shrui_build
                                         : loom_scalar_shrsi_build)
                       : nullptr;
      if (build) {
        check(
            build(&builder_, left, right, type(output_type, ast), source, &op));
        return result(op);
      }
    }
    auto build =
        token == cxx::TokenKind::T_PLUS
            ? (floating ? loom_scalar_addf_build : loom_scalar_addi_build)
        : token == cxx::TokenKind::T_MINUS
            ? (floating ? loom_scalar_subf_build : loom_scalar_subi_build)
        : token == cxx::TokenKind::T_STAR
            ? (floating ? loom_scalar_mulf_build : loom_scalar_muli_build)
        : token == cxx::TokenKind::T_SLASH && floating ? loom_scalar_divf_build
        : token == cxx::TokenKind::T_LESS_LESS && !floating
            ? loom_scalar_shli_build
            : nullptr;
    if (!build) {
      fail(ast, "unsupported binary operator");
    }
    check(
        build(&builder_, 0, left, right, type(output_type, ast), source, &op));
    return result(op);
  }

  loom_type_t value_type(loom_value_id_t value) {
    return loom_module_value_type(module_, value);
  }

  loom_string_id_t string(const std::string& value) {
    loom_string_id_t result;
    check(loom_builder_intern_string(
        &builder_, iree_make_cstring_view(value.c_str()), &result));
    return result;
  }

  loom_value_id_t name(loom_value_id_t value, const std::string& hint) {
    // A C++ alias of an existing SSA value keeps the original value's name.
    if (loom_module_value(module_, value)->name_id == LOOM_STRING_ID_INVALID) {
      check(loom_module_set_value_name(module_, value, string(hint)));
    }
    return value;
  }

  loom_value_id_t result(loom_op_t* op, const std::string& hint = "") {
    auto value = loom_op_results(op)[0];
    return hint.empty() ? value : name(value, hint);
  }

  loom_value_id_t constant(int64_t value, loom_scalar_type_t scalar,
                           loom_location_id_t source = LOOM_LOCATION_UNKNOWN) {
    loom_op_t* op;
    auto* build =
        scalar == LOOM_SCALAR_TYPE_INDEX || scalar == LOOM_SCALAR_TYPE_OFFSET
            ? loom_index_constant_build
            : loom_scalar_constant_build;
    check(build(&builder_, loom_attr_i64(value), loom_type_scalar(scalar),
                source, &op));
    return result(op);
  }

  loom_symbol_ref_t declare(cxx::FunctionSymbol* function) {
    if (auto found = callees_.find(function); found != callees_.end()) {
      return found->second;
    }
    if (!function->templateArguments().empty() && function->declaration()) {
      launches_.declaration(function, function->declaration()->attributeList);
    }
    std::string spelling = qualified_name(function);
    for (const auto& argument : function->templateArguments()) {
      spelling += "_" + cxx::to_string(argument);
    }
    for (char& ch : spelling) {
      if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '_') {
        ch = '_';
      }
    }
    auto ordinal = symbol_names_[spelling]++;
    if (ordinal) {
      spelling += "_" + std::to_string(ordinal);
    }
    loom_symbol_id_t id;
    check(loom_module_add_symbol(module_, string(spelling), &id));
    loom_symbol_ref_t callee = {0, id};
    callees_[function] = callee;
    functions_.push_back(function);
    return callee;
  }

  void function(cxx::FunctionSymbol* symbol) {
    auto* definition = symbol->declaration();
    if (!definition) {
      fail(unit_.ast(), "reachable function has no definition");
    }
    auto* body = cxx::ast_cast<cxx::CompoundStatementFunctionBodyAST>(
        definition->functionBody);
    if (!body) {
      fail(definition, "unsupported function body");
    }
    mutations_.accept(body->statement);
    auto parameters = symbol->parameters();
    std::vector<loom_type_t> arguments;
    for (auto* parameter : parameters) {
      arguments.push_back(type(parameter->type(), definition));
    }
    auto* signature = cxx::type_cast<cxx::FunctionType>(symbol->type());
    if (!signature || signature->isVariadic()) {
      fail(definition, "variadic functions are not admitted");
    }
    bool returns_void = signature->returnType()->kind() == cxx::TypeKind::kVoid;
    kernel_ = annotated(symbol, "kernel");
    loom_op_t* op;
    if (kernel_) {
      if (!returns_void) {
        fail(definition, "kernel must return void");
      }
      check(loom_kernel_def_build(&builder_, 0, 0, {}, 0, 0,
                                  callees_.at(symbol), nullptr, 0,
                                  arguments.data(), arguments.size(), nullptr,
                                  0, location(definition), &op));
      auto saved =
          loom_builder_enter_region(&builder_, op, loom_kernel_def_config(op));
      auto name_id =
          module_->symbols.entries[callees_.at(symbol).symbol_id].name_id;
      auto spelling = module_->strings.entries[name_id];
      launches_.build(symbol, {spelling.data, spelling.size}, &builder_,
                      location(definition));
      loom_builder_restore(&builder_, saved);
    } else {
      launches_.reject_ordinary_function(symbol);
      std::vector<loom_type_t> results;
      if (!returns_void) {
        results.push_back(type(signature->returnType(), definition));
      }
      check(loom_func_def_build(
          &builder_,
          (annotated(symbol, "device") ? LOOM_FUNC_DEF_BUILD_FLAG_HAS_CC : 0) |
              (exported_.contains(symbol)
                   ? LOOM_FUNC_DEF_BUILD_FLAG_HAS_VISIBILITY
                   : 0) |
              (annotated(symbol, "force_inline")
                   ? LOOM_FUNC_DEF_BUILD_FLAG_HAS_INLINE_POLICY
                   : 0),
          exported_.contains(symbol) ? LOOM_FUNC_VISIBILITY_PUBLIC : 0, 0,
          annotated(symbol, "device") ? LOOM_FUNC_CC_DEVICE : 0, 0, 0,
          annotated(symbol, "force_inline") ? LOOM_INLINE_POLICY_INLINE : 0, {},
          0, {}, 0, {}, callees_.at(symbol), arguments.data(), arguments.size(),
          results.data(), results.size(), nullptr, 0, nullptr, 0,
          location(definition), &op));
    }
    auto* region = kernel_ ? loom_kernel_def_body(op) : loom_func_def_body(op);
    auto saved = loom_builder_enter_region(&builder_, op, region);
    values_.clear();
    for (size_t index = 0; index < parameters.size(); ++index) {
      values_[parameters[index]] =
          name(loom_region_entry_arg_id(region, index),
               cxx::to_string(parameters[index]->name()));
    }
    bool returned = false;
    for (auto* child : cxx::ListView{body->statement->statementList}) {
      if (returned) {
        fail(child, "statements after return are not supported");
      }
      if (auto* ret = cxx::ast_cast<cxx::ReturnStatementAST>(child)) {
        std::vector<loom_value_id_t> returns;
        if (ret->expression) {
          returns.push_back(expression(ret->expression));
        }
        loom_op_t* terminator;
        if (kernel_) {
          check(
              loom_kernel_return_build(&builder_, location(ret), &terminator));
        } else {
          check(loom_func_return_build(&builder_, returns.data(),
                                       returns.size(), location(ret),
                                       &terminator));
        }
        returned = true;
      } else {
        statement(child);
      }
    }
    if (!returned) {
      if (!returns_void) {
        fail(definition, "non-void helper needs a final return");
      }
      loom_op_t* terminator;
      if (kernel_) {
        check(loom_kernel_return_build(&builder_, location(definition),
                                       &terminator));
      } else {
        check(loom_func_return_build(&builder_, nullptr, 0,
                                     location(definition), &terminator));
      }
    }
    loom_builder_restore(&builder_, saved);
  }

  struct Access {
    // Typed view containing this memory access.
    loom_value_id_t view;
    // Full-rank dynamic index for a known array, absent for a scalar
    // projection.
    std::optional<loom_value_id_t> index;
  };

  Access address(cxx::SubscriptExpressionAST* ast) {
    if (ast->symbol) {
      fail(ast, "overloaded indexing is not admitted");
    }
    auto* pointer = cxx::type_cast<cxx::PointerType>(
        unqualified(ast->baseExpression->type));
    auto* array = cxx::type_cast<cxx::BoundedArrayType>(
        unqualified(ast->baseExpression->type));
    if (!pointer && !array) {
      fail(ast, "subscript base must be a scalar pointer or shared array");
    }
    if (unqualified(ast->indexExpression->type)->kind() !=
        cxx::TypeKind::kUnsignedInt) {
      fail(ast, "pointer subscripts require unsigned int offsets");
    }
    auto root = expression(ast->baseExpression);
    auto index = expression(ast->indexExpression);
    auto* element_type =
        pointer ? pointer->elementType() : array->elementType();
    auto element = type(element_type, ast);
    auto bytes = unit_.control()->memoryLayout()->sizeOf(element_type);
    if (!bytes) {
      fail(ast, "unknown element size");
    }
    auto offset_type = loom_type_scalar(LOOM_SCALAR_TYPE_OFFSET);
    auto index_type = loom_type_scalar(LOOM_SCALAR_TYPE_INDEX);
    loom_op_t* cast;
    // Enter offset first: fixed-width inputs zero-extend there, whereas a
    // direct i32-to-index conversion would interpret unsigned C++ bits as
    // signed.
    check(loom_index_cast_build(&builder_, index, value_type(index),
                                offset_type, location(ast), &cast));
    auto wide_index = result(cast);
    check(loom_index_cast_build(&builder_, wide_index, offset_type, index_type,
                                location(ast), &cast));
    if (array) {
      return {array_views_.at(root), result(cast)};
    }
    auto size = constant(*bytes, LOOM_SCALAR_TYPE_OFFSET, location(ast));
    loom_op_t* offset;
    check(loom_index_scale_build(&builder_, result(cast), size, offset_type,
                                 location(ast), &offset));
    loom_op_t* view;
    auto view_type = loom_type_shaped_1d(LOOM_TYPE_VIEW,
                                         loom_type_element_type(element), 1, 0);
    check(loom_buffer_view_build(&builder_, root, result(offset), view_type,
                                 location(ast), &view));
    return {result(view), std::nullopt};
  }

  loom_value_id_t constant_value(const cxx::ConstValue& value,
                                 cxx::ExpressionAST* ast) {
    cxx::ASTInterpreter interpreter(&unit_);
    auto source = location(ast);
    auto target = type(ast->type, ast);
    loom_attribute_t attribute;
    if (is_float(ast->type)) {
      auto number = interpreter.toDouble(value);
      if (!number) {
        fail(ast, "invalid floating literal");
      }
      attribute = loom_attr_f64(*number);
    } else {
      auto number = interpreter.toInt(value);
      if (!number) {
        fail(ast, "invalid integer literal");
      }
      // Literals are represented in the signed storage width of their IR
      // scalar type; signedness remains a source fact at each operation.
      auto bytes = *unit_.control()->memoryLayout()->sizeOf(ast->type);
      int64_t stored =
          bytes == 1   ? std::bit_cast<int8_t>(static_cast<uint8_t>(*number))
          : bytes == 2 ? std::bit_cast<int16_t>(static_cast<uint16_t>(*number))
          : bytes == 4 ? std::bit_cast<int32_t>(static_cast<uint32_t>(*number))
                       : *number;
      attribute = loom_attr_i64(stored);
    }
    loom_op_t* op;
    check(
        loom_scalar_constant_build(&builder_, attribute, target, source, &op));
    return result(op);
  }

  loom_value_id_t expression(cxx::ExpressionAST* ast) {
    if (!ast) {
      throw std::runtime_error("missing expression");
    }
    auto source = location(ast);
    if (auto* constant = cxx::ast_cast<cxx::ConstExpressionAST>(ast)) {
      return constant_value(*constant->constValue, ast);
    }
    if (auto* nested = cxx::ast_cast<cxx::NestedExpressionAST>(ast)) {
      return expression(nested->expression);
    }
    if (auto* equal = cxx::ast_cast<cxx::EqualInitializerAST>(ast)) {
      return expression(equal->expression);
    }
    if (auto* initializer =
            cxx::ast_cast<cxx::DefaultInitializerExpressionAST>(ast)) {
      return expression(initializer->expression);
    }
    if (auto* cast = cxx::ast_cast<cxx::ImplicitCastExpressionAST>(ast)) {
      if (cast->conversionFunction) {
        fail(ast, "user-defined conversions are not admitted");
      }
      return convert(cast->expression, cast->type, ast);
    }
    if (auto* cast = cxx::ast_cast<cxx::BuiltinBitCastExpressionAST>(ast)) {
      auto input = type(cast->expression->type, ast);
      auto output = type(cast->type, ast);
      if (loom_type_kind(input) != LOOM_TYPE_SCALAR ||
          loom_type_kind(output) != LOOM_TYPE_SCALAR ||
          unit_.control()->memoryLayout()->sizeOf(cast->expression->type) !=
              unit_.control()->memoryLayout()->sizeOf(cast->type)) {
        fail(ast, "bit_cast requires equal-width supported scalar types");
      }
      auto value = expression(cast->expression);
      loom_op_t* op;
      check(loom_scalar_bitcast_build(&builder_, value, input, output, source,
                                      &op));
      return result(op);
    }
    if (auto* cast = cxx::ast_cast<cxx::CastExpressionAST>(ast)) {
      if (loom_type_kind(type(cast->type, ast)) != LOOM_TYPE_SCALAR ||
          loom_type_kind(type(cast->expression->type, ast)) !=
              LOOM_TYPE_SCALAR) {
        fail(ast, "explicit casts are restricted to numeric scalar values");
      }
      return convert(cast->expression, cast->type, ast);
    }
    if (auto* cast = cxx::ast_cast<cxx::CppCastExpressionAST>(ast)) {
      if (cast->castOp != cxx::TokenKind::T_STATIC_CAST ||
          loom_type_kind(type(cast->type, ast)) != LOOM_TYPE_SCALAR ||
          loom_type_kind(type(cast->expression->type, ast)) !=
              LOOM_TYPE_SCALAR) {
        fail(ast, "only numeric static_cast is admitted");
      }
      return convert(cast->expression, cast->type, ast);
    }
    if (auto* cast = cxx::ast_cast<cxx::TypeConstructionAST>(ast)) {
      auto output = type(ast->type, ast);
      if (cast->constructorSymbol ||
          loom_type_kind(output) != LOOM_TYPE_SCALAR ||
          (cast->expressionList && cast->expressionList->next)) {
        fail(ast, "functional casts require a scalar and at most one argument");
      }
      if (cast->expressionList) {
        return convert(cast->expressionList->value, ast->type, ast);
      }
      loom_op_t* op;
      check(loom_scalar_constant_build(
          &builder_,
          is_float(ast->type) ? loom_attr_f64(0.0) : loom_attr_i64(0), output,
          source, &op));
      return result(op);
    }
    if (auto* select = cxx::ast_cast<cxx::ConditionalExpressionAST>(ast)) {
      auto output = type(select->type, ast);
      if (loom_type_kind(output) != LOOM_TYPE_SCALAR) {
        fail(ast, "conditional expressions require scalar results");
      }
      auto condition = expression(select->condition);
      loom_op_t* op;
      check(loom_scf_if_build(&builder_, LOOM_SCF_IF_BUILD_FLAG_HAS_ELSE_REGION,
                              condition, &output, 1, nullptr, 0, source, &op));
      auto saved =
          loom_builder_enter_region(&builder_, op, loom_scf_if_then_region(op));
      auto value = expression(select->iftrueExpression);
      loom_op_t* yield;
      check(loom_scf_yield_build(&builder_, &value, 1, source, &yield));
      loom_builder_restore(&builder_, saved);
      saved =
          loom_builder_enter_region(&builder_, op, loom_scf_if_else_region(op));
      value = expression(select->iffalseExpression);
      check(loom_scf_yield_build(&builder_, &value, 1, source, &yield));
      loom_builder_restore(&builder_, saved);
      return result(op);
    }
    if (auto* id = cxx::ast_cast<cxx::IdExpressionAST>(ast)) {
      if (auto found = values_.find(id->symbol); found != values_.end()) {
        return found->second;
      }
      if (auto* variable = cxx::symbol_cast<cxx::VariableSymbol>(id->symbol)) {
        if (variable->constValue() &&
            (variable->isConstexpr() ||
             unit_.typeTraits().is_const(variable->type()))) {
          return name(constant_value(*variable->constValue(), ast),
                      cxx::to_string(variable->name()));
        }
      }
      fail(ast, "unbound source value: " +
                    cxx::to_string(id->symbol ? id->symbol->name() : nullptr));
    }
    if (auto* literal = cxx::ast_cast<cxx::BoolLiteralExpressionAST>(ast)) {
      return constant(literal->isTrue, LOOM_SCALAR_TYPE_I1, source);
    }
    if (cxx::ast_cast<cxx::IntLiteralExpressionAST>(ast) ||
        cxx::ast_cast<cxx::FloatLiteralExpressionAST>(ast)) {
      cxx::ASTInterpreter interpreter(&unit_);
      auto value = interpreter.evaluate(ast);
      if (!value) {
        fail(ast, "literal has no constant value");
      }
      return constant_value(*value, ast);
    }
    if (auto* member = cxx::ast_cast<cxx::MemberExpressionAST>(ast)) {
      auto* base = cxx::ast_cast<cxx::IdExpressionAST>(member->baseExpression);
      if (!base || !member->symbol ||
          member->accessOp != cxx::TokenKind::T_DOT) {
        fail(ast, "only topology member access is admitted");
      }
      auto axis = cxx::to_string(member->symbol->name());
      loom_kernel_dimension_t dimension;
      if (axis == "x") {
        dimension = LOOM_KERNEL_DIMENSION_X;
      } else if (axis == "y") {
        dimension = LOOM_KERNEL_DIMENSION_Y;
      } else if (axis == "z") {
        dimension = LOOM_KERNEL_DIMENSION_Z;
      } else {
        fail(ast, "unknown topology axis");
      }
      auto build = annotated(base->symbol, "workitem_id")
                       ? loom_kernel_workitem_id_build
                   : annotated(base->symbol, "workgroup_id")
                       ? loom_kernel_workgroup_id_build
                   : annotated(base->symbol, "workgroup_size")
                       ? loom_kernel_workgroup_size_build
                   : annotated(base->symbol, "workgroup_count")
                       ? loom_kernel_workgroup_count_build
                       : nullptr;
      if (!build) {
        fail(ast, "member base is not an owned topology intrinsic");
      }
      loom_op_t* op;
      auto coordinate = loom_type_scalar(LOOM_SCALAR_TYPE_INDEX);
      check(build(&builder_, dimension, coordinate, source, &op));
      auto value =
          name(result(op), cxx::to_string(base->symbol->name()) + "_" + axis);
      check(loom_index_cast_build(&builder_, value, coordinate,
                                  type(ast->type, ast), source, &op));
      return result(op);
    }
    if (auto* binary = cxx::ast_cast<cxx::BinaryExpressionAST>(ast)) {
      if (binary->symbol) {
        fail(ast, "overloaded arithmetic is not admitted");
      }
      auto left = expression(binary->leftExpression);
      auto right = expression(binary->rightExpression);
      // C++ promotes shift operands independently. Loom's shift operands have
      // one width. Every defined source shift count fits the promoted left
      // width; normalize that count without changing any defined execution.
      if (binary->op == cxx::TokenKind::T_LESS_LESS ||
          binary->op == cxx::TokenKind::T_GREATER_GREATER) {
        right = convert_value(right, binary->rightExpression->type,
                              binary->leftExpression->type, ast);
      }
      return binary_value(binary->op, left, right, binary->leftExpression->type,
                          ast->type, ast);
    }
    if (auto* unary = cxx::ast_cast<cxx::UnaryExpressionAST>(ast)) {
      if (unary->symbol) {
        fail(ast, "overloaded unary operations are not supported");
      }
      auto value = expression(unary->expression);
      if (unary->op == cxx::TokenKind::T_PLUS) {
        return value;
      }
      loom_op_t* op;
      auto output = type(ast->type, ast);
      if (unary->op == cxx::TokenKind::T_MINUS) {
        if (is_float(ast->type)) {
          check(
              loom_scalar_negf_build(&builder_, 0, value, output, source, &op));
        } else {
          auto zero = constant(0, loom_type_element_type(output), source);
          check(loom_scalar_subi_build(&builder_, 0, zero, value, output,
                                       source, &op));
        }
        return result(op);
      }
      if (unary->op == cxx::TokenKind::T_TILDE ||
          unary->op == cxx::TokenKind::T_EXCLAIM) {
        value = convert_value(value, unary->expression->type, ast->type, ast);
        auto mask = constant(unary->op == cxx::TokenKind::T_EXCLAIM ? 1 : -1,
                             loom_type_element_type(output), source);
        check(loom_scalar_xori_build(&builder_, value, mask, output, source,
                                     &op));
        return result(op);
      }
      fail(ast, "unsupported unary value expression");
    }
    if (auto* subscript = cxx::ast_cast<cxx::SubscriptExpressionAST>(ast)) {
      auto access = address(subscript);
      loom_op_t* op;
      int64_t selector = access.index ? INT64_MIN : 0;
      check(loom_view_load_build(&builder_, 0, 0, access.view,
                                 access.index ? &*access.index : nullptr,
                                 access.index ? 1 : 0, &selector, 1, 0, 0,
                                 type(ast->type, ast), source, &op));
      return result(op);
    }
    if (auto* call = cxx::ast_cast<cxx::CallExpressionAST>(ast)) {
      auto* callee = cxx::ast_cast<cxx::IdExpressionAST>(call->baseExpression);
      auto* function =
          callee ? cxx::symbol_cast<cxx::FunctionSymbol>(callee->symbol)
                 : nullptr;
      if (!function) {
        fail(ast, "call must resolve to a function symbol");
      }
      std::vector<loom_value_id_t> arguments;
      for (auto* argument : cxx::ListView{call->expressionList}) {
        arguments.push_back(expression(argument));
      }
      auto result_type = type(ast->type, ast);
      loom_op_t* op;
      if (annotated(function, "subgroup_size")) {
        if (!arguments.empty() ||
            !loom_type_equal(result_type,
                             loom_type_scalar(LOOM_SCALAR_TYPE_I32))) {
          fail(ast, "subgroup_size requires unsigned subgroup_size()");
        }
        auto index_type = loom_type_scalar(LOOM_SCALAR_TYPE_INDEX);
        check(loom_kernel_subgroup_size_build(&builder_, index_type, source,
                                              &op));
        auto size = result(op);
        check(loom_index_cast_build(&builder_, size, index_type, result_type,
                                    source, &op));
        return result(op);
      }
      if (annotated(function, "shuffle_xor") && arguments.size() == 3) {
        check(loom_kernel_subgroup_shuffle_build(
            &builder_, LOOM_KERNEL_SUBGROUP_SHUFFLE_MODE_XOR, arguments[0],
            arguments[1], arguments[2], result_type, source, &op));
        return result(op);
      }
      if (auto value = intrinsics_.call(function, arguments, math_flags_,
                                        &builder_, source)) {
        return *value;
      }
      if (!function->declaration()) {
        fail(
            ast,
            "call must resolve to an owned intrinsic or defined device helper");
      }
      auto symbol = declare(function);
      check(loom_func_call_build(&builder_, 0, 0, 0, 0, symbol,
                                 arguments.data(), arguments.size(),
                                 &result_type, 1, nullptr, 0, source, &op));
      return result(op);
    }
    fail(ast,
         "unsupported expression: " + std::string(cxx::to_string(ast->kind())));
  }

  void statement(cxx::StatementAST* ast) {
    if (auto* compound = cxx::ast_cast<cxx::CompoundStatementAST>(ast)) {
      for (auto* child : cxx::ListView{compound->statementList}) {
        statement(child);
      }
      return;
    }
    if (auto* declaration = cxx::ast_cast<cxx::DeclarationStatementAST>(ast)) {
      if (cxx::ast_cast<cxx::EmptyDeclarationAST>(declaration->declaration)) {
        return;
      }
      auto* simple =
          cxx::ast_cast<cxx::SimpleDeclarationAST>(declaration->declaration);
      if (!simple) {
        fail(ast, "unsupported local declaration");
      }
      for (auto* variable : cxx::ListView{simple->initDeclaratorList}) {
        auto* source_variable =
            cxx::symbol_cast<cxx::VariableSymbol>(variable->symbol);
        if (!source_variable || source_variable->isStatic() ||
            source_variable->isExtern() || source_variable->isThreadLocal()) {
          fail(ast, "local storage duration must be automatic or __shared__");
        }
        if (variable->symbol && annotated(variable->symbol, "workgroup")) {
          auto* array = cxx::type_cast<cxx::BoundedArrayType>(
              unqualified(variable->symbol->type()));
          if (!array || variable->initializer || !kernel_) {
            fail(ast,
                 "shared storage must be an uninitialized fixed scalar array "
                 "in the kernel");
          }
          type(array, variable);
          auto* layout = unit_.control()->memoryLayout();
          auto bytes = layout->sizeOf(array);
          auto alignment = layout->alignmentOf(array);
          if (!bytes || !alignment) {
            fail(ast, "unknown shared array layout");
          }
          auto length =
              constant(*bytes, LOOM_SCALAR_TYPE_OFFSET, location(variable));
          loom_op_t* op;
          check(loom_buffer_alloca_build(
              &builder_, LOOM_VALUE_FACT_MEMORY_SPACE_WORKGROUP,
              std::max<int64_t>(*alignment,
                                source_variable->explicitAlignment()),
              length, loom_type_buffer(), location(variable), &op));
          auto root =
              name(result(op), cxx::to_string(variable->symbol->name()));
          values_[variable->symbol] = root;
          auto base = constant(0, LOOM_SCALAR_TYPE_OFFSET, location(variable));
          auto element = type(array->elementType(), variable);
          auto view_type = loom_type_shaped_1d(LOOM_TYPE_VIEW,
                                               loom_type_element_type(element),
                                               array->size(), 0);
          check(loom_buffer_view_build(&builder_, root, base, view_type,
                                       location(variable), &op));
          array_views_[root] = name(
              result(op), cxx::to_string(variable->symbol->name()) + "_view");
          continue;
        }
        if (!variable->initializer || !variable->symbol) {
          fail(ast, "locals require initializers");
        }
        if (cxx::type_cast<cxx::BoundedArrayType>(
                unqualified(variable->symbol->type()))) {
          fail(ast, "local arrays require __shared__ in this slice");
        }
        type(variable->symbol->type(), variable);
        values_[variable->symbol] =
            name(expression(variable->initializer),
                 cxx::to_string(variable->symbol->name()));
      }
      return;
    }
    if (auto* branch = cxx::ast_cast<cxx::IfStatementAST>(ast)) {
      if (branch->initializer || branch->constexprLoc) {
        fail(ast, "if initializer/constexpr is outside this slice");
      }
      auto condition = expression(branch->condition);
      auto saved_values = values_;
      auto written = live_mutations(ast);
      std::vector<loom_type_t> types;
      for (auto* symbol : written) {
        types.push_back(value_type(values_.at(symbol)));
      }
      loom_op_t* op;
      auto flags = branch->elseStatement || !written.empty()
                       ? LOOM_SCF_IF_BUILD_FLAG_HAS_ELSE_REGION
                       : 0;
      check(loom_scf_if_build(&builder_, flags, condition, types.data(),
                              types.size(), nullptr, 0, location(ast), &op));
      auto saved =
          loom_builder_enter_region(&builder_, op, loom_scf_if_then_region(op));
      statement(branch->statement);
      loom_op_t* yield;
      auto yielded = current(written);
      check(loom_scf_yield_build(&builder_, yielded.data(), yielded.size(),
                                 location(ast), &yield));
      values_ = saved_values;
      loom_builder_restore(&builder_, saved);
      if (flags) {
        saved = loom_builder_enter_region(&builder_, op,
                                          loom_scf_if_else_region(op));
        if (branch->elseStatement) {
          statement(branch->elseStatement);
        }
        yielded = current(written);
        check(loom_scf_yield_build(&builder_, yielded.data(), yielded.size(),
                                   location(ast), &yield));
        values_ = saved_values;
        loom_builder_restore(&builder_, saved);
      }
      for (size_t index = 0; index < written.size(); ++index) {
        values_[written[index]] = name(loom_op_results(op)[index],
                                       cxx::to_string(written[index]->name()));
      }
      return;
    }
    if (auto* loop = cxx::ast_cast<cxx::ForStatementAST>(ast)) {
      LoopSchedule schedule(unit_, diagnostics_, loop->attributeList);
      if (!loop->condition) {
        fail(ast, "for loops require a condition");
      }
      if (loop->initializer) {
        statement(loop->initializer);
      }
      unsigned counted_step = 1;
      if (auto* induction = counted_induction(loop, counted_step)) {
        counted_loop(loop, induction, counted_step, schedule);
        return;
      }
      if (!schedule.empty()) {
        fail(
            ast,
            "loop scheduling requires a nonwrapping unsigned counted for loop");
      }
      conditional_loop(ast, loop->condition, loop->statement, loop->expression,
                       LoopTest::BeforeBody);
      return;
    }
    if (auto* loop = cxx::ast_cast<cxx::WhileStatementAST>(ast)) {
      if (!LoopSchedule(unit_, diagnostics_, loop->attributeList).empty()) {
        fail(ast, "loop scheduling requires a counted for loop");
      }
      conditional_loop(ast, loop->condition, loop->statement, nullptr,
                       LoopTest::BeforeBody);
      return;
    }
    if (auto* loop = cxx::ast_cast<cxx::DoStatementAST>(ast)) {
      if (!LoopSchedule(unit_, diagnostics_, loop->attributeList).empty()) {
        fail(ast, "loop scheduling requires a counted for loop");
      }
      conditional_loop(ast, loop->expression, loop->statement, nullptr,
                       LoopTest::AfterBody);
      return;
    }
    if (auto* expression_statement =
            cxx::ast_cast<cxx::ExpressionStatementAST>(ast)) {
      if (expression_statement->expression) {
        effect(expression_statement->expression);
      }
      return;
    }
    fail(ast,
         "unsupported statement: " + std::string(cxx::to_string(ast->kind())));
  }

  enum class LoopTest { BeforeBody, AfterBody };

  // The before region's forwarded values are also the loop's final results.
  // A post-test loop therefore runs its body there and uses an identity after
  // region; the final body mutations survive the false condition.
  void conditional_loop(cxx::StatementAST* ast,
                        cxx::ExpressionAST* condition_expression,
                        cxx::StatementAST* body, cxx::ExpressionAST* step,
                        LoopTest test) {
    auto written = live_mutations(ast);
    auto initial = current(written);
    auto outer_values = values_;
    auto source = location(ast);
    loom_op_t* op;
    check(loom_scf_while_build(&builder_, initial.data(), initial.size(),
                               nullptr, 0, source, &op));
    auto* before = loom_scf_while_before(op);
    auto saved = loom_builder_enter_region(&builder_, op, before);
    bind(written, before);
    if (test == LoopTest::AfterBody) {
      statement(body);
    }
    auto condition = expression(condition_expression);
    auto forwarded = current(written);
    loom_op_t* terminator;
    check(loom_scf_condition_build(&builder_, condition, forwarded.data(),
                                   forwarded.size(), source, &terminator));
    loom_builder_restore(&builder_, saved);
    auto* after = loom_scf_while_after(op);
    saved = loom_builder_enter_region(&builder_, op, after);
    values_ = outer_values;
    bind(written, after);
    if (test == LoopTest::BeforeBody) {
      statement(body);
    }
    if (step) {
      effect(step);
    }
    auto yielded = current(written);
    check(loom_scf_yield_build(&builder_, yielded.data(), yielded.size(),
                               source, &terminator));
    loom_builder_restore(&builder_, saved);
    values_ = outer_values;
    for (size_t index = 0; index < written.size(); ++index) {
      values_[written[index]] = name(loom_op_results(op)[index],
                                     cxx::to_string(written[index]->name()));
    }
  }

  cxx::ExpressionAST* without_cast(cxx::ExpressionAST* ast) {
    while (auto* cast = cxx::ast_cast<cxx::ImplicitCastExpressionAST>(ast)) {
      ast = cast->expression;
    }
    return ast;
  }

  // The unit-step unsigned interval cannot wrap before its strict upper bound.
  // A stable scalar/literal upper bound in that same unsigned width may be
  // evaluated once. Wider comparisons can observe induction wraparound and
  // retain their general while semantics, as do mutable bounds.
  cxx::Symbol* counted_induction(cxx::ForStatementAST* loop,
                                 unsigned& step_value) {
    auto* declaration =
        cxx::ast_cast<cxx::DeclarationStatementAST>(loop->initializer);
    auto* initial =
        declaration
            ? cxx::ast_cast<cxx::SimpleDeclarationAST>(declaration->declaration)
            : nullptr;
    auto* condition = cxx::ast_cast<cxx::BinaryExpressionAST>(loop->condition);
    auto* step = cxx::ast_cast<cxx::UnaryExpressionAST>(loop->expression);
    auto* compound =
        cxx::ast_cast<cxx::CompoundAssignmentExpressionAST>(loop->expression);
    if (!initial || !initial->initDeclaratorList ||
        initial->initDeclaratorList->next || !condition || condition->symbol ||
        condition->op != cxx::TokenKind::T_LESS ||
        !cxx::ast_cast<cxx::CompoundStatementAST>(loop->statement)) {
      return nullptr;
    }
    auto* induction = initial->initDeclaratorList->value->symbol;
    auto* left = cxx::ast_cast<cxx::IdExpressionAST>(
        without_cast(condition->leftExpression));
    cxx::IdExpressionAST* increment = nullptr;
    if (step && !step->symbol && step->op == cxx::TokenKind::T_PLUS_PLUS) {
      increment = cxx::ast_cast<cxx::IdExpressionAST>(step->expression);
    } else if (compound && !compound->symbol &&
               compound->op == cxx::TokenKind::T_PLUS_EQUAL &&
               cxx::ast_cast<cxx::IntLiteralExpressionAST>(
                   compound->rightExpression) &&
               cxx::ast_cast<cxx::IntLiteralExpressionAST>(
                   condition->rightExpression)) {
      cxx::ASTInterpreter interpreter(&unit_);
      auto amount =
          interpreter.toInt(*interpreter.evaluate(compound->rightExpression));
      auto upper =
          interpreter.toInt(*interpreter.evaluate(condition->rightExpression));
      if (amount && upper && *amount > 0 && *amount <= UINT32_MAX &&
          *upper >= 0 && *upper <= UINT32_MAX - *amount + 1) {
        step_value = static_cast<unsigned>(*amount);
        increment =
            cxx::ast_cast<cxx::IdExpressionAST>(compound->targetExpression);
      }
    }
    auto* upper = without_cast(condition->rightExpression);
    auto* bound = cxx::ast_cast<cxx::IdExpressionAST>(upper);
    if (!left || !increment || left->symbol != induction ||
        increment->symbol != induction ||
        unqualified(induction->type())->kind() != cxx::TypeKind::kUnsignedInt ||
        unqualified(condition->leftExpression->type)->kind() !=
            cxx::TypeKind::kUnsignedInt ||
        unqualified(condition->rightExpression->type)->kind() !=
            cxx::TypeKind::kUnsignedInt ||
        (!bound && !cxx::ast_cast<cxx::IntLiteralExpressionAST>(upper))) {
      return nullptr;
    }
    const auto& body_writes = mutations_.written(loop->statement);
    const auto& loop_writes = mutations_.written(loop);
    if (std::ranges::find(body_writes, induction) != body_writes.end() ||
        (bound &&
         std::ranges::find(loop_writes, bound->symbol) != loop_writes.end())) {
      return nullptr;
    }
    return induction;
  }

  loom_value_id_t unsigned_offset(loom_value_id_t value,
                                  loom_location_id_t source) {
    loom_op_t* cast;
    check(loom_index_cast_build(&builder_, value, value_type(value),
                                loom_type_scalar(LOOM_SCALAR_TYPE_OFFSET),
                                source, &cast));
    return result(cast);
  }

  void counted_loop(cxx::ForStatementAST* loop, cxx::Symbol* induction,
                    unsigned step_value, const LoopSchedule& schedule) {
    auto source = location(loop);
    auto* condition = cxx::ast_cast<cxx::BinaryExpressionAST>(loop->condition);
    auto lower = unsigned_offset(values_.at(induction), source);
    auto upper =
        unsigned_offset(expression(condition->rightExpression), source);
    auto step = constant(step_value, LOOM_SCALAR_TYPE_OFFSET, source);
    auto written = live_mutations(loop);
    std::erase(written, induction);
    auto initial = current(written);
    auto outer_values = values_;
    auto* op = schedule.build(&builder_, lower, upper, step, initial, source);
    auto* body = loom_scf_for_body(op);
    auto saved = loom_builder_enter_region(&builder_, op, body);
    auto iteration = name(loom_region_entry_arg_id(body, 0),
                          cxx::to_string(induction->name()));
    loom_op_t* cast;
    check(loom_index_cast_build(&builder_, iteration, value_type(iteration),
                                type(induction->type(), loop), source, &cast));
    values_[induction] = result(cast);
    for (size_t index = 0; index < written.size(); ++index) {
      values_[written[index]] = name(loom_region_entry_arg_id(body, index + 1),
                                     cxx::to_string(written[index]->name()));
    }
    statement(loop->statement);
    auto yielded = current(written);
    loom_op_t* terminator;
    check(loom_scf_yield_build(&builder_, yielded.data(), yielded.size(),
                               source, &terminator));
    loom_builder_restore(&builder_, saved);
    values_ = outer_values;
    values_.erase(induction);
    for (size_t index = 0; index < written.size(); ++index) {
      values_[written[index]] = name(loom_op_results(op)[index],
                                     cxx::to_string(written[index]->name()));
    }
  }

  std::vector<cxx::Symbol*> live_mutations(cxx::AST* owner) {
    std::vector<cxx::Symbol*> result;
    for (auto* symbol : mutations_.written(owner)) {
      if (values_.contains(symbol)) {
        if (loom_type_kind(value_type(values_.at(symbol))) !=
            LOOM_TYPE_SCALAR) {
          fail(owner, "only scalar locals may cross control-flow joins");
        }
        result.push_back(symbol);
      }
    }
    return result;
  }
  std::vector<loom_value_id_t> current(
      const std::vector<cxx::Symbol*>& symbols) {
    std::vector<loom_value_id_t> result;
    for (auto* symbol : symbols) {
      result.push_back(values_.at(symbol));
    }
    return result;
  }
  void bind(const std::vector<cxx::Symbol*>& symbols, loom_region_t* region) {
    for (size_t index = 0; index < symbols.size(); ++index) {
      values_[symbols[index]] = name(loom_region_entry_arg_id(region, index),
                                     cxx::to_string(symbols[index]->name()));
    }
  }

  void effect(cxx::ExpressionAST* ast) {
    if (auto* call = cxx::ast_cast<cxx::CallExpressionAST>(ast)) {
      auto* id = cxx::ast_cast<cxx::IdExpressionAST>(call->baseExpression);
      if (id && annotated(id->symbol, "assume")) {
        auto* condition = call->expressionList && !call->expressionList->next
                              ? cxx::ast_cast<cxx::BinaryExpressionAST>(
                                    call->expressionList->value)
                              : nullptr;
        auto* binding = condition ? cxx::ast_cast<cxx::IdExpressionAST>(
                                        without_cast(condition->leftExpression))
                                  : nullptr;
        if (!condition || condition->symbol ||
            condition->op != cxx::TokenKind::T_LESS || !binding ||
            !is_unsigned(binding->type) ||
            !cxx::ast_cast<cxx::IntLiteralExpressionAST>(
                condition->rightExpression)) {
          fail(ast,
               "assume requires an unsigned scalar binding < positive i32 "
               "literal");
        }
        cxx::ASTInterpreter interpreter(&unit_);
        auto bound = interpreter.toInt(
            *interpreter.evaluate(condition->rightExpression));
        if (!bound || *bound <= 0 || *bound > INT32_MAX) {
          fail(ast, "assume upper bound must fit positive signed i32");
        }
        auto value = expression(binding);
        auto value_type = type(binding->type, ast);
        loom_predicate_t predicate = {
            .kind = LOOM_PREDICATE_RANGE,
            .arg_count = 3,
            .arg_tags = {LOOM_PRED_ARG_VALUE, LOOM_PRED_ARG_CONST,
                         LOOM_PRED_ARG_CONST},
            .args = {value, 0, *bound - 1},
        };
        loom_op_t* op;
        check(loom_scalar_assume_build(&builder_, &value, 1, &predicate, 1,
                                       &value_type, 1, location(ast), &op));
        values_[binding->symbol] =
            name(result(op), cxx::to_string(binding->symbol->name()));
        return;
      }
      if (!id || !annotated(id->symbol, "barrier")) {
        if (unqualified(ast->type)->kind() != cxx::TypeKind::kVoid) {
          expression(ast);
          return;
        }
        auto* function =
            id ? cxx::symbol_cast<cxx::FunctionSymbol>(id->symbol) : nullptr;
        if (!function || !function->declaration() ||
            annotated(function, "kernel")) {
          fail(
              ast,
              "void call requires a defined ordinary function or an intrinsic");
        }
        std::vector<loom_value_id_t> arguments;
        for (auto* argument : cxx::ListView{call->expressionList}) {
          arguments.push_back(expression(argument));
        }
        auto callee = declare(function);
        loom_op_t* op;
        check(loom_func_call_build(&builder_, 0, 0, 0, 0, callee,
                                   arguments.data(), arguments.size(), nullptr,
                                   0, nullptr, 0, location(ast), &op));
        return;
      }
      if (call->expressionList) {
        fail(ast, "barrier does not take arguments");
      }
      // HIP __syncthreads covers both global and shared memory. Loom names one
      // memory space per barrier, so preserve both fences explicitly.
      loom_op_t* op;
      check(loom_kernel_barrier_build(
          &builder_, LOOM_VALUE_FACT_MEMORY_SPACE_GLOBAL,
          LOOM_ATOMIC_SCOPE_WORKGROUP, LOOM_ATOMIC_ORDERING_ACQ_REL,
          location(ast), &op));
      check(loom_kernel_barrier_build(
          &builder_, LOOM_VALUE_FACT_MEMORY_SPACE_WORKGROUP,
          LOOM_ATOMIC_SCOPE_WORKGROUP, LOOM_ATOMIC_ORDERING_ACQ_REL,
          location(ast), &op));
      return;
    }
    if (auto* assignment =
            cxx::ast_cast<cxx::CompoundAssignmentExpressionAST>(ast)) {
      auto* destination =
          cxx::ast_cast<cxx::IdExpressionAST>(assignment->targetExpression);
      if (!destination || assignment->symbol) {
        fail(ast, "compound assignment requires a builtin scalar local");
      }
      auto old = expression(destination);
      auto value = expression(assignment->rightExpression);
      auto* promoted = assignment->leftExpression->type;
      old = convert_value(old, destination->type, promoted, ast);
      value = convert_value(value, assignment->rightExpression->type, promoted,
                            ast);
      auto updated = binary_value(cxx::get_underlying_binary_op(assignment->op),
                                  old, value, promoted, promoted, ast);
      updated = convert_value(updated, promoted, destination->type, ast);
      values_[destination->symbol] =
          name(updated, cxx::to_string(destination->symbol->name()));
      return;
    }
    if (auto* assignment = cxx::ast_cast<cxx::AssignmentExpressionAST>(ast)) {
      if (assignment->symbol || assignment->op != cxx::TokenKind::T_EQUAL) {
        fail(ast, "only builtin plain assignment is admitted here");
      }
      auto value = expression(assignment->rightExpression);
      if (auto* destination =
              cxx::ast_cast<cxx::IdExpressionAST>(assignment->leftExpression)) {
        if (!values_.contains(destination->symbol)) {
          fail(ast, "assignment requires an owned automatic source binding");
        }
        values_[destination->symbol] =
            name(value, cxx::to_string(destination->symbol->name()));
        return;
      }
      auto* destination = cxx::ast_cast<cxx::SubscriptExpressionAST>(
          assignment->leftExpression);
      if (!destination) {
        fail(ast, "unsupported assignment destination");
      }
      auto access = address(destination);
      int64_t selector = access.index ? INT64_MIN : 0;
      loom_op_t* op;
      check(loom_view_store_build(&builder_, 0, 0, value, access.view,
                                  access.index ? &*access.index : nullptr,
                                  access.index ? 1 : 0, &selector, 1, 0, 0,
                                  location(ast), &op));
      return;
    }
    cxx::ExpressionAST* destination = nullptr;
    bool decrement = false;
    if (auto* increment = cxx::ast_cast<cxx::PostIncrExpressionAST>(ast)) {
      if ((increment->op != cxx::TokenKind::T_PLUS_PLUS &&
           increment->op != cxx::TokenKind::T_MINUS_MINUS) ||
          increment->symbol) {
        fail(ast, "only builtin increment/decrement is admitted");
      }
      decrement = increment->op == cxx::TokenKind::T_MINUS_MINUS;
      destination = increment->baseExpression;
    } else if (auto* increment = cxx::ast_cast<cxx::UnaryExpressionAST>(ast)) {
      if ((increment->op != cxx::TokenKind::T_PLUS_PLUS &&
           increment->op != cxx::TokenKind::T_MINUS_MINUS) ||
          increment->symbol) {
        fail(ast, "only builtin increment/decrement is admitted");
      }
      decrement = increment->op == cxx::TokenKind::T_MINUS_MINUS;
      destination = increment->expression;
    }
    if (auto* id = cxx::ast_cast<cxx::IdExpressionAST>(destination)) {
      if (!unit_.typeTraits().is_integral(id->type) ||
          unqualified(id->type)->kind() == cxx::TypeKind::kBool) {
        fail(ast, "increment requires an integer local");
      }
      auto old = expression(id);
      auto one =
          constant(1, loom_type_element_type(value_type(old)), location(ast));
      loom_op_t* op;
      auto build = decrement ? loom_scalar_subi_build : loom_scalar_addi_build;
      check(build(&builder_, 0, old, one, value_type(old), location(ast), &op));
      values_[id->symbol] =
          name(result(op), cxx::to_string(id->symbol->name()));
      return;
    }
    fail(ast, "unsupported effect expression: " +
                  std::string(cxx::to_string(ast->kind())));
  }

  // Source AST/symbol lifetime ends after construction and verification.
  cxx::TranslationUnit& unit_;
  // Source diagnostics share frontend byte ranges and the caller sink.
  Diagnostics& diagnostics_;
  // Retained generated operation bindings for reached source declarations.
  Intrinsics intrinsics_;
  // Admitted launch contracts, including bounds from function redeclarations.
  LaunchContracts launches_;
  // Output arena owner.
  loom_module_t* module_;
  // Current insertion point in the structured output.
  loom_builder_t builder_ = {};
  // Borrowed source configuration for this invocation.
  const loom_cxx_import_options_t& options_;
  // Explicit source-level permission for approximate math function results.
  uint8_t math_flags_;
  // Selected externally visible definitions; all other callees stay private.
  std::unordered_set<cxx::FunctionSymbol*> exported_;
  // Whether the current function has a kernel launch contract.
  bool kernel_ = false;
  // Bound symbols, never identifier spellings, key source-to-SSA mappings.
  std::unordered_map<cxx::Symbol*, loom_value_id_t> values_;
  // Each reachable function receives one output symbol.
  std::unordered_map<cxx::FunctionSymbol*, loom_symbol_ref_t> callees_;
  // Worklist preserves discovery order across concrete helper instantiations.
  std::vector<cxx::FunctionSymbol*> functions_;
  // Deterministic disambiguation for overloaded or colliding helper names.
  std::unordered_map<std::string, unsigned> symbol_names_;
  // Source qualification is computed once for each discovered function.
  std::unordered_map<cxx::FunctionSymbol*, std::string> qualified_names_;
  // Retained source provenance avoids reconstructing repeated AST locations.
  std::unordered_map<cxx::AST*, loom_location_id_t> locations_;
  // Source-owned mutation facts retained for all reached function bodies.
  Mutations mutations_;
  // Fixed array extents become named views at their declarations.
  std::unordered_map<loom_value_id_t, loom_value_id_t> array_views_;
};
}  // namespace

void translate(cxx::TranslationUnit& unit, Diagnostics& diagnostics,
               loom_module_t* module,
               const loom_cxx_import_options_t& options) {
  Translator(unit, diagnostics, module, options).translate();
}

}  // namespace loom::cxx_import
