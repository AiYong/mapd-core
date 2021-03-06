/*
 * Copyright 2018 OmniSci, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "Execute.h"
#include "WindowContext.h"

llvm::Value* Executor::codegenWindowFunction(const Analyzer::WindowFunction* window_func,
                                             const size_t target_index,
                                             const CompilationOptions& co) {
  const auto window_func_context =
      WindowProjectNodeContext::get()->activateWindowFunctionContext(target_index);
  switch (window_func->getKind()) {
    case SqlWindowFunctionKind::ROW_NUMBER:
    case SqlWindowFunctionKind::RANK:
    case SqlWindowFunctionKind::DENSE_RANK:
    case SqlWindowFunctionKind::NTILE: {
      return cgen_state_->emitCall(
          "row_number_window_func",
          {ll_int(reinterpret_cast<const int64_t>(window_func_context->output())),
           posArg(nullptr)});
    }
    case SqlWindowFunctionKind::PERCENT_RANK:
    case SqlWindowFunctionKind::CUME_DIST: {
      return cgen_state_->emitCall(
          "percent_window_func",
          {ll_int(reinterpret_cast<const int64_t>(window_func_context->output())),
           posArg(nullptr)});
    }
    case SqlWindowFunctionKind::LAG:
    case SqlWindowFunctionKind::LEAD:
    case SqlWindowFunctionKind::FIRST_VALUE:
    case SqlWindowFunctionKind::LAST_VALUE: {
      CHECK(WindowProjectNodeContext::get());
      const auto& args = window_func->getArgs();
      CHECK(!args.empty());
      const auto arg_lvs = codegen(args.front().get(), true, co);
      CHECK_EQ(arg_lvs.size(), size_t(1));
      return arg_lvs.front();
    }
    case SqlWindowFunctionKind::AVG:
    case SqlWindowFunctionKind::MIN:
    case SqlWindowFunctionKind::MAX:
    case SqlWindowFunctionKind::SUM:
    case SqlWindowFunctionKind::COUNT: {
      return codegenWindowFunctionAggregate(window_func, window_func_context, co);
    }
    default: { LOG(FATAL) << "Invalid window function kind"; }
  }
  return nullptr;
}

namespace {

std::string get_window_agg_name(const SqlWindowFunctionKind kind,
                                const SQLTypeInfo& window_func_ti) {
  std::string agg_name;
  switch (kind) {
    case SqlWindowFunctionKind::MIN: {
      agg_name = "agg_min";
      break;
    }
    case SqlWindowFunctionKind::MAX: {
      agg_name = "agg_max";
      break;
    }
    case SqlWindowFunctionKind::AVG:
    case SqlWindowFunctionKind::SUM: {
      agg_name = "agg_sum";
      break;
    }
    case SqlWindowFunctionKind::COUNT: {
      agg_name = "agg_count";
      break;
    }
    default: { LOG(FATAL) << "Invalid window function kind"; }
  }
  switch (window_func_ti.get_type()) {
    case kFLOAT: {
      agg_name += "_float";
      break;
    }
    case kDOUBLE: {
      agg_name += "_double";
      break;
    }
    default: { break; }
  }
  return agg_name;
}

}  // namespace

llvm::Value* Executor::codegenWindowFunctionAggregate(
    const Analyzer::WindowFunction* window_func,
    const WindowFunctionContext* window_func_context,
    const CompilationOptions& co) {
  const auto reset_state_false_bb =
      codegenWindowResetStateControlFlow(window_func_context);
  const auto pi64_type =
      llvm::PointerType::get(get_int_type(64, cgen_state_->context_), 0);
  const auto aggregate_state_i64 =
      ll_int(reinterpret_cast<const int64_t>(window_func_context->aggregateState()));
  auto aggregate_state =
      cgen_state_->ir_builder_.CreateIntToPtr(aggregate_state_i64, pi64_type);
  const auto& args = window_func->getArgs();
  const auto& window_func_ti =
      ((window_func->getKind() == SqlWindowFunctionKind::COUNT && !args.empty()) ||
       window_func->getKind() == SqlWindowFunctionKind::AVG)
          ? args.front()->get_type_info()
          : window_func->get_type_info();
  const auto window_func_null_val = window_func_ti.is_fp()
                                        ? inlineFpNull(window_func_ti)
                                        : castToTypeIn(inlineIntNull(window_func_ti), 64);
  codegenWindowFunctionStateInit(
      window_func->getKind(), aggregate_state, window_func_null_val, window_func_ti);
  if (window_func->getKind() == SqlWindowFunctionKind::AVG) {
    const auto count_zero = ll_int(int64_t(0));
    const auto aggregate_state_count_i64 = ll_int(
        reinterpret_cast<const int64_t>(window_func_context->aggregateStateCount()));
    auto aggregate_state_count =
        cgen_state_->ir_builder_.CreateIntToPtr(aggregate_state_count_i64, pi64_type);
    cgen_state_->emitCall("agg_id", {aggregate_state_count, count_zero});
  }
  cgen_state_->ir_builder_.CreateBr(reset_state_false_bb);
  cgen_state_->ir_builder_.SetInsertPoint(reset_state_false_bb);
  CHECK(WindowProjectNodeContext::get());
  return codegenWindowFunctionAggregateCalls(window_func,
                                             window_func_context,
                                             co,
                                             aggregate_state,
                                             window_func_null_val,
                                             window_func_ti);
}

llvm::BasicBlock* Executor::codegenWindowResetStateControlFlow(
    const WindowFunctionContext* window_func_context) {
  const auto bitset =
      ll_int(reinterpret_cast<const int64_t>(window_func_context->partitionStart()));
  const auto min_val = ll_int(int64_t(0));
  const auto max_val = ll_int(window_func_context->elementCount() - 1);
  const auto null_val = ll_int(inline_int_null_value<int64_t>());
  const auto null_bool_val = ll_int<int8_t>(inline_int_null_value<int8_t>());
  const auto reset_state = toBool(cgen_state_->emitCall(
      "bit_is_set",
      {bitset, posArg(nullptr), min_val, max_val, null_val, null_bool_val}));
  const auto reset_state_true_bb = llvm::BasicBlock::Create(
      cgen_state_->context_, "reset_state.true", cgen_state_->row_func_);
  const auto reset_state_false_bb = llvm::BasicBlock::Create(
      cgen_state_->context_, "reset_state.false", cgen_state_->row_func_);
  cgen_state_->ir_builder_.CreateCondBr(
      reset_state, reset_state_true_bb, reset_state_false_bb);
  cgen_state_->ir_builder_.SetInsertPoint(reset_state_true_bb);
  return reset_state_false_bb;
}

void Executor::codegenWindowFunctionStateInit(const SqlWindowFunctionKind kind,
                                              llvm::Value* aggregate_state,
                                              llvm::Value* window_func_null_val,
                                              const SQLTypeInfo& window_func_ti) {
  llvm::Value* window_func_init_val;
  if (kind == SqlWindowFunctionKind::COUNT) {
    switch (window_func_ti.get_type()) {
      case kFLOAT: {
        window_func_init_val = ll_fp(float(0));
        break;
      }
      case kDOUBLE: {
        window_func_init_val = ll_fp(double(0));
        break;
      }
      default: {
        window_func_init_val = ll_int(int64_t(0));
        break;
      }
    }
  } else {
    window_func_init_val = window_func_null_val;
  }
  const auto pi32_type =
      llvm::PointerType::get(get_int_type(32, cgen_state_->context_), 0);
  switch (window_func_ti.get_type()) {
    case kDOUBLE: {
      cgen_state_->emitCall("agg_id_double", {aggregate_state, window_func_init_val});
      break;
    }
    case kFLOAT: {
      aggregate_state =
          cgen_state_->ir_builder_.CreateBitCast(aggregate_state, pi32_type);
      cgen_state_->emitCall("agg_id_float", {aggregate_state, window_func_init_val});
      break;
    }
    default: {
      cgen_state_->emitCall("agg_id", {aggregate_state, window_func_init_val});
      break;
    }
  }
}

llvm::Value* Executor::codegenWindowFunctionAggregateCalls(
    const Analyzer::WindowFunction* window_func,
    const WindowFunctionContext* window_func_context,
    const CompilationOptions& co,
    llvm::Value* aggregate_state,
    llvm::Value* window_func_null_val,
    const SQLTypeInfo& window_func_ti) {
  const auto& args = window_func->getArgs();
  llvm::Value* crt_val;
  if (args.empty()) {
    CHECK(window_func->getKind() == SqlWindowFunctionKind::COUNT);
    crt_val = ll_int(int64_t(1));
  } else {
    const auto arg_lvs = codegen(args.front().get(), true, co);
    CHECK_EQ(arg_lvs.size(), size_t(1));
    crt_val = window_func_ti.get_type() == kFLOAT ? arg_lvs.front()
                                                  : castToTypeIn(arg_lvs.front(), 64);
  }
  const auto agg_name = get_window_agg_name(window_func->getKind(), window_func_ti);
  llvm::Value* multiplicity_lv = nullptr;
  if (args.empty()) {
    cgen_state_->emitCall(agg_name, {aggregate_state, crt_val});
  } else {
    std::vector<llvm::Value*> args{aggregate_state, crt_val, window_func_null_val};
    if (window_function_requires_multiplicity(window_func->getKind())) {
      multiplicity_lv = codegenWindowAggregateCallWithMultiplicity(
          agg_name, window_func_null_val, args, window_func_context);
    } else {
      cgen_state_->emitCall(agg_name + "_skip_val", args);
    }
  }
  if (window_func->getKind() == SqlWindowFunctionKind::AVG) {
    return codegenWindowAvgEpilogue(window_func_context,
                                    crt_val,
                                    aggregate_state,
                                    window_func_null_val,
                                    multiplicity_lv,
                                    window_func_ti);
  }
  if (window_func->getKind() == SqlWindowFunctionKind::COUNT) {
    return cgen_state_->ir_builder_.CreateLoad(aggregate_state);
  }
  switch (window_func_ti.get_type()) {
    case kFLOAT: {
      return cgen_state_->emitCall("load_float", {aggregate_state});
    }
    case kDOUBLE: {
      return cgen_state_->emitCall("load_double", {aggregate_state});
    }
    default: { return cgen_state_->ir_builder_.CreateLoad(aggregate_state); }
  }
}

llvm::Value* Executor::codegenWindowAggregateCallWithMultiplicity(
    const std::string& agg_name,
    llvm::Value* window_func_null_val,
    const std::vector<llvm::Value*>& args,
    const WindowFunctionContext* window_func_context) {
  const auto pi32_type =
      llvm::PointerType::get(get_int_type(32, cgen_state_->context_), 0);
  const auto multiplicities_lv = cgen_state_->ir_builder_.CreateIntToPtr(
      ll_int(reinterpret_cast<const uint64_t>(window_func_context->multiplicities())),
      pi32_type);
  llvm::Value* multiplicity_lv = cgen_state_->ir_builder_.CreateLoad(
      cgen_state_->ir_builder_.CreateGEP(multiplicities_lv, posArg(nullptr)));
  auto args_with_multiplicity = args;
  args_with_multiplicity.push_back(multiplicity_lv);
  cgen_state_->emitCall(agg_name + "_skip_val_rep", args_with_multiplicity);
  return multiplicity_lv;
}

llvm::Value* Executor::codegenWindowAvgEpilogue(
    const WindowFunctionContext* window_func_context,
    llvm::Value* crt_val,
    llvm::Value* aggregate_state,
    llvm::Value* window_func_null_val,
    llvm::Value* multiplicity_lv,
    const SQLTypeInfo& window_func_ti) {
  const auto pi32_type =
      llvm::PointerType::get(get_int_type(32, cgen_state_->context_), 0);
  const auto pi64_type =
      llvm::PointerType::get(get_int_type(64, cgen_state_->context_), 0);
  const auto aggregate_state_type =
      window_func_ti.get_type() == kFLOAT ? pi32_type : pi64_type;
  const auto aggregate_state_count_i64 =
      ll_int(reinterpret_cast<const int64_t>(window_func_context->aggregateStateCount()));
  auto aggregate_state_count = cgen_state_->ir_builder_.CreateIntToPtr(
      aggregate_state_count_i64, aggregate_state_type);
  std::string agg_count_func_name = "agg_count";
  switch (window_func_ti.get_type()) {
    case kFLOAT: {
      agg_count_func_name += "_float";
      break;
    }
    case kDOUBLE: {
      agg_count_func_name += "_double";
      break;
    }
    default: { break; }
  }
  agg_count_func_name += "_skip_val_rep";
  cgen_state_->emitCall(
      agg_count_func_name,
      {aggregate_state_count, crt_val, window_func_null_val, multiplicity_lv});
  const auto double_null_lv = inlineFpNull(SQLTypeInfo(kDOUBLE));
  switch (window_func_ti.get_type()) {
    case kFLOAT: {
      return cgen_state_->emitCall(
          "load_avg_float", {aggregate_state, aggregate_state_count, double_null_lv});
    }
    case kDOUBLE: {
      return cgen_state_->emitCall(
          "load_avg_double", {aggregate_state, aggregate_state_count, double_null_lv});
    }
    default: {
      return cgen_state_->emitCall(
          "load_avg_int", {aggregate_state, aggregate_state_count, double_null_lv});
    }
  }
}
