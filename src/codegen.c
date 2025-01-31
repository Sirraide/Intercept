#include <ast.h>
#include <codegen.h>
#include <codegen/codegen_forward.h>
#include <codegen/ir/ir-target.h>
#include <codegen/llvm/llvm_target.h>
#include <codegen/opt/opt.h>
#include <codegen/x86_64/arch_x86_64.h>
#include <error.h>
#include <ir/ir.h>
#include <ir_parser.h>
#include <parser.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <utils.h>
#include <vector.h>

#define DIAG(sev, loc, ...)                                                                                 \
  do {                                                                                                      \
    ctx->has_err = true;                                                                                    \
    issue_diagnostic(DIAG_ERR, (ctx)->ast->filename.data, as_span((ctx)->ast->source), (loc), __VA_ARGS__); \
    return;                                                                                                 \
  } while (0)

#define ERR(...) DIAG(DIAG_ERR, expr->source_location, __VA_ARGS__)

char codegen_verbose = 1;

bool parameter_is_in_register(CodegenContext *context, IRFunction *function, usz parameter_index) {
  STATIC_ASSERT(ARCH_COUNT == 2, "Exhaustive handling of architectures");
  switch (context->arch) {
  case ARCH_X86_64: return parameter_is_in_register_x86_64(context, function, parameter_index);
  default: ICE("Unrecognized architecture %d!", context->arch);
  }
  UNREACHABLE();
}

static bool parameter_is_passed_as_pointer(CodegenContext *context, IRFunction *function, usz parameter_index) {
  STATIC_ASSERT(CG_CALL_CONV_COUNT == 2, "Exhaustive handling of calling conventions");
  Type *type = ir_typeof(function);

  switch (context->call_convention) {
  default: ICE("Unrecognized calling convention %d!", context->call_convention);

  case CG_CALL_CONV_MSWIN:
    return type_sizeof(type->function.parameters.data[parameter_index].type) > 8;

  case CG_CALL_CONV_SYSV:
    // FIXME: This is not how sysv works, nearly at all. But it's good enough for us right now.
    return type_sizeof(type->function.parameters.data[parameter_index].type) > 16;
  }
  UNREACHABLE();
}

/// ===========================================================================
///  Code generation.
/// ===========================================================================
static void codegen_expr(CodegenContext *ctx, Node *expr);

// Emit an lvalue.
static void codegen_lvalue(CodegenContext *ctx, Node *lval) {
  if (lval->address) return;
  switch (lval->kind) {
  default: ICE("Unhandled node kind %d", lval->kind);

  /// Variable declaration.
  case NODE_DECLARATION:
    /// Create a static variable if need be.
    if (lval->declaration.linkage != LINKAGE_LOCALVAR) {
      IRStaticVariable *var = ir_create_static(
        ctx,
        lval,
        lval->type,
        string_dup(lval->declaration.name)
      );

      lval->address = ir_insert_static_ref(ctx, var);

      /// Emit initialiser.
      if (
        lval->declaration.init &&
        lval->declaration.init->kind == NODE_LITERAL &&
        lval->declaration.init->literal.type != TK_LBRACK
      ) {
        if (lval->declaration.init->literal.type == TK_NUMBER) {
          ir_static_var_init(var, ir_create_int_lit(ctx, lval->declaration.init->literal.integer));
        } else if (lval->declaration.init->literal.type == TK_STRING) {
          ir_static_var_init(var, ir_create_interned_str_lit(ctx, lval->declaration.init->literal.string_index));
        } else ICE("Unhandled literal type for static variable initialisation.");
        return;
      }
    } else {
      lval->address = ir_insert_alloca(ctx, lval->type);
    }

    /// Emit initialiser.
    if (lval->declaration.init) {
      codegen_expr(ctx, lval->declaration.init);
      ir_insert_store(ctx, lval->declaration.init->ir, lval->address);
    }

    return;

  case NODE_MEMBER_ACCESS: {
    codegen_lvalue(ctx, lval->member_access.struct_);
    // When member has zero byte offset, we can just use the address of the
    // struct with a modified type.
    if (lval->member_access.member->byte_offset)
      lval->address = ir_insert_add(
        ctx,
        lval->member_access.struct_->address,
        ir_insert_immediate(ctx, t_integer, lval->member_access.member->byte_offset)
      );
    else {
      lval->address = ir_insert_copy(ctx, lval->member_access.struct_->address);
    }
    ir_set_type(lval->address, ast_make_type_pointer(ctx->ast, lval->source_location, lval->member_access.member->type));
  } break;

  case NODE_IF:
    TODO("`if` as an lvalue is not yet supported, but it's in the plans bb");

  case NODE_UNARY: {
    if (!lval->unary.postfix && lval->unary.op == TK_AT) {
      // mutual recursion go brrr
      codegen_expr(ctx, lval->unary.value);
      lval->address = lval->unary.value->ir;
      return;
    } else ICE("Unary operator %s is not an lvalue", token_type_to_string(TK_AT));
  }

  case NODE_VARIABLE_REFERENCE:
    ASSERT(lval->var->val.node->address,
           "Cannot reference variable that has not yet been emitted.");
    if (ir_kind(lval->var->val.node->address) == IR_STATIC_REF)
      lval->address = ir_insert_static_ref(ctx, ir_static_ref_var(lval->var->val.node->address));
    else lval->address = lval->var->val.node->address;
    break;

  case NODE_CAST: {
    codegen_lvalue(ctx, lval->cast.value);
    lval->address = lval->cast.value->address;
  } break;

  // TODO: String literals are lvalues...

  /* TODO: references
  case NODE_BLOCK:
  case NODE_CALL:
  case NODE_CAST:
  */
  }
}

/// Emit an expression.
static void codegen_expr(CodegenContext *ctx, Node *expr) {
  if (expr->emitted) return;
  expr->emitted = true;

  STATIC_ASSERT(NODE_COUNT == 19, "Exhaustive handling of node types during code generation (AST->IR).");
  switch (expr->kind) {
  default: ICE("Unrecognized expression kind: %d", expr->kind);

  /// A function node yields its address.
  ///
  /// FIXME: Replacing function references with the functions they
  /// point to discards location information, the result of which
  /// is that errors now point to the function declaration rather
  /// than the use that caused the problem; to fix this, we should
  /// hold on to function references and emit IR function references
  /// for them in here.
  case NODE_FUNCTION:
      expr->ir = ir_insert_func_ref(ctx, expr->function.ir);
      if (expr->type->function.attr_inline)
        ERR("Cannot take address of inline function '%S'", expr->function.name);
      return;

  case NODE_MODULE_REFERENCE:
    ERR("Module reference must not be used unless to access module exports");

  /// Root node.
  case NODE_ROOT: {
    /// Emit everything that isn’t a function.
    foreach_val (child, expr->root.children) {
      if (child->kind == NODE_FUNCTION) continue;
      codegen_expr(ctx, child);
    }

    /// If the last expression doesn’t return anything, return 0.
    if (!ir_is_closed(ctx->insert_point)) ir_insert_return(ctx, vector_back(expr->root.children)->ir);
    return;
  }

  case NODE_DECLARATION:
    codegen_lvalue(ctx, expr);
    return;

  case NODE_MEMBER_ACCESS:
  case NODE_VARIABLE_REFERENCE:
    codegen_lvalue(ctx, expr);
    expr->ir = ir_insert_load(
      ctx,
      type_get_element(ir_typeof(expr->address)),
      expr->address
    );
    return;

  case NODE_STRUCTURE_DECLARATION:
    return;

  /// If expression.
  ///
  /// Each box is a basic block within intermediate representation,
  /// and edges represent control flow from top to bottom.
  ///
  ///      +---------+
  ///      | current |
  ///      +---------+
  ///     /           \
  /// +------+    +------+
  /// | then |    | else |
  /// +------+    +------+
  ///         \  /
  ///       +------+
  ///       | join |
  ///       +------+
  ///
  case NODE_IF: {
    /// Emit the condition.
    codegen_expr(ctx, expr->if_.condition);

    IRBlock *then_block = ir_block(ctx);
    IRBlock *last_then_block = then_block;
    IRBlock *else_block = ir_block(ctx);
    IRBlock *last_else_block = else_block;
    IRBlock *join_block = ir_block(ctx);

    /// Generate the branch.
    ir_insert_cond_br(ctx, expr->if_.condition->ir, then_block, else_block);

    /// Emit the then block.
    ir_block_attach(ctx, then_block);
    codegen_expr(ctx, expr->if_.then);

    /// Branch to the join block to skip the else branch.
    last_then_block = ctx->insert_point;
    if (!ir_is_closed(ctx->insert_point)) ir_insert_br(ctx, join_block);

    /// Generate the else block if there is one.
    ir_block_attach(ctx, else_block);
    if (expr->if_.else_) {
      codegen_expr(ctx, expr->if_.else_);
      last_else_block = ctx->insert_point;
    }

    /// Branch to the join block from the else branch.
    if (!ir_is_closed(ctx->insert_point)) ir_insert_br(ctx, join_block);

    /// Attach the join block.
    ir_block_attach(ctx, join_block);

    /// Insert a phi node for the result of the if in the join block.
    if (!type_is_void(expr->type)) {
      IRInstruction *phi = ir_insert_phi(ctx, expr->type);
      ir_phi_add_arg(phi, last_then_block, expr->if_.then->ir);
      ir_phi_add_arg(phi, last_else_block, expr->if_.else_->ir);
      expr->ir = phi;
    }
    return;
  }

  /// While expression.
  ///
  /// +---------+
  /// | current |
  /// +---------+        ,---------+
  ///      |             |         |
  /// +--------------------+       |
  /// | compute condition  |       |
  /// | conditional branch |       |
  /// +--------------------+       |
  ///      |             |         |
  ///      |      +------------+   |
  ///      |      | body       |   |
  ///      |      +------------+   |
  ///      |             |         |
  ///      |            ...        |
  ///      |             |         |
  ///  +----------+      `---------+
  ///  | join     |
  ///  +----------+
  case NODE_WHILE: {
    IRBlock *while_cond_block = ir_block(ctx);
    IRBlock *join_block = ir_block(ctx);

    /// Branch to the new condition block, then attach that as the current block.
    ir_insert_br(ctx, while_cond_block);
    ir_block_attach(ctx, while_cond_block);

    // Emit condition
    codegen_expr(ctx, expr->while_.condition);

    /// If while body is empty, don't use body block.
    if (expr->while_.body->block.children.size == 0) {
      ir_insert_cond_br(ctx, expr->while_.condition->ir, while_cond_block, join_block);
      ir_block_attach(ctx, join_block);
      return;
    }

    /// Otherwise, emit the body of the while loop.
    IRBlock *while_body_block = ir_block(ctx);
    ir_insert_cond_br(ctx, expr->while_.condition->ir, while_body_block, join_block);
    ir_block_attach(ctx, while_body_block);
    codegen_expr(ctx, expr->while_.body);

    /// Emit a branch to the join block and attach the join block.
    if (!ir_is_closed(ctx->insert_point)) ir_insert_br(ctx, while_cond_block);
    ir_block_attach(ctx, join_block);
    return;
  }

  /// Block expression.
  case NODE_BLOCK: {
    /// Emit everything that isn’t a function.
    Node *last = NULL;
    foreach_val (child, expr->block.children) {
      if (child->kind == NODE_FUNCTION) continue;
      last = child;
      codegen_expr(ctx, child);
    }

    /// The yield of a block is that of its last expression;
    /// If a block doesn’t yield `void`, then it is guaranteed
    /// to not be empty, which is why we don’t check its size here.
    if (!type_is_void(expr->type)) {
      ASSERT(last && last->ir);
      expr->ir = last->ir;
    }
    return;
  }

  /// Function call.
  case NODE_CALL: {
    ASSERT(expr->call.intrinsic == INTRIN_COUNT, "Refusing to codegen intrinsic as a regular call");
    IRInstruction *call = NULL;

    /// Direct call.
    if (expr->call.callee->kind == NODE_FUNCTION) {
      call = ir_create_call(ctx, expr->call.callee->function.ir);
    }

    /// Indirect call.
    else {
      codegen_expr(ctx, expr->call.callee);
      call = ir_create_call(ctx, expr->call.callee->ir);
    }

    /// Emit the arguments.
    foreach_val (arg, expr->call.arguments) {
      if (type_is_reference(arg->type)) {
        codegen_lvalue(ctx, arg);
        ir_call_add_arg(call, arg->address);
      }
      else {
        codegen_expr(ctx, arg);
        ir_call_add_arg(call, arg->ir);
      }
    }

    ir_insert(ctx, call);
    expr->ir = call;
    return;
  }

  /// Intrinsic.
  case NODE_INTRINSIC_CALL: {
    ASSERT(expr->call.callee->kind = NODE_FUNCTION_REFERENCE);
    STATIC_ASSERT(INTRIN_COUNT == 7, "Handle all intrinsics in codegen");
    switch (expr->call.intrinsic) {
      case INTRIN_COUNT:
      case INTRIN_BACKEND_COUNT:
        ICE("Call is not an intrinsic");

      case INTRIN_BUILTIN_LINE:
      case INTRIN_BUILTIN_FILENAME:
        UNREACHABLE();

      /// System call.
      case INTRIN_BUILTIN_SYSCALL:
        /// Syscalls are not a thing on Windows.
        if (ctx->call_convention == CG_CALL_CONV_MSWIN) {
          ERR("Sorry, syscalls are not supported on Windows.");
        }

        expr->ir = ir_create_intrinsic(ctx, t_integer, expr->call.intrinsic);
        foreach_val (arg, expr->call.arguments) {
          if (type_is_reference(arg->type)) {
            codegen_lvalue(ctx, arg);
            ir_call_add_arg(expr->ir, arg->address);
          } else {
            codegen_expr(ctx, arg);
            ir_call_add_arg(expr->ir, arg->ir);
          }
        }
        ir_insert(ctx, expr->ir);
        return;

      /// Inline call.
      case INTRIN_BUILTIN_INLINE: {
        Node *call = expr->call.arguments.data[0];
        codegen_expr(ctx, call);
        ir_call_force_inline(call->ir, true);
        expr->ir = call->ir;
        expr->address = call->address;
        return;
      }

      /// Debug trap.
      case INTRIN_BUILTIN_DEBUGTRAP:
        expr->ir = ir_insert_intrinsic(ctx, t_void, expr->call.intrinsic);
        return;

      /// Memory copy.
      case INTRIN_BUILTIN_MEMCPY:
        expr->ir = ir_create_intrinsic(ctx, t_void, expr->call.intrinsic);
        foreach_val (arg, expr->call.arguments) {
          if (type_is_reference(arg->type)) {
            codegen_lvalue(ctx, arg);
            ir_call_add_arg(expr->ir, arg->address);
          } else {
            codegen_expr(ctx, arg);
            ir_call_add_arg(expr->ir, arg->ir);
          }
        }
        ir_insert(ctx, expr->ir);
        return;
    }

    UNREACHABLE();
  }

  /// Typecast.
  case NODE_CAST: {
    Type *t_to = expr->type;
    Type *t_from = expr->cast.value->type;

    usz to_sz = type_sizeof(t_to);
    usz from_sz = type_sizeof(t_from);

    bool from_signed = type_is_signed(t_from);

    codegen_expr(ctx, expr->cast.value);

    if (from_sz == to_sz) {
      expr->ir = ir_insert_bitcast(ctx, t_to, expr->cast.value->ir);
      return;
    }
    else if (from_sz < to_sz) {
      // smaller to larger: sign extend if needed, otherwise zero extend.
      if (from_signed)
        expr->ir = ir_insert_sext(ctx, t_to, expr->cast.value->ir);
      else expr->ir = ir_insert_zext(ctx, t_to, expr->cast.value->ir);
      return;
    }
    else if (from_sz > to_sz) {
      // larger to smaller: truncate.
      expr->ir = ir_insert_trunc(ctx, t_to, expr->cast.value->ir);
      return;
    }
    UNREACHABLE();
  }

  /// Binary expression.
  case NODE_BINARY: {
    Node *const lhs = expr->binary.lhs;
    Node *const rhs = expr->binary.rhs;

    /// Assignment needs to be handled separately.
    if (expr->binary.op == TK_COLON_EQ) {
      /// Emit the RHS because we need that in any case.
      codegen_expr(ctx, rhs);
      codegen_lvalue(ctx, lhs);
      expr->ir = ir_insert_store(ctx, rhs->ir, lhs->address);
      return;
    }

    if (expr->binary.op == TK_LBRACK) {
      IRInstruction *subs_lhs = NULL;
      Type *reference_stripped_lhs_type = type_strip_references(lhs->type);
      if (!type_is_array(reference_stripped_lhs_type) && !type_is_pointer(reference_stripped_lhs_type))
        ERR("Subscript operator may only operate on arrays and pointers, which type %T is not", lhs->type);

      if (lhs->kind == NODE_VARIABLE_REFERENCE) {
        IRInstruction *var_decl = lhs->var->val.node->address;
        IRType kind = ir_kind(var_decl);
        Type *ty = ir_typeof(var_decl);
        if (kind == IR_PARAMETER || kind == IR_STATIC_REF || kind == IR_ALLOCA)
          if (type_is_pointer(ty) && type_is_pointer(ty->pointer.to))
            subs_lhs = ir_insert_load(ctx, type_get_element(ty), var_decl);
          else subs_lhs = var_decl;
        else {
          ir_print_instruction(stdout, var_decl);
          ERR("Unhandled variable reference IR instruction kind %i aka %S", (int) kind, ir_kind_to_str(kind));
        }
      } else if (is_lvalue(lhs)) {
        codegen_lvalue(ctx, lhs);
        subs_lhs = lhs->address;
      } else if (lhs->kind == NODE_LITERAL && lhs->literal.type == TK_STRING) {
        codegen_expr(ctx, lhs);
        if (rhs->kind == NODE_LITERAL && rhs->literal.type == TK_NUMBER) {
          string str = ctx->ast->strings.data[lhs->literal.string_index];
          if (rhs->literal.integer >= str.size) {
            ERR("Out of bounds: subscript %U too large for string literal.", rhs->literal.integer);
          }
          if (rhs->literal.integer)
            expr->ir = ir_insert_add(ctx, lhs->ir, ir_insert_immediate(ctx, t_integer, rhs->literal.integer));
          else expr->ir = lhs->ir;
          return;
        }
        subs_lhs = lhs->ir;
      }
      else ERR("LHS of subscript operator has invalid kind %i", (int) lhs->kind);

      // Subscript of array should result in pointer to base type, not pointer to array type.
      {
        Type *ty = ir_typeof(subs_lhs);
        if ((type_is_pointer(ty) || type_is_reference(ty)) && type_is_array(ty->pointer.to)) {
          Type *element_ptr = ast_make_type_pointer(
            ctx->ast,
            ty->source_location,
            ty->pointer.to->array.of
          );
          subs_lhs = ir_insert_bitcast(ctx, element_ptr, subs_lhs);
        }
      }

      if (rhs->kind == NODE_LITERAL && rhs->literal.type == TK_NUMBER && rhs->literal.integer == 0) {
        expr->ir = subs_lhs;
        return;
      }

      codegen_expr(ctx, rhs);

      IRInstruction *scaled_rhs = NULL;
      // An array subscript needs multiplied by the sizeof the array's base type.
      if (type_is_array(reference_stripped_lhs_type)) {
        IRInstruction *immediate = ir_insert_immediate(
          ctx,
          t_integer,
          type_sizeof(reference_stripped_lhs_type->array.of)
        );
        scaled_rhs = ir_insert_mul(ctx, rhs->ir, immediate);
      }
      // A pointer subscript needs multiplied by the sizeof the pointer's base type.
      else if (type_is_pointer(reference_stripped_lhs_type)) {
        IRInstruction *immediate = ir_insert_immediate(
          ctx,
          t_integer,
          type_sizeof(reference_stripped_lhs_type->pointer.to)
        );
        scaled_rhs = ir_insert_mul(ctx, rhs->ir, immediate);
      } expr->ir = ir_insert_add(ctx, subs_lhs, scaled_rhs);
      return;
    }

    /// Emit the operands.
    codegen_expr(ctx, lhs);
    codegen_expr(ctx, rhs);

    /// Emit the binary instruction.
    switch (expr->binary.op) {
      default: ICE("Cannot emit binary expression of type %d", expr->binary.op);
      case TK_LBRACK: UNREACHABLE();
      case TK_LT: expr->ir = ir_insert_lt(ctx, lhs->ir, rhs->ir); return;
      case TK_LE: expr->ir = ir_insert_le(ctx, lhs->ir, rhs->ir); return;
      case TK_GT: expr->ir = ir_insert_gt(ctx, lhs->ir, rhs->ir); return;
      case TK_GE: expr->ir = ir_insert_ge(ctx, lhs->ir, rhs->ir); return;
      case TK_EQ: expr->ir = ir_insert_eq(ctx, lhs->ir, rhs->ir); return;
      case TK_NE: expr->ir = ir_insert_ne(ctx, lhs->ir, rhs->ir); return;
      case TK_PLUS: expr->ir = ir_insert_add(ctx, lhs->ir, rhs->ir); return;
      case TK_MINUS: expr->ir = ir_insert_sub(ctx, lhs->ir, rhs->ir); return;
      case TK_STAR: expr->ir = ir_insert_mul(ctx, lhs->ir, rhs->ir); return;
      case TK_SLASH: expr->ir = ir_insert_div(ctx, lhs->ir, rhs->ir); return;
      case TK_PERCENT: expr->ir = ir_insert_mod(ctx, lhs->ir, rhs->ir); return;
      case TK_SHL: expr->ir = ir_insert_shl(ctx, lhs->ir, rhs->ir); return;
      case TK_SHR: expr->ir = ir_insert_sar(ctx, lhs->ir, rhs->ir); return;
      case TK_AMPERSAND: expr->ir = ir_insert_and(ctx, lhs->ir, rhs->ir); return;
      case TK_PIPE: expr->ir = ir_insert_or(ctx, lhs->ir, rhs->ir); return;
    }
  }

  /// Unary expression.
  case NODE_UNARY: {
    /// Addressof expressions are special because we don’t emit their operand.
    if (expr->unary.op == TK_AMPERSAND && !expr->unary.postfix) {
      if (expr->literal.type == TK_STRING) {
        TODO("IR code generation of addressof string literal");
      } else {
        codegen_lvalue(ctx, expr->unary.value);
        expr->ir = expr->unary.value->address;
      }
      return;
    }

    /// Emit the operand.
    codegen_expr(ctx, expr->unary.value);

    /// Prefix expressions.
    if (!expr->unary.postfix) {
      switch (expr->unary.op) {
      default: ICE("Cannot emit unary prefix expression of token type %s", token_type_to_string(expr->unary.op));

        /// Load a value from a pointer.
        case TK_AT:
          if (expr->unary.value->type->kind == TYPE_POINTER && expr->unary.value->type->pointer.to->kind == TYPE_FUNCTION)
            expr->ir = expr->unary.value->ir;
          else expr->ir = ir_insert_load(ctx, type_get_element(ir_typeof(expr->unary.value->ir)), expr->unary.value->ir);
          return;

        /// One’s complement negation.
        case TK_TILDE: expr->ir = ir_insert_not(ctx, expr->unary.value->ir); return;
      }
    }

    /// Postfix expressions.
    else {
      switch (expr->unary.op) {
        default: ICE("Cannot emit unary postfix expression of type %d", expr->unary.op);
      }
    }
  }

  /// Literal expression. Only integer literals are supported for now.
  case NODE_LITERAL: {
    switch (expr->literal.type) {

    case TK_NUMBER: {
      expr->ir = ir_insert_immediate(ctx, expr->type, expr->literal.integer);
    } break;

    case TK_STRING: {

      // FIXME: This name shouldn't be needed here, but static
      // variables are required to have names as of right now. We
      // should really have it so that the backend can gracefully
      // handle empty string for static names, and it will
      // automatically generate one (i.e. exactly what we do here).
      static size_t string_literal_count = 0;
      IRStaticVariable *var = ir_create_static(ctx, expr, expr->type, format("__str_lit%zu", string_literal_count++));
      expr->ir = ir_insert_static_ref(ctx, var);
      // Set static initialiser so backend will properly fill in data from string literal.
      ir_static_var_init(var, ir_create_interned_str_lit(ctx, expr->literal.string_index));
    } break;

    // Array
    case TK_LBRACK: {
      expr->ir = ir_insert_alloca(ctx, expr->type);

      // Emit a store from each expression in the initialiser as an element in the array.
      IRInstruction *address = ir_insert_copy(ctx, expr->ir);
      ir_set_type(address, ast_make_type_pointer(ctx->ast, expr->source_location, expr->type->array.of));
      usz index = 0;
      foreach_val (node, expr->literal.compound) {
        codegen_expr(ctx, node);
        ir_insert_store(ctx, node->ir, address);
        if (index == expr->literal.compound.size - 1) break;
        // Iterate address
        IRInstruction *element_byte_size = ir_insert_immediate(ctx, t_integer, type_sizeof(expr->type->array.of));
        address = ir_insert_add(ctx, address, element_byte_size);
        ++index;
      }
      expr->ir = ir_insert_load(ctx, type_get_element(ir_typeof(expr->ir)) , expr->ir);
    } break;

    default:
      DIAG(DIAG_SORRY, expr->source_location, "Emitting literals of type %T not supported", expr->type);
    }
  } return;

  case NODE_FOR: {
    /* FOR INIT COND ITER BODY
     *
     * +------------------+
     * | current          |
     * | emit initialiser |
     * +------------------+
     *      |
     *      |             ,-------------+
     *      |             |             |
     * +--------------------+           |
     * | conditional branch |           |
     * +--------------------+           |
     *      |             |             |
     *      |      +----------------+   |
     *      |      | body           |   |
     *      |      | emit iterator  |   |
     *      |      +----------------+   |
     *      |             |             |
     *      |            ...            |
     *      |             |             |
     *  +----------+      `-------------+
     *  | join     |
     *  +----------+
     *
     */

    IRBlock *cond_block = ir_block(ctx);
    IRBlock *body_block = ir_block(ctx);
    IRBlock *join_block = ir_block(ctx);

    codegen_expr(ctx, expr->for_.init);
    ir_insert_br(ctx, cond_block);

    ir_block_attach(ctx, cond_block);
    codegen_expr(ctx, expr->for_.condition);
    ir_insert_cond_br(ctx, expr->for_.condition->ir, body_block, join_block);

    ir_block_attach(ctx, body_block);
    codegen_expr(ctx, expr->for_.body);
    codegen_expr(ctx, expr->for_.iterator);
    ir_insert_br(ctx, cond_block);

    ir_block_attach(ctx, join_block);

    return;
  }

  case NODE_RETURN: {
    if (expr->return_.value) codegen_expr(ctx, expr->return_.value);
    expr->ir = ir_insert_return(ctx, expr->return_.value ? expr->return_.value->ir : NULL);
    return;
  }

  /// Function reference. These should have all been removed by the semantic analyser.
  case NODE_FUNCTION_REFERENCE: UNREACHABLE();
  }
}

/// Emit a function.
void codegen_function(CodegenContext *ctx, Node *node) {
  ASSERT(node->function.body);
  ctx->insert_point = ir_entry_block(node->function.ir);
  ctx->function = node->function.ir;

  /// Next, emit all parameter declarations and store
  /// the initial parameter values in them.
  // TODO: Make this backend dependent?
  foreach_index(i, node->function.param_decls) {
    Node *decl = node->function.param_decls.data[i];
    IRInstruction *p = ir_parameter(ctx->function, i);
    if (type_is_reference(decl->type))
      decl->address = p;
    else if (parameter_is_passed_as_pointer(ctx, ctx->function, i)) {
      Type *ty = ir_typeof(p);
      ir_set_type(p, ast_make_type_pointer(ctx->ast, ty->source_location, ty));
      decl->address = p;
    } else {
      /// Allocate a variable for the parameter.
      codegen_lvalue(ctx, decl);
      /// Store the parameter value in the variable.
      ir_insert_store(ctx, p, decl->address);
    }
  }

  /// Emit the function body.
  codegen_expr(ctx, node->function.body);

  /// If we can return from here, and this function doesn’t return void,
  /// then return the return value; otherwise, just return nothing.
  if (!ir_is_closed(ctx->insert_point)) {
    ir_insert_return(ctx, !type_is_void(node->type->function.return_type)
        ? node->function.body->ir
        : NULL);
  }
}

/// ===========================================================================
///  Driver
/// ===========================================================================
void codegen_lower(CodegenContext *context) {
  STATIC_ASSERT(ARCH_COUNT == 2, "Exhaustive handling of architectures");
  switch (context->arch) {
    case ARCH_X86_64:
      codegen_lower_x86_64(context);
      break;
    default:
      TODO("Handle %d code generation architecture.", context->arch);
  }
}

void codegen_early_lowering(CodegenContext *context) {
  STATIC_ASSERT(ARCH_COUNT == 2, "Exhaustive handling of architectures");
  if (context->target == TARGET_LLVM) return;
  switch (context->arch) {
    case ARCH_X86_64:
      codegen_lower_early_x86_64(context);
      break;
    default:
      TODO("Handle %d code generation architecture.", context->arch);
  }
}

void codegen_emit(CodegenContext *context) {
  STATIC_ASSERT(ARCH_COUNT == 2, "Exhaustive handling of architectures");

  if (context->target == TARGET_LLVM) {
    codegen_emit_llvm(context);
    return;
  }

  switch (context->arch) {
  case ARCH_X86_64: {
    codegen_emit_x86_64(context);
  } break;
  case ARCH_NONE: FALLTHROUGH;
  case ARCH_COUNT: UNREACHABLE();
  }
}

bool codegen
(CodegenLanguage lang,
 CodegenArchitecture arch,
 CodegenTarget target,
 CodegenCallingConvention call_convention,
 const char *infile,
 const char *outfile, Module *ast,
 string ir
 )
{
  if (!outfile) ICE("codegen(): outfile can not be NULL!");
  // Open file for writing.
  FILE *code = fopen(outfile, "wb");
  if (!code) ICE("codegen(): failed to open file at path: \"%s\"\n", outfile);

  CodegenContext *context = codegen_context_create(ast, arch, target, call_convention, code);

  switch (lang) {
    /// Parse an IR file.
    case LANG_IR: {
        if (!ir_parse(context, infile, ir)) {
          fclose(code);
          return false;
        }
    } break;

    /// Codegen an Intercept program.
    case LANG_FUN: {
      if (!ast->is_module) {
        /// Create the main function.
        Type* c_int = ast_make_type_integer(ast, (loc){0}, true, context->ffi.cint_size);
        Parameter argc =  {
          .name = string_create("__argc__"),
          .type = c_int,
          .source_location = {0},
        };
        Parameter argv =  {
          .name = string_create("__argv__"),
          .type = ast_make_type_pointer(ast, (loc){0}, ast_make_type_pointer(ast, (loc){0}, t_byte)),
          .source_location = {0},
        };
        Parameter envp =  {
          .name = string_create("__envp__"),
          .type = ast_make_type_pointer(ast, (loc){0}, ast_make_type_pointer(ast, (loc){0}, t_byte)),
          .source_location = {0},
        };

        Parameters main_params = {0};
        vector_push(main_params, argc);
        vector_push(main_params, argv);
        vector_push(main_params, envp);

        /// FIXME: return type should be int as well, but that currently breaks the x86_64 backend.
        Type *main_type = ast_make_type_function(context->ast, (loc){0}, t_integer, main_params);
        context->entry = ir_create_function(context, string_create("main"), main_type, LINKAGE_EXPORTED);
      } else {
        Parameters entry_params = {0};
        Type *entry_type = ast_make_type_function(context->ast, (loc){0}, t_void, entry_params);
        context->entry = ir_create_function(context, format("__module%S_entry", context->ast->module_name), entry_type, LINKAGE_EXPORTED);
      }

      ir_attribute(context->entry, FUNC_ATTR_NOMANGLE, true);

      /// Create the remaining functions and set the address of each function.
      foreach_val (func, ast->functions) {
        func->function.ir = ir_create_function(context, string_dup(func->function.name), func->type, func->function.linkage);
        ir_location(func->function.ir, func->source_location);

        /// Handle attributes.
        // TODO: Should we propagate "discardable" to the IR?
#define F(name, var_name) ir_attribute(func->function.ir, FUNC_ATTR_##name, func->type->function.attr_##var_name);
        SHARED_FUNCTION_ATTRIBUTES(F)
#undef F
      }

      foreach_val (import, context->ast->imports) {
        foreach_val (n, import->exports) {
          // Declarations need an IR instruction generated, just to match how we
          // deal with regular declarations.
          if (n->kind == NODE_DECLARATION) codegen_lvalue(context, n);
        }
      }

      /// Emit the main function.
      context->insert_point = ir_entry_block(context->entry);
      context->function = context->entry;
      codegen_expr(context, ast->root);

      /// Emit the remaining functions that aren’t extern.
      foreach_val (func, ast->functions)
        if (ir_func_is_definition(func->function.ir))
          codegen_function(context, func);
    } break;

    /// Anything else is not supported.
    default: ICE("Language %d not supported.", lang);
  }

  /// Don’t codegen a faulty program.
  if (context->has_err) return false;

  /// Perform mandatory inlining.
  if (!codegen_process_inline_calls(context)) return false;

  if (debug_ir || print_ir2) {
    ir_print(stdout, context);
  }

  /// Early lowering before optimisation.
  codegen_early_lowering(context);

  if (optimise) {
    codegen_optimise(context);
    if (debug_ir || print_ir2) {
      print("\n====== Optimised ====== \n");
      ir_print(stdout, context);
    }
  }

  if (print_dot_dj) {
    ir_print_dot_dj(context);
    exit(42);
  }

  if (print_dot_cfg) {
    ir_print_dot_cfg(context);
    exit(42);
  }

  if (print_ir2) exit(42);

  /// No need to lower anything if we’re emitting LLVM IR.
  if (target != TARGET_LLVM) {
    codegen_lower(context);

    if (debug_ir) {
      print("\n====== Lowered ====== \n");
      ir_print(stdout, context);
    }
  }

  codegen_emit(context);

  codegen_context_free(context);

  fclose(code);
  return true;
}


static void mangle_type_to(string_buffer *buf, Type *t) {
  ASSERT(t);
  switch (t->kind) {
    default: TODO("Handle type kind %d in type mangling!", (int)t->kind);

    case TYPE_STRUCT:
      if (t->structure.decl->struct_decl->name.size)
        format_to(buf, "%Z%S", t->structure.decl->struct_decl->name.size, t->structure.decl->struct_decl->name);
      else {
        static usz struct_count = 0;
        format_to(buf, "%Z%Z", number_width(struct_count), struct_count);
        ++struct_count;
      }
      break;

    case TYPE_PRIMITIVE:
      format_to(buf, "%Z%S", t->primitive.name.size, t->primitive.name);
      break;

    case TYPE_NAMED:
      if (!t->named->val.type) format_to(buf, "%Z%S", t->named->name.size, t->named->name);
      else mangle_type_to(buf, t->named->val.type);
      break;

    case TYPE_INTEGER: {
      usz length = 1 + number_width(t->integer.bit_width);
      format_to(buf, "%Z%c%Z", length, t->integer.is_signed ? 's' : 'u', t->integer.bit_width);
    } break;

    case TYPE_POINTER:
      format_to(buf, "P");
      mangle_type_to(buf, t->pointer.to);
      break;

    case TYPE_REFERENCE:
      format_to(buf, "R");
      mangle_type_to(buf, t->reference.to);
      break;

    case TYPE_ARRAY:
      format_to(buf, "A%ZE", t->array.size);
      mangle_type_to(buf, t->array.of);
      break;

    case TYPE_FUNCTION:
      format_to(buf, "F");
      mangle_type_to(buf, t->function.return_type);
      foreach (param, t->function.parameters) mangle_type_to(buf, param->type);
      format_to(buf, "E");
      break;
  }
}

void mangle_function_name(IRFunction *function) {
  if (ir_attribute(function, FUNC_ATTR_NOMANGLE)) return;

  string_buffer buf = {0};
  span name = ir_name(function);
  format_to(&buf, "_XF%Z%S", name.size, name);
  mangle_type_to(&buf, ir_typeof(function));

  /// FIXME: Mangled name should not override original name.
  ir_name(function, ((string){buf.data, buf.size}));
}
