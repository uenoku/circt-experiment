//===- TranslateToCpp.cpp - Export mockturtle C++ -------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "CIRCTMockturtle/CIRCTMockturtleTranslations.h"
#include "circt/Dialect/Comb/CombDialect.h"
#include "circt/Dialect/Comb/CombOps.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "circt/Dialect/HW/HWOps.h"
#include "circt/Dialect/Synth/SynthDialect.h"
#include "circt/Dialect/Synth/SynthOps.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Operation.h"
#include "mlir/Tools/mlir-translate/Translation.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/raw_ostream.h"

#include <cctype>

using namespace circt;
using namespace circt::mockturtle_plugin;
using namespace mlir;

namespace {

enum class NetworkKind { AIG, XAG, MIG, XMG };

StringRef getNetworkClass(NetworkKind kind) {
  switch (kind) {
  case NetworkKind::AIG:
    return "aig_network";
  case NetworkKind::XAG:
    return "xag_network";
  case NetworkKind::MIG:
    return "mig_network";
  case NetworkKind::XMG:
    return "xmg_network";
  }
  llvm_unreachable("unknown mockturtle network kind");
}

std::string sanitizeIdentifier(StringRef name) {
  std::string result;
  result.reserve(name.size() + 1);
  for (char c : name) {
    if (std::isalnum(static_cast<unsigned char>(c)) || c == '_')
      result.push_back(c);
    else
      result.push_back('_');
  }
  if (result.empty() ||
      !(std::isalpha(static_cast<unsigned char>(result.front())) ||
        result.front() == '_'))
    result.insert(result.begin(), '_');

  static const char *const keywords[] = {"alignas",
                                         "alignof",
                                         "and",
                                         "and_eq",
                                         "asm",
                                         "atomic_cancel",
                                         "atomic_commit",
                                         "atomic_noexcept",
                                         "auto",
                                         "bitand",
                                         "bitor",
                                         "bool",
                                         "break",
                                         "case",
                                         "catch",
                                         "char",
                                         "char16_t",
                                         "char32_t",
                                         "class",
                                         "compl",
                                         "concept",
                                         "const",
                                         "consteval",
                                         "constexpr",
                                         "constinit",
                                         "const_cast",
                                         "continue",
                                         "co_await",
                                         "co_return",
                                         "co_yield",
                                         "decltype",
                                         "default",
                                         "delete",
                                         "do",
                                         "double",
                                         "dynamic_cast",
                                         "else",
                                         "enum",
                                         "explicit",
                                         "export",
                                         "extern",
                                         "false",
                                         "float",
                                         "for",
                                         "friend",
                                         "goto",
                                         "if",
                                         "inline",
                                         "int",
                                         "long",
                                         "mutable",
                                         "namespace",
                                         "new",
                                         "noexcept",
                                         "not",
                                         "not_eq",
                                         "nullptr",
                                         "operator",
                                         "or",
                                         "or_eq",
                                         "private",
                                         "protected",
                                         "public",
                                         "reflexpr",
                                         "register",
                                         "reinterpret_cast",
                                         "requires",
                                         "return",
                                         "short",
                                         "signed",
                                         "sizeof",
                                         "static",
                                         "static_assert",
                                         "static_cast",
                                         "struct",
                                         "switch",
                                         "synchronized",
                                         "template",
                                         "this",
                                         "thread_local",
                                         "throw",
                                         "true",
                                         "try",
                                         "typedef",
                                         "typeid",
                                         "typename",
                                         "union",
                                         "unsigned",
                                         "using",
                                         "virtual",
                                         "void",
                                         "volatile",
                                         "wchar_t",
                                         "while",
                                         "xor",
                                         "xor_eq"};
  if (llvm::is_contained(keywords, result))
    result += "_";
  return result;
}

std::string makeUnique(StringRef base, llvm::StringSet<> &used) {
  std::string name = sanitizeIdentifier(base);
  std::string candidate = name;
  unsigned index = 0;
  while (!used.insert(candidate).second)
    candidate = (Twine(name) + "_" + Twine(++index)).str();
  return candidate;
}

std::string escapeCString(StringRef text) {
  std::string result;
  result.reserve(text.size());
  for (char c : text) {
    switch (c) {
    case '\\':
      result += "\\\\";
      break;
    case '"':
      result += "\\\"";
      break;
    case '\n':
      result += "\\n";
      break;
    case '\r':
      result += "\\r";
      break;
    case '\t':
      result += "\\t";
      break;
    default:
      result.push_back(c);
      break;
    }
  }
  return result;
}

bool isSingleBit(Type type) {
  auto intType = dyn_cast<IntegerType>(type);
  return intType && intType.getWidth() == 1;
}

LogicalResult checkSingleBit(Value value, Operation *op) {
  if (isSingleBit(value.getType()))
    return success();
  return op->emitError("mockturtle C++ translation currently supports only i1 "
                       "signals, got ")
         << value.getType();
}

NetworkKind inferNetworkKind(hw::HWModuleOp module) {
  bool hasAnd = false;
  bool hasXor = false;
  bool hasMajority = false;
  module.walk([&](Operation *op) {
    hasAnd |= isa<synth::aig::AndInverterOp, comb::AndOp>(op);
    hasXor |= isa<synth::XorInverterOp, comb::XorOp>(op);
    hasMajority |= isa<synth::MajorityOp>(op);
  });

  if (hasMajority && hasXor)
    return NetworkKind::XMG;
  if (hasMajority)
    return NetworkKind::MIG;
  if (hasXor)
    return NetworkKind::XAG;
  (void)hasAnd;
  return NetworkKind::AIG;
}

std::string maybeInvert(StringRef expr, bool inverted) {
  if (!inverted)
    return expr.str();
  return (Twine("!") + expr).str();
}

struct CppEmitter {
  explicit CppEmitter(raw_ostream &os) : os(os) {}

  LogicalResult emit(ModuleOp module);

private:
  LogicalResult emitHWModule(hw::HWModuleOp module);
  LogicalResult emitOperation(Operation *op);
  FailureOr<std::string> getValue(Value value);
  FailureOr<std::string> reduce(Operation *op, OperandRange operands,
                                StringRef method, StringRef identity);
  void emitPassHelpers();
  void emitHeader();
  void emitMain();

  raw_ostream &os;
  SmallVector<std::string> builders;
  llvm::DenseMap<Value, std::string> valueNames;
  llvm::StringSet<> usedNames;
};

void CppEmitter::emitHeader() {
  os << "#include <mockturtle/algorithms/aig_balancing.hpp>\n";
  os << "#include <mockturtle/algorithms/aig_resub.hpp>\n";
  os << "#include <mockturtle/algorithms/balancing.hpp>\n";
  os << "#include <mockturtle/algorithms/cleanup.hpp>\n";
  os << "#include <mockturtle/algorithms/functional_reduction.hpp>\n";
  os << "#include <mockturtle/algorithms/mig_algebraic_rewriting.hpp>\n";
  os << "#include <mockturtle/algorithms/mig_inv_optimization.hpp>\n";
  os << "#include <mockturtle/algorithms/mig_inv_propagation.hpp>\n";
  os << "#include <mockturtle/algorithms/mig_resub.hpp>\n";
  os << "#include <mockturtle/algorithms/node_resynthesis/sop_factoring.hpp>\n";
  os << "#include <mockturtle/algorithms/refactoring.hpp>\n";
  os << "#include <mockturtle/algorithms/xag_algebraic_rewriting.hpp>\n";
  os << "#include <mockturtle/algorithms/xag_balancing.hpp>\n";
  os << "#include <mockturtle/algorithms/xag_resub.hpp>\n";
  os << "#include <mockturtle/algorithms/xmg_algebraic_rewriting.hpp>\n";
  os << "#include <mockturtle/algorithms/xmg_resub.hpp>\n";
  os << "#include <mockturtle/networks/aig.hpp>\n";
  os << "#include <mockturtle/networks/mig.hpp>\n";
  os << "#include <mockturtle/networks/xag.hpp>\n";
  os << "#include <mockturtle/networks/xmg.hpp>\n";
  os << "#include <mockturtle/views/depth_view.hpp>\n";
  os << "#include <mockturtle/views/fanout_view.hpp>\n";
  os << "#include <iostream>\n\n";
}

void CppEmitter::emitPassHelpers() {
  os << "template <typename Ntk>\n";
  os << "void run_mockturtle_refactor(\n";
  os << "    Ntk &ntk,\n";
  os << "    mockturtle::refactoring_params const &ps = {}) {\n";
  os << "  mockturtle::sop_factoring<Ntk> sop;\n";
  os << "  mockturtle::refactoring(ntk, sop, ps);\n";
  os << "  ntk = mockturtle::cleanup_dangling(ntk);\n";
  os << "}\n\n";

  os << "template <typename Ntk>\n";
  os << "void run_mockturtle_functional_reduction(\n";
  os << "    Ntk &ntk,\n";
  os << "    mockturtle::functional_reduction_params const &ps = {}) {\n";
  os << "  mockturtle::functional_reduction(ntk, ps);\n";
  os << "}\n\n";

  os << "template <typename Ntk>\n";
  os << "void run_mockturtle_sop_balancing(\n";
  os << "    Ntk &ntk, mockturtle::lut_map_params const &ps = {}) {\n";
  os << "  ntk = mockturtle::sop_balancing(ntk, ps);\n";
  os << "}\n\n";

  os << "template <typename Ntk>\n";
  os << "void run_mockturtle_esop_balancing(\n";
  os << "    Ntk &ntk, mockturtle::lut_map_params const &ps = {}) {\n";
  os << "  ntk = mockturtle::esop_balancing(ntk, ps);\n";
  os << "}\n\n";

  os << "void run_mockturtle_aig_balancing(\n";
  os << "    mockturtle::aig_network &ntk,\n";
  os << "    mockturtle::aig_balancing_params const &ps = {}) {\n";
  os << "  mockturtle::aig_balance(ntk, ps);\n";
  os << "}\n\n";

  os << "void run_mockturtle_xag_balancing(\n";
  os << "    mockturtle::xag_network &ntk,\n";
  os << "    mockturtle::xag_balancing_params const &ps = {}) {\n";
  os << "  mockturtle::xag_balance(ntk, ps);\n";
  os << "}\n\n";

  os << "void run_mockturtle_aig_resubstitution(\n";
  os << "    mockturtle::aig_network &ntk,\n";
  os << "    mockturtle::resubstitution_params const &ps = {}) {\n";
  os << "  mockturtle::aig_resubstitution(ntk, ps);\n";
  os << "}\n\n";

  os << "void run_mockturtle_aig_resubstitution2(\n";
  os << "    mockturtle::aig_network &ntk,\n";
  os << "    mockturtle::resubstitution_params const &ps = {}) {\n";
  os << "  mockturtle::depth_view<mockturtle::aig_network> depth_view{ntk};\n";
  os << "  mockturtle::fanout_view<decltype(depth_view)> "
        "fanout_view{depth_view};\n";
  os << "  mockturtle::aig_resubstitution2(fanout_view, ps);\n";
  os << "}\n\n";

  os << "void run_mockturtle_xag_resubstitution(\n";
  os << "    mockturtle::xag_network &ntk,\n";
  os << "    mockturtle::resubstitution_params const &ps = {}) {\n";
  os << "  mockturtle::depth_view<mockturtle::xag_network> depth_view{ntk};\n";
  os << "  mockturtle::fanout_view<decltype(depth_view)> "
        "fanout_view{depth_view};\n";
  os << "  mockturtle::xag_resubstitution(fanout_view, ps);\n";
  os << "}\n\n";

  os << "void run_mockturtle_mig_resubstitution(\n";
  os << "    mockturtle::mig_network &ntk,\n";
  os << "    mockturtle::resubstitution_params const &ps = {}) {\n";
  os << "  mockturtle::depth_view<mockturtle::mig_network> depth_view{ntk};\n";
  os << "  mockturtle::fanout_view<decltype(depth_view)> "
        "fanout_view{depth_view};\n";
  os << "  mockturtle::mig_resubstitution(fanout_view, ps);\n";
  os << "}\n\n";

  os << "void run_mockturtle_mig_resubstitution2(\n";
  os << "    mockturtle::mig_network &ntk,\n";
  os << "    mockturtle::resubstitution_params const &ps = {}) {\n";
  os << "  mockturtle::depth_view<mockturtle::mig_network> depth_view{ntk};\n";
  os << "  mockturtle::fanout_view<decltype(depth_view)> "
        "fanout_view{depth_view};\n";
  os << "  mockturtle::mig_resubstitution2(fanout_view, ps);\n";
  os << "}\n\n";

  os << "void run_mockturtle_xmg_resubstitution(\n";
  os << "    mockturtle::xmg_network &ntk,\n";
  os << "    mockturtle::resubstitution_params const &ps = {}) {\n";
  os << "  mockturtle::xmg_resubstitution(ntk, ps);\n";
  os << "}\n\n";

  os << "void run_mockturtle_mig_algebraic_rewrite_depth(\n";
  os << "    mockturtle::mig_network &ntk,\n";
  os << "    mockturtle::mig_algebraic_depth_rewriting_params const &ps = {}) "
        "{\n";
  os << "  mockturtle::depth_view<mockturtle::mig_network> depth_view{ntk};\n";
  os << "  mockturtle::mig_algebraic_depth_rewriting(depth_view, ps);\n";
  os << "}\n\n";

  os << "void run_mockturtle_xag_algebraic_rewrite_depth(\n";
  os << "    mockturtle::xag_network &ntk,\n";
  os << "    mockturtle::xag_algebraic_depth_rewriting_params const &ps = {}) "
        "{\n";
  os << "  mockturtle::depth_view<mockturtle::xag_network> depth_view{ntk};\n";
  os << "  mockturtle::xag_algebraic_depth_rewriting(depth_view, ps);\n";
  os << "}\n\n";

  os << "void run_mockturtle_xmg_algebraic_rewrite_depth(\n";
  os << "    mockturtle::xmg_network &ntk,\n";
  os << "    mockturtle::xmg_algebraic_depth_rewriting_params const &ps = {}) "
        "{\n";
  os << "  mockturtle::depth_view<mockturtle::xmg_network> depth_view{ntk};\n";
  os << "  mockturtle::xmg_algebraic_depth_rewriting(depth_view, ps);\n";
  os << "}\n\n";

  os << "void run_mockturtle_mig_inv_propagation(\n";
  os << "    mockturtle::mig_network &ntk) {\n";
  os << "  mockturtle::mig_inv_propagation(ntk);\n";
  os << "}\n\n";

  os << "void run_mockturtle_mig_inv_optimization(\n";
  os << "    mockturtle::mig_network &ntk) {\n";
  os << "  mockturtle::fanout_view<mockturtle::mig_network> "
        "fanout_view{ntk};\n";
  os << "  mockturtle::mig_inv_optimization(fanout_view);\n";
  os << "}\n\n";
}

FailureOr<std::string> CppEmitter::getValue(Value value) {
  auto it = valueNames.find(value);
  if (it != valueNames.end())
    return it->second;
  if (auto *definingOp = value.getDefiningOp()) {
    if (failed(emitOperation(definingOp)))
      return failure();
    it = valueNames.find(value);
    if (it != valueNames.end())
      return it->second;
  }
  emitError(value.getLoc()) << "cannot translate value without a mockturtle "
                               "definition";
  return failure();
}

FailureOr<std::string> CppEmitter::reduce(Operation *op, OperandRange operands,
                                          StringRef method,
                                          StringRef identity) {
  if (operands.empty())
    return (Twine("ntk.get_constant(") + identity + ")").str();

  auto result = getValue(operands.front());
  if (failed(result))
    return failure();
  for (Value operand : llvm::drop_begin(operands)) {
    auto rhs = getValue(operand);
    if (failed(rhs))
      return failure();
    result = (Twine("ntk.") + method + "(" + *result + ", " + *rhs + ")").str();
  }
  return result;
}

LogicalResult CppEmitter::emitOperation(Operation *op) {
  if (op->getNumResults() != 1)
    return success();

  Value resultValue = op->getResult(0);
  if (failed(checkSingleBit(resultValue, op)))
    return failure();

  std::string expr;
  if (auto constant = dyn_cast<hw::ConstantOp>(op)) {
    expr = (Twine("ntk.get_constant(") +
            (constant.getValue().isZero() ? "false" : "true") + ")")
               .str();
  } else if (auto andOp = dyn_cast<synth::aig::AndInverterOp>(op)) {
    if (andOp.getNumOperands() == 0)
      expr = "ntk.get_constant(true)";
    else {
      auto lhs = getValue(andOp.getOperand(0));
      if (failed(lhs))
        return failure();
      expr = maybeInvert(*lhs, andOp.isInverted(0));
      for (unsigned i = 1, e = andOp.getNumOperands(); i != e; ++i) {
        auto rhs = getValue(andOp.getOperand(i));
        if (failed(rhs))
          return failure();
        expr = (Twine("ntk.create_and(") + expr + ", " +
                maybeInvert(*rhs, andOp.isInverted(i)) + ")")
                   .str();
      }
    }
  } else if (auto xorOp = dyn_cast<synth::XorInverterOp>(op)) {
    if (xorOp.getNumOperands() == 0)
      expr = "ntk.get_constant(false)";
    else {
      auto lhs = getValue(xorOp.getOperand(0));
      if (failed(lhs))
        return failure();
      expr = maybeInvert(*lhs, xorOp.isInverted(0));
      for (unsigned i = 1, e = xorOp.getNumOperands(); i != e; ++i) {
        auto rhs = getValue(xorOp.getOperand(i));
        if (failed(rhs))
          return failure();
        expr = (Twine("ntk.create_xor(") + expr + ", " +
                maybeInvert(*rhs, xorOp.isInverted(i)) + ")")
                   .str();
      }
    }
  } else if (auto majority = dyn_cast<synth::MajorityOp>(op)) {
    if (majority.getNumOperands() != 3)
      return op->emitError("expected synth.majority to have three operands");
    SmallVector<std::string> inputs;
    for (unsigned i = 0; i != 3; ++i) {
      auto operand = getValue(majority.getOperand(i));
      if (failed(operand))
        return failure();
      inputs.push_back(maybeInvert(*operand, majority.isInverted(i)));
    }
    expr = (Twine("ntk.create_maj(") + inputs[0] + ", " + inputs[1] + ", " +
            inputs[2] + ")")
               .str();
  } else if (auto andOp = dyn_cast<comb::AndOp>(op)) {
    auto reduced = reduce(op, andOp.getInputs(), "create_and", "true");
    if (failed(reduced))
      return failure();
    expr = *reduced;
  } else if (auto orOp = dyn_cast<comb::OrOp>(op)) {
    auto reduced = reduce(op, orOp.getInputs(), "create_or", "false");
    if (failed(reduced))
      return failure();
    expr = *reduced;
  } else if (auto xorOp = dyn_cast<comb::XorOp>(op)) {
    auto reduced = reduce(op, xorOp.getInputs(), "create_xor", "false");
    if (failed(reduced))
      return failure();
    expr = *reduced;
  } else {
    return op->emitError("unsupported operation in mockturtle C++ translation");
  }

  auto name = makeUnique("n", usedNames);
  os << "  auto " << name << " = " << expr << ";\n";
  valueNames[resultValue] = name;
  return success();
}

LogicalResult CppEmitter::emitHWModule(hw::HWModuleOp module) {
  NetworkKind kind = inferNetworkKind(module);
  StringRef networkClass = getNetworkClass(kind);
  auto moduleName = module.getModuleName();
  auto builderName =
      makeUnique((Twine("build_") + moduleName).str(), usedNames);
  builders.push_back(builderName);

  os << "mockturtle::" << networkClass << " " << builderName << "() {\n";
  os << "  mockturtle::" << networkClass << " ntk;\n";

  valueNames.clear();
  llvm::StringSet<> localUsed;
  localUsed.insert("ntk");

  Block *body = module.getBodyBlock();
  for (auto [index, arg] : llvm::enumerate(body->getArguments())) {
    if (failed(checkSingleBit(arg, module.getOperation())))
      return failure();
    std::string name = makeUnique(module.getInputName(index), localUsed);
    os << "  auto " << name << " = ntk.create_pi();";
    os << " // " << escapeCString(module.getInputName(index)) << "\n";
    valueNames[arg] = name;
    usedNames.insert(name);
  }

  auto *terminator = body->getTerminator();
  for (Operation &op : body->without_terminator()) {
    if (failed(emitOperation(&op)))
      return failure();
  }

  auto output = dyn_cast<hw::OutputOp>(terminator);
  if (!output)
    return terminator->emitError("expected hw.output terminator");

  for (auto [index, value] : llvm::enumerate(output.getOutputs())) {
    if (failed(checkSingleBit(value, output.getOperation())))
      return failure();
    auto expr = getValue(value);
    if (failed(expr))
      return failure();
    os << "  ntk.create_po(" << *expr << ");";
    os << " // " << escapeCString(module.getOutputName(index)) << "\n";
  }

  os << "  return ntk;\n";
  os << "}\n\n";
  return success();
}

void CppEmitter::emitMain() {
  os << "int main() {\n";
  if (builders.empty()) {
    os << "  return 0;\n";
  } else {
    os << "  auto ntk = " << builders.front() << "();\n";
    os << "  std::cout << \"pis=\" << ntk.num_pis()\n";
    os << "            << \" pos=\" << ntk.num_pos()\n";
    os << "            << \" gates=\" << ntk.num_gates() << '\\n';\n";
    os << "  return 0;\n";
  }
  os << "}\n";
}

LogicalResult CppEmitter::emit(ModuleOp module) {
  emitHeader();
  emitPassHelpers();

  auto modules = module.getOps<hw::HWModuleOp>();
  if (modules.empty())
    return module.emitError("no hw.module found for mockturtle C++ export");

  for (auto hwModule : modules)
    if (failed(emitHWModule(hwModule)))
      return failure();

  emitMain();
  return success();
}

} // namespace

void circt::mockturtle_plugin::registerTranslations() {
  static TranslateFromMLIRRegistration toMockturtleCpp(
      "mockturtle-mlir-to-cpp",
      "translate CIRCT HW/Synth MLIR to executable mockturtle C++",
      [](ModuleOp module, raw_ostream &output) {
        return CppEmitter(output).emit(module);
      },
      [](DialectRegistry &registry) {
        registry
            .insert<comb::CombDialect, hw::HWDialect, synth::SynthDialect>();
      });
}
