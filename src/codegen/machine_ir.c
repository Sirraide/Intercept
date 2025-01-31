#include <ast.h>
#include <codegen.h>
#include <codegen/codegen_forward.h>
#include <codegen/machine_ir.h>
#include <ir/ir.h>
#include <stdarg.h>
#include <utils.h>
#include <vector.h>

MIROperand mir_op_function(MIRFunction *f) {
  ASSERT(f, "Cannot create MIR function operand from NULL MIR function.");
  MIROperand out = {0};
  out.kind = MIR_OP_FUNCTION;
  out.value.function = f;
  return out;
}

MIROperand mir_op_block(MIRBlock *block) {
  ASSERT(block, "Cannot create MIR block operand from NULL MIR block.");
  MIROperand out = {0};
  out.kind = MIR_OP_BLOCK;
  out.value.block = block;
  return out;
}

MIROperand mir_op_reference(MIRInstruction *inst) {
  ASSERT(inst, "Invalid argument");
  MIROperand out = {0};
  out.kind = MIR_OP_REGISTER;
  while (inst->lowered) inst = inst->lowered;
  out.value.reg.value = inst->reg;
  if (inst->origin) out.value.reg.size = (uint16_t)type_sizeof(ir_typeof(inst->origin));
  return out;
}

/// Create a new MIROperand referencing a new stack allocation of given
/// size, and also add a frame object for it to the given function.
/// NOTE: Only used when referencing a local, not when creating!
MIROperand mir_op_local_ref(MIRFunction *function, usz size) {
  DBGASSERT(function, "Invalid argument");
  DBGASSERT(size, "Zero size stack allocation...");

  MIROperand out = {0};
  out.kind = MIR_OP_LOCAL_REF;
  out.value.local_ref = function->frame_objects.size;

  MIRFrameObject frame_obj = { size, (usz)-1, -1 };

  vector_push(function->frame_objects, frame_obj);
  return out;
}

/// Create a new MIROperand referencing a new stack allocation of given
/// size, and also add a frame object for it to the given function.
/// NOTE: Only used when referencing a local, not when creating!
MIROperand mir_op_local_ref_fo(MIRFunction *function, MIRFrameObject *fo) {
  DBGASSERT(function, "Invalid argument");
  DBGASSERT(fo, "Invalid argument");

  MIROperand out = {0};
  out.kind = MIR_OP_LOCAL_REF;

  if (fo->lowered != (usz)-1) {
    DBGASSERT(fo->lowered < function->frame_objects.size,
              "FrameObject lowered index is larger than amount of frame objects present!");
    out.value.local_ref = fo->lowered;
    return out;
  }

  out.value.local_ref = function->frame_objects.size;
  fo->lowered = function->frame_objects.size;

  MIRFrameObject frame_obj = { fo->size, (usz)-1, -1 };
  vector_push(function->frame_objects, frame_obj);


  return out;
}

/// Create a new MIROperand referencing the given stack allocation, and
/// also add a frame object for it to the given function.
/// NOTE: Only used when referencing a local, not when creating!
MIROperand mir_op_local_ref_ir(MIRFunction *function, IRInstruction *alloca) {
  DBGASSERT(function, "Invalid argument");
  DBGASSERT(alloca, "Invalid argument");

  MIROperand out = {0};
  out.kind = MIR_OP_LOCAL_REF;

  /// Alloca has already been referenced; return the index of the
  /// existing stack frame object that references it.
  // NOTE: Relies on every alloca->offset being -1 upon input.
  if (ir_alloca_offset(alloca) != (usz)-1) {
    out.value.local_ref = ir_alloca_offset(alloca);
    return out;
  }
  /// Otherwise, we need to add a new frame object.
  out.value.local_ref = function->frame_objects.size;
  ir_alloca_offset(alloca, out.value.local_ref);
  MIRFrameObject frame_obj = { ir_alloca_size(alloca), (usz)-1, -1 };
  vector_push(function->frame_objects, frame_obj);

  return out;
}

MIROperand mir_op_static_ref(IRInstruction *static_ref) {
  DBGASSERT(static_ref, "Invalid argument");

  MIROperand out = {0};
  out.kind = MIR_OP_STATIC_REF;
  out.value.static_ref = static_ref;

  return out;
}

MIROperand mir_op_reference_ir(MIRFunction *function, IRInstruction *inst) {
  ASSERT(inst, "Invalid argument");
  if (ir_register(inst)) return mir_op_register(
    ir_register(inst),
    (uint16_t) type_sizeof(ir_typeof(inst)),
    false
  );

  /// Inline operands if possible.
  switch (ir_kind(inst)) {
    default: break;
    case IR_IMMEDIATE: return mir_op_immediate((i64)ir_imm(inst));
    case IR_ALLOCA: return mir_op_local_ref_ir(function, inst);
    case IR_STATIC_REF: return mir_op_static_ref(inst);
    case IR_FUNC_REF: return mir_op_function(ir_mir(ir_func_ref_func(inst)));
  }

  if (!ir_mir(inst)) {
    ir_print_instruction(stdout, inst);
    ICE("Must translate IRInstruction into MIR before taking reference to it.");
  }

  return mir_op_reference(ir_mir(inst));
}

MIROperand mir_op_immediate(int64_t imm) {
  MIROperand out = {0};
  out.kind = MIR_OP_IMMEDIATE;
  out.value.imm = imm;
  return out;
}

MIROperand mir_op_name(const char *name) {
  MIROperand out = {0};
  out.kind = MIR_OP_NAME;
  out.value.name = name;
  return out;
}

MIROperand mir_op_register(RegisterDescriptor reg, uint16_t size, bool defining_use) {
  MIROperand out = {0};
  out.kind = MIR_OP_REGISTER;
  out.value.reg.value = reg;
  out.value.reg.size = size;
  out.value.reg.defining_use = defining_use;
  return out;
}

static bool mir_make_arch = false;
static size_t mir_alloc_id = 0;
static size_t mir_arch_alloc_id = MIR_ARCH_START;
MIRInstruction *mir_makenew(uint32_t opcode) {
  MIRInstruction *mir = calloc(1, sizeof(*mir));
  ASSERT(mir, "Memory allocation failure");
  if (mir_make_arch)
    mir->id = ++mir_arch_alloc_id;
  else mir->id = ++mir_alloc_id;
  mir->opcode = opcode;
  return mir;
}
MIRInstruction *mir_makecopy(MIRInstruction *original) {
  MIRInstruction *mir = mir_makenew(MIR_UNREACHABLE);
  mir->opcode = original->opcode;
  mir->operand_count = original->operand_count;
  if (mir->operand_count <= MIR_OPERAND_SSO_THRESHOLD)
    memcpy(mir->operands.arr, original->operands.arr, sizeof(mir->operands.arr));
  else vector_append(mir->operands.vec, original->operands.vec);
  mir->origin = original->origin;
  vector_append(mir->clobbers, original->clobbers);
  return mir;
}

/// Function is only needed to update instruction count. May pass NULL.
void mir_push_with_reg_into_block(MIRFunction *f, MIRBlock *block, MIRInstruction *mi, MIRRegister reg) {
  DBGASSERT(f, "Invalid argument");
  DBGASSERT(block, "Invalid argument");
  DBGASSERT(mi, "Invalid argument");
  vector_push(block->instructions, mi);
  mi->block = block;
  mi->reg = reg;
  if (f) f->inst_count++;
}
void mir_insert_instruction_with_reg(MIRBlock *bb, MIRInstruction *mi, usz index, MIRRegister reg) {
  DBGASSERT(bb, "Invalid argument");
  DBGASSERT(mi, "Invalid argument");
  vector_insert(bb->instructions, bb->instructions.data + index, mi);
  mi->block = bb;
  mi->reg = reg;
  if (bb->function) ++bb->function->inst_count;
}
void mir_remove_instruction(MIRInstruction *mi) {
  ASSERT(mi, "Invalid argument");
  ASSERT(mi->block, "Cannot remove MIR instruction that has no block reference");
  vector_remove_element(mi->block->instructions, mi);
  if (mi->block->function) --mi->block->function->inst_count;
  mi->block = NULL;
}
void mir_insert_instruction(MIRBlock *bb, MIRInstruction *mi, usz index) {
  ASSERT(bb, "Invalid argument");
  if (bb->function) mir_insert_instruction_with_reg(bb, mi, index, bb->function->inst_count + (size_t)MIR_ARCH_START);
  else mir_insert_instruction_with_reg(bb, mi, index, mi->reg);
}
void mir_prepend_instruction(MIRFunction *f, MIRInstruction *mi) {
  ASSERT(f, "Invalid argument");
  ASSERT(f->blocks.size, "Function must have at least one block in order to prepend an instruction to it");
  mir_insert_instruction(vector_front(f->blocks), mi, 0);
}
void mir_append_instruction(MIRFunction *f, MIRInstruction *mi) {
  ASSERT(f, "Invalid argument");
  ASSERT(f->blocks.size, "Function must have at least one block in order to append an instruction to it");
  MIRBlock *bb = vector_back(f->blocks);
  mir_insert_instruction(bb, mi, bb->instructions.size - 1);
}

static void mir_push_into_block(MIRFunction *f, MIRBlock *block, MIRInstruction *mi) {
  if (mi->origin && ir_register(mi->origin))
    mir_push_with_reg_into_block(f, block, mi, ir_register(mi->origin));
  else mir_push_with_reg_into_block(f, block, mi, (MIRRegister)(f->inst_count + (size_t)MIR_ARCH_START));
}

void mir_push_with_reg(MIRFunction *f, MIRInstruction *mi, MIRRegister reg) {
  mir_push_with_reg_into_block(f, vector_back(f->blocks), mi, reg);
}

static void mir_push(MIRFunction *f, MIRInstruction *mi) {
  mir_push_with_reg(f, mi, (MIRRegister)(f->inst_count + (size_t)MIR_ARCH_START));
}

MIRFunction *mir_function(IRFunction *ir_f) {
  MIRFunction* f = calloc(1, sizeof(*f));
  f->origin = ir_f;
  f->name = string_dup(ir_name(ir_f));
  ir_mir(ir_f, f);
  return f;
}

MIRBlock *mir_block_makenew(MIRFunction *function, span name) {
  MIRBlock* bb = calloc(1, sizeof(*bb));
  bb->function = function;
  bb->name = string_dup(name);
  vector_push(function->blocks, bb);
  return bb;
}

MIRBlock *mir_block(MIRFunction *function, IRBlock *ir_bb) {
  MIRBlock *bb = mir_block_makenew(function, ir_name(ir_bb));
  bb->origin = ir_bb;
  ir_mir(ir_bb, bb);
  return bb;
}

MIRBlock *mir_block_copy(MIRFunction *function, MIRBlock *original) {
  ASSERT(function, "Invalid argument");
  ASSERT(original, "Invalid argument");
  MIRBlock *bb = mir_block_makenew(function, as_span(original->name));
  bb->origin = original->origin;
  bb->is_entry = original->is_entry;
  bb->is_exit = original->is_exit;
  original->lowered = bb;
  return bb;
}

MIRInstruction *mir_imm(int64_t imm) {
  MIRInstruction* mir = mir_makenew(MIR_IMMEDIATE);
  mir_add_op(mir, mir_op_immediate(imm));
  return mir;
}

MIRInstruction *mir_from_ir_copy(MIRFunction *function, IRInstruction *copy) {
  MIRInstruction *mir = mir_makenew(MIR_COPY);
  mir->origin = copy;
  mir_add_op(mir, mir_op_reference_ir(function, ir_operand(copy)));
  ir_mir(copy, mir);
  return mir;
}

/// Return non-zero iff given instruction needs a register.
static bool needs_register(IRInstruction *instruction) {
  STATIC_ASSERT(IR_COUNT == 40, "Exhaustively handle all instruction types");
  ASSERT(instruction);
  switch (ir_kind(instruction)) {
    case IR_LOAD:
    case IR_PHI:
    case IR_COPY:
    case IR_IMMEDIATE:
    case IR_INTRINSIC:
    case IR_CALL:
    case IR_REGISTER:
    case IR_NOT:
    case IR_ZERO_EXTEND:
    case IR_SIGN_EXTEND:
    case IR_TRUNCATE:
    case IR_BITCAST:
    ALL_BINARY_INSTRUCTION_CASES()
      return true;

    case IR_POISON:
      ICE("Refusing to codegen poison value");

    case IR_PARAMETER:
      ICE("Unlowered parameter instruction in register allocator");

    /// Allocas and static refs need a register iff they are actually used.
    case IR_ALLOCA:
    case IR_STATIC_REF:
    case IR_FUNC_REF:
      return ir_use_count(instruction);

    default:
      return false;
  }
}

/// Remove MIR instructions from given function that have an
/// MIR_IMMEDIATE or MIR_FUNC_REF opcode, as these are inlined into
/// operands with no load instruction required. The only reason we
/// include them at all is to satisfy `phi` nonsense, among other
/// things.
static void remove_inlined(MIRFunction *function) {
  MIRInstructionVector instructions_to_remove = {0};
  foreach_val (block, function->blocks) {
    // Gather immediate instructions to remove
    vector_clear(instructions_to_remove);
    foreach_val (instruction, block->instructions) {
      if (instruction->opcode != MIR_IMMEDIATE && instruction->opcode != MIR_FUNC_REF) continue;
      vector_push(instructions_to_remove, instruction);
    }
    // Remove gathered instructions
    foreach_val (to_remove, instructions_to_remove) {
      vector_remove_element(block->instructions, to_remove);
    }
  }
  vector_delete(instructions_to_remove);
}

/// For each argument of each phi instruction, add in a copy to the phi's virtual register.
static void phi2copy(MIRFunction *function) {
  IRBlock *last_block = NULL;
  MIRInstructionVector instructions_to_remove = {0};
  foreach_val (block, function->blocks) {
    vector_clear(instructions_to_remove);
    foreach_val (instruction, block->instructions) {
      if (instruction->opcode != MIR_PHI) continue;
      IRInstruction *phi = instruction->origin;
      ASSERT(ir_parent(phi) != last_block, "Multiple PHI instructions in a single block are not allowed!");
      last_block = ir_parent(phi);

      /// Single PHI argument means that we can replace it with a simple copy.
      usz args_count = ir_phi_args_count(phi);
      if (args_count == 1) {
        instruction->opcode = MIR_COPY;
        mir_op_clear(instruction);
        mir_add_op(instruction, mir_op_reference_ir(function, ir_phi_arg(phi, 0)->value));
        continue;
      }

      /// For each of the PHI arguments, we basically insert a copy.
      /// Where we insert it depends on some complicated factors
      /// that have to do with control flow.
      for (usz i = 0; i < args_count; i++) {
        STATIC_ASSERT(IR_COUNT == 40, "Handle all branch types");
        const IRPhiArgument *arg = ir_phi_arg(phi, i);
        IRInstruction *branch = ir_terminator(arg->block);
        switch (ir_kind(branch)) {
        /// If the predecessor returns or is unreachable, then the PHI
        /// is never going to be reached, so we can just ignore
        /// this argument.
        case IR_UNREACHABLE:
        case IR_RETURN: continue;

        /// For direct branches, we just insert the copy before the branch.
        case IR_BRANCH: {
          if (needs_register(arg->value)) {
            MIRInstruction *copy = mir_makenew(MIR_COPY);
            MIRInstruction *value_mir = ir_mir(arg->value);
            ASSERT(value_mir);
            ASSERT(value_mir->block);
            copy->block = value_mir->block;
            copy->reg = instruction->reg;
            mir_add_op(copy, mir_op_reference_ir(function, arg->value));
            // Insert copy before branch machine instruction.
            // mir_push_before(branch->machine_inst, copy);
            MIRInstructionVector *instructions = &value_mir->block->instructions;
            vector_insert(*instructions, instructions->data + (instructions->size - 1), copy);
          } else {
            print("\n\n%31Offending block%m:\n");
            ir_print_block(stdout, ir_parent(arg->value));
            ICE("Block ends with instruction that does not return value.");
          }
        } break;

        /// Indirect branches are a bit more complicated. We need to insert an
        /// additional block for the copy instruction and replace the branch
        /// to the phi block with a branch to that block.
        case IR_BRANCH_CONDITIONAL: {

          // Create a COPY of the argument into the MIR PHI's vreg.
          // When we eventually remove the MIR PHI, what will be left is a bunch
          // of copies into the same virtual register. RA can then fill this virtual
          // register in with a single register and boom our PHI is codegenned
          // properly.
          MIRInstruction *copy = mir_makenew(MIR_COPY);
          mir_add_op(copy, mir_op_reference_ir(function, arg->value));
          copy->reg = instruction->reg;

          // Possible FIXME: This relies on backend filling empty block
          // names with something.
          MIRBlock *critical_edge_trampoline = mir_block_makenew(function, literal_span(""));
          mir_push_into_block(function, critical_edge_trampoline, copy);
          // Branch to phi block from critical edge
          MIRInstruction *critical_edge_branch = mir_makenew(MIR_BRANCH);
          critical_edge_branch->block = instruction->block;
          mir_push_into_block(function, critical_edge_trampoline, critical_edge_branch);

          // The critical edge trampoline block is now complete. This
          // means we can replace the branch of the argument block to that
          // of this critical edge trampoline.

          // Condition is first operand, then the "then" branch, then "else".
          IRBlock *phi_parent = ir_parent(phi);
          MIRInstruction *branch_mir = ir_mir(branch);
          MIROperand *branch_then = mir_get_op(branch_mir, 1);
          MIROperand *branch_else = mir_get_op(branch_mir, 2);
          if (branch_then->value.block->origin == phi_parent)
            *branch_then = mir_op_block(critical_edge_trampoline);
          else {
            ASSERT(branch_else->value.block->origin == phi_parent,
                   "Branch to phi block is neither true nor false branch of conditional branch!");
            *branch_else = mir_op_block(critical_edge_trampoline);
          }
        } break;
        default: UNREACHABLE();
        }

        // Add index of phi instruction to list of instructions to remove.
        vector_push(instructions_to_remove, instruction);
      }

      foreach_val (to_remove, instructions_to_remove) {
        vector_remove_element(block->instructions, to_remove);
      }
    }
  }
  vector_delete(instructions_to_remove);
}

MIRFunctionVector mir_from_ir(CodegenContext *context) {
  MIRFunctionVector out = {0};
  // Forward function references require this.
  foreach_val (f, context->functions) {
    MIRFunction *function = mir_function(f);
    // Forward block references require this.
    FOREACH_BLOCK(bb, function->origin)
      (void)mir_block(function, bb);
    vector_push(out, function);
  }
  foreach_val (function, out) {
    // NOTE for devs: function->origin == IRFunction*
    if (!ir_func_is_definition(function->origin)) continue;

    ASSERT(function->blocks.size, "Zero blocks in non-extern MIRFunction... what have you done?!");
    // NOTE: This assumes the first block of the function is the entry
    // point; it may be smart to set the entry point within the IR,
    // that reordering optimisations may truly happen to any block.
    vector_front(function->blocks)->is_entry = true;

    foreach_val (mir_bb, function->blocks) {
      IRBlock *bb = mir_bb->origin;
      ASSERT(bb, "Origin of general MIR block not set (what gives?)");

      STATIC_ASSERT(IR_COUNT == 40, "Handle all IR instructions");
      FOREACH_INSTRUCTION(inst, bb) {
        switch (ir_kind(inst)) {

        case IR_POISON: ICE("Refusing to codegen poison value");

        case IR_IMMEDIATE: {
          MIRInstruction *mir = mir_makenew(MIR_IMMEDIATE);
          mir->origin = inst;
          ir_mir(inst, mir);
          mir_push_into_block(function, mir_bb, mir);
        } break;

        case IR_FUNC_REF: {
          MIRInstruction *mir = mir_makenew(MIR_FUNC_REF);
          ir_mir(inst, mir);
          mir->origin = inst;
          mir_add_op(mir, mir_op_reference_ir(function, inst));
          mir_push_into_block(function, mir_bb, mir);
        } break;

        case IR_REGISTER:
          break;

        case IR_PHI: {
          MIRInstruction *mir = mir_makenew(MIR_PHI);
          mir->origin = inst;
          ir_mir(inst, mir);
          mir_push_into_block(function, mir_bb, mir);
        } break;

        case IR_INTRINSIC: {
          MIRInstruction *mir = mir_makenew(MIR_INTRINSIC);
          mir->origin = inst;
          ir_mir(inst, mir);

          // Intrinsic kind
          mir_add_op(mir, mir_op_immediate(ir_intrinsic_kind(inst)));

          // Call arguments
          for(usz i = 0; i < ir_call_args_count(inst); i++)
            mir_add_op(mir, mir_op_reference_ir(function, ir_call_arg(inst, i)));

          mir_push_into_block(function, mir_bb, mir);
        } break;

        case IR_CALL: {
          MIRInstruction *mir = mir_makenew(MIR_CALL);
          mir->origin = inst;
          ir_mir(inst, mir);

          // Call target (destination)
          IRValue callee = ir_callee(inst);
          if (!ir_call_is_direct(inst)) mir_add_op(mir, mir_op_reference_ir(function, callee.inst));
          else mir_add_op(mir, mir_op_function(ir_mir(callee.func)));

          // Call arguments
          for (usz i = 0; i < ir_call_args_count(inst); i++)
            mir_add_op(mir, mir_op_reference_ir(function, ir_call_arg(inst, i)));

          mir_push_into_block(function, mir_bb, mir);
        } break;

        case IR_LOAD: {
          MIRInstruction *mir = mir_makenew(MIR_LOAD);
          mir->origin = inst;

          // Address of load
          MIROperand addr = mir_op_reference_ir(function, ir_operand(inst));
          mir_add_op(mir, addr);
          // Size of load (if needed)
          if (addr.kind == MIR_OP_REGISTER) {
            MIROperand size = mir_op_immediate((i64)type_sizeof(ir_typeof(inst)));
            mir_add_op(mir, size);
          }
          ir_mir(inst, mir);
          mir_push_into_block(function, mir_bb, mir);
        } break;

        case IR_NOT: FALLTHROUGH;
        case IR_BITCAST: {
          MIRInstruction *mir = mir_makenew((uint32_t)ir_kind(inst));
          mir->origin = inst;
          mir_add_op(mir, mir_op_reference_ir(function, ir_operand(inst)));
          ir_mir(inst, mir);
          mir_push_into_block(function, mir_bb, mir);
        } break;

        case IR_ZERO_EXTEND: FALLTHROUGH;
        case IR_SIGN_EXTEND: FALLTHROUGH;
        case IR_TRUNCATE: {
          MIRInstruction *mir = mir_makenew((uint32_t)ir_kind(inst));
          mir->origin = inst;
          // Thing to truncate
          mir_add_op(mir, mir_op_reference_ir(function, ir_operand(inst)));
          // Amount of bytes to truncate from
          mir_add_op(mir, mir_op_immediate((i64)type_sizeof(ir_typeof(ir_operand(inst)))));
          // Amount of bytes to truncate to
          mir_add_op(mir, mir_op_immediate((i64)type_sizeof(ir_typeof(inst))));
          ir_mir(inst, mir);
          mir_push_into_block(function, mir_bb, mir);
        } break;

        case IR_COPY: {
          MIRInstruction *mir = mir_from_ir_copy(function, inst);
          mir_push_into_block(function, mir_bb, mir);
        } break;

        case IR_RETURN: {
          MIRInstruction *mir = mir_makenew(MIR_RETURN);
          mir->origin = inst;
          if (ir_operand(inst)) mir_add_op(mir, mir_op_reference_ir(function, ir_operand(inst)));
          ir_mir(inst, mir);
          mir_push_into_block(function, mir_bb, mir);
          mir_bb->is_exit = true;
        } break;
        case IR_BRANCH: {
          MIRInstruction *mir = mir_makenew(MIR_BRANCH);
          MIRBlock *dest = ir_mir(ir_dest(inst));
          mir->origin = inst;
          mir_add_op(mir, mir_op_block(dest));
          ir_mir(inst, mir);
          mir_push_into_block(function, mir_bb, mir);
          // CFG
          vector_push(mir_bb->successors, dest);
          vector_push(dest->predecessors, mir_bb);
        } break;
        case IR_BRANCH_CONDITIONAL: {
          MIRInstruction *mir = mir_makenew(MIR_BRANCH_CONDITIONAL);
          MIRBlock *mir_then = ir_mir(ir_then(inst));
          MIRBlock *mir_else = ir_mir(ir_else(inst));
          mir->origin = inst;
          mir_add_op(mir, mir_op_reference_ir(function, ir_cond(inst)));
          mir_add_op(mir, mir_op_block(mir_then));
          mir_add_op(mir, mir_op_block(mir_else));
          ir_mir(inst, mir);
          mir_push_into_block(function, mir_bb, mir);
          // CFG
          vector_push(mir_bb->successors, mir_then);
          vector_push(mir_bb->successors, mir_else);
          vector_push(mir_then->predecessors, mir_bb);
          vector_push(mir_else->predecessors, mir_bb);
        } break;
        case IR_ADD:
        case IR_SUB:
        case IR_MUL:
        case IR_DIV:
        case IR_MOD:
        case IR_SHL:
        case IR_SAR:
        case IR_SHR:
        case IR_AND:
        case IR_OR:
        case IR_LT:
        case IR_LE:
        case IR_GT:
        case IR_GE:
        case IR_EQ:
        case IR_NE: {
          MIRInstruction *mir = mir_makenew((uint32_t)ir_kind(inst));
          mir->origin = inst;
          mir_add_op(mir, mir_op_reference_ir(function, ir_lhs(inst)));
          mir_add_op(mir, mir_op_reference_ir(function, ir_rhs(inst)));
          ir_mir(inst, mir);
          mir_push_into_block(function, mir_bb, mir);
        } break;
        case IR_STATIC_REF: {
          MIRInstruction *mir = mir_makenew((uint32_t)ir_kind(inst));
          ir_mir(inst, mir);
          mir->origin = inst;
          mir_add_op(mir, mir_op_reference_ir(function, inst));
          mir_push_into_block(function, mir_bb, mir);
        } break;
        case IR_STORE: {
          MIRInstruction *mir = mir_makenew(MIR_STORE);
          mir->origin = inst;
          MIROperand value = mir_op_reference_ir(function, ir_store_value(inst));
          MIROperand addr = mir_op_reference_ir(function, ir_store_addr(inst));
          mir_add_op(mir, value);
          mir_add_op(mir, addr);
          // Size of store (if needed)
          if (addr.kind == MIR_OP_REGISTER && value.kind == MIR_OP_IMMEDIATE)
            mir_add_op(mir, mir_op_immediate((i64)type_sizeof(ir_typeof(ir_store_value(inst)))));
          ir_mir(inst, mir);
          mir_push_into_block(function, mir_bb, mir);
        } break;
        case IR_ALLOCA: {
          MIRInstruction *mir = mir_makenew(MIR_ALLOCA);
          mir->origin = inst;
          ir_alloca_offset(inst, (usz)-1); // Implementation detail for referencing frame objects
          mir_add_op(mir, mir_op_local_ref_ir(function, inst));
          ir_mir(inst, mir);
          mir_push_into_block(function, mir_bb, mir);
        } break;

        case IR_UNREACHABLE: {
          MIRInstruction *mir = mir_makenew(MIR_UNREACHABLE);
          mir->origin = inst;
          ir_mir(inst, mir);
          mir_push_into_block(function, mir_bb, mir);
          mir_bb->is_exit = true;
        } break;

        case IR_PARAMETER:
        case IR_LIT_INTEGER:
        case IR_LIT_STRING:
        case IR_COUNT: UNREACHABLE();

        } // switch (ir_kind(inst))
      }
    }

    phi2copy(function);
    remove_inlined(function);
  }

  mir_make_arch = true;
  return out;
}

const char *mir_operand_kind_string(MIROperandKind opkind) {
  STATIC_ASSERT(MIR_OP_COUNT == 8, "Exhaustive handling of MIR operand kinds");
  switch (opkind) {
  case MIR_OP_NONE: return "none";
  case MIR_OP_REGISTER: return "register";
  case MIR_OP_IMMEDIATE: return "immediate";
  case MIR_OP_BLOCK: return "block";
  case MIR_OP_FUNCTION: return "function";
  case MIR_OP_STATIC_REF: return "static";
  case MIR_OP_LOCAL_REF: return "local";
  case MIR_OP_OP_REF: return "(isel)operand";
  case MIR_OP_INST_REF: return "(isel)instruction";
  default: break;
  }
  return "";
}

const char *mir_common_opcode_mnemonic(uint32_t opcode) {
  STATIC_ASSERT(MIR_COUNT == 39, "Exhaustive handling of MIRCommonOpcodes (string conversion)");
  switch ((MIROpcodeCommon)opcode) {
  case MIR_IMMEDIATE: return "m.immediate";
  case MIR_INTRINSIC: return "m.intrinsic";
  case MIR_CALL: return "m.call";
  case MIR_NOT: return "m.not";
  case MIR_ZERO_EXTEND: return "m.zero_extend";
  case MIR_SIGN_EXTEND: return "m.sign_extend";
  case MIR_TRUNCATE: return "m.truncate";
  case MIR_BITCAST: return "m.bitcast";
  case MIR_COPY: return "m.copy";
  case MIR_LOAD: return "m.load";
  case MIR_RETURN: return "m.return";
  case MIR_BRANCH: return "m.branch";
  case MIR_BRANCH_CONDITIONAL: return "m.branch_conditional";
  case MIR_ADD: return "m.add";
  case MIR_SUB: return "m.sub";
  case MIR_MUL: return "m.mul";
  case MIR_DIV: return "m.div";
  case MIR_MOD: return "m.mod";
  case MIR_SHL: return "m.shl";
  case MIR_SAR: return "m.sar";
  case MIR_SHR: return "m.shr";
  case MIR_AND: return "m.and";
  case MIR_OR: return "m.or";
  case MIR_LT: return "m.lt";
  case MIR_LE: return "m.le";
  case MIR_GT: return "m.gt";
  case MIR_GE: return "m.ge";
  case MIR_EQ: return "m.eq";
  case MIR_NE: return "m.ne";
  case MIR_STATIC_REF: return "m.static_reference";
  case MIR_FUNC_REF: return "m.function_reference";
  case MIR_STORE: return "m.store";
  case MIR_ALLOCA: return "m.alloca";
  case MIR_PHI: return "m.phi";
  case MIR_REGISTER: return "m.register";
  case MIR_UNREACHABLE: return "m.unreachable";
  case MIR_PARAMETER: return "m.parameter";
  case MIR_LIT_INTEGER: return "m.literal_integer";
  case MIR_LIT_STRING: return "m.literal_string";
  case MIR_COUNT: return "<invalid>";
  case MIR_ARCH_START: break;
  }
  switch ((MIROpcodePseudo)opcode) {
  case MPSEUDO_START: return "pseudo:start";
  case MPSEUDO_R2R: return "pseudo:r2r";
  case MPSEUDO_END: return "pseudo:end";
  case MPSEUDO_COUNT: return "pseudo:count";
  }
  return "";
}

// Function param required because of frame objects.
void print_mir_operand(MIRFunction *function, MIROperand *op) {
  STATIC_ASSERT(MIR_OP_COUNT == 8, "Exhaustive handling of MIR operand kinds");
  switch (op->kind) {
  case MIR_OP_REGISTER: {
    // Print register name
    print("%V", (usz)op->value.reg.value);

    // Print register size
    print(" %37%u%m", op->value.reg.size);

    // Print register is defining use
    if (op->value.reg.defining_use) print(" %35DEF%m");

  } break;
  case MIR_OP_IMMEDIATE: {
    print("%35%I%m", op->value.imm);
  } break;
  case MIR_OP_BLOCK: {
    ASSERT(op->value.block, "Invalid block operand");
    print("%37Block:%33%S%m", op->value.block->name);
  } break;
  case MIR_OP_FUNCTION: {
    ASSERT(op->value.function, "Invalid function operand");
    print("%37Function:%33%S%m", op->value.function->name);
  } break;
  case MIR_OP_NAME: {
    print("%37\"%33%s%37\"%m", op->value.name);
  } break;
  case MIR_OP_STATIC_REF: {
    print(
      "%37\"%33%S%37\" %36%T%m",
      ir_static_ref_var(op->value.static_ref)->name,
      ir_static_ref_var(op->value.static_ref)->type
    );
  } break;
  case MIR_OP_LOCAL_REF: {
    ASSERT(function, "Function required to print local ref operand");
    ASSERT(op->value.local_ref < function->frame_objects.size,
           "Index out of bounds (stack frame object referenced has higher index than there are frame objects)");
    print("%37Stack:%35%Z %37#%Z", (usz)op->value.local_ref, (usz)(function->frame_objects.data + op->value.local_ref)->size);
  } break;
  case MIR_OP_ANY: {
    print("ANY");
  } break;
  case MIR_OP_OP_REF: {
    print("OP_REF inst:%u op:%u", op->value.op_ref.pattern_instruction_index, op->value.op_ref.operand_index);
  } break;
  case MIR_OP_INST_REF: {
    print("INST_REF %u", op->value.inst_ref);
  } break;
  case MIR_OP_COUNT:
  case MIR_OP_NONE: UNREACHABLE();
  }
}


void print_mir_instruction_with_function_with_mnemonic(MIRFunction *function, MIRInstruction *mir, OpcodeMnemonicFunction opcode_mnemonic) {
  ASSERT(opcode_mnemonic, "Invalid argument");
  ASSERT(mir, "Invalid argument");
  print("%V %37| ", (usz)mir->reg);
  const char *mnemonic = opcode_mnemonic(mir->opcode);
  if (mnemonic && *mnemonic != '\0') print("%31%s%m ", opcode_mnemonic(mir->opcode));
  else print("%31op%d%36 ", (int)mir->opcode);
  FOREACH_MIR_OPERAND(mir, op) {
    if (op->kind == MIR_OP_NONE) break;
    print_mir_operand(function, op);
    if (op != opbase + mir->operand_count - 1 && (op + 1)->kind != MIR_OP_NONE)
      print("%37, ");
  }
  if (mir->clobbers.size) {
    print(" clobbers ");
    foreach (clobbered, mir->clobbers) {
      print("%V", (usz)clobbered->value);
      if ((usz)(clobbered - mir->clobbers.data) < mir->clobbers.size - 1) print("%37, ");
    }
  }
  print("\n%m");
}

void print_mir_instruction_with_mnemonic(MIRInstruction *mir, OpcodeMnemonicFunction opcode_mnemonic) {
  ASSERT(opcode_mnemonic, "Invalid argument");
  ASSERT(mir, "Invalid argument");
  ASSERT(mir->block,
         "Cannot print instruction without MIRBlock reference (need block to get function for frame object operands)");
  ASSERT(mir->block->function,
         "Cannot print instruction without being able to reach MIRFunction (block->function invalid); need function for frame object operands");
  print_mir_instruction_with_function_with_mnemonic(mir->block->function, mir, opcode_mnemonic);
}

void print_mir_instruction(MIRInstruction *mir) {
  print_mir_instruction_with_mnemonic(mir, &mir_common_opcode_mnemonic);
}

void print_mir_block_with_mnemonic(MIRBlock *block, OpcodeMnemonicFunction opcode_mnemonic) {
  ASSERT(block, "Invalid argument");
  ASSERT(block->function, "Cannot print block without MIRFunction reference (frame objects)");
  ASSERT(opcode_mnemonic, "Invalid argument");
  print("%S: ", block->name);
  if (block->is_entry) print("ENTRY");
  if (block->is_exit) print("EXITS");
  print(" predecessors: { ");
  foreach_val (predecessor, block->predecessors)
    print("%S,", predecessor->name);
  print("\b }");
  print(" successors: { ");
  foreach_val (succecessor, block->successors)
    print("%S,", succecessor->name);
  print("\b }\n");
  foreach_val (inst, block->instructions)
    print_mir_instruction_with_mnemonic(inst, opcode_mnemonic);
}
void print_mir_block(MIRBlock *block) {
  print_mir_block_with_mnemonic(block, &mir_common_opcode_mnemonic);
}
void print_mir_function_with_mnemonic(MIRFunction *function, OpcodeMnemonicFunction opcode_mnemonic) {
  ASSERT(function, "Invalid argument");
  print("| %Z Frame Objects\n", function->frame_objects.size);
  if (function->frame_objects.size) {
    foreach_index (i, function->frame_objects) {
      MIRFrameObject *fo = function->frame_objects.data + i;
      print("|   idx:%Z sz:%Z\n", (usz)i, (usz)fo->size);
    }
  }
  print("%S {\n", function->name);
  foreach_val (block, function->blocks) {
    print_mir_block_with_mnemonic(block, opcode_mnemonic);
  }
  print("}\n");
}
void print_mir_function(MIRFunction *function) {
  print_mir_function_with_mnemonic(function, &mir_common_opcode_mnemonic);
}

/// Clear the given instructions operands.
void mir_op_clear(MIRInstruction *inst) {
  ASSERT(inst, "Invalid argument");
  inst->operand_count = 0;
  inst->operands.arr[0].kind = MIR_OP_NONE;
}

void mir_add_op(MIRInstruction *inst, MIROperand op) {
  ASSERT(inst, "Invalid argument");
  ASSERT(op.kind != MIR_OP_NONE, "Refuse to add NONE operand.");
  if (inst->operand_count < MIR_OPERAND_SSO_THRESHOLD) {
    inst->operands.arr[inst->operand_count] = op;
  } else if (inst->operand_count == MIR_OPERAND_SSO_THRESHOLD) {
    MIROperand tmp[MIR_OPERAND_SSO_THRESHOLD];
    memcpy(tmp, inst->operands.arr, sizeof(inst->operands.arr[0]) * MIR_OPERAND_SSO_THRESHOLD);
    memset(&inst->operands.vec, 0, sizeof(inst->operands.vec));
    for (size_t i = 0; i < MIR_OPERAND_SSO_THRESHOLD; ++i)
      vector_push(inst->operands.vec, tmp[i]);

    vector_push(inst->operands.vec, op);
  } else {
    // inst->operand_count > MIR_OPERAND_SSO_THRESHOLD
    vector_push(inst->operands.vec, op);
  }
  ++inst->operand_count;
}

MIROperand *mir_get_op(MIRInstruction *inst, size_t index) {
  ASSERT(inst, "Invalid argument");
  ASSERT(index < inst->operand_count, "Index out of bounds (greater than operand count)");
  //if (index >= inst->operand_count) return NULL;
  if (inst->operand_count <= MIR_OPERAND_SSO_THRESHOLD) {
    MIROperand *out = inst->operands.arr + index;
    DBGASSERT(out->kind != MIR_OP_NONE, "Index out of bounds (found OP_NONE in array)");
    return out;
  }
  DBGASSERT(index < inst->operands.vec.size, "Index out of bounds (greater than vector size)");
  return inst->operands.vec.data + index;
}

MIRInstruction *mir_find_by_vreg(MIRFunction *f, size_t reg) {
  ASSERT(f, "Invalid argument");
  ASSERT(reg >= (size_t)MIR_ARCH_START, "Invalid MIR virtual register");

  // Bad linear lookup == sad times
  foreach_val (block, f->blocks)
      foreach_val (inst, block->instructions)
      if (inst->reg == reg) return inst;

  ICE("Could not find machine instruction in function \"%S\" with register %Z\n", f->name, reg);
}

MIRFrameObject *mir_get_frame_object(MIRFunction *function, MIROperandLocal op) {
  ASSERT(function, "Invalid argument");
  ASSERT(function->frame_objects.size > op, "Index out of bounds (stack frame object you are trying to access does not exist)");
  return function->frame_objects.data + op;
}

static bool mir_operand_kinds_match_v(MIRInstruction *inst, usz operand_count, va_list args) {
  for (usz i = 0; i < operand_count; ++i) {
    MIROperandKind expected_kind = (MIROperandKind)va_arg(args, int);
    if (expected_kind == MIR_OP_ANY) continue;
    if (mir_get_op(inst, i)->kind != expected_kind) return false;
  }
  return true;
}

bool mir_operand_kinds_match(MIRInstruction *inst, usz operand_count, ...) {
  if (inst->operand_count != operand_count) return false;

  va_list args;
  va_start(args, operand_count);
  bool out = mir_operand_kinds_match_v(inst, operand_count, args);
  va_end(args);
  return out;
}
