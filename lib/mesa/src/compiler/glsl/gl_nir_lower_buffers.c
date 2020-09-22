/*
 * Copyright © 2019 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "compiler/nir/nir.h"
#include "compiler/nir/nir_builder.h"
#include "gl_nir.h"
#include "ir_uniform.h"

#include "main/compiler.h"
#include "main/mtypes.h"

static nir_ssa_def *
get_block_array_index(nir_builder *b, nir_deref_instr *deref,
                      const struct gl_shader_program *shader_program)
{
   unsigned array_elements = 1;

   /* Build a block name such as "block[2][0]" for finding in the list of
    * blocks later on as well as an optional dynamic index which gets added
    * to the block index later.
    */
   int binding = 0;
   const char *block_name = "";
   nir_ssa_def *nonconst_index = NULL;
   while (deref->deref_type == nir_deref_type_array) {
      nir_deref_instr *parent = nir_deref_instr_parent(deref);
      assert(parent && glsl_type_is_array(parent->type));
      unsigned arr_size = glsl_get_length(parent->type);

      if (nir_src_is_const(deref->arr.index)) {
         unsigned arr_index = nir_src_as_uint(deref->arr.index);

         /* We're walking the deref from the tail so prepend the array index */
         block_name = ralloc_asprintf(b->shader, "[%u]%s", arr_index,
                                      block_name);

         binding += arr_index * array_elements;
      } else {
         nir_ssa_def *arr_index = nir_ssa_for_src(b, deref->arr.index, 1);
         arr_index = nir_umin(b, arr_index, nir_imm_int(b, arr_size - 1));
         nir_ssa_def *arr_offset = nir_amul_imm(b, arr_index, array_elements);
         if (nonconst_index)
            nonconst_index = nir_iadd(b, nonconst_index, arr_offset);
         else
            nonconst_index = arr_offset;

         /* We're walking the deref from the tail so prepend the array index */
         block_name = ralloc_asprintf(b->shader, "[0]%s", block_name);
      }

      array_elements *= arr_size;
      deref = parent;
   }

   assert(deref->deref_type == nir_deref_type_var);
   binding += deref->var->data.binding;
   block_name = ralloc_asprintf(b->shader, "%s%s",
                                glsl_get_type_name(deref->var->interface_type),
                                block_name);

   struct gl_linked_shader *linked_shader =
      shader_program->_LinkedShaders[b->shader->info.stage];

   unsigned num_blocks;
   struct gl_uniform_block **blocks;
   if (deref->mode == nir_var_mem_ubo) {
      num_blocks = linked_shader->Program->info.num_ubos;
      blocks = linked_shader->Program->sh.UniformBlocks;
   } else {
      assert(deref->mode == nir_var_mem_ssbo);
      num_blocks = linked_shader->Program->info.num_ssbos;
      blocks = linked_shader->Program->sh.ShaderStorageBlocks;
   }

   /* Block names are optional with ARB_gl_spirv so use the binding instead. */
   bool use_bindings = shader_program->data->spirv;

   for (unsigned i = 0; i < num_blocks; i++) {
      if (( use_bindings && binding == blocks[i]->Binding) ||
          (!use_bindings && strcmp(block_name, blocks[i]->Name) == 0)) {
         if (nonconst_index)
            return nir_iadd_imm(b, nonconst_index, i);
         else
            return nir_imm_int(b, i);
      }
   }

   /* TODO: Investigate if we could change the code to assign Bindings to the
    * blocks that were not explicitly assigned, so we can always compare
    * bindings.
    */

   if (use_bindings)
      unreachable("Failed to find the block by binding");
   else
      unreachable("Failed to find the block by name");
}

static void
get_block_index_offset(nir_variable *var,
                       const struct gl_shader_program *shader_program,
                       gl_shader_stage stage,
                       unsigned *index, unsigned *offset)
{

   struct gl_linked_shader *linked_shader =
      shader_program->_LinkedShaders[stage];

   unsigned num_blocks;
   struct gl_uniform_block **blocks;
   if (var->data.mode == nir_var_mem_ubo) {
      num_blocks = linked_shader->Program->info.num_ubos;
      blocks = linked_shader->Program->sh.UniformBlocks;
   } else {
      assert(var->data.mode == nir_var_mem_ssbo);
      num_blocks = linked_shader->Program->info.num_ssbos;
      blocks = linked_shader->Program->sh.ShaderStorageBlocks;
   }

   /* Block names are optional with ARB_gl_spirv so use the binding instead. */
   bool use_bindings = shader_program->data->spirv;

   for (unsigned i = 0; i < num_blocks; i++) {
      const char *block_name = glsl_get_type_name(var->interface_type);
      if (( use_bindings && blocks[i]->Binding == var->data.binding) ||
          (!use_bindings && strcmp(block_name, blocks[i]->Name) == 0)) {
         *index = i;
         *offset = blocks[i]->Uniforms[var->data.location].Offset;
         return;
      }
   }

   if (use_bindings)
      unreachable("Failed to find the block by binding");
   else
      unreachable("Failed to find the block by name");
}

static bool
lower_buffer_interface_derefs_impl(nir_function_impl *impl,
                                   const struct gl_shader_program *shader_program)
{
   bool progress = false;

   nir_builder b;
   nir_builder_init(&b, impl);

   nir_foreach_block(block, impl) {
      nir_foreach_instr_safe(instr, block) {
         switch (instr->type) {
         case nir_instr_type_deref: {
            nir_deref_instr *deref = nir_instr_as_deref(instr);
            if (!(deref->mode & (nir_var_mem_ubo | nir_var_mem_ssbo)))
               break;

            /* We use nir_address_format_32bit_index_offset */
            assert(deref->dest.is_ssa);
            assert(deref->dest.ssa.bit_size == 32);
            deref->dest.ssa.num_components = 2;

            progress = true;

            b.cursor = nir_before_instr(&deref->instr);

            nir_ssa_def *ptr;
            if (deref->deref_type == nir_deref_type_var &&
                !glsl_type_is_interface(glsl_without_array(deref->var->type))) {
               /* This variable is contained in an interface block rather than
                * containing one.  We need the block index and its offset
                * inside that block
                */
               unsigned index, offset;
               get_block_index_offset(deref->var, shader_program,
                                      b.shader->info.stage,
                                      &index, &offset);
               ptr = nir_imm_ivec2(&b, index, offset);
            } else if (glsl_type_is_interface(deref->type)) {
               /* This is the last deref before the block boundary.
                * Everything after this point is a byte offset and will be
                * handled by nir_lower_explicit_io().
                */
               nir_ssa_def *index = get_block_array_index(&b, deref,
                                                          shader_program);
               ptr = nir_vec2(&b, index, nir_imm_int(&b, 0));
            } else {
               /* This will get handled by nir_lower_explicit_io(). */
               break;
            }

            nir_deref_instr *cast = nir_build_deref_cast(&b, ptr, deref->mode,
                                                         deref->type, 0);
            nir_ssa_def_rewrite_uses(&deref->dest.ssa,
                                     nir_src_for_ssa(&cast->dest.ssa));
            nir_deref_instr_remove_if_unused(deref);
            break;
         }

         case nir_instr_type_intrinsic: {
            nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
            switch (intrin->intrinsic) {
            case nir_intrinsic_load_deref: {
               nir_deref_instr *deref = nir_src_as_deref(intrin->src[0]);
               if (!(deref->mode & (nir_var_mem_ubo | nir_var_mem_ssbo)))
                  break;

               /* UBO and SSBO Booleans are 32-bit integers where any non-zero
                * value is considered true.  NIR Booleans, on the other hand
                * are 1-bit values until you get to a very late stage of the
                * compilation process.  We need to turn those 1-bit loads into
                * a 32-bit load wrapped in an i2b to get a proper NIR boolean
                * from the SSBO.
                */
               if (glsl_type_is_boolean(deref->type)) {
                  assert(intrin->dest.is_ssa);
                  b.cursor = nir_after_instr(&intrin->instr);
                  intrin->dest.ssa.bit_size = 32;
                  nir_ssa_def *bval = nir_i2b(&b, &intrin->dest.ssa);
                  nir_ssa_def_rewrite_uses_after(&intrin->dest.ssa,
                                                 nir_src_for_ssa(bval),
                                                 bval->parent_instr);
                  progress = true;
               }
               break;
            }

            case nir_intrinsic_store_deref: {
               nir_deref_instr *deref = nir_src_as_deref(intrin->src[0]);
               if (!(deref->mode & (nir_var_mem_ubo | nir_var_mem_ssbo)))
                  break;

               /* SSBO Booleans are 32-bit integers where any non-zero value
                * is considered true.  NIR Booleans, on the other hand are
                * 1-bit values until you get to a very late stage of the
                * compilation process.  We need to turn those 1-bit stores
                * into a b2i32 followed by a 32-bit store.  Technically the
                * value we write doesn't have to be 0/1 so once Booleans are
                * lowered to 32-bit values, we have an unneeded sanitation
                * step but in practice it doesn't cost much.
                */
               if (glsl_type_is_boolean(deref->type)) {
                  assert(intrin->src[1].is_ssa);
                  b.cursor = nir_before_instr(&intrin->instr);
                  nir_ssa_def *ival = nir_b2i32(&b, intrin->src[1].ssa);
                  nir_instr_rewrite_src(&intrin->instr, &intrin->src[1],
                                        nir_src_for_ssa(ival));
                  progress = true;
               }
               break;
            }

            case nir_intrinsic_copy_deref:
               unreachable("copy_deref should be lowered by now");
               break;

            default:
               /* Nothing to do */
               break;
            }
            break;
         }

         default:
            break; /* Nothing to do */
         }
      }
   }

   if (progress) {
      nir_metadata_preserve(impl, nir_metadata_block_index |
                                  nir_metadata_dominance);
   }

   return progress;
}

bool
gl_nir_lower_buffers(nir_shader *shader,
                     const struct gl_shader_program *shader_program)
{
   bool progress = false;

   /* First, we lower the derefs to turn block variable and array derefs into
    * a nir_address_format_32bit_index_offset pointer.  From there forward,
    * we leave the derefs in place and let nir_lower_explicit_io handle them.
    */
   nir_foreach_function(function, shader) {
      if (function->impl &&
          lower_buffer_interface_derefs_impl(function->impl, shader_program))
         progress = true;
   }

   /* If that did something, we validate and then call nir_lower_explicit_io
    * to finish the process.
    */
   if (progress) {
      nir_validate_shader(shader, "Lowering buffer interface derefs");
      nir_lower_explicit_io(shader, nir_var_mem_ubo | nir_var_mem_ssbo,
                            nir_address_format_32bit_index_offset);
   }

   return progress;
}
