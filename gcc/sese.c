/* Single entry single exit control flow regions.
   Copyright (C) 2008, 2009, 2010
   Free Software Foundation, Inc.
   Contributed by Jan Sjodin <jan.sjodin@amd.com> and
   Sebastian Pop <sebastian.pop@amd.com>.

This file is part of GCC.

GCC is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 3, or (at your option)
any later version.

GCC is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with GCC; see the file COPYING3.  If not see
<http://www.gnu.org/licenses/>.  */

#include "config.h"
#include "system.h"
#include "coretypes.h"
#include "tm.h"
#include "ggc.h"
#include "tree.h"
#include "rtl.h"
#include "basic-block.h"
#include "diagnostic.h"
#include "tree-pretty-print.h"
#include "tree-flow.h"
#include "toplev.h"
#include "tree-dump.h"
#include "timevar.h"
#include "cfgloop.h"
#include "tree-chrec.h"
#include "tree-data-ref.h"
#include "tree-scalar-evolution.h"
#include "tree-pass.h"
#include "domwalk.h"
#include "value-prof.h"
#include "pointer-set.h"
#include "gimple.h"
#include "sese.h"

/* Print to stderr the element ELT.  */

static void
debug_rename_elt (rename_map_elt elt)
{
  fprintf (stderr, "(");
  print_generic_expr (stderr, elt->old_name, 0);
  fprintf (stderr, ", ");
  print_generic_expr (stderr, elt->expr, 0);
  fprintf (stderr, ")\n");
}

/* Helper function for debug_rename_map.  */

static int
debug_rename_map_1 (void **slot, void *s ATTRIBUTE_UNUSED)
{
  struct rename_map_elt_s *entry = (struct rename_map_elt_s *) *slot;
  debug_rename_elt (entry);
  return 1;
}

/* Print to stderr all the elements of RENAME_MAP.  */

DEBUG_FUNCTION void
debug_rename_map (htab_t rename_map)
{
  htab_traverse (rename_map, debug_rename_map_1, NULL);
}

/* Computes a hash function for database element ELT.  */

hashval_t
rename_map_elt_info (const void *elt)
{
  return SSA_NAME_VERSION (((const struct rename_map_elt_s *) elt)->old_name);
}

/* Compares database elements E1 and E2.  */

int
eq_rename_map_elts (const void *e1, const void *e2)
{
  const struct rename_map_elt_s *elt1 = (const struct rename_map_elt_s *) e1;
  const struct rename_map_elt_s *elt2 = (const struct rename_map_elt_s *) e2;

  return (elt1->old_name == elt2->old_name);
}



/* Print to stderr the element ELT.  */

static void
debug_ivtype_elt (ivtype_map_elt elt)
{
  fprintf (stderr, "(%s, ", elt->cloog_iv);
  print_generic_expr (stderr, elt->type, 0);
  fprintf (stderr, ")\n");
}

/* Helper function for debug_ivtype_map.  */

static int
debug_ivtype_map_1 (void **slot, void *s ATTRIBUTE_UNUSED)
{
  struct ivtype_map_elt_s *entry = (struct ivtype_map_elt_s *) *slot;
  debug_ivtype_elt (entry);
  return 1;
}

/* Print to stderr all the elements of MAP.  */

DEBUG_FUNCTION void
debug_ivtype_map (htab_t map)
{
  htab_traverse (map, debug_ivtype_map_1, NULL);
}

/* Computes a hash function for database element ELT.  */

hashval_t
ivtype_map_elt_info (const void *elt)
{
  return htab_hash_pointer (((const struct ivtype_map_elt_s *) elt)->cloog_iv);
}

/* Compares database elements E1 and E2.  */

int
eq_ivtype_map_elts (const void *e1, const void *e2)
{
  const struct ivtype_map_elt_s *elt1 = (const struct ivtype_map_elt_s *) e1;
  const struct ivtype_map_elt_s *elt2 = (const struct ivtype_map_elt_s *) e2;

  return (elt1->cloog_iv == elt2->cloog_iv);
}



/* Record LOOP as occuring in REGION.  */

static void
sese_record_loop (sese region, loop_p loop)
{
  if (sese_contains_loop (region, loop))
    return;

  bitmap_set_bit (SESE_LOOPS (region), loop->num);
  VEC_safe_push (loop_p, heap, SESE_LOOP_NEST (region), loop);
}

/* Build the loop nests contained in REGION.  Returns true when the
   operation was successful.  */

void
build_sese_loop_nests (sese region)
{
  unsigned i;
  basic_block bb;
  struct loop *loop0, *loop1;

  FOR_EACH_BB (bb)
    if (bb_in_sese_p (bb, region))
      {
	struct loop *loop = bb->loop_father;

	/* Only add loops if they are completely contained in the SCoP.  */
	if (loop->header == bb
	    && bb_in_sese_p (loop->latch, region))
	  sese_record_loop (region, loop);
      }

  /* Make sure that the loops in the SESE_LOOP_NEST are ordered.  It
     can be the case that an inner loop is inserted before an outer
     loop.  To avoid this, semi-sort once.  */
  for (i = 0; VEC_iterate (loop_p, SESE_LOOP_NEST (region), i, loop0); i++)
    {
      if (VEC_length (loop_p, SESE_LOOP_NEST (region)) == i + 1)
	break;

      loop1 = VEC_index (loop_p, SESE_LOOP_NEST (region), i + 1);
      if (loop0->num > loop1->num)
	{
	  VEC_replace (loop_p, SESE_LOOP_NEST (region), i, loop1);
	  VEC_replace (loop_p, SESE_LOOP_NEST (region), i + 1, loop0);
	}
    }
}

/* For a USE in BB, if BB is outside REGION, mark the USE in the
   LIVEOUTS set.  */

static void
sese_build_liveouts_use (sese region, bitmap liveouts, basic_block bb,
			 tree use)
{
  unsigned ver;
  basic_block def_bb;

  if (TREE_CODE (use) != SSA_NAME)
    return;

  ver = SSA_NAME_VERSION (use);
  def_bb = gimple_bb (SSA_NAME_DEF_STMT (use));

  if (!def_bb
      || !bb_in_sese_p (def_bb, region)
      || bb_in_sese_p (bb, region))
    return;

  bitmap_set_bit (liveouts, ver);
}

/* Marks for rewrite all the SSA_NAMES defined in REGION and that are
   used in BB that is outside of the REGION.  */

static void
sese_build_liveouts_bb (sese region, bitmap liveouts, basic_block bb)
{
  gimple_stmt_iterator bsi;
  edge e;
  edge_iterator ei;
  ssa_op_iter iter;
  use_operand_p use_p;

  FOR_EACH_EDGE (e, ei, bb->succs)
    for (bsi = gsi_start_phis (e->dest); !gsi_end_p (bsi); gsi_next (&bsi))
      sese_build_liveouts_use (region, liveouts, bb,
			       PHI_ARG_DEF_FROM_EDGE (gsi_stmt (bsi), e));

  for (bsi = gsi_start_bb (bb); !gsi_end_p (bsi); gsi_next (&bsi))
    {
      gimple stmt = gsi_stmt (bsi);

      if (is_gimple_debug (stmt))
	continue;

      FOR_EACH_SSA_USE_OPERAND (use_p, stmt, iter, SSA_OP_ALL_USES)
	sese_build_liveouts_use (region, liveouts, bb, USE_FROM_PTR (use_p));
    }
}

/* For a USE in BB, return true if BB is outside REGION and it's not
   in the LIVEOUTS set.  */

static bool
sese_bad_liveouts_use (sese region, bitmap liveouts, basic_block bb,
		       tree use)
{
  unsigned ver;
  basic_block def_bb;

  if (TREE_CODE (use) != SSA_NAME)
    return false;

  ver = SSA_NAME_VERSION (use);

  /* If it's in liveouts, the variable will get a new PHI node, and
     the debug use will be properly adjusted.  */
  if (bitmap_bit_p (liveouts, ver))
    return false;

  def_bb = gimple_bb (SSA_NAME_DEF_STMT (use));

  if (!def_bb
      || !bb_in_sese_p (def_bb, region)
      || bb_in_sese_p (bb, region))
    return false;

  return true;
}

/* Reset debug stmts that reference SSA_NAMES defined in REGION that
   are not marked as liveouts.  */

static void
sese_reset_debug_liveouts_bb (sese region, bitmap liveouts, basic_block bb)
{
  gimple_stmt_iterator bsi;
  ssa_op_iter iter;
  use_operand_p use_p;

  for (bsi = gsi_start_bb (bb); !gsi_end_p (bsi); gsi_next (&bsi))
    {
      gimple stmt = gsi_stmt (bsi);

      if (!is_gimple_debug (stmt))
	continue;

      FOR_EACH_SSA_USE_OPERAND (use_p, stmt, iter, SSA_OP_ALL_USES)
	if (sese_bad_liveouts_use (region, liveouts, bb,
				   USE_FROM_PTR (use_p)))
	  {
	    gimple_debug_bind_reset_value (stmt);
	    update_stmt (stmt);
	    break;
	  }
    }
}

/* Build the LIVEOUTS of REGION: the set of variables defined inside
   and used outside the REGION.  */

static void
sese_build_liveouts (sese region, bitmap liveouts)
{
  basic_block bb;

  FOR_EACH_BB (bb)
    sese_build_liveouts_bb (region, liveouts, bb);
  if (MAY_HAVE_DEBUG_INSNS)
    FOR_EACH_BB (bb)
      sese_reset_debug_liveouts_bb (region, liveouts, bb);
}

/* Builds a new SESE region from edges ENTRY and EXIT.  */

sese
new_sese (edge entry, edge exit)
{
  sese region = XNEW (struct sese_s);

  SESE_ENTRY (region) = entry;
  SESE_EXIT (region) = exit;
  SESE_LOOPS (region) = BITMAP_ALLOC (NULL);
  SESE_LOOP_NEST (region) = VEC_alloc (loop_p, heap, 3);
  SESE_ADD_PARAMS (region) = true;
  SESE_PARAMS (region) = VEC_alloc (tree, heap, 3);

  return region;
}

/* Deletes REGION.  */

void
free_sese (sese region)
{
  if (SESE_LOOPS (region))
    SESE_LOOPS (region) = BITMAP_ALLOC (NULL);

  VEC_free (tree, heap, SESE_PARAMS (region));
  VEC_free (loop_p, heap, SESE_LOOP_NEST (region));

  XDELETE (region);
}

/* Add exit phis for USE on EXIT.  */

static void
sese_add_exit_phis_edge (basic_block exit, tree use, edge false_e, edge true_e)
{
  gimple phi = create_phi_node (use, exit);

  create_new_def_for (gimple_phi_result (phi), phi,
		      gimple_phi_result_ptr (phi));
  add_phi_arg (phi, use, false_e, UNKNOWN_LOCATION);
  add_phi_arg (phi, use, true_e, UNKNOWN_LOCATION);
}

/* Insert in the block BB phi nodes for variables defined in REGION
   and used outside the REGION.  The code generation moves REGION in
   the else clause of an "if (1)" and generates code in the then
   clause that is at this point empty:

   | if (1)
   |   empty;
   | else
   |   REGION;
*/

void
sese_insert_phis_for_liveouts (sese region, basic_block bb,
			       edge false_e, edge true_e)
{
  unsigned i;
  bitmap_iterator bi;
  bitmap liveouts = BITMAP_ALLOC (NULL);

  update_ssa (TODO_update_ssa);

  sese_build_liveouts (region, liveouts);
  EXECUTE_IF_SET_IN_BITMAP (liveouts, 0, i, bi)
    sese_add_exit_phis_edge (bb, ssa_name (i), false_e, true_e);
  BITMAP_FREE (liveouts);

  update_ssa (TODO_update_ssa);
}

/* Returns the first successor edge of BB with EDGE_TRUE_VALUE flag set.  */

edge
get_true_edge_from_guard_bb (basic_block bb)
{
  edge e;
  edge_iterator ei;

  FOR_EACH_EDGE (e, ei, bb->succs)
    if (e->flags & EDGE_TRUE_VALUE)
      return e;

  gcc_unreachable ();
  return NULL;
}

/* Returns the first successor edge of BB with EDGE_TRUE_VALUE flag cleared.  */

edge
get_false_edge_from_guard_bb (basic_block bb)
{
  edge e;
  edge_iterator ei;

  FOR_EACH_EDGE (e, ei, bb->succs)
    if (!(e->flags & EDGE_TRUE_VALUE))
      return e;

  gcc_unreachable ();
  return NULL;
}

/* Returns the expression associated to OLD_NAME in RENAME_MAP.  */

static tree
get_rename (htab_t rename_map, tree old_name)
{
  struct rename_map_elt_s tmp;
  PTR *slot;

  gcc_assert (TREE_CODE (old_name) == SSA_NAME);
  tmp.old_name = old_name;
  slot = htab_find_slot (rename_map, &tmp, NO_INSERT);

  if (slot && *slot)
    return ((rename_map_elt) *slot)->expr;

  return NULL_TREE;
}

/* Register in RENAME_MAP the rename tuple (OLD_NAME, EXPR).  */

static void
set_rename (htab_t rename_map, tree old_name, tree expr)
{
  struct rename_map_elt_s tmp;
  PTR *slot;

  if (old_name == expr)
    return;

  tmp.old_name = old_name;
  slot = htab_find_slot (rename_map, &tmp, INSERT);

  if (!slot)
    return;

  if (*slot)
    free (*slot);

  *slot = new_rename_map_elt (old_name, expr);
}

/* Renames the scalar uses of the statement COPY, using the
   substitution map RENAME_MAP, inserting the gimplification code at
   GSI_TGT, for the translation REGION, with the original copied
   statement in LOOP, and using the induction variable renaming map
   IV_MAP.  */

static void
rename_uses (gimple copy, htab_t rename_map, gimple_stmt_iterator *gsi_tgt,
	     sese region, loop_p loop, VEC (tree, heap) *iv_map)
{
  use_operand_p use_p;
  ssa_op_iter op_iter;

  if (is_gimple_debug (copy))
    {
      if (gimple_debug_bind_p (copy))
	gimple_debug_bind_reset_value (copy);
      else
	gcc_unreachable ();

      return;
    }

  FOR_EACH_SSA_USE_OPERAND (use_p, copy, op_iter, SSA_OP_ALL_USES)
    {
      tree old_name = USE_FROM_PTR (use_p);
      tree new_expr, scev;
      gimple_seq stmts;

      if (TREE_CODE (old_name) != SSA_NAME
	  || !is_gimple_reg (old_name)
	  || SSA_NAME_IS_DEFAULT_DEF (old_name))
	continue;

      new_expr = get_rename (rename_map, old_name);
      if (new_expr)
	{
	  tree type_old_name = TREE_TYPE (old_name);
	  tree type_new_expr = TREE_TYPE (new_expr);

	  if (type_old_name != type_new_expr
	      || (TREE_CODE (new_expr) != SSA_NAME
		  && is_gimple_reg (old_name)))
	    {
	      tree var = create_tmp_var (type_old_name, "var");

	      if (type_old_name != type_new_expr)
		new_expr = fold_convert (type_old_name, new_expr);

	      new_expr = build2 (MODIFY_EXPR, type_old_name, var, new_expr);
	      new_expr = force_gimple_operand (new_expr, &stmts, true, NULL);
	      gsi_insert_seq_before (gsi_tgt, stmts, GSI_SAME_STMT);
	    }

	  replace_exp (use_p, new_expr);
	  continue;
	}

      scev = scalar_evolution_in_region (region, loop, old_name);

      /* At this point we should know the exact scev for each
	 scalar SSA_NAME used in the scop: all the other scalar
	 SSA_NAMEs should have been translated out of SSA using
	 arrays with one element.  */
      gcc_assert (!chrec_contains_undetermined (scev));

      new_expr = chrec_apply_map (scev, iv_map);

      /* The apply should produce an expression tree containing
	 the uses of the new induction variables.  We should be
	 able to use new_expr instead of the old_name in the newly
	 generated loop nest.  */
      gcc_assert (!chrec_contains_undetermined (new_expr)
		  && !tree_contains_chrecs (new_expr, NULL));

      /* Replace the old_name with the new_expr.  */
      new_expr = force_gimple_operand (new_expr, &stmts, true, NULL);
      gsi_insert_seq_before (gsi_tgt, stmts, GSI_SAME_STMT);
      replace_exp (use_p, new_expr);
      set_rename (rename_map, old_name, new_expr);
    }
}

/* Duplicates the statements of basic block BB into basic block NEW_BB
   and compute the new induction variables according to the IV_MAP.  */

static void
graphite_copy_stmts_from_block (basic_block bb, basic_block new_bb,
				htab_t rename_map,
				VEC (tree, heap) *iv_map, sese region)
{
  gimple_stmt_iterator gsi, gsi_tgt;
  loop_p loop = bb->loop_father;

  gsi_tgt = gsi_start_bb (new_bb);
  for (gsi = gsi_start_bb (bb); !gsi_end_p (gsi); gsi_next (&gsi))
    {
      def_operand_p def_p;
      ssa_op_iter op_iter;
      gimple stmt = gsi_stmt (gsi);
      gimple copy;
      tree lhs;

      /* Do not copy labels or conditions.  */
      if (gimple_code (stmt) == GIMPLE_LABEL
	  || gimple_code (stmt) == GIMPLE_COND)
	continue;

      /* Do not copy induction variables.  */
      if (is_gimple_assign (stmt)
	  && (lhs = gimple_assign_lhs (stmt))
	  && TREE_CODE (lhs) == SSA_NAME
	  && is_gimple_reg (lhs)
	  && scev_analyzable_p (lhs, region))
	continue;

      /* Create a new copy of STMT and duplicate STMT's virtual
	 operands.  */
      copy = gimple_copy (stmt);
      gsi_insert_after (&gsi_tgt, copy, GSI_NEW_STMT);
      mark_sym_for_renaming (gimple_vop (cfun));

      maybe_duplicate_eh_stmt (copy, stmt);
      gimple_duplicate_stmt_histograms (cfun, copy, cfun, stmt);

      /* Create new names for all the definitions created by COPY and
	 add replacement mappings for each new name.  */
      FOR_EACH_SSA_DEF_OPERAND (def_p, copy, op_iter, SSA_OP_ALL_DEFS)
 	{
 	  tree old_name = DEF_FROM_PTR (def_p);
 	  tree new_name = create_new_def_for (old_name, copy, def_p);
	  set_rename (rename_map, old_name, new_name);
 	}

      rename_uses (copy, rename_map, &gsi_tgt, region, loop, iv_map);

      update_stmt (copy);
    }
}

/* Copies BB and includes in the copied BB all the statements that can
   be reached following the use-def chains from the memory accesses,
   and returns the next edge following this new block.  */

edge
copy_bb_and_scalar_dependences (basic_block bb, sese region,
				edge next_e, VEC (tree, heap) *iv_map)
{
  basic_block new_bb = split_edge (next_e);
  htab_t rename_map = htab_create (10, rename_map_elt_info,
				   eq_rename_map_elts, free);

  next_e = single_succ_edge (new_bb);
  graphite_copy_stmts_from_block (bb, new_bb, rename_map, iv_map, region);
  remove_phi_nodes (new_bb);
  htab_delete (rename_map);

  return next_e;
}

/* Returns the outermost loop in SCOP that contains BB.  */

struct loop *
outermost_loop_in_sese (sese region, basic_block bb)
{
  struct loop *nest;

  nest = bb->loop_father;
  while (loop_outer (nest)
	 && loop_in_sese_p (loop_outer (nest), region))
    nest = loop_outer (nest);

  return nest;
}

/* Sets the false region of an IF_REGION to REGION.  */

void
if_region_set_false_region (ifsese if_region, sese region)
{
  basic_block condition = if_region_get_condition_block (if_region);
  edge false_edge = get_false_edge_from_guard_bb (condition);
  basic_block dummy = false_edge->dest;
  edge entry_region = SESE_ENTRY (region);
  edge exit_region = SESE_EXIT (region);
  basic_block before_region = entry_region->src;
  basic_block last_in_region = exit_region->src;
  void **slot = htab_find_slot_with_hash (current_loops->exits, exit_region,
					  htab_hash_pointer (exit_region),
					  NO_INSERT);

  entry_region->flags = false_edge->flags;
  false_edge->flags = exit_region->flags;

  redirect_edge_pred (entry_region, condition);
  redirect_edge_pred (exit_region, before_region);
  redirect_edge_pred (false_edge, last_in_region);
  redirect_edge_succ (false_edge, single_succ (dummy));
  delete_basic_block (dummy);

  exit_region->flags = EDGE_FALLTHRU;
  recompute_all_dominators ();

  SESE_EXIT (region) = false_edge;

  if (if_region->false_region)
    free (if_region->false_region);
  if_region->false_region = region;

  if (slot)
    {
      struct loop_exit *loop_exit = ggc_alloc_cleared_loop_exit ();

      memcpy (loop_exit, *((struct loop_exit **) slot), sizeof (struct loop_exit));
      htab_clear_slot (current_loops->exits, slot);

      slot = htab_find_slot_with_hash (current_loops->exits, false_edge,
				       htab_hash_pointer (false_edge),
				       INSERT);
      loop_exit->e = false_edge;
      *slot = loop_exit;
      false_edge->src->loop_father->exits->next = loop_exit;
    }
}

/* Creates an IFSESE with CONDITION on edge ENTRY.  */

static ifsese
create_if_region_on_edge (edge entry, tree condition)
{
  edge e;
  edge_iterator ei;
  sese sese_region = XNEW (struct sese_s);
  sese true_region = XNEW (struct sese_s);
  sese false_region = XNEW (struct sese_s);
  ifsese if_region = XNEW (struct ifsese_s);
  edge exit = create_empty_if_region_on_edge (entry, condition);

  if_region->region = sese_region;
  if_region->region->entry = entry;
  if_region->region->exit = exit;

  FOR_EACH_EDGE (e, ei, entry->dest->succs)
    {
      if (e->flags & EDGE_TRUE_VALUE)
	{
	  true_region->entry = e;
	  true_region->exit = single_succ_edge (e->dest);
	  if_region->true_region = true_region;
	}
      else if (e->flags & EDGE_FALSE_VALUE)
	{
	  false_region->entry = e;
	  false_region->exit = single_succ_edge (e->dest);
	  if_region->false_region = false_region;
	}
    }

  return if_region;
}

/* Moves REGION in a condition expression:
   | if (1)
   |   ;
   | else
   |   REGION;
*/

ifsese
move_sese_in_condition (sese region)
{
  basic_block pred_block = split_edge (SESE_ENTRY (region));
  ifsese if_region;

  SESE_ENTRY (region) = single_succ_edge (pred_block);
  if_region = create_if_region_on_edge (single_pred_edge (pred_block), integer_one_node);
  if_region_set_false_region (if_region, region);

  return if_region;
}

/* Replaces the condition of the IF_REGION with CONDITION:
   | if (CONDITION)
   |   true_region;
   | else
   |   false_region;
*/

void
set_ifsese_condition (ifsese if_region, tree condition)
{
  sese region = if_region->region;
  edge entry = region->entry;
  basic_block bb = entry->dest;
  gimple last = last_stmt (bb);
  gimple_stmt_iterator gsi = gsi_last_bb (bb);
  gimple cond_stmt;

  gcc_assert (gimple_code (last) == GIMPLE_COND);

  gsi_remove (&gsi, true);
  gsi = gsi_last_bb (bb);
  condition = force_gimple_operand_gsi (&gsi, condition, true, NULL,
					false, GSI_NEW_STMT);
  cond_stmt = gimple_build_cond_from_tree (condition, NULL_TREE, NULL_TREE);
  gsi = gsi_last_bb (bb);
  gsi_insert_after (&gsi, cond_stmt, GSI_NEW_STMT);
}

/* Returns the scalar evolution of T in REGION.  Every variable that
   is not defined in the REGION is considered a parameter.  */

tree
scalar_evolution_in_region (sese region, loop_p loop, tree t)
{
  gimple def;
  struct loop *def_loop;
  basic_block before = block_before_sese (region);

  if (TREE_CODE (t) != SSA_NAME
      || loop_in_sese_p (loop, region))
    return instantiate_scev (before, loop,
			     analyze_scalar_evolution (loop, t));

  if (!defined_in_sese_p (t, region))
    return t;

  def = SSA_NAME_DEF_STMT (t);
  def_loop = loop_containing_stmt (def);

  if (loop_in_sese_p (def_loop, region))
    {
      t = analyze_scalar_evolution (def_loop, t);
      def_loop = superloop_at_depth (def_loop, loop_depth (loop) + 1);
      t = compute_overall_effect_of_inner_loop (def_loop, t);
      return t;
    }
  else
    return instantiate_scev (before, loop, t);
}
