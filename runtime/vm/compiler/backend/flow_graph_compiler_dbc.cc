// Copyright (c) 2016, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#include "vm/globals.h"  // Needed here to get TARGET_ARCH_DBC.
#if defined(TARGET_ARCH_DBC)

#include "vm/compiler/backend/flow_graph_compiler.h"

#include "vm/ast_printer.h"
#include "vm/compiler/backend/il_printer.h"
#include "vm/compiler/backend/locations.h"
#include "vm/compiler/jit/compiler.h"
#include "vm/cpu.h"
#include "vm/dart_entry.h"
#include "vm/deopt_instructions.h"
#include "vm/instructions.h"
#include "vm/object_store.h"
#include "vm/parser.h"
#include "vm/simulator.h"
#include "vm/stack_frame.h"
#include "vm/stub_code.h"
#include "vm/symbols.h"

namespace dart {

DEFINE_FLAG(bool, trap_on_deoptimization, false, "Trap on deoptimization.");
DEFINE_FLAG(bool, unbox_mints, true, "Optimize 64-bit integer arithmetic.");
DEFINE_FLAG(bool, unbox_doubles, true, "Optimize double arithmetic.");
DECLARE_FLAG(bool, enable_simd_inline);
DECLARE_FLAG(charp, optimization_filter);

FlowGraphCompiler::~FlowGraphCompiler() {
  // BlockInfos are zone-allocated, so their destructors are not called.
  // Verify the labels explicitly here.
  for (int i = 0; i < block_info_.length(); ++i) {
    ASSERT(!block_info_[i]->jump_label()->IsLinked());
  }
}

bool FlowGraphCompiler::SupportsUnboxedDoubles() {
#if defined(ARCH_IS_64_BIT)
  return true;
#else
  // We use 64-bit wide stack slots to unbox doubles.
  return false;
#endif
}

bool FlowGraphCompiler::SupportsUnboxedInt64() {
  return false;
}

bool FlowGraphCompiler::SupportsUnboxedSimd128() {
  return false;
}

bool FlowGraphCompiler::SupportsHardwareDivision() {
  return true;
}

bool FlowGraphCompiler::CanConvertInt64ToDouble() {
  return false;
}

void FlowGraphCompiler::EnterIntrinsicMode() {
  ASSERT(!intrinsic_mode());
  intrinsic_mode_ = true;
}

void FlowGraphCompiler::ExitIntrinsicMode() {
  ASSERT(intrinsic_mode());
  intrinsic_mode_ = false;
}

RawTypedData* CompilerDeoptInfo::CreateDeoptInfo(FlowGraphCompiler* compiler,
                                                 DeoptInfoBuilder* builder,
                                                 const Array& deopt_table) {
  if (deopt_env_ == NULL) {
    ++builder->current_info_number_;
    return TypedData::null();
  }

  intptr_t stack_height = compiler->StackSize();
  AllocateIncomingParametersRecursive(deopt_env_, &stack_height);

  intptr_t slot_ix = 0;
  Environment* current = deopt_env_;

  // Emit all kMaterializeObject instructions describing objects to be
  // materialized on the deoptimization as a prefix to the deoptimization info.
  EmitMaterializations(deopt_env_, builder);

  // The real frame starts here.
  builder->MarkFrameStart();

  Zone* zone = compiler->zone();

  builder->AddCallerFp(slot_ix++);
  builder->AddReturnAddress(current->function(), deopt_id(), slot_ix++);
  builder->AddPcMarker(Function::ZoneHandle(zone), slot_ix++);
  builder->AddConstant(Function::ZoneHandle(zone), slot_ix++);

  // Emit all values that are needed for materialization as a part of the
  // expression stack for the bottom-most frame. This guarantees that GC
  // will be able to find them during materialization.
  slot_ix = builder->EmitMaterializationArguments(slot_ix);

  if (lazy_deopt_with_result_) {
    ASSERT(reason() == ICData::kDeoptAtCall);
    builder->AddCopy(
        NULL,
        Location::StackSlot(
            compiler_frame_layout.FrameSlotForVariableIndex(-stack_height)),
        slot_ix++);
  }

  // For the innermost environment, set outgoing arguments and the locals.
  for (intptr_t i = current->Length() - 1;
       i >= current->fixed_parameter_count(); i--) {
    builder->AddCopy(current->ValueAt(i), current->LocationAt(i), slot_ix++);
  }

  builder->AddCallerFp(slot_ix++);

  Environment* previous = current;
  current = current->outer();
  while (current != NULL) {
    // For any outer environment the deopt id is that of the call instruction
    // which is recorded in the outer environment.
    builder->AddReturnAddress(current->function(),
                              DeoptId::ToDeoptAfter(current->deopt_id()),
                              slot_ix++);

    builder->AddPcMarker(previous->function(), slot_ix++);
    builder->AddConstant(previous->function(), slot_ix++);

    // The values of outgoing arguments can be changed from the inlined call so
    // we must read them from the previous environment.
    for (intptr_t i = previous->fixed_parameter_count() - 1; i >= 0; i--) {
      builder->AddCopy(previous->ValueAt(i), previous->LocationAt(i),
                       slot_ix++);
    }

    // Set the locals, note that outgoing arguments are not in the environment.
    for (intptr_t i = current->Length() - 1;
         i >= current->fixed_parameter_count(); i--) {
      builder->AddCopy(current->ValueAt(i), current->LocationAt(i), slot_ix++);
    }

    builder->AddCallerFp(slot_ix++);

    // Iterate on the outer environment.
    previous = current;
    current = current->outer();
  }
  // The previous pointer is now the outermost environment.
  ASSERT(previous != NULL);

  // For the outermost environment, set caller PC.
  builder->AddCallerPc(slot_ix++);

  builder->AddPcMarker(previous->function(), slot_ix++);
  builder->AddConstant(previous->function(), slot_ix++);

  // For the outermost environment, set the incoming arguments.
  for (intptr_t i = previous->fixed_parameter_count() - 1; i >= 0; i--) {
    builder->AddCopy(previous->ValueAt(i), previous->LocationAt(i), slot_ix++);
  }

  return builder->CreateDeoptInfo(deopt_table);
}

void FlowGraphCompiler::RecordAfterCallHelper(TokenPosition token_pos,
                                              intptr_t deopt_id,
                                              intptr_t argument_count,
                                              CallResult result,
                                              LocationSummary* locs) {
  RecordSafepoint(locs);
  // Marks either the continuation point in unoptimized code or the
  // deoptimization point in optimized code, after call.
  const intptr_t deopt_id_after = DeoptId::ToDeoptAfter(deopt_id);
  if (is_optimizing()) {
    // Return/ReturnTOS instruction drops incoming arguments so
    // we have to drop outgoing arguments from the innermost environment.
    // On all other architectures caller drops outgoing arguments itself
    // hence the difference.
    pending_deoptimization_env_->DropArguments(argument_count);
    CompilerDeoptInfo* info = AddDeoptIndexAtCall(deopt_id_after);
    if (result == kHasResult) {
      info->mark_lazy_deopt_with_result();
    }
    // This descriptor is needed for exception handling in optimized code.
    AddCurrentDescriptor(RawPcDescriptors::kOther, deopt_id_after, token_pos);
  } else {
    // Add deoptimization continuation point after the call and before the
    // arguments are removed.
    AddCurrentDescriptor(RawPcDescriptors::kDeopt, deopt_id_after, token_pos);
  }
}

void FlowGraphCompiler::RecordAfterCall(Instruction* instr, CallResult result) {
  RecordAfterCallHelper(instr->token_pos(), instr->deopt_id(),
                        instr->ArgumentCount(), result, instr->locs());
}

void CompilerDeoptInfoWithStub::GenerateCode(FlowGraphCompiler* compiler,
                                             intptr_t stub_ix) {
  UNREACHABLE();
}

#define __ assembler()->

void FlowGraphCompiler::GenerateAssertAssignable(TokenPosition token_pos,
                                                 intptr_t deopt_id,
                                                 const AbstractType& dst_type,
                                                 const String& dst_name,
                                                 LocationSummary* locs) {
  SubtypeTestCache& test_cache = SubtypeTestCache::Handle();
  if (!dst_type.IsVoidType() && dst_type.IsInstantiated()) {
    test_cache = SubtypeTestCache::New();
  } else if (!dst_type.IsInstantiated() &&
             (dst_type.IsTypeParameter() || dst_type.IsType())) {
    test_cache = SubtypeTestCache::New();
  }

  if (is_optimizing()) {
    __ Push(locs->in(0).reg());  // Instance.
    __ Push(locs->in(1).reg());  // Instantiator type arguments.
    __ Push(locs->in(2).reg());  // Function type arguments.
  }
  __ PushConstant(dst_type);
  __ PushConstant(dst_name);

  if (dst_type.IsMalformedOrMalbounded()) {
    __ BadTypeError();
  } else {
    bool may_be_smi = false;
    if (!dst_type.IsVoidType() && dst_type.IsInstantiated()) {
      const Class& type_class = Class::Handle(zone(), dst_type.type_class());
      if (type_class.NumTypeArguments() == 0) {
        const Class& smi_class = Class::Handle(zone(), Smi::Class());
        may_be_smi = smi_class.IsSubtypeOf(
            TypeArguments::Handle(zone()), type_class,
            TypeArguments::Handle(zone()), NULL, NULL, Heap::kOld);
      }
    }
    __ AssertAssignable(may_be_smi ? 1 : 0, __ AddConstant(test_cache));
  }

  if (is_optimizing()) {
    // Register allocator does not think that our first input (also used as
    // output) needs to be kept alive across the call because that is how code
    // is written on other platforms (where registers are always spilled across
    // the call): inputs are consumed by operation and output is produced so
    // neither are alive at the safepoint.
    // We have to mark the slot alive manually to ensure that GC
    // visits it.
    locs->SetStackBit(locs->out(0).reg());
  }
  AddCurrentDescriptor(RawPcDescriptors::kOther, deopt_id, token_pos);
  const intptr_t kArgCount = 0;
  RecordAfterCallHelper(token_pos, deopt_id, kArgCount,
                        FlowGraphCompiler::kHasResult, locs);
  if (is_optimizing()) {
    // Assert assignable keeps the instance on the stack as the result,
    // all other arguments are popped.
    ASSERT(locs->out(0).reg() == locs->in(0).reg());
    __ Drop1();
  }
}

void FlowGraphCompiler::EmitInstructionEpilogue(Instruction* instr) {
  if (!is_optimizing()) {
    Definition* defn = instr->AsDefinition();
    if ((defn != NULL) && (defn->tag() != Instruction::kPushArgument) &&
        (defn->tag() != Instruction::kStoreIndexed) &&
        (defn->tag() != Instruction::kStoreStaticField) &&
        (defn->tag() != Instruction::kStoreLocal) &&
        (defn->tag() != Instruction::kStoreInstanceField) &&
        (defn->tag() != Instruction::kDropTemps) && !defn->HasTemp()) {
      __ Drop1();
    }
  }
}

void FlowGraphCompiler::GenerateInlinedGetter(intptr_t offset) {
  __ Move(0, -(1 + compiler_frame_layout.param_end_from_fp));
  ASSERT(offset % kWordSize == 0);
  if (Utils::IsInt(8, offset / kWordSize)) {
    __ LoadField(0, 0, offset / kWordSize);
  } else {
    __ LoadFieldExt(0, 0);
    __ Nop(offset / kWordSize);
  }
  __ Return(0);
}

void FlowGraphCompiler::GenerateInlinedSetter(intptr_t offset) {
  __ Move(0, -(2 + compiler_frame_layout.param_end_from_fp));
  __ Move(1, -(1 + compiler_frame_layout.param_end_from_fp));
  ASSERT(offset % kWordSize == 0);
  if (Utils::IsInt(8, offset / kWordSize)) {
    __ StoreField(0, offset / kWordSize, 1);
  } else {
    __ StoreFieldExt(0, 1);
    __ Nop(offset / kWordSize);
  }
  __ LoadConstant(0, Object::Handle());
  __ Return(0);
}

void FlowGraphCompiler::EmitFrameEntry() {
  const Function& function = parsed_function().function();
  const intptr_t num_fixed_params = function.num_fixed_parameters();
  const int num_locals = parsed_function().num_stack_locals();

  if (CanOptimizeFunction() && function.IsOptimizable() &&
      (!is_optimizing() || may_reoptimize())) {
    __ HotCheck(!is_optimizing(), GetOptimizationThreshold());
  }

  if (is_optimizing()) {
    __ EntryOptimized(num_fixed_params,
                      flow_graph_.graph_entry()->spill_slot_count());
  } else {
    __ Entry(num_locals);
  }

  if (!is_optimizing()) {
    if (parsed_function().has_arg_desc_var()) {
      // TODO(kustermann): If dbc simulator put the args_desc_ into the
      // _special_regs, we could replace these 3 with the MoveSpecial bytecode.
      const intptr_t slot_index = compiler_frame_layout.FrameSlotForVariable(
          parsed_function().arg_desc_var());
      __ LoadArgDescriptor();
      __ StoreLocal(LocalVarIndex(0, slot_index));
      __ Drop(1);
    }
  }
}

void FlowGraphCompiler::CompileGraph() {
  InitCompiler();

  if (TryIntrinsify()) {
    // Skip regular code generation.
    return;
  }

  EmitFrameEntry();
  VisitBlocks();
}

uint16_t FlowGraphCompiler::ToEmbeddableCid(intptr_t cid,
                                            Instruction* instruction) {
  if (!Utils::IsUint(16, cid)) {
    instruction->Unsupported(this);
    UNREACHABLE();
  }
  return static_cast<uint16_t>(cid);
}

#undef __
#define __ compiler_->assembler()->

void ParallelMoveResolver::EmitMove(int index) {
  MoveOperands* move = moves_[index];
  const Location source = move->src();
  const Location destination = move->dest();
  if (source.IsStackSlot() && destination.IsRegister()) {
    // Only allow access to the arguments (which have in the non-inverted stack
    // positive indices).
    ASSERT(source.base_reg() == FPREG);
    ASSERT(source.stack_index() > compiler_frame_layout.param_end_from_fp);
    __ Move(destination.reg(), -source.stack_index());
  } else if (source.IsRegister() && destination.IsRegister()) {
    __ Move(destination.reg(), source.reg());
  } else if (source.IsArgsDescRegister()) {
    ASSERT(destination.IsRegister());
    __ LoadArgDescriptorOpt(destination.reg());
  } else if (source.IsExceptionRegister()) {
    ASSERT(destination.IsRegister());
    __ MoveSpecial(destination.reg(), Simulator::kExceptionSpecialIndex);
  } else if (source.IsStackTraceRegister()) {
    ASSERT(destination.IsRegister());
    __ MoveSpecial(destination.reg(), Simulator::kStackTraceSpecialIndex);
  } else if (source.IsConstant() && destination.IsRegister()) {
    if (source.constant_instruction()->representation() == kUnboxedDouble) {
      const Register result = destination.reg();
      const Object& constant = source.constant();
      if (Utils::DoublesBitEqual(Double::Cast(constant).value(), 0.0)) {
        __ BitXor(result, result, result);
      } else {
        __ LoadConstant(result, constant);
        __ UnboxDouble(result, result);
      }
    } else {
      __ LoadConstant(destination.reg(), source.constant());
    }
  } else {
    compiler_->Bailout("Unsupported move");
    UNREACHABLE();
  }

  move->Eliminate();
}

void ParallelMoveResolver::EmitSwap(int index) {
  MoveOperands* move = moves_[index];
  const Location source = move->src();
  const Location destination = move->dest();
  ASSERT(source.IsRegister() && destination.IsRegister());
  __ Swap(destination.reg(), source.reg());

  // The swap of source and destination has executed a move from source to
  // destination.
  move->Eliminate();

  // Any unperformed (including pending) move with a source of either
  // this move's source or destination needs to have their source
  // changed to reflect the state of affairs after the swap.
  for (int i = 0; i < moves_.length(); ++i) {
    const MoveOperands& other_move = *moves_[i];
    if (other_move.Blocks(source)) {
      moves_[i]->set_src(destination);
    } else if (other_move.Blocks(destination)) {
      moves_[i]->set_src(source);
    }
  }
}

void ParallelMoveResolver::MoveMemoryToMemory(const Address& dst,
                                              const Address& src) {
  UNREACHABLE();
}

// Do not call or implement this function. Instead, use the form below that
// uses an offset from the frame pointer instead of an Address.
void ParallelMoveResolver::Exchange(Register reg, const Address& mem) {
  UNREACHABLE();
}

// Do not call or implement this function. Instead, use the form below that
// uses offsets from the frame pointer instead of Addresses.
void ParallelMoveResolver::Exchange(const Address& mem1, const Address& mem2) {
  UNREACHABLE();
}

void ParallelMoveResolver::Exchange(Register reg,
                                    Register base_reg,
                                    intptr_t stack_offset) {
  UNIMPLEMENTED();
}

void ParallelMoveResolver::Exchange(Register base_reg1,
                                    intptr_t stack_offset1,
                                    Register base_reg2,
                                    intptr_t stack_offset2) {
  UNIMPLEMENTED();
}

void ParallelMoveResolver::SpillScratch(Register reg) {
  UNIMPLEMENTED();
}

void ParallelMoveResolver::RestoreScratch(Register reg) {
  UNIMPLEMENTED();
}

void ParallelMoveResolver::SpillFpuScratch(FpuRegister reg) {
  UNIMPLEMENTED();
}

void ParallelMoveResolver::RestoreFpuScratch(FpuRegister reg) {
  UNIMPLEMENTED();
}

#undef __

}  // namespace dart

#endif  // defined TARGET_ARCH_DBC
