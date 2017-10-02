/*
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2001-2002 by NaN Holding BV.
 * All rights reserved.
 *
 * The Original Code is: all of this file.
 *
 * Contributor(s): none yet.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file blender/editors/sculpt_paint/paint_vertex.c
 *  \ingroup edsculpt
 *
 * Used for vertex color & weight paint and mode switching.
 *
 * \note This file is already big,
 * use `paint_vertex_color_ops.c` & `paint_vertex_weight_ops.c` for general purpose operators.
 */

#include "MEM_guardedalloc.h"

#include "BLI_listbase.h"
#include "BLI_rect.h"
#include "BLI_math.h"
#include "BLI_array_utils.h"
#include "BLI_task.h"

#include "DNA_armature_types.h"
#include "DNA_mesh_types.h"
#include "DNA_particle_types.h"
#include "DNA_scene_types.h"
#include "DNA_brush_types.h"
#include "DNA_object_types.h"

#include "RNA_access.h"
#include "RNA_define.h"

#include "BKE_brush.h"
#include "BKE_context.h"
#include "BKE_depsgraph.h"
#include "BKE_deform.h"
#include "BKE_mesh.h"
#include "BKE_mesh_mapping.h"
#include "BKE_object_deform.h"
#include "BKE_paint.h"
#include "BKE_report.h"
#include "BKE_subsurf.h"

#include "WM_api.h"
#include "WM_types.h"

#include "ED_object.h"
#include "ED_mesh.h"
#include "ED_screen.h"
#include "ED_view3d.h"

#include "bmesh.h"
#include "BKE_ccg.h"

#include "sculpt_intern.h"
#include "paint_intern.h"  /* own include */

/* Use for 'blur' brush, align with PBVH nodes, created and freed on each update. */
struct VPaintAverageAccum {
	uint len;
	uint value[3];
};

struct WPaintAverageAccum {
	uint len;
	double value;
};

static void defweight_prev_restore_or_init(MDeformVert *dvert_prev, MDeformVert *dvert_curr, int index)
{
	MDeformVert *dv_curr = &dvert_curr[index];
	MDeformVert *dv_prev = &dvert_prev[index];
	if (dv_prev->flag == 1) {
		dv_prev->flag = 0;
		defvert_copy(dv_prev, dv_curr);
	}
	else {
		defvert_copy(dv_curr, dv_prev);
	}
}

/* check if we can do partial updates and have them draw realtime
 * (without rebuilding the 'derivedFinal') */
static bool vertex_paint_use_fast_update_check(Object *ob)
{
	DerivedMesh *dm = ob->derivedFinal;

	if (dm) {
		Mesh *me = BKE_mesh_from_object(ob);
		if (me && me->mloopcol) {
			return (me->mloopcol == CustomData_get_layer(&dm->loopData, CD_MLOOPCOL));
		}
	}

	return false;
}

static void paint_last_stroke_update(Scene *scene, ARegion *ar, const float mval[2])
{
	const int mval_i[2] = {mval[0], mval[1]};
	float world[3];

	if (ED_view3d_autodist_simple(ar, mval_i, world, 0, NULL)) {
		UnifiedPaintSettings *ups = &scene->toolsettings->unified_paint_settings;
		ups->average_stroke_counter++;
		add_v3_v3(ups->average_stroke_accum, world);
		ups->last_stroke_valid = true;
	}
}

/* polling - retrieve whether cursor should be set or operator should be done */

/* Returns true if vertex paint mode is active */
int vertex_paint_mode_poll(bContext *C)
{
	Object *ob = CTX_data_active_object(C);

	return ob && ob->mode == OB_MODE_VERTEX_PAINT && ((Mesh *)ob->data)->totpoly;
}

int vertex_paint_poll(bContext *C)
{
	if (vertex_paint_mode_poll(C) &&
	    BKE_paint_brush(&CTX_data_tool_settings(C)->vpaint->paint))
	{
		ScrArea *sa = CTX_wm_area(C);
		if (sa && sa->spacetype == SPACE_VIEW3D) {
			ARegion *ar = CTX_wm_region(C);
			if (ar->regiontype == RGN_TYPE_WINDOW)
				return 1;
		}
	}
	return 0;
}

int weight_paint_mode_poll(bContext *C)
{
	Object *ob = CTX_data_active_object(C);

	return ob && ob->mode == OB_MODE_WEIGHT_PAINT && ((Mesh *)ob->data)->totpoly;
}

int weight_paint_poll(bContext *C)
{
	Object *ob = CTX_data_active_object(C);
	ScrArea *sa;

	if ((ob != NULL) &&
	    (ob->mode & OB_MODE_WEIGHT_PAINT) &&
	    (BKE_paint_brush(&CTX_data_tool_settings(C)->wpaint->paint) != NULL) &&
	    (sa = CTX_wm_area(C)) &&
	    (sa->spacetype == SPACE_VIEW3D))
	{
		ARegion *ar = CTX_wm_region(C);
		if (ar->regiontype == RGN_TYPE_WINDOW) {
			return 1;
		}
	}
	return 0;
}

static VPaint *new_vpaint(int wpaint)
{
	VPaint *vp = MEM_callocN(sizeof(VPaint), "VPaint");

	vp->flag = (wpaint) ? 0 : VP_SPRAY;
	vp->paint.flags |= PAINT_SHOW_BRUSH;

	return vp;
}

uint vpaint_get_current_col(Scene *scene, VPaint *vp)
{
	Brush *brush = BKE_paint_brush(&vp->paint);
	uchar col[4];
	rgb_float_to_uchar(col, BKE_brush_color_get(scene, brush));
	col[3] = 255; /* alpha isn't used, could even be removed to speedup paint a little */
	return *(uint *)col;
}

/* wpaint has 'wpaint_blend' */
static uint vpaint_blend(
        VPaint *vp, uint color_curr, uint color_orig,
        uint color_paint, const int alpha_i,
        /* pre scaled from [0-1] --> [0-255] */
        const int brush_alpha_value_i)
{
	Brush *brush = BKE_paint_brush(&vp->paint);
	const int tool = brush->vertexpaint_tool;

	uint color_blend = ED_vpaint_blend_tool(tool, color_curr, color_paint, alpha_i);

	/* if no spray, clip color adding with colorig & orig alpha */
	if ((vp->flag & VP_SPRAY) == 0) {
		uint color_test, a;
		char *cp, *ct, *co;

		color_test = ED_vpaint_blend_tool(tool, color_orig, color_paint, brush_alpha_value_i);

		cp = (char *)&color_blend;
		ct = (char *)&color_test;
		co = (char *)&color_orig;

		for (a = 0; a < 4; a++) {
			if (ct[a] < co[a]) {
				if (cp[a] < ct[a]) cp[a] = ct[a];
				else if (cp[a] > co[a]) cp[a] = co[a];
			}
			else {
				if (cp[a] < co[a]) cp[a] = co[a];
				else if (cp[a] > ct[a]) cp[a] = ct[a];
			}
		}
	}

	if ((brush->flag & BRUSH_LOCK_ALPHA) &&
	    !ELEM(tool, PAINT_BLEND_ALPHA_SUB, PAINT_BLEND_ALPHA_ADD))
	{
		char *cp, *cc;
		cp = (char *)&color_blend;
		cc = (char *)&color_curr;
		cp[3] = cc[3];
	}

	return color_blend;
}


/* whats _dl mean? */
static float calc_vp_strength_col_dl(
        VPaint *vp, const ViewContext *vc, const float co[3],
        const float mval[2], const float brush_size_pressure, float rgba[4])
{
	float co_ss[2];  /* screenspace */

	if (ED_view3d_project_float_object(
	        vc->ar,
	        co, co_ss,
	        V3D_PROJ_TEST_CLIP_BB | V3D_PROJ_TEST_CLIP_NEAR) == V3D_PROJ_RET_OK)
	{
		const float dist_sq = len_squared_v2v2(mval, co_ss);

		if (dist_sq <= SQUARE(brush_size_pressure)) {
			Brush *brush = BKE_paint_brush(&vp->paint);
			const float dist = sqrtf(dist_sq);
			float factor;

			if (brush->mtex.tex && rgba) {
				if (brush->mtex.brush_map_mode == MTEX_MAP_MODE_3D) {
					BKE_brush_sample_tex_3D(vc->scene, brush, co, rgba, 0, NULL);
				}
				else {
					const float co_ss_3d[3] = {co_ss[0], co_ss[1], 0.0f};  /* we need a 3rd empty value */
					BKE_brush_sample_tex_3D(vc->scene, brush, co_ss_3d, rgba, 0, NULL);
				}
				factor = rgba[3];
			}
			else {
				factor = 1.0f;
			}
			return factor * BKE_brush_curve_strength_clamped(brush, dist, brush_size_pressure);
		}
	}
	if (rgba)
		zero_v4(rgba);
	return 0.0f;
}

static float calc_vp_alpha_col_dl(
        VPaint *vp, const ViewContext *vc,
        float vpimat[3][3], const DMCoNo *v_co_no,
        const float mval[2],
        const float brush_size_pressure, const float brush_alpha_pressure, float rgba[4])
{
	float strength = calc_vp_strength_col_dl(vp, vc, v_co_no->co, mval, brush_size_pressure, rgba);

	if (strength > 0.0f) {
		float alpha = brush_alpha_pressure * strength;

		if (vp->flag & VP_NORMALS) {
			float dvec[3];

			/* transpose ! */
			dvec[2] = dot_v3v3(vpimat[2], v_co_no->no);
			if (dvec[2] > 0.0f) {
				dvec[0] = dot_v3v3(vpimat[0], v_co_no->no);
				dvec[1] = dot_v3v3(vpimat[1], v_co_no->no);

				alpha *= dvec[2] / len_v3(dvec);
			}
			else {
				return 0.0f;
			}
		}

		return alpha;
	}

	return 0.0f;
}

/* vpaint has 'vpaint_blend' */
static float wpaint_blend(
        VPaint *wp, float weight,
        const float alpha, float paintval,
        const float UNUSED(brush_alpha_value),
        const short do_flip)
{
	Brush *brush = BKE_paint_brush(&wp->paint);
	int tool = brush->vertexpaint_tool;

	if (do_flip) {
		switch (tool) {
			case PAINT_BLEND_MIX:
				paintval = 1.f - paintval; break;
			case PAINT_BLEND_ADD:
				tool = PAINT_BLEND_SUB; break;
			case PAINT_BLEND_SUB:
				tool = PAINT_BLEND_ADD; break;
			case PAINT_BLEND_LIGHTEN:
				tool = PAINT_BLEND_DARKEN; break;
			case PAINT_BLEND_DARKEN:
				tool = PAINT_BLEND_LIGHTEN; break;
		}
	}

	weight = ED_wpaint_blend_tool(tool, weight, paintval, alpha);

	CLAMP(weight, 0.0f, 1.0f);

	return weight;
}

/* ----------------------------------------------------- */

static void do_weight_paint_normalize_all(MDeformVert *dvert, const int defbase_tot, const bool *vgroup_validmap)
{
	float sum = 0.0f, fac;
	uint i, tot = 0;
	MDeformWeight *dw;

	for (i = dvert->totweight, dw = dvert->dw; i != 0; i--, dw++) {
		if (dw->def_nr < defbase_tot && vgroup_validmap[dw->def_nr]) {
			tot++;
			sum += dw->weight;
		}
	}

	if ((tot == 0) || (sum == 1.0f)) {
		return;
	}

	if (sum != 0.0f) {
		fac = 1.0f / sum;

		for (i = dvert->totweight, dw = dvert->dw; i != 0; i--, dw++) {
			if (dw->def_nr < defbase_tot && vgroup_validmap[dw->def_nr]) {
				dw->weight *= fac;
			}
		}
	}
	else {
		/* hrmf, not a factor in this case */
		fac = 1.0f / tot;

		for (i = dvert->totweight, dw = dvert->dw; i != 0; i--, dw++) {
			if (dw->def_nr < defbase_tot && vgroup_validmap[dw->def_nr]) {
				dw->weight = fac;
			}
		}
	}
}

/**
 * A version of #do_weight_paint_normalize_all that includes locked weights
 * but only changes unlocked weights.
 */
static bool do_weight_paint_normalize_all_locked(
        MDeformVert *dvert, const int defbase_tot, const bool *vgroup_validmap,
        const bool *lock_flags)
{
	float sum = 0.0f, fac;
	float sum_unlock = 0.0f;
	float lock_weight = 0.0f;
	uint i, tot = 0;
	MDeformWeight *dw;

	if (lock_flags == NULL) {
		do_weight_paint_normalize_all(dvert, defbase_tot, vgroup_validmap);
		return true;
	}

	for (i = dvert->totweight, dw = dvert->dw; i != 0; i--, dw++) {
		if (dw->def_nr < defbase_tot && vgroup_validmap[dw->def_nr]) {
			sum += dw->weight;

			if (lock_flags[dw->def_nr]) {
				lock_weight += dw->weight;
			}
			else {
				tot++;
				sum_unlock += dw->weight;
			}
		}
	}

	if (sum == 1.0f) {
		return true;
	}

	if (tot == 0) {
		return false;
	}

	if (lock_weight >= 1.0f) {
		/* locked groups make it impossible to fully normalize,
		 * zero out what we can and return false */
		for (i = dvert->totweight, dw = dvert->dw; i != 0; i--, dw++) {
			if (dw->def_nr < defbase_tot && vgroup_validmap[dw->def_nr]) {
				if (lock_flags[dw->def_nr] == false) {
					dw->weight = 0.0f;
				}
			}
		}

		return (lock_weight == 1.0f);
	}
	else if (sum_unlock != 0.0f) {
		fac = (1.0f - lock_weight) / sum_unlock;

		for (i = dvert->totweight, dw = dvert->dw; i != 0; i--, dw++) {
			if (dw->def_nr < defbase_tot && vgroup_validmap[dw->def_nr]) {
				if (lock_flags[dw->def_nr] == false) {
					dw->weight *= fac;
					/* paranoid but possibly with float error */
					CLAMP(dw->weight, 0.0f, 1.0f);
				}
			}
		}
	}
	else {
		/* hrmf, not a factor in this case */
		fac = (1.0f - lock_weight) / tot;
		/* paranoid but possibly with float error */
		CLAMP(fac, 0.0f, 1.0f);

		for (i = dvert->totweight, dw = dvert->dw; i != 0; i--, dw++) {
			if (dw->def_nr < defbase_tot && vgroup_validmap[dw->def_nr]) {
				if (lock_flags[dw->def_nr] == false) {
					dw->weight = fac;
				}
			}
		}
	}

	return true;
}

/**
 * \note same as function above except it does a second pass without active group
 * if normalize fails with it.
 */
static void do_weight_paint_normalize_all_locked_try_active(
        MDeformVert *dvert, const int defbase_tot, const bool *vgroup_validmap,
        const bool *lock_flags, const bool *lock_with_active)
{
	/* first pass with both active and explicitly locked groups restricted from change */

	bool success = do_weight_paint_normalize_all_locked(dvert, defbase_tot, vgroup_validmap, lock_with_active);

	if (!success) {
		/**
		 * Locks prevented the first pass from full completion, so remove restriction on active group; e.g:
		 *
		 * - With 1.0 weight painted into active:
		 *   nonzero locked weight; first pass zeroed out unlocked weight; scale 1 down to fit.
		 * - With 0.0 weight painted into active:
		 *   no unlocked groups; first pass did nothing; increase 0 to fit.
		 */
		do_weight_paint_normalize_all_locked(dvert, defbase_tot, vgroup_validmap, lock_flags);
	}
}

#if 0 /* UNUSED */
static bool has_unselected_unlocked_bone_group(
        int defbase_tot, bool *defbase_sel, int selected,
        const bool *lock_flags, const bool *vgroup_validmap)
{
	int i;
	if (defbase_tot == selected) {
		return false;
	}
	for (i = 0; i < defbase_tot; i++) {
		if (vgroup_validmap[i] && !defbase_sel[i] && !lock_flags[i]) {
			return true;
		}
	}
	return false;
}
#endif

static void multipaint_clamp_change(
        MDeformVert *dvert, const int defbase_tot, const bool *defbase_sel,
        float *change_p)
{
	int i;
	MDeformWeight *dw;
	float val;
	float change = *change_p;

	/* verify that the change does not cause values exceeding 1 and clamp it */
	for (i = dvert->totweight, dw = dvert->dw; i != 0; i--, dw++) {
		if (dw->def_nr < defbase_tot && defbase_sel[dw->def_nr]) {
			if (dw->weight) {
				val = dw->weight * change;
				if (val > 1) {
					change = 1.0f / dw->weight;
				}
			}
		}
	}

	*change_p = change;
}

static bool multipaint_verify_change(MDeformVert *dvert, const int defbase_tot, float change, const bool *defbase_sel)
{
	int i;
	MDeformWeight *dw;
	float val;

	/* in case the change is reduced, you need to recheck
	 * the earlier values to make sure they are not 0
	 * (precision error) */
	for (i = dvert->totweight, dw = dvert->dw; i != 0; i--, dw++) {
		if (dw->def_nr < defbase_tot && defbase_sel[dw->def_nr]) {
			if (dw->weight) {
				val = dw->weight * change;
				/* the value should never reach zero while multi-painting if it
				 * was nonzero beforehand */
				if (val <= 0) {
					return false;
				}
			}
		}
	}

	return true;
}

static void multipaint_apply_change(MDeformVert *dvert, const int defbase_tot, float change, const bool *defbase_sel)
{
	int i;
	MDeformWeight *dw;

	/* apply the valid change */
	for (i = dvert->totweight, dw = dvert->dw; i != 0; i--, dw++) {
		if (dw->def_nr < defbase_tot && defbase_sel[dw->def_nr]) {
			if (dw->weight) {
				dw->weight = dw->weight * change;
				CLAMP(dw->weight, 0.0f, 1.0f);
			}
		}
	}
}

/**
 * Variables stored both for 'active' and 'mirror' sides.
 */
struct WeightPaintGroupData {
	/** index of active group or its mirror
	 *
	 * - 'active' is always `ob->actdef`.
	 * - 'mirror' is -1 when 'ME_EDIT_MIRROR_X' flag id disabled,
	 *   otherwise this will be set to the mirror or the active group (if the group isn't mirrored).
	 */
	int index;
	/** lock that includes the 'index' as locked too
	 *
	 * - 'active' is set of locked or active/selected groups
	 * - 'mirror' is set of locked or mirror groups
	 */
	const bool *lock;
};

/* struct to avoid passing many args each call to do_weight_paint_vertex()
 * this _could_ be made a part of the operators 'WPaintData' struct, or at
 * least a member, but for now keep its own struct, initialized on every
 * paint stroke update - campbell */
typedef struct WeightPaintInfo {

	int defbase_tot;

	/* both must add up to 'defbase_tot' */
	int defbase_tot_sel;
	int defbase_tot_unsel;

	struct WeightPaintGroupData active, mirror;

	/* boolean array for locked bones,
	 * length of defbase_tot */
	const bool *lock_flags;
	/* boolean array for selected bones,
	 * length of defbase_tot, cant be const because of how its passed */
	const bool *defbase_sel;
	/* same as WeightPaintData.vgroup_validmap,
	 * only added here for convenience */
	const bool *vgroup_validmap;

	bool do_flip;
	bool do_multipaint;
	bool do_auto_normalize;

	float brush_alpha_value;  /* result of BKE_brush_alpha_get() */
} WeightPaintInfo;

static void do_weight_paint_vertex_single(
        /* vars which remain the same for every vert */
        VPaint *wp, Object *ob, const WeightPaintInfo *wpi,
        /* vars which change on each stroke */
        const uint index, float alpha, float paintweight)
{
	Mesh *me = ob->data;
	MDeformVert *dv = &me->dvert[index];
	bool topology = (me->editflag & ME_EDIT_MIRROR_TOPO) != 0;

	MDeformWeight *dw;

	/* mirror vars */
	int index_mirr;
	int vgroup_mirr;

	MDeformVert *dv_mirr;
	MDeformWeight *dw_mirr;

	/* from now on we can check if mirrors enabled if this var is -1 and not bother with the flag */
	if (me->editflag & ME_EDIT_MIRROR_X) {
		index_mirr = mesh_get_x_mirror_vert(ob, NULL, index, topology);
		vgroup_mirr = wpi->mirror.index;

		/* another possible error - mirror group _and_ active group are the same (which is fine),
		 * but we also are painting onto a center vertex - this would paint the same weight twice */
		if (index_mirr == index && vgroup_mirr == wpi->active.index) {
			index_mirr = vgroup_mirr = -1;
		}
	}
	else {
		index_mirr = vgroup_mirr = -1;
	}

	if ((wp->flag & VP_SPRAY) == 0) {
		struct MDeformVert *dvert_prev = ob->sculpt->mode.wpaint.dvert_prev;
		defweight_prev_restore_or_init(dvert_prev, me->dvert, index);
		if (index_mirr != -1) {
			defweight_prev_restore_or_init(dvert_prev, me->dvert, index_mirr);
		}
	}

	if (wp->flag & VP_ONLYVGROUP) {
		dw = defvert_find_index(dv, wpi->active.index);
	}
	else {
		dw = defvert_verify_index(dv, wpi->active.index);
	}

	if (dw == NULL) {
		return;
	}

	/* get the mirror def vars */
	if (index_mirr != -1) {
		dv_mirr = &me->dvert[index_mirr];
		if (wp->flag & VP_ONLYVGROUP) {
			dw_mirr = defvert_find_index(dv_mirr, vgroup_mirr);

			if (dw_mirr == NULL) {
				index_mirr = vgroup_mirr = -1;
				dv_mirr = NULL;
			}
		}
		else {
			if (index != index_mirr) {
				dw_mirr = defvert_verify_index(dv_mirr, vgroup_mirr);
			}
			else {
				/* dv and dv_mirr are the same */
				int totweight_prev = dv_mirr->totweight;
				int dw_offset = (int)(dw - dv_mirr->dw);
				dw_mirr = defvert_verify_index(dv_mirr, vgroup_mirr);

				/* if we added another, get our old one back */
				if (totweight_prev != dv_mirr->totweight) {
					dw = &dv_mirr->dw[dw_offset];
				}
			}
		}
	}
	else {
		dv_mirr = NULL;
		dw_mirr = NULL;
	}

	/* If there are no normalize-locks or multipaint,
	 * then there is no need to run the more complicated checks */

	{
		dw->weight = wpaint_blend(
		        wp, dw->weight, alpha, paintweight,
		        wpi->brush_alpha_value, wpi->do_flip);

		/* WATCH IT: take care of the ordering of applying mirror -> normalize,
		 * can give wrong results [#26193], least confusing if normalize is done last */

		/* apply mirror */
		if (index_mirr != -1) {
			/* copy, not paint again */
			dw_mirr->weight = dw->weight;
		}

		/* apply normalize */
		if (wpi->do_auto_normalize) {
			/* note on normalize - this used to be applied after painting and normalize all weights,
			 * in some ways this is good because there is feedback where the more weights involved would
			 * 'resist' so you couldn't instantly zero out other weights by painting 1.0 on the active.
			 *
			 * However this gave a problem since applying mirror, then normalize both verts
			 * the resulting weight wont match on both sides.
			 *
			 * If this 'resisting', slower normalize is nicer, we could call
			 * do_weight_paint_normalize_all() and only use...
			 * do_weight_paint_normalize_all_active() when normalizing the mirror vertex.
			 * - campbell
			 */
			do_weight_paint_normalize_all_locked_try_active(
			        dv, wpi->defbase_tot, wpi->vgroup_validmap, wpi->lock_flags, wpi->active.lock);

			if (index_mirr != -1) {
				/* only normalize if this is not a center vertex, else we get a conflict, normalizing twice */
				if (index != index_mirr) {
					do_weight_paint_normalize_all_locked_try_active(
					        dv_mirr, wpi->defbase_tot, wpi->vgroup_validmap, wpi->lock_flags, wpi->mirror.lock);
				}
				else {
					/* this case accounts for...
					 * - painting onto a center vertex of a mesh
					 * - x mirror is enabled
					 * - auto normalize is enabled
					 * - the group you are painting onto has a L / R version
					 *
					 * We want L/R vgroups to have the same weight but this cant be if both are over 0.5,
					 * We _could_ have special check for that, but this would need its own normalize function which
					 * holds 2 groups from changing at once.
					 *
					 * So! just balance out the 2 weights, it keeps them equal and everything normalized.
					 *
					 * While it wont hit the desired weight immediately as the user waggles their mouse,
					 * constant painting and re-normalizing will get there. this is also just simpler logic.
					 * - campbell */
					dw_mirr->weight = dw->weight = (dw_mirr->weight + dw->weight) * 0.5f;
				}
			}
		}
	}
}

static void do_weight_paint_vertex_multi(
        /* vars which remain the same for every vert */
        VPaint *wp, Object *ob, const WeightPaintInfo *wpi,
        /* vars which change on each stroke */
        const uint index, float alpha, float paintweight)
{
	Mesh *me = ob->data;
	MDeformVert *dv = &me->dvert[index];
	bool topology = (me->editflag & ME_EDIT_MIRROR_TOPO) != 0;

	/* mirror vars */
	int index_mirr = -1;
	MDeformVert *dv_mirr = NULL;

	/* weights */
	float curw, neww, change, curw_mirr, change_mirr;

	/* from now on we can check if mirrors enabled if this var is -1 and not bother with the flag */
	if (me->editflag & ME_EDIT_MIRROR_X) {
		index_mirr = mesh_get_x_mirror_vert(ob, NULL, index, topology);

		if (index_mirr != -1 && index_mirr != index) {
			dv_mirr = &me->dvert[index_mirr];
		}
		else {
			index_mirr = -1;
		}
	}

	if ((wp->flag & VP_SPRAY) == 0) {
		struct MDeformVert *dvert_prev = ob->sculpt->mode.wpaint.dvert_prev;
		defweight_prev_restore_or_init(dvert_prev, me->dvert, index);
		if (index_mirr != -1) {
			defweight_prev_restore_or_init(dvert_prev, me->dvert, index_mirr);
		}
	}

	/* compute weight change by applying the brush to average or sum of group weights */
	curw = BKE_defvert_multipaint_collective_weight(
	        dv, wpi->defbase_tot, wpi->defbase_sel, wpi->defbase_tot_sel, wpi->do_auto_normalize);

	if (curw == 0.0f) {
		/* note: no weight to assign to this vertex, could add all groups? */
		return;
	}

	neww = wpaint_blend(wp, curw, alpha, paintweight, wpi->brush_alpha_value, wpi->do_flip);

	change = neww / curw;

	/* verify for all groups that 0 < result <= 1 */
	multipaint_clamp_change(dv, wpi->defbase_tot, wpi->defbase_sel, &change);

	if (dv_mirr != NULL) {
		curw_mirr = BKE_defvert_multipaint_collective_weight(
		        dv_mirr, wpi->defbase_tot, wpi->defbase_sel, wpi->defbase_tot_sel, wpi->do_auto_normalize);

		if (curw_mirr == 0.0f) {
			/* can't mirror into a zero weight vertex */
			dv_mirr = NULL;
		}
		else {
			/* mirror is changed to achieve the same collective weight value */
			float orig = change_mirr = curw * change / curw_mirr;

			multipaint_clamp_change(dv_mirr, wpi->defbase_tot, wpi->defbase_sel, &change_mirr);

			if (!multipaint_verify_change(dv_mirr, wpi->defbase_tot, change_mirr, wpi->defbase_sel)) {
				return;
			}

			change *= change_mirr / orig;
		}
	}

	if (!multipaint_verify_change(dv, wpi->defbase_tot, change, wpi->defbase_sel)) {
		return;
	}

	/* apply validated change to vertex and mirror */
	multipaint_apply_change(dv, wpi->defbase_tot, change, wpi->defbase_sel);

	if (dv_mirr != NULL) {
		multipaint_apply_change(dv_mirr, wpi->defbase_tot, change_mirr, wpi->defbase_sel);
	}

	/* normalize */
	if (wpi->do_auto_normalize) {
		do_weight_paint_normalize_all_locked_try_active(
		        dv, wpi->defbase_tot, wpi->vgroup_validmap, wpi->lock_flags, wpi->active.lock);

		if (dv_mirr != NULL) {
			do_weight_paint_normalize_all_locked_try_active(
			        dv_mirr, wpi->defbase_tot, wpi->vgroup_validmap, wpi->lock_flags, wpi->active.lock);
		}
	}
}

static void do_weight_paint_vertex(
        /* vars which remain the same for every vert */
        VPaint *wp, Object *ob, const WeightPaintInfo *wpi,
        /* vars which change on each stroke */
        const uint index, float alpha, float paintweight)
{
	if (wpi->do_multipaint) {
		do_weight_paint_vertex_multi(wp, ob, wpi, index, alpha, paintweight);
	}
	else {
		do_weight_paint_vertex_single(wp, ob, wpi, index, alpha, paintweight);
	}
}


/* Toggle operator for turning vertex paint mode on or off (copied from sculpt.c) */
static void vertex_paint_init_session(Scene *scene, Object *ob)
{
	if (ob->sculpt == NULL) {
		ob->sculpt = MEM_callocN(sizeof(SculptSession), "sculpt session");
		BKE_sculpt_update_mesh_elements(scene, scene->toolsettings->sculpt, ob, 0, false);
	}
}

static void vertex_paint_init_session_data(const ToolSettings *ts, Object *ob)
{
	/* Create maps */
	struct SculptVertexPaintGeomMap *gmap = NULL;
	const Brush *brush = NULL;
	if (ob->mode == OB_MODE_VERTEX_PAINT) {
		gmap = &ob->sculpt->mode.vpaint.gmap;
		brush = BKE_paint_brush(&ts->vpaint->paint);
		ob->sculpt->mode_type = OB_MODE_VERTEX_PAINT;
	}
	else if (ob->mode == OB_MODE_WEIGHT_PAINT) {
		gmap = &ob->sculpt->mode.wpaint.gmap;
		brush = BKE_paint_brush(&ts->wpaint->paint);
		ob->sculpt->mode_type = OB_MODE_WEIGHT_PAINT;
	}
	else {
		ob->sculpt->mode_type = 0;
		BLI_assert(0);
		return;
	}

	Mesh *me = ob->data;

	if (gmap->vert_to_loop == NULL) {
		gmap->vert_map_mem = NULL;
		gmap->vert_to_loop = NULL;
		gmap->poly_map_mem = NULL;
		gmap->vert_to_poly = NULL;
		BKE_mesh_vert_loop_map_create(
		        &gmap->vert_to_loop,
		        &gmap->vert_map_mem,
		        me->mpoly, me->mloop, me->totvert, me->totpoly, me->totloop);
		BKE_mesh_vert_poly_map_create(
		        &gmap->vert_to_poly,
		        &gmap->poly_map_mem,
		        me->mpoly, me->mloop, me->totvert, me->totpoly, me->totloop);
	}

	/* Create average brush arrays */
	if (ob->mode == OB_MODE_VERTEX_PAINT) {
		if ((ts->vpaint->flag & VP_SPRAY) == 0) {
			if (ob->sculpt->mode.vpaint.previous_color == NULL) {
				ob->sculpt->mode.vpaint.previous_color =
				        MEM_callocN(me->totloop * sizeof(uint), __func__);
			}
		}
		else {
			MEM_SAFE_FREE(ob->sculpt->mode.vpaint.previous_color);
		}

		if (brush && brush->flag & BRUSH_ACCUMULATE) {
			if (ob->sculpt->mode.vpaint.previous_accum == NULL) {
				ob->sculpt->mode.vpaint.previous_accum =
				        MEM_callocN(me->totloop * sizeof(float), __func__);
			}
		}
		else {
			MEM_SAFE_FREE(ob->sculpt->mode.vpaint.previous_accum);
		}
	}
	else if (ob->mode == OB_MODE_WEIGHT_PAINT) {
		if ((ts->wpaint->flag & VP_SPRAY) == 0) {
			if (ob->sculpt->mode.wpaint.alpha_weight == NULL) {
				ob->sculpt->mode.wpaint.alpha_weight =
				        MEM_callocN(me->totvert * sizeof(float), __func__);
			}
			if (ob->sculpt->mode.wpaint.dvert_prev == NULL) {
				ob->sculpt->mode.wpaint.dvert_prev =
				        MEM_callocN(me->totvert * sizeof(MDeformVert), __func__);
				MDeformVert *dv = ob->sculpt->mode.wpaint.dvert_prev;
				for (int i = 0; i < me->totvert; i++, dv++) {
					/* Use to show this isn't initialized, never apply to the mesh data. */
					dv->flag = 1;
				}
			}
		}
		else {
			MEM_SAFE_FREE(ob->sculpt->mode.wpaint.alpha_weight);
			if (ob->sculpt->mode.wpaint.dvert_prev != NULL) {
				BKE_defvert_array_free_elems(ob->sculpt->mode.wpaint.dvert_prev, me->totvert);
				MEM_freeN(ob->sculpt->mode.wpaint.dvert_prev);
				ob->sculpt->mode.wpaint.dvert_prev = NULL;
			}
		}
		if (brush && brush->flag & BRUSH_ACCUMULATE) {
			if (ob->sculpt->mode.wpaint.previous_accum == NULL) {
				ob->sculpt->mode.wpaint.previous_accum =
				        MEM_callocN(me->totvert * sizeof(float), __func__);
			}
		}
		else {
			MEM_SAFE_FREE(ob->sculpt->mode.wpaint.previous_accum);
		}
	}

}

/* *************** set wpaint operator ****************** */

/**
 * \note Keep in sync with #vpaint_mode_toggle_exec
 */
static int wpaint_mode_toggle_exec(bContext *C, wmOperator *op)
{
	Object *ob = CTX_data_active_object(C);
	const int mode_flag = OB_MODE_WEIGHT_PAINT;
	const bool is_mode_set = (ob->mode & mode_flag) != 0;
	Scene *scene = CTX_data_scene(C);
	VPaint *wp = scene->toolsettings->wpaint;
	Mesh *me;

	if (!is_mode_set) {
		if (!ED_object_mode_compat_set(C, ob, mode_flag, op->reports)) {
			return OPERATOR_CANCELLED;
		}
	}

	me = BKE_mesh_from_object(ob);

	if (ob->mode & mode_flag) {
		ob->mode &= ~mode_flag;

		if (me->editflag & ME_EDIT_PAINT_VERT_SEL) {
			BKE_mesh_flush_select_from_verts(me);
		}
		else if (me->editflag & ME_EDIT_PAINT_FACE_SEL) {
			BKE_mesh_flush_select_from_polys(me);
		}

		/* weight paint specific */
		ED_mesh_mirror_spatial_table(NULL, NULL, NULL, NULL, 'e');
		ED_mesh_mirror_topo_table(NULL, NULL, 'e');

		/* If the cache is not released by a cancel or a done, free it now. */
		if (ob->sculpt->cache) {
			sculpt_cache_free(ob->sculpt->cache);
			ob->sculpt->cache = NULL;
		}

		BKE_sculptsession_free(ob);

		paint_cursor_delete_textures();
	}
	else {
		ob->mode |= mode_flag;

		if (wp == NULL)
			wp = scene->toolsettings->wpaint = new_vpaint(1);

		paint_cursor_start(C, weight_paint_poll);

		BKE_paint_init(scene, ePaintWeight, PAINT_CURSOR_WEIGHT_PAINT);

		/* weight paint specific */
		ED_mesh_mirror_spatial_table(ob, NULL, NULL, NULL, 's');
		ED_vgroup_sync_from_pose(ob);

		/* Create vertex/weight paint mode session data */
		if (ob->sculpt) {
			BKE_sculptsession_free(ob);
		}
		vertex_paint_init_session(scene, ob);
	}

	/* Weightpaint works by overriding colors in mesh,
	 * so need to make sure we recalc on enter and
	 * exit (exit needs doing regardless because we
	 * should redeform).
	 */
	DAG_id_tag_update(&me->id, 0);

	WM_event_add_notifier(C, NC_SCENE | ND_MODE, scene);

	return OPERATOR_FINISHED;
}

/* for switching to/from mode */
static int paint_poll_test(bContext *C)
{
	Object *ob = CTX_data_active_object(C);
	if (ob == NULL || ob->type != OB_MESH)
		return 0;
	if (!ob->data || ID_IS_LINKED_DATABLOCK(ob->data))
		return 0;
	if (CTX_data_edit_object(C))
		return 0;
	return 1;
}

void PAINT_OT_weight_paint_toggle(wmOperatorType *ot)
{

	/* identifiers */
	ot->name = "Weight Paint Mode";
	ot->idname = "PAINT_OT_weight_paint_toggle";
	ot->description = "Toggle weight paint mode in 3D view";

	/* api callbacks */
	ot->exec = wpaint_mode_toggle_exec;
	ot->poll = paint_poll_test;

	/* flags */
	ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/* ************ weight paint operator ********** */

struct WPaintData {
	ViewContext vc;

	struct WeightPaintGroupData active, mirror;

	void *vp_handle;
	DMCoNo *vertexcosnos;

	float wpimat[3][3];

	/* variables for auto normalize */
	const bool *vgroup_validmap; /* stores if vgroups tie to deforming bones or not */
	const bool *lock_flags;

	/* variables for multipaint */
	const bool *defbase_sel;      /* set of selected groups */
	int defbase_tot_sel;          /* number of selected groups */
	bool do_multipaint;           /* true if multipaint enabled and multiple groups selected */

	int defbase_tot;
};

/* Initialize the stroke cache invariants from operator properties */
static void vwpaint_update_cache_invariants(
        bContext *C, VPaint *vd, SculptSession *ss, wmOperator *op, const float mouse[2])
{
	StrokeCache *cache;
	Scene *scene = CTX_data_scene(C);
	UnifiedPaintSettings *ups = &CTX_data_tool_settings(C)->unified_paint_settings;
	Brush *brush = BKE_paint_brush(&vd->paint);
	ViewContext *vc = paint_stroke_view_context(op->customdata);
	Object *ob = CTX_data_active_object(C);
	float mat[3][3];
	float view_dir[3] = {0.0f, 0.0f, 1.0f};
	int mode;

	/* VW paint needs to allocate stroke cache before update is called. */
	if (!ss->cache) {
		cache = MEM_callocN(sizeof(StrokeCache), "stroke cache");
		ss->cache = cache;
	}
	else {
		cache = ss->cache;
	}

	/* Initial mouse location */
	if (mouse)
		copy_v2_v2(cache->initial_mouse, mouse);
	else
		zero_v2(cache->initial_mouse);

	mode = RNA_enum_get(op->ptr, "mode");
	cache->invert = mode == BRUSH_STROKE_INVERT;
	cache->alt_smooth = mode == BRUSH_STROKE_SMOOTH;
	/* not very nice, but with current events system implementation
	* we can't handle brush appearance inversion hotkey separately (sergey) */
	if (cache->invert) ups->draw_inverted = true;
	else ups->draw_inverted = false;

	copy_v2_v2(cache->mouse, cache->initial_mouse);
	/* Truly temporary data that isn't stored in properties */
	cache->vc = vc;
	cache->brush = brush;
	cache->first_time = 1;

	/* cache projection matrix */
	ED_view3d_ob_project_mat_get(cache->vc->rv3d, ob, cache->projection_mat);

	invert_m4_m4(ob->imat, ob->obmat);
	copy_m3_m4(mat, cache->vc->rv3d->viewinv);
	mul_m3_v3(mat, view_dir);
	copy_m3_m4(mat, ob->imat);
	mul_m3_v3(mat, view_dir);
	normalize_v3_v3(cache->true_view_normal, view_dir);

	copy_v3_v3(cache->view_normal, cache->true_view_normal);
	cache->bstrength = BKE_brush_alpha_get(scene, brush);
	cache->is_last_valid = false;
}

/* Initialize the stroke cache variants from operator properties */
static void vwpaint_update_cache_variants(bContext *C, VPaint *vd, Object *ob, PointerRNA *ptr)
{
	Scene *scene = CTX_data_scene(C);
	SculptSession *ss = ob->sculpt;
	StrokeCache *cache = ss->cache;
	Brush *brush = BKE_paint_brush(&vd->paint);

	/* This effects the actual brush radius, so things farther away
	 * are compared with a larger radius and vise versa. */
	if (cache->first_time) {
		RNA_float_get_array(ptr, "location", cache->true_location);
	}

	RNA_float_get_array(ptr, "mouse", cache->mouse);

	/* XXX: Use pressure value from first brush step for brushes which don't
	 * support strokes (grab, thumb). They depends on initial state and
	 * brush coord/pressure/etc.
	 * It's more an events design issue, which doesn't split coordinate/pressure/angle
	 * changing events. We should avoid this after events system re-design */
	if (paint_supports_dynamic_size(brush, ePaintSculpt) || cache->first_time) {
		cache->pressure = RNA_float_get(ptr, "pressure");
	}

	/* Truly temporary data that isn't stored in properties */
	if (cache->first_time) {
		if (!BKE_brush_use_locked_size(scene, brush)) {
			cache->initial_radius = paint_calc_object_space_radius(
			        cache->vc, cache->true_location, BKE_brush_size_get(scene, brush));
			BKE_brush_unprojected_radius_set(scene, brush, cache->initial_radius);
		}
		else {
			cache->initial_radius = BKE_brush_unprojected_radius_get(scene, brush);
		}
	}

	if (BKE_brush_use_size_pressure(scene, brush) && paint_supports_dynamic_size(brush, ePaintSculpt)) {
		cache->radius = cache->initial_radius * cache->pressure;
	}
	else {
		cache->radius = cache->initial_radius;
	}

	cache->radius_squared = cache->radius * cache->radius;

	if (ss->pbvh) {
		BKE_pbvh_update(ss->pbvh, PBVH_UpdateRedraw, NULL);
		BKE_pbvh_update(ss->pbvh, PBVH_UpdateBB, NULL);
	}
}

static bool wpaint_stroke_test_start(bContext *C, wmOperator *op, const float mouse[2])
{
	Scene *scene = CTX_data_scene(C);
	struct PaintStroke *stroke = op->customdata;
	ToolSettings *ts = scene->toolsettings;
	Object *ob = CTX_data_active_object(C);
	Mesh *me = BKE_mesh_from_object(ob);
	struct WPaintData *wpd;
	struct WPaintVGroupIndex vgroup_index;
	int defbase_tot, defbase_tot_sel;
	bool *defbase_sel;
	SculptSession *ss = ob->sculpt;
	VPaint *vd = CTX_data_tool_settings(C)->wpaint;

	float mat[4][4], imat[4][4];

	if (ED_wpaint_ensure_data(C, op->reports, WPAINT_ENSURE_MIRROR, &vgroup_index) == false) {
		return false;
	}

	{
		/* check if we are attempting to paint onto a locked vertex group,
		 * and other options disallow it from doing anything useful */
		bDeformGroup *dg;
		dg = BLI_findlink(&ob->defbase, vgroup_index.active);
		if (dg->flag & DG_LOCK_WEIGHT) {
			BKE_report(op->reports, RPT_WARNING, "Active group is locked, aborting");
			return false;
		}
		if (vgroup_index.mirror != -1) {
			dg = BLI_findlink(&ob->defbase, vgroup_index.mirror);
			if (dg->flag & DG_LOCK_WEIGHT) {
				BKE_report(op->reports, RPT_WARNING, "Mirror group is locked, aborting");
				return false;
			}
		}
	}

	/* check that multipaint groups are unlocked */
	defbase_tot = BLI_listbase_count(&ob->defbase);
	defbase_sel = BKE_object_defgroup_selected_get(ob, defbase_tot, &defbase_tot_sel);

	if (ts->multipaint && defbase_tot_sel > 1) {
		int i;
		bDeformGroup *dg;

		if (me->editflag & ME_EDIT_MIRROR_X) {
			BKE_object_defgroup_mirror_selection(ob, defbase_tot, defbase_sel, defbase_sel, &defbase_tot_sel);
		}

		for (i = 0; i < defbase_tot; i++) {
			if (defbase_sel[i]) {
				dg = BLI_findlink(&ob->defbase, i);
				if (dg->flag & DG_LOCK_WEIGHT) {
					BKE_report(op->reports, RPT_WARNING, "Multipaint group is locked, aborting");
					MEM_freeN(defbase_sel);
					return false;
				}
			}
		}
	}

	/* ALLOCATIONS! no return after this line */
	/* make mode data storage */
	wpd = MEM_callocN(sizeof(struct WPaintData), "WPaintData");
	paint_stroke_set_mode_data(stroke, wpd);
	view3d_set_viewcontext(C, &wpd->vc);

	wpd->active.index = vgroup_index.active;
	wpd->mirror.index = vgroup_index.mirror;

	/* multipaint */
	wpd->defbase_tot = defbase_tot;
	wpd->defbase_sel = defbase_sel;
	wpd->defbase_tot_sel = defbase_tot_sel > 1 ? defbase_tot_sel : 1;
	wpd->do_multipaint = (ts->multipaint && defbase_tot_sel > 1);

	/* set up auto-normalize, and generate map for detecting which
	 * vgroups affect deform bones */
	wpd->lock_flags = BKE_object_defgroup_lock_flags_get(ob, wpd->defbase_tot);
	if (ts->auto_normalize || ts->multipaint || wpd->lock_flags) {
		wpd->vgroup_validmap = BKE_object_defgroup_validmap_get(ob, wpd->defbase_tot);
	}

	if (wpd->do_multipaint && ts->auto_normalize) {
		bool *tmpflags;
		tmpflags = MEM_mallocN(sizeof(bool) * defbase_tot, __func__);
		if (wpd->lock_flags) {
			BLI_array_binary_or(tmpflags, wpd->defbase_sel, wpd->lock_flags, wpd->defbase_tot);
		}
		else {
			memcpy(tmpflags, wpd->defbase_sel, sizeof(*tmpflags) * wpd->defbase_tot);
		}
		wpd->active.lock = tmpflags;
	}
	else if (ts->auto_normalize) {
		bool *tmpflags;

		tmpflags = wpd->lock_flags ?
		        MEM_dupallocN(wpd->lock_flags) :
		        MEM_callocN(sizeof(bool) * defbase_tot, __func__);
		tmpflags[wpd->active.index] = true;
		wpd->active.lock = tmpflags;

		tmpflags = wpd->lock_flags ?
		        MEM_dupallocN(wpd->lock_flags) :
		        MEM_callocN(sizeof(bool) * defbase_tot, __func__);
		tmpflags[(wpd->mirror.index != -1) ? wpd->mirror.index : wpd->active.index] = true;
		wpd->mirror.lock = tmpflags;
	}

	/* painting on subsurfs should give correct points too, this returns me->totvert amount */
	ob->sculpt->building_vp_handle = true;
	wpd->vp_handle = ED_vpaint_proj_handle_create(scene, ob, &wpd->vertexcosnos);
	ob->sculpt->building_vp_handle = false;

	/* imat for normals */
	mul_m4_m4m4(mat, wpd->vc.rv3d->viewmat, ob->obmat);
	invert_m4_m4(imat, mat);
	copy_m3_m4(wpd->wpimat, imat);

	/* If not previously created, create vertex/weight paint mode session data */
	vertex_paint_init_session(scene, ob);
	vwpaint_update_cache_invariants(C, vd, ss, op, mouse);
	vertex_paint_init_session_data(ts, ob);

	if (ob->sculpt->mode.wpaint.dvert_prev != NULL) {
		MDeformVert *dv = ob->sculpt->mode.wpaint.dvert_prev;
		for (int i = 0; i < me->totvert; i++, dv++) {
			/* Use to show this isn't initialized, never apply to the mesh data. */
			dv->flag = 1;
		}
	}

	return true;
}

static float dot_vf3vs3(const float brushNormal[3], const short vertexNormal[3])
{
	float normal[3];
	normal_short_to_float_v3(normal, vertexNormal);
	return dot_v3v3(brushNormal, normal);
}

static void get_brush_alpha_data(
        Scene *scene, SculptSession *ss, Brush *brush,
        float *r_brush_size_pressure, float *r_brush_alpha_value, float *r_brush_alpha_pressure)
{
	*r_brush_size_pressure =
	        BKE_brush_size_get(scene, brush) *
	        (BKE_brush_use_size_pressure(scene, brush) ? ss->cache->pressure : 1.0f);
	*r_brush_alpha_value =
	        BKE_brush_alpha_get(scene, brush);
	*r_brush_alpha_pressure =
	        (BKE_brush_use_alpha_pressure(scene, brush) ? ss->cache->pressure : 1.0f);
}

static float wpaint_get_active_weight(const MDeformVert *dv, const WeightPaintInfo *wpi)
{
	if (wpi->do_multipaint) {
		float weight = BKE_defvert_multipaint_collective_weight(
		     dv, wpi->defbase_tot, wpi->defbase_sel, wpi->defbase_tot_sel, wpi->do_auto_normalize);

		CLAMP(weight, 0.0f, 1.0f);
		return weight;
	}
	else {
		return defvert_find_weight(dv, wpi->active.index);
	}
}

static void do_wpaint_brush_blur_task_cb_ex(
        void *userdata, void *UNUSED(userdata_chunk), const int n, const int UNUSED(thread_id))
{
	SculptThreadedTaskData *data = userdata;
	SculptSession *ss = data->ob->sculpt;
	CCGDerivedMesh *ccgdm = BKE_pbvh_get_ccgdm(ss->pbvh);
	const struct SculptVertexPaintGeomMap *gmap = &ss->mode.wpaint.gmap;

	Brush *brush = data->brush;
	StrokeCache *cache = ss->cache;
	Scene *scene = CTX_data_scene(data->C);

	const float brush_strength = cache->bstrength;
	float brush_size_pressure, brush_alpha_value, brush_alpha_pressure;
	get_brush_alpha_data(scene, ss, brush, &brush_size_pressure, &brush_alpha_value, &brush_alpha_pressure);
	const bool use_face_sel = (data->me->editflag & ME_EDIT_PAINT_FACE_SEL) != 0;
	const bool use_vert_sel = (data->me->editflag & ME_EDIT_PAINT_VERT_SEL) != 0;

	SculptBrushTest test;
	sculpt_brush_test_init(ss, &test);

	/* For each vertex */
	PBVHVertexIter vd;
	BKE_pbvh_vertex_iter_begin(ss->pbvh, data->nodes[n], vd, PBVH_ITER_UNIQUE)
	{
		/* Test to see if the vertex coordinates are within the spherical brush region. */
		if (sculpt_brush_test_sq(&test, vd.co)) {
			/* For grid based pbvh, take the vert whose loop coopresponds to the current grid.
			 * Otherwise, take the current vert. */
			const int v_index = ccgdm ? data->me->mloop[vd.grid_indices[vd.g]].v : vd.vert_indices[vd.i];
			const float grid_alpha = ccgdm ? 1.0f / vd.gridsize : 1.0f;
			const char v_flag = data->me->mvert[v_index].flag;
			/* If the vertex is selected */
			if (!(use_face_sel || use_vert_sel) || v_flag & SELECT) {
				/* Get the average poly weight */
				int total_hit_loops = 0;
				float weight_final = 0.0f;
				for (int j = 0; j < gmap->vert_to_poly[v_index].count; j++) {
					const int p_index = gmap->vert_to_poly[v_index].indices[j];
					const MPoly *mp = &data->me->mpoly[p_index];

					total_hit_loops += mp->totloop;
					for (int k = 0; k < mp->totloop; k++) {
						const int l_index = mp->loopstart + k;
						const MLoop *ml = &data->me->mloop[l_index];
						const MDeformVert *dv = &data->me->dvert[ml->v];
						weight_final += wpaint_get_active_weight(dv, data->wpi);
					}
				}

				/* Apply the weight to the vertex. */
				if (total_hit_loops != 0) {
					const float view_dot = (vd.no) ? dot_vf3vs3(cache->sculpt_normal_symm, vd.no) : 1.0;
					if (view_dot > 0.0f) {
						const float brush_fade = BKE_brush_curve_strength(brush, sqrtf(test.dist), cache->radius);
						float final_alpha =
						        brush_fade * brush_strength *
						        grid_alpha * brush_alpha_pressure;

						if (data->vp->flag & VP_NORMALS) {
							final_alpha *= view_dot;
						}
						if (brush->flag & BRUSH_ACCUMULATE) {
							float mask_accum = ss->mode.wpaint.previous_accum[v_index];
							final_alpha = min_ff(final_alpha + mask_accum, brush_strength);
							ss->mode.wpaint.previous_accum[v_index] = final_alpha;
						}

						weight_final /= total_hit_loops;
						/* Only paint visable verts */
						do_weight_paint_vertex(
						        data->vp, data->ob, data->wpi,
						        v_index, final_alpha, weight_final);
					}
				}
			}
		}
	}
	BKE_pbvh_vertex_iter_end;
}

static void do_wpaint_brush_smear_task_cb_ex(
        void *userdata, void *UNUSED(userdata_chunk), const int n, const int UNUSED(thread_id))
{
	SculptThreadedTaskData *data = userdata;
	SculptSession *ss = data->ob->sculpt;
	CCGDerivedMesh *ccgdm = BKE_pbvh_get_ccgdm(ss->pbvh);
	const struct SculptVertexPaintGeomMap *gmap = &ss->mode.wpaint.gmap;

	Brush *brush = data->brush;
	Scene *scene = CTX_data_scene(data->C);
	StrokeCache *cache = ss->cache;
	const float brush_strength = cache->bstrength;
	float brush_size_pressure, brush_alpha_value, brush_alpha_pressure;
	get_brush_alpha_data(scene, ss, brush, &brush_size_pressure, &brush_alpha_value, &brush_alpha_pressure);
	const bool use_face_sel = (data->me->editflag & ME_EDIT_PAINT_FACE_SEL) != 0;
	const bool use_vert_sel = (data->me->editflag & ME_EDIT_PAINT_VERT_SEL) != 0;
	float brush_dir[3];

	sub_v3_v3v3(brush_dir, cache->location, cache->last_location);
	project_plane_v3_v3v3(brush_dir, brush_dir, cache->view_normal);

	if (normalize_v3(brush_dir) != 0.0f) {

		SculptBrushTest test;
		sculpt_brush_test_init(ss, &test);

		/* For each vertex */
		PBVHVertexIter vd;
		BKE_pbvh_vertex_iter_begin(ss->pbvh, data->nodes[n], vd, PBVH_ITER_UNIQUE)
		{
			/* Test to see if the vertex coordinates are within the spherical brush region. */
			if (sculpt_brush_test_fast(&test, vd.co)) {
				const float view_dot = (vd.no) ? dot_vf3vs3(cache->sculpt_normal_symm, vd.no) : 1.0;
				if (view_dot > 0.0f) {
					bool do_color = false;

					/* For grid based pbvh, take the vert whose loop cooresponds to the current grid.
					 * Otherwise, take the current vert. */
					const int v_index = ccgdm ? data->me->mloop[vd.grid_indices[vd.g]].v : vd.vert_indices[vd.i];
					const float grid_alpha = ccgdm ? 1.0f / vd.gridsize : 1.0f;
					const MVert *mv_curr = &data->me->mvert[v_index];

					/* If the vertex is selected */
					if (!(use_face_sel || use_vert_sel) || mv_curr->flag & SELECT) {
						/* Minimum dot product between brush direction and current
						 * to neighbor direction is 0.0, meaning orthogonal. */
						float stroke_dot_max = 0.0f;

						/* Get the color of the loop in the opposite direction of the brush movement
						 * (this callback is specifically for smear.) */
						float weight_final = 0.0;
						for (int j = 0; j < gmap->vert_to_poly[v_index].count; j++) {
							const int p_index = gmap->vert_to_poly[v_index].indices[j];
							const MPoly *mp = &data->me->mpoly[p_index];
							const MLoop *ml_other = &data->me->mloop[mp->loopstart];
							for (int k = 0; k < mp->totloop; k++, ml_other++) {
								const uint v_other_index = ml_other->v;
								if (v_other_index != v_index) {
									const MVert *mv_other = &data->me->mvert[v_other_index];

									/* Get the direction from the selected vert to the neighbor. */
									float other_dir[3];
									sub_v3_v3v3(other_dir, mv_curr->co, mv_other->co);
									project_plane_v3_v3v3(other_dir, other_dir, cache->view_normal);

									normalize_v3(other_dir);

									const float stroke_dot = dot_v3v3(other_dir, brush_dir);

									if (stroke_dot > stroke_dot_max) {
										stroke_dot_max = stroke_dot;
										MDeformVert *dv = &data->me->dvert[v_other_index];
										weight_final = wpaint_get_active_weight(dv, data->wpi);
										do_color = true;
									}
								}
							}
						}
						/* Apply weight to vertex */
						if (do_color) {
							const float brush_fade = BKE_brush_curve_strength(brush, test.dist, cache->radius);
							float final_alpha =
							        brush_fade * brush_strength *
							        grid_alpha * brush_alpha_pressure;

							if (data->vp->flag & VP_NORMALS) {
								final_alpha *= view_dot;
							}
							do_weight_paint_vertex(
							        data->vp, data->ob, data->wpi,
							        v_index, final_alpha, (float)weight_final);
						}
					}
				}
			}
		}
		BKE_pbvh_vertex_iter_end;
	}
}

static void do_wpaint_brush_draw_task_cb_ex(
        void *userdata, void *UNUSED(userdata_chunk), const int n, const int UNUSED(thread_id))
{
	SculptThreadedTaskData *data = userdata;
	SculptSession *ss = data->ob->sculpt;
	CCGDerivedMesh *ccgdm = BKE_pbvh_get_ccgdm(ss->pbvh);
	Scene *scene = CTX_data_scene(data->C);

	Brush *brush = data->brush;
	StrokeCache *cache = ss->cache;
	const float brush_strength = cache->bstrength;
	const float paintweight = BKE_brush_weight_get(scene, brush);
	float brush_size_pressure, brush_alpha_value, brush_alpha_pressure;
	get_brush_alpha_data(scene, ss, brush, &brush_size_pressure, &brush_alpha_value, &brush_alpha_pressure);
	const bool use_face_sel = (data->me->editflag & ME_EDIT_PAINT_FACE_SEL) != 0;
	const bool use_vert_sel = (data->me->editflag & ME_EDIT_PAINT_VERT_SEL) != 0;

	SculptBrushTest test;
	sculpt_brush_test_init(ss, &test);

	/* For each vertex */
	PBVHVertexIter vd;
	BKE_pbvh_vertex_iter_begin(ss->pbvh, data->nodes[n], vd, PBVH_ITER_UNIQUE)
	{
		/* Test to see if the vertex coordinates are within the spherical brush region. */
		if (sculpt_brush_test_sq(&test, vd.co)) {
			/* Note: grids are 1:1 with corners (aka loops).
			 * For multires, take the vert whose loop cooresponds to the current grid.
			 * Otherwise, take the current vert. */
			const int v_index = ccgdm ? data->me->mloop[vd.grid_indices[vd.g]].v : vd.vert_indices[vd.i];
			const float grid_alpha = ccgdm ? 1.0f / vd.gridsize : 1.0f;

			const char v_flag = data->me->mvert[v_index].flag;
			/* If the vertex is selected */
			if (!(use_face_sel || use_vert_sel) || v_flag & SELECT) {
				const float view_dot = (vd.no) ? dot_vf3vs3(cache->sculpt_normal_symm, vd.no) : 1.0;
				if (view_dot > 0.0f) {
					const float brush_fade = BKE_brush_curve_strength(brush, sqrtf(test.dist), cache->radius);
					float final_alpha = brush_fade * brush_strength * grid_alpha * brush_alpha_pressure;

					if (data->vp->flag & VP_NORMALS) {
						final_alpha *= view_dot;
					}
					if (brush->flag & BRUSH_ACCUMULATE) {
						float mask_accum = ss->mode.wpaint.previous_accum[v_index];
						final_alpha = min_ff(final_alpha + mask_accum, brush_strength);
						ss->mode.wpaint.previous_accum[v_index] = final_alpha;
					}

					/* Non-spray logic. */
					if ((data->vp->flag & VP_SPRAY) == 0) {
						/* Only paint if we have greater alpha. */
						if (ss->mode.wpaint.alpha_weight[v_index] < final_alpha) {
							ss->mode.wpaint.alpha_weight[v_index] = final_alpha;
						}
						else {
							continue;
						}
					}

					do_weight_paint_vertex(
					        data->vp, data->ob, data->wpi,
					        v_index, final_alpha, paintweight);
				}
			}
		}
	}
	BKE_pbvh_vertex_iter_end;
}

static void do_wpaint_brush_calc_average_weight_cb_ex(
        void *userdata, void *UNUSED(userdata_chunk), const int n, const int UNUSED(thread_id))
{
	SculptThreadedTaskData *data = userdata;
	SculptSession *ss = data->ob->sculpt;
	StrokeCache *cache = ss->cache;
	CCGDerivedMesh *ccgdm = BKE_pbvh_get_ccgdm(ss->pbvh);

	const bool use_face_sel = (data->me->editflag & ME_EDIT_PAINT_FACE_SEL) != 0;
	const bool use_vert_sel = (data->me->editflag & ME_EDIT_PAINT_VERT_SEL) != 0;

	struct WPaintAverageAccum *accum = (struct WPaintAverageAccum *)data->custom_data + n;
	accum->len = 0;
	accum->value = 0.0;

	SculptBrushTest test;
	sculpt_brush_test_init(ss, &test);

	/* For each vertex */
	PBVHVertexIter vd;
	BKE_pbvh_vertex_iter_begin(ss->pbvh, data->nodes[n], vd, PBVH_ITER_UNIQUE)
	{
		/* Test to see if the vertex coordinates are within the spherical brush region. */
		if (sculpt_brush_test_sq(&test, vd.co)) {
			const float view_dot = (vd.no) ? dot_vf3vs3(cache->sculpt_normal_symm, vd.no) : 1.0;
			if (view_dot > 0.0 && BKE_brush_curve_strength(data->brush, sqrtf(test.dist), cache->radius) > 0.0) {
				const int v_index = ccgdm ? data->me->mloop[vd.grid_indices[vd.g]].v : vd.vert_indices[vd.i];
				// const float grid_alpha = ccgdm ? 1.0f / vd.gridsize : 1.0f;
				const char v_flag = data->me->mvert[v_index].flag;

				/* If the vertex is selected. */
				if (!(use_face_sel || use_vert_sel) || v_flag & SELECT) {
					const MDeformVert *dv = &data->me->dvert[v_index];
					accum->len += 1;
					accum->value += wpaint_get_active_weight(dv, data->wpi);
				}
			}
		}
	}
	BKE_pbvh_vertex_iter_end;
}

static void calculate_average_weight(SculptThreadedTaskData *data, PBVHNode **UNUSED(nodes), int totnode)
{
	Scene *scene = CTX_data_scene(data->C);
	UnifiedPaintSettings *ups = &scene->toolsettings->unified_paint_settings;

	struct WPaintAverageAccum *accum = MEM_mallocN(sizeof(*accum) * totnode, __func__);
	data->custom_data = accum;

	BLI_task_parallel_range_ex(
	        0, totnode, data, NULL, 0, do_wpaint_brush_calc_average_weight_cb_ex,
	        ((data->sd->flags & SCULPT_USE_OPENMP) && totnode > SCULPT_THREADED_LIMIT), false);

	uint accum_len = 0;
	double accum_weight = 0.0;
	for (int i = 0; i < totnode; i++) {
		accum_len += accum[i].len;
		accum_weight += accum[i].value;
	}
	if (accum_len != 0) {
		accum_weight /= accum_len;
		if (ups->flag & UNIFIED_PAINT_WEIGHT)
			ups->weight = (float)accum_weight;
		else
			data->brush->weight = (float)accum_weight;
	}

	MEM_SAFE_FREE(data->custom_data);  /* 'accum' */
}


static void wpaint_paint_leaves(
        bContext *C, Object *ob, Sculpt *sd, VPaint *vp, struct WPaintData *wpd, WeightPaintInfo *wpi,
        Mesh *me, PBVHNode **nodes, int totnode)
{
	Brush *brush = ob->sculpt->cache->brush;

	/* threaded loop over nodes */
	SculptThreadedTaskData data = {
		.sd = sd, .ob = ob, .brush = brush, .nodes = nodes, .vp = vp, .wpd = wpd, .wpi = wpi, .me = me, .C = C,
	};

	switch (brush->vertexpaint_tool) {
		case PAINT_BLEND_AVERAGE:
			calculate_average_weight(&data, nodes, totnode);
			BLI_task_parallel_range_ex(
			        0, totnode, &data, NULL, 0,
			        do_wpaint_brush_draw_task_cb_ex, true, false);
			break;
		case PAINT_BLEND_SMEAR:
			BLI_task_parallel_range_ex(
			        0, totnode, &data, NULL, 0,
			        do_wpaint_brush_smear_task_cb_ex, true, false);
			break;
		case PAINT_BLEND_BLUR:
			BLI_task_parallel_range_ex(
			        0, totnode, &data, NULL, 0,
			        do_wpaint_brush_blur_task_cb_ex, true, false);
			break;
		default:
			BLI_task_parallel_range_ex(
			        0, totnode, &data, NULL, 0,
			        do_wpaint_brush_draw_task_cb_ex, true, false);
			break;
	}
}

static void wpaint_do_paint(
        bContext *C, Object *ob, VPaint *wp, Sculpt *sd, struct WPaintData *wpd, WeightPaintInfo *wpi,
        Mesh *me, Brush *brush, const char symm, const int axis, const int i, const float angle)
{
	SculptSession *ss = ob->sculpt;
	ss->cache->radial_symmetry_pass = i;
	sculpt_cache_calc_brushdata_symm(ss->cache, symm, axis, angle);

	SculptSearchSphereData data;
	PBVHNode **nodes = NULL;
	int totnode;


	/* Build a list of all nodes that are potentially within the brush's area of influence */
	data.ss = ss;
	data.sd = sd;
	data.radius_squared = ss->cache->radius_squared;
	data.original = true;
	BKE_pbvh_search_gather(ss->pbvh, sculpt_search_sphere_cb, &data, &nodes, &totnode);

	sculpt_pbvh_calc_area_normal(brush, ob, nodes, totnode, true, ss->cache->sculpt_normal_symm);
	wpaint_paint_leaves(C, ob, sd, wp, wpd, wpi, me, nodes, totnode);

	if (nodes)
		MEM_freeN(nodes);
}

static void wpaint_do_radial_symmetry(
        bContext *C, Object *ob, VPaint *wp, Sculpt *sd, struct WPaintData *wpd, WeightPaintInfo *wpi,
        Mesh *me, Brush *brush, const char symm, const int axis)
{
	for (int i = 1; i < wp->radial_symm[axis - 'X']; i++) {
		const float angle = (2.0 * M_PI) * i / wp->radial_symm[axis - 'X'];
		wpaint_do_paint(C, ob, wp, sd, wpd, wpi, me, brush, symm, axis, i, angle);
	}
}

/* near duplicate of: sculpt.c's, 'do_symmetrical_brush_actions' and 'vpaint_do_symmetrical_brush_actions'. */
static void wpaint_do_symmetrical_brush_actions(
        bContext *C, Object *ob, VPaint *wp, Sculpt *sd, struct WPaintData *wpd, WeightPaintInfo *wpi)
{
	Brush *brush = BKE_paint_brush(&wp->paint);
	Mesh *me = ob->data;
	SculptSession *ss = ob->sculpt;
	StrokeCache *cache = ss->cache;
	const char symm = wp->paint.symmetry_flags & PAINT_SYMM_AXIS_ALL;
	int i = 0;

	/* initial stroke */
	wpaint_do_paint(C, ob, wp, sd, wpd, wpi, me, brush, 0, 'X', 0, 0);
	wpaint_do_radial_symmetry(C, ob, wp, sd, wpd, wpi, me, brush, 0, 'X');
	wpaint_do_radial_symmetry(C, ob, wp, sd, wpd, wpi, me, brush, 0, 'Y');
	wpaint_do_radial_symmetry(C, ob, wp, sd, wpd, wpi, me, brush, 0, 'Z');

	cache->symmetry = symm;

	/* symm is a bit combination of XYZ - 1 is mirror X; 2 is Y; 3 is XY; 4 is Z; 5 is XZ; 6 is YZ; 7 is XYZ */
	for (i = 1; i <= symm; i++) {
		if ((symm & i && (symm != 5 || i != 3) && (symm != 6 || (i != 3 && i != 5)))) {
			cache->mirror_symmetry_pass = i;
			cache->radial_symmetry_pass = 0;
			sculpt_cache_calc_brushdata_symm(cache, i, 0, 0);

			if (i & (1 << 0)) {
				wpaint_do_paint(C, ob, wp, sd, wpd, wpi, me, brush, i, 'X', 0, 0);
				wpaint_do_radial_symmetry(C, ob, wp, sd, wpd, wpi, me, brush, i, 'X');
			}
			if (i & (1 << 1)) {
				wpaint_do_paint(C, ob, wp, sd, wpd, wpi, me, brush, i, 'Y', 0, 0);
				wpaint_do_radial_symmetry(C, ob, wp, sd, wpd, wpi, me, brush, i, 'Y');
			}
			if (i & (1 << 2)) {
				wpaint_do_paint(C, ob, wp, sd, wpd, wpi, me, brush, i, 'Z', 0, 0);
				wpaint_do_radial_symmetry(C, ob, wp, sd, wpd, wpi, me, brush, i, 'Z');
			}
		}
	}
	copy_v3_v3(cache->true_last_location, cache->true_location);
	cache->is_last_valid = true;
}

static void wpaint_stroke_update_step(bContext *C, struct PaintStroke *stroke, PointerRNA *itemptr)
{
	Scene *scene = CTX_data_scene(C);
	ToolSettings *ts = CTX_data_tool_settings(C);
	VPaint *wp = ts->wpaint;
	Brush *brush = BKE_paint_brush(&wp->paint);
	struct WPaintData *wpd = paint_stroke_mode_data(stroke);
	ViewContext *vc;
	Object *ob = CTX_data_active_object(C);

	SculptSession *ss = ob->sculpt;
	Sculpt *sd = CTX_data_tool_settings(C)->sculpt;

	vwpaint_update_cache_variants(C, wp, ob, itemptr);

	float mat[4][4];
	float mval[2];

	const float brush_alpha_value = BKE_brush_alpha_get(scene, brush);

	/* intentionally don't initialize as NULL, make sure we initialize all members below */
	WeightPaintInfo wpi;

	/* cannot paint if there is no stroke data */
	if (wpd == NULL) {
		/* XXX: force a redraw here, since even though we can't paint,
		 * at least view won't freeze until stroke ends */
		ED_region_tag_redraw(CTX_wm_region(C));
		return;
	}

	vc = &wpd->vc;
	ob = vc->obact;

	view3d_operator_needs_opengl(C);
	ED_view3d_init_mats_rv3d(ob, vc->rv3d);

	/* load projection matrix */
	mul_m4_m4m4(mat, vc->rv3d->persmat, ob->obmat);


	/* *** setup WeightPaintInfo - pass onto do_weight_paint_vertex *** */
	wpi.defbase_tot =        wpd->defbase_tot;
	wpi.defbase_sel =        wpd->defbase_sel;
	wpi.defbase_tot_sel =    wpd->defbase_tot_sel;

	wpi.defbase_tot_unsel =  wpi.defbase_tot - wpi.defbase_tot_sel;
	wpi.active =             wpd->active;
	wpi.mirror =             wpd->mirror;
	wpi.lock_flags =         wpd->lock_flags;
	wpi.vgroup_validmap =    wpd->vgroup_validmap;
	wpi.do_flip =            RNA_boolean_get(itemptr, "pen_flip");
	wpi.do_multipaint =      wpd->do_multipaint;
	wpi.do_auto_normalize =  ((ts->auto_normalize != 0) && (wpi.vgroup_validmap != NULL));
	wpi.brush_alpha_value =  brush_alpha_value;
	/* *** done setting up WeightPaintInfo *** */

	wpaint_do_symmetrical_brush_actions(C, ob, wp, sd, wpd, &wpi);

	swap_m4m4(vc->rv3d->persmat, mat);

	/* calculate pivot for rotation around seletion if needed */
	/* also needed for "View Selected" on last stroke */
	paint_last_stroke_update(scene, vc->ar, mval);

	DAG_id_tag_update(ob->data, 0);
	WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, ob);
	swap_m4m4(wpd->vc.rv3d->persmat, mat);

	rcti r;
	if (sculpt_get_redraw_rect(vc->ar, CTX_wm_region_view3d(C), ob, &r)) {
		if (ss->cache) {
			ss->cache->current_r = r;
		}

		/* previous is not set in the current cache else
		 * the partial rect will always grow */
		if (ss->cache) {
			if (!BLI_rcti_is_empty(&ss->cache->previous_r))
				BLI_rcti_union(&r, &ss->cache->previous_r);
		}

		r.xmin += vc->ar->winrct.xmin - 2;
		r.xmax += vc->ar->winrct.xmin + 2;
		r.ymin += vc->ar->winrct.ymin - 2;
		r.ymax += vc->ar->winrct.ymin + 2;

		ss->partial_redraw = 1;
	}
	ED_region_tag_redraw_partial(vc->ar, &r);
}

static void wpaint_stroke_done(const bContext *C, struct PaintStroke *stroke)
{
	Object *ob = CTX_data_active_object(C);
	struct WPaintData *wpd = paint_stroke_mode_data(stroke);

	if (wpd) {
		ED_vpaint_proj_handle_free(wpd->vp_handle);

		if (wpd->defbase_sel)
			MEM_freeN((void *)wpd->defbase_sel);
		if (wpd->vgroup_validmap)
			MEM_freeN((void *)wpd->vgroup_validmap);
		if (wpd->lock_flags)
			MEM_freeN((void *)wpd->lock_flags);
		if (wpd->active.lock)
			MEM_freeN((void *)wpd->active.lock);
		if (wpd->mirror.lock)
			MEM_freeN((void *)wpd->mirror.lock);

		MEM_freeN(wpd);
	}

	/* and particles too */
	if (ob->particlesystem.first) {
		ParticleSystem *psys;
		int i;

		for (psys = ob->particlesystem.first; psys; psys = psys->next) {
			for (i = 0; i < PSYS_TOT_VG; i++) {
				if (psys->vgroup[i] == ob->actdef) {
					psys->recalc |= PSYS_RECALC_RESET;
					break;
				}
			}
		}
	}

	DAG_id_tag_update(ob->data, 0);

	WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, ob);

	sculpt_cache_free(ob->sculpt->cache);
	ob->sculpt->cache = NULL;
}


static int wpaint_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
	int retval;

	op->customdata = paint_stroke_new(
	        C, op, sculpt_stroke_get_location, wpaint_stroke_test_start,
	        wpaint_stroke_update_step, NULL,
	        wpaint_stroke_done, event->type);

	if ((retval = op->type->modal(C, op, event)) == OPERATOR_FINISHED) {
		paint_stroke_data_free(op);
		return OPERATOR_FINISHED;
	}
	/* add modal handler */
	WM_event_add_modal_handler(C, op);

	OPERATOR_RETVAL_CHECK(retval);
	BLI_assert(retval == OPERATOR_RUNNING_MODAL);

	return OPERATOR_RUNNING_MODAL;
}

static int wpaint_exec(bContext *C, wmOperator *op)
{
	op->customdata = paint_stroke_new(
	        C, op, sculpt_stroke_get_location, wpaint_stroke_test_start,
	        wpaint_stroke_update_step, NULL,
	        wpaint_stroke_done, 0);

	/* frees op->customdata */
	paint_stroke_exec(C, op);

	return OPERATOR_FINISHED;
}

static void wpaint_cancel(bContext *C, wmOperator *op)
{
	Object *ob = CTX_data_active_object(C);
	if (ob->sculpt->cache) {
		sculpt_cache_free(ob->sculpt->cache);
		ob->sculpt->cache = NULL;
	}

	paint_stroke_cancel(C, op);
}

void PAINT_OT_weight_paint(wmOperatorType *ot)
{
	/* identifiers */
	ot->name = "Weight Paint";
	ot->idname = "PAINT_OT_weight_paint";
	ot->description = "Paint a stroke in the current vertex group's weights";

	/* api callbacks */
	ot->invoke = wpaint_invoke;
	ot->modal = paint_stroke_modal;
	ot->exec = wpaint_exec;
	ot->poll = weight_paint_poll;
	ot->cancel = wpaint_cancel;

	/* flags */
	ot->flag = OPTYPE_UNDO | OPTYPE_BLOCKING;

	paint_stroke_operator_properties(ot);
}

/* ************ set / clear vertex paint mode ********** */

/**
 * \note Keep in sync with #wpaint_mode_toggle_exec
 */
static int vpaint_mode_toggle_exec(bContext *C, wmOperator *op)
{
	Object *ob = CTX_data_active_object(C);
	const int mode_flag = OB_MODE_VERTEX_PAINT;
	const bool is_mode_set = (ob->mode & mode_flag) != 0;
	Scene *scene = CTX_data_scene(C);
	VPaint *vp = scene->toolsettings->vpaint;
	Mesh *me;

	if (!is_mode_set) {
		if (!ED_object_mode_compat_set(C, ob, mode_flag, op->reports)) {
			return OPERATOR_CANCELLED;
		}
	}

	me = BKE_mesh_from_object(ob);

	/* toggle: end vpaint */
	if (is_mode_set) {
		ob->mode &= ~mode_flag;

		if (me->editflag & ME_EDIT_PAINT_FACE_SEL) {
			BKE_mesh_flush_select_from_polys(me);
		}
		else if (me->editflag & ME_EDIT_PAINT_VERT_SEL) {
			BKE_mesh_flush_select_from_verts(me);
		}

		/* If the cache is not released by a cancel or a done, free it now. */
		if (ob->sculpt->cache) {
			sculpt_cache_free(ob->sculpt->cache);
			ob->sculpt->cache = NULL;
		}

		BKE_sculptsession_free(ob);

		paint_cursor_delete_textures();
	}
	else {
		ob->mode |= mode_flag;

		ED_mesh_color_ensure(me, NULL);

		if (vp == NULL)
			vp = scene->toolsettings->vpaint = new_vpaint(0);

		paint_cursor_start(C, vertex_paint_poll);

		BKE_paint_init(scene, ePaintVertex, PAINT_CURSOR_VERTEX_PAINT);

		/* Create vertex/weight paint mode session data */
		if (ob->sculpt) {
			if (ob->sculpt->cache) {
				sculpt_cache_free(ob->sculpt->cache);
				ob->sculpt->cache = NULL;
			}
			BKE_sculptsession_free(ob);
		}
		vertex_paint_init_session(scene, ob);
	}

	/* update modifier stack for mapping requirements */
	DAG_id_tag_update(&me->id, 0);

	WM_event_add_notifier(C, NC_SCENE | ND_MODE, scene);

	return OPERATOR_FINISHED;
}

void PAINT_OT_vertex_paint_toggle(wmOperatorType *ot)
{
	/* identifiers */
	ot->name = "Vertex Paint Mode";
	ot->idname = "PAINT_OT_vertex_paint_toggle";
	ot->description = "Toggle the vertex paint mode in 3D view";

	/* api callbacks */
	ot->exec = vpaint_mode_toggle_exec;
	ot->poll = paint_poll_test;

	/* flags */
	ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}



/* ********************** vertex paint operator ******************* */

/* Implementation notes:
 *
 * Operator->invoke()
 * - validate context (add mcol)
 * - create customdata storage
 * - call paint once (mouse click)
 * - add modal handler
 *
 * Operator->modal()
 * - for every mousemove, apply vertex paint
 * - exit on mouse release, free customdata
 *   (return OPERATOR_FINISHED also removes handler and operator)
 *
 * For future:
 * - implement a stroke event (or mousemove with past positons)
 * - revise whether op->customdata should be added in object, in set_vpaint
 */

typedef struct PolyFaceMap {
	struct PolyFaceMap *next, *prev;
	int facenr;
} PolyFaceMap;

struct VPaintData {
	ViewContext vc;
	uint paintcol;

	struct VertProjHandle *vp_handle;
	struct DMCoNo *vertexcosnos;

	float vpimat[3][3];

	/* modify 'me->mcol' directly, since the derived mesh is drawing from this
	 * array, otherwise we need to refresh the modifier stack */
	bool use_fast_update;

	/* loops tagged as having been painted, to apply shared vertex color
	 * blending only to modified loops */
	bool *mlooptag;

	bool is_texbrush;
};

static bool vpaint_stroke_test_start(bContext *C, struct wmOperator *op, const float mouse[2])
{
	Scene *scene = CTX_data_scene(C);
	ToolSettings *ts = scene->toolsettings;
	struct PaintStroke *stroke = op->customdata;
	VPaint *vp = ts->vpaint;
	Brush *brush = BKE_paint_brush(&vp->paint);
	struct VPaintData *vpd;
	Object *ob = CTX_data_active_object(C);
	Mesh *me;
	float mat[4][4], imat[4][4];
	SculptSession *ss = ob->sculpt;

	/* context checks could be a poll() */
	me = BKE_mesh_from_object(ob);
	if (me == NULL || me->totpoly == 0)
		return false;

	ED_mesh_color_ensure(me, NULL);
	if (me->mloopcol == NULL)
		return false;

	/* make mode data storage */
	vpd = MEM_callocN(sizeof(*vpd), "VPaintData");
	paint_stroke_set_mode_data(stroke, vpd);
	view3d_set_viewcontext(C, &vpd->vc);

	vpd->paintcol = vpaint_get_current_col(scene, vp);

	vpd->is_texbrush = !(brush->vertexpaint_tool == PAINT_BLEND_BLUR) &&
	                   brush->mtex.tex;

	/* are we painting onto a modified mesh?,
	 * if not we can skip face map trickiness */
	if (vertex_paint_use_fast_update_check(ob)) {
		vpd->use_fast_update = true;
/*		printf("Fast update!\n");*/
	}
	else {
		vpd->use_fast_update = false;
/*		printf("No fast update!\n");*/
	}

	/* to keep tracked of modified loops for shared vertex color blending */
	if (brush->vertexpaint_tool == PAINT_BLEND_BLUR) {
		vpd->mlooptag = MEM_mallocN(sizeof(bool) * me->totloop, "VPaintData mlooptag");
	}

	/* Create projection handle */
	if (vpd->is_texbrush) {
		ob->sculpt->building_vp_handle = true;
		vpd->vp_handle = ED_vpaint_proj_handle_create(scene, ob, &vpd->vertexcosnos);
		ob->sculpt->building_vp_handle = false;
	}

	/* some old cruft to sort out later */
	mul_m4_m4m4(mat, vpd->vc.rv3d->viewmat, ob->obmat);
	invert_m4_m4(imat, mat);
	copy_m3_m4(vpd->vpimat, imat);

	/* If not previously created, create vertex/weight paint mode session data */
	vertex_paint_init_session(scene, ob);
	vwpaint_update_cache_invariants(C, vp, ss, op, mouse);
	vertex_paint_init_session_data(ts, ob);

	if (ob->sculpt->mode.vpaint.previous_color != NULL) {
		memset(ob->sculpt->mode.vpaint.previous_color, 0, sizeof(uint) * me->totloop);
	}

	return 1;
}

static void do_vpaint_brush_calc_average_color_cb_ex(
        void *userdata, void *UNUSED(userdata_chunk), const int n, const int UNUSED(thread_id))
{
	SculptThreadedTaskData *data = userdata;
	SculptSession *ss = data->ob->sculpt;
	CCGDerivedMesh *ccgdm = BKE_pbvh_get_ccgdm(ss->pbvh);
	const struct SculptVertexPaintGeomMap *gmap = &ss->mode.vpaint.gmap;

	StrokeCache *cache = ss->cache;
	uint *lcol = data->lcol;
	char *col;
	const bool use_vert_sel = (data->me->editflag & (ME_EDIT_PAINT_FACE_SEL | ME_EDIT_PAINT_VERT_SEL)) != 0;

	struct VPaintAverageAccum *accum = (struct VPaintAverageAccum *)data->custom_data + n;
	accum->len = 0;
	memset(accum->value, 0, sizeof(accum->value));

	SculptBrushTest test;
	sculpt_brush_test_init(ss, &test);

	/* For each vertex */
	PBVHVertexIter vd;
	BKE_pbvh_vertex_iter_begin(ss->pbvh, data->nodes[n], vd, PBVH_ITER_UNIQUE)
	{
		/* Test to see if the vertex coordinates are within the spherical brush region. */
		if (sculpt_brush_test_fast(&test, vd.co)) {
			const int v_index = ccgdm ? data->me->mloop[vd.grid_indices[vd.g]].v : vd.vert_indices[vd.i];
			if (BKE_brush_curve_strength(data->brush, test.dist, cache->radius) > 0.0) {
				/* If the vertex is selected for painting. */
				const MVert *mv = &data->me->mvert[v_index];
				if (!use_vert_sel || mv->flag & SELECT) {
					accum->len += gmap->vert_to_loop[v_index].count;
					/* if a vertex is within the brush region, then add it's color to the blend. */
					for (int j = 0; j < gmap->vert_to_loop[v_index].count; j++) {
						const int l_index = gmap->vert_to_loop[v_index].indices[j];
						col = (char *)(&lcol[l_index]);
						/* Color is squared to compensate the sqrt color encoding. */
						accum->value[0] += col[0] * col[0];
						accum->value[1] += col[1] * col[1];
						accum->value[2] += col[2] * col[2];
					}
				}
			}
		}
	}
	BKE_pbvh_vertex_iter_end;
}

static void handle_texture_brush(
        SculptThreadedTaskData *data, PBVHVertexIter vd, float size_pressure, float alpha_pressure,
        float *r_alpha, uint *r_color)
{
	SculptSession *ss = data->ob->sculpt;
	CCGDerivedMesh *ccgdm = BKE_pbvh_get_ccgdm(ss->pbvh);
	const int v_index = ccgdm ? data->me->mloop[vd.grid_indices[vd.g]].v : vd.vert_indices[vd.i];

	float rgba[4];
	float rgba_br[3];

	*r_alpha = calc_vp_alpha_col_dl(
	        data->vp, &data->vpd->vc, data->vpd->vpimat,
	        &data->vpd->vertexcosnos[v_index], ss->cache->mouse, size_pressure, alpha_pressure, rgba);
	rgb_uchar_to_float(rgba_br, (const uchar *)&data->vpd->paintcol);
	mul_v3_v3(rgba_br, rgba);
	rgb_float_to_uchar((uchar *)r_color, rgba_br);
}

static void do_vpaint_brush_draw_task_cb_ex(
        void *userdata, void *UNUSED(userdata_chunk), const int n, const int UNUSED(thread_id))
{
	SculptThreadedTaskData *data = userdata;
	SculptSession *ss = data->ob->sculpt;
	CCGDerivedMesh *ccgdm = BKE_pbvh_get_ccgdm(ss->pbvh);
	const struct SculptVertexPaintGeomMap *gmap = &ss->mode.vpaint.gmap;

	Brush *brush = data->brush;
	StrokeCache *cache = ss->cache;
	const float brush_strength = cache->bstrength;
	uint *lcol = data->lcol;
	Scene *scene = CTX_data_scene(data->C);
	float brush_size_pressure, brush_alpha_value, brush_alpha_pressure;
	get_brush_alpha_data(scene, ss, brush, &brush_size_pressure, &brush_alpha_value, &brush_alpha_pressure);
	const bool use_vert_sel = (data->me->editflag & (ME_EDIT_PAINT_FACE_SEL | ME_EDIT_PAINT_VERT_SEL)) != 0;
	const bool use_face_sel = (data->me->editflag & ME_EDIT_PAINT_FACE_SEL) != 0;

	SculptBrushTest test;
	sculpt_brush_test_init(ss, &test);

	/* For each vertex */
	PBVHVertexIter vd;
	BKE_pbvh_vertex_iter_begin(ss->pbvh, data->nodes[n], vd, PBVH_ITER_UNIQUE)
	{
		/* Test to see if the vertex coordinates are within the spherical brush region. */
		if (sculpt_brush_test_sq(&test, vd.co)) {
			/* Note: Grids are 1:1 with corners (aka loops).
			 * For grid based pbvh, take the vert whose loop cooresponds to the current grid.
			 * Otherwise, take the current vert. */
			const int v_index = ccgdm ? data->me->mloop[vd.grid_indices[vd.g]].v : vd.vert_indices[vd.i];
			const float grid_alpha = ccgdm ? 1.0f / vd.gridsize : 1.0f;
			const MVert *mv = &data->me->mvert[v_index];

			/* If the vertex is selected for painting. */
			if (!use_vert_sel || mv->flag & SELECT) {
				/* Calc the dot prod. between ray norm on surf and current vert
				 * (ie splash prevention factor), and only paint front facing verts. */
				const float view_dot = (vd.no) ? dot_vf3vs3(cache->sculpt_normal_symm, vd.no) : 1.0;
				if (view_dot > 0.0f) {
					const float brush_fade = BKE_brush_curve_strength(brush, sqrtf(test.dist), cache->radius);
					uint color_final = data->vpd->paintcol;

					/* If we're painting with a texture, sample the texture color and alpha. */
					float tex_alpha = 1.0;
					if (data->vpd->is_texbrush) {
						handle_texture_brush(
						        data, vd, brush_size_pressure, brush_alpha_pressure,
						        &tex_alpha, &color_final);
					}
					/* For each poly owning this vert, paint each loop belonging to this vert. */
					for (int j = 0; j < gmap->vert_to_poly[v_index].count; j++) {
						const int p_index = gmap->vert_to_poly[v_index].indices[j];
						const int l_index = gmap->vert_to_loop[v_index].indices[j];
						BLI_assert(data->me->mloop[l_index].v == v_index);
						const MPoly *mp = &data->me->mpoly[p_index];
						if (!use_face_sel || mp->flag & ME_FACE_SEL) {
							uint color_orig = 0;  /* unused when array is NULL */
							if (ss->mode.vpaint.previous_color != NULL) {
								/* Get the previous loop color */
								if (ss->mode.vpaint.previous_color[l_index] == 0) {
									ss->mode.vpaint.previous_color[l_index] = lcol[l_index];
								}
								color_orig = ss->mode.vpaint.previous_color[l_index];
							}
							float final_alpha =
							        255 * brush_fade * brush_strength *
							        tex_alpha * brush_alpha_pressure * grid_alpha;

							if (data->vp->flag & VP_NORMALS) {
								final_alpha *= view_dot;
							}
							if (brush->flag & BRUSH_ACCUMULATE) {
								float mask_accum = ss->mode.vpaint.previous_accum[l_index];
								final_alpha = min_ff(final_alpha + mask_accum, 255.0f * brush_strength);
								ss->mode.vpaint.previous_accum[l_index] = final_alpha;
							}

							/* Mix the new color with the original based on final_alpha. */
							lcol[l_index] = vpaint_blend(
							        data->vp, lcol[l_index], color_orig, color_final,
							        final_alpha, 255 * brush_strength);
						}
					}
				}
			}
		}
	}
	BKE_pbvh_vertex_iter_end;
}

static void do_vpaint_brush_blur_task_cb_ex(
        void *userdata, void *UNUSED(userdata_chunk), const int n, const int UNUSED(thread_id))
{
	SculptThreadedTaskData *data = userdata;
	SculptSession *ss = data->ob->sculpt;
	CCGDerivedMesh *ccgdm = BKE_pbvh_get_ccgdm(ss->pbvh);

	Scene *scene = CTX_data_scene(data->C);
	const struct SculptVertexPaintGeomMap *gmap = &ss->mode.vpaint.gmap;
	Brush *brush = data->brush;
	StrokeCache *cache = ss->cache;
	const float brush_strength = cache->bstrength;
	uint *lcol = data->lcol;
	float brush_size_pressure, brush_alpha_value, brush_alpha_pressure;
	get_brush_alpha_data(scene, ss, brush, &brush_size_pressure, &brush_alpha_value, &brush_alpha_pressure);
	const bool use_vert_sel = (data->me->editflag & (ME_EDIT_PAINT_FACE_SEL | ME_EDIT_PAINT_VERT_SEL)) != 0;
	const bool use_face_sel = (data->me->editflag & ME_EDIT_PAINT_FACE_SEL) != 0;

	SculptBrushTest test;
	sculpt_brush_test_init(ss, &test);

	/* For each vertex */
	PBVHVertexIter vd;
	BKE_pbvh_vertex_iter_begin(ss->pbvh, data->nodes[n], vd, PBVH_ITER_UNIQUE)
	{
		/* Test to see if the vertex coordinates are within the spherical brush region. */
		if (sculpt_brush_test_sq(&test, vd.co)) {
			/* For grid based pbvh, take the vert whose loop cooresponds to the current grid.
			 * Otherwise, take the current vert. */
			const int v_index = ccgdm ? data->me->mloop[vd.grid_indices[vd.g]].v : vd.vert_indices[vd.i];
			const float grid_alpha = ccgdm ? 1.0f / vd.gridsize : 1.0f;
			const MVert *mv = &data->me->mvert[v_index];

			const float view_dot = (vd.no) ? dot_vf3vs3(cache->sculpt_normal_symm, vd.no) : 1.0;
			if (view_dot > 0.0f) {
				const float brush_fade = BKE_brush_curve_strength(brush, sqrtf(test.dist), cache->radius);

				/* If the vertex is selected for painting. */
				if (!use_vert_sel || mv->flag & SELECT) {
					/* Get the average poly color */
					uint color_final = 0;
					int total_hit_loops = 0;
					uint blend[4] = {0};
					for (int j = 0; j < gmap->vert_to_poly[v_index].count; j++) {
						int p_index = gmap->vert_to_poly[v_index].indices[j];
						const MPoly *mp = &data->me->mpoly[p_index];
						if (!use_face_sel || mp->flag & ME_FACE_SEL) {
							total_hit_loops += mp->totloop;
							for (int k = 0; k < mp->totloop; k++) {
								const uint l_index = mp->loopstart + k;
								const char *col = (const char *)(&lcol[l_index]);
								/* Color is squared to compensate the sqrt color encoding. */
								blend[0] += (uint)col[0] * (uint)col[0];
								blend[1] += (uint)col[1] * (uint)col[1];
								blend[2] += (uint)col[2] * (uint)col[2];
								blend[3] += (uint)col[3] * (uint)col[3];
							}
						}
					}
					if (total_hit_loops != 0) {
						/* Use rgb^2 color averaging. */
						char *col = (char *)(&color_final);
						col[0] = round_fl_to_uchar(sqrtf(divide_round_i(blend[0], total_hit_loops)));
						col[1] = round_fl_to_uchar(sqrtf(divide_round_i(blend[1], total_hit_loops)));
						col[2] = round_fl_to_uchar(sqrtf(divide_round_i(blend[2], total_hit_loops)));
						col[3] = round_fl_to_uchar(sqrtf(divide_round_i(blend[3], total_hit_loops)));

						/* For each poly owning this vert, paint each loop belonging to this vert. */
						for (int j = 0; j < gmap->vert_to_poly[v_index].count; j++) {
							const int p_index = gmap->vert_to_poly[v_index].indices[j];
							const int l_index = gmap->vert_to_loop[v_index].indices[j];
							BLI_assert(data->me->mloop[l_index].v == v_index);
							const MPoly *mp = &data->me->mpoly[p_index];
							if (!use_face_sel || mp->flag & ME_FACE_SEL) {
								uint color_orig = 0;  /* unused when array is NULL */
								if (ss->mode.vpaint.previous_color != NULL) {
									/* Get the previous loop color */
									if (ss->mode.vpaint.previous_color[l_index] == 0) {
										ss->mode.vpaint.previous_color[l_index] = lcol[l_index];
									}
									color_orig = ss->mode.vpaint.previous_color[l_index];
								}
								float final_alpha =
								        255 * brush_fade * brush_strength *
								        brush_alpha_pressure * grid_alpha;

								if (data->vp->flag & VP_NORMALS) {
									final_alpha *= view_dot;
								}
								/* Mix the new color with the original
								 * based on the brush strength and the curve. */
								lcol[l_index] = vpaint_blend(
								        data->vp, lcol[l_index], color_orig, *((uint *)col),
								        final_alpha, 255 * brush_strength);
							}
						}
					}
				}
			}
		}
	}
	BKE_pbvh_vertex_iter_end;
}

static void do_vpaint_brush_smear_task_cb_ex(
        void *userdata, void *UNUSED(userdata_chunk), const int n, const int UNUSED(thread_id))
{
	SculptThreadedTaskData *data = userdata;
	SculptSession *ss = data->ob->sculpt;
	CCGDerivedMesh *ccgdm = BKE_pbvh_get_ccgdm(ss->pbvh);

	Scene *scene = CTX_data_scene(data->C);
	const struct SculptVertexPaintGeomMap *gmap = &ss->mode.vpaint.gmap;
	Brush *brush = data->brush;
	StrokeCache *cache = ss->cache;
	const float brush_strength = cache->bstrength;
	uint *lcol = data->lcol;
	float brush_size_pressure, brush_alpha_value, brush_alpha_pressure;
	get_brush_alpha_data(scene, ss, brush, &brush_size_pressure, &brush_alpha_value, &brush_alpha_pressure);
	float brush_dir[3];
	const bool use_vert_sel = (data->me->editflag & (ME_EDIT_PAINT_FACE_SEL | ME_EDIT_PAINT_VERT_SEL)) != 0;
	const bool use_face_sel = (data->me->editflag & ME_EDIT_PAINT_FACE_SEL) != 0;

	sub_v3_v3v3(brush_dir, cache->location, cache->last_location);
	project_plane_v3_v3v3(brush_dir, brush_dir, cache->view_normal);

	if (normalize_v3(brush_dir) != 0.0f) {

		SculptBrushTest test;
		sculpt_brush_test_init(ss, &test);

		/* For each vertex */
		PBVHVertexIter vd;
		BKE_pbvh_vertex_iter_begin(ss->pbvh, data->nodes[n], vd, PBVH_ITER_UNIQUE)
		{
			/* Test to see if the vertex coordinates are within the spherical brush region. */
			if (sculpt_brush_test_sq(&test, vd.co)) {
				/* For grid based pbvh, take the vert whose loop cooresponds to the current grid.
				 * Otherwise, take the current vert. */
				const int v_index = ccgdm ? data->me->mloop[vd.grid_indices[vd.g]].v : vd.vert_indices[vd.i];
				const float grid_alpha = ccgdm ? 1.0f / vd.gridsize : 1.0f;
				const MVert *mv_curr = &data->me->mvert[v_index];

				/* if the vertex is selected for painting. */
				if (!use_vert_sel || mv_curr->flag & SELECT) {
					/* Calc the dot prod. between ray norm on surf and current vert
					 * (ie splash prevention factor), and only paint front facing verts. */
					const float view_dot = (vd.no) ? dot_vf3vs3(cache->sculpt_normal_symm, vd.no) : 1.0;
					if (view_dot > 0.0f) {
						const float brush_fade = BKE_brush_curve_strength(brush, sqrtf(test.dist), cache->radius);

						bool do_color = false;
						/* Minimum dot product between brush direction and current
						 * to neighbor direction is 0.0, meaning orthogonal. */
						float stroke_dot_max = 0.0f;

						/* Get the color of the loop in the opposite direction of the brush movement */
						uint color_final = 0;
						for (int j = 0; j < gmap->vert_to_poly[v_index].count; j++) {
							const int p_index = gmap->vert_to_poly[v_index].indices[j];
							const int l_index = gmap->vert_to_loop[v_index].indices[j];
							BLI_assert(data->me->mloop[l_index].v == v_index);
							const MPoly *mp = &data->me->mpoly[p_index];
							if (!use_face_sel || mp->flag & ME_FACE_SEL) {
								const MLoop *ml_other = &data->me->mloop[mp->loopstart];
								for (int k = 0; k < mp->totloop; k++, ml_other++) {
									const uint v_other_index = ml_other->v;
									if (v_other_index != v_index) {
										const MVert *mv_other = &data->me->mvert[v_other_index];

										/* Get the direction from the selected vert to the neighbor. */
										float other_dir[3];
										sub_v3_v3v3(other_dir, mv_curr->co, mv_other->co);
										project_plane_v3_v3v3(other_dir, other_dir, cache->view_normal);

										normalize_v3(other_dir);

										const float stroke_dot = dot_v3v3(other_dir, brush_dir);

										if (stroke_dot > stroke_dot_max) {
											stroke_dot_max = stroke_dot;
											color_final = lcol[mp->loopstart + k];
											do_color = true;
										}
									}
								}
							}
						}

						if (do_color) {
							/* For each poly owning this vert, paint each loop belonging to this vert. */
							for (int j = 0; j < gmap->vert_to_poly[v_index].count; j++) {
								const int p_index = gmap->vert_to_poly[v_index].indices[j];
								const int l_index = gmap->vert_to_loop[v_index].indices[j];
								BLI_assert(data->me->mloop[l_index].v == v_index);
								const MPoly *mp = &data->me->mpoly[p_index];
								if (!use_face_sel || mp->flag & ME_FACE_SEL) {
									/* Get the previous loop color */
									uint color_orig = 0;  /* unused when array is NULL */
									if (ss->mode.vpaint.previous_color != NULL) {
										/* Get the previous loop color */
										if (ss->mode.vpaint.previous_color[l_index] == 0) {
											ss->mode.vpaint.previous_color[l_index] = lcol[l_index];
										}
										color_orig = ss->mode.vpaint.previous_color[l_index];
									}
									float final_alpha =
									        255 * brush_fade * brush_strength *
									        brush_alpha_pressure * grid_alpha;

									if (data->vp->flag & VP_NORMALS) {
										final_alpha *= view_dot;
									}
									/* Mix the new color with the original
									 * based on the brush strength and the curve. */
									lcol[l_index] = vpaint_blend(
									        data->vp, lcol[l_index], color_orig, color_final,
									        final_alpha, 255 * brush_strength);
								}
							}
						}
					}
				}
			}
		}
		BKE_pbvh_vertex_iter_end;
	}
}

static void calculate_average_color(SculptThreadedTaskData *data, PBVHNode **UNUSED(nodes), int totnode)
{
	struct VPaintAverageAccum *accum = MEM_mallocN(sizeof(*accum) * totnode, __func__);
	data->custom_data = accum;

	BLI_task_parallel_range_ex(
	        0, totnode, data, NULL, 0, do_vpaint_brush_calc_average_color_cb_ex,
	        true, false);

	uint accum_len = 0;
	uint accum_value[3] = {0};
	uchar blend[4] = {0};
	for (int i = 0; i < totnode; i++) {
		accum_len += accum[i].len;
		accum_value[0] += accum[i].value[0];
		accum_value[1] += accum[i].value[1];
		accum_value[2] += accum[i].value[2];
	}
	if (accum_len != 0) {
		blend[0] = round_fl_to_uchar(sqrtf(divide_round_i(accum_value[0], accum_len)));
		blend[1] = round_fl_to_uchar(sqrtf(divide_round_i(accum_value[1], accum_len)));
		blend[2] = round_fl_to_uchar(sqrtf(divide_round_i(accum_value[2], accum_len)));
		blend[3] = 255;
		data->vpd->paintcol = *((uint *)blend);
	}

	MEM_SAFE_FREE(data->custom_data);  /* 'accum' */
}

static void vpaint_paint_leaves(
        bContext *C, Sculpt *sd, VPaint *vp, struct VPaintData *vpd,
        Object *ob, Mesh *me, PBVHNode **nodes, int totnode)
{
	Brush *brush = ob->sculpt->cache->brush;

	SculptThreadedTaskData data = {
		.sd = sd, .ob = ob, .brush = brush, .nodes = nodes, .vp = vp, .vpd = vpd,
		.lcol = (uint *)me->mloopcol, .me = me, .C = C,
	};
	switch (brush->vertexpaint_tool) {
		case PAINT_BLEND_AVERAGE:
			calculate_average_color(&data, nodes, totnode);
			BLI_task_parallel_range_ex(
			    0, totnode, &data, NULL, 0,
			    do_vpaint_brush_draw_task_cb_ex, true, false);
			break;
		case PAINT_BLEND_BLUR:
			BLI_task_parallel_range_ex(
			    0, totnode, &data, NULL, 0,
			    do_vpaint_brush_blur_task_cb_ex, true, false);
			break;
		case PAINT_BLEND_SMEAR:
			BLI_task_parallel_range_ex(
			    0, totnode, &data, NULL, 0,
			    do_vpaint_brush_smear_task_cb_ex, true, false);
			break;
		default:
			BLI_task_parallel_range_ex(
			    0, totnode, &data, NULL, 0,
			    do_vpaint_brush_draw_task_cb_ex, true, false);
			break;
	}
}

static void vpaint_do_paint(
        bContext *C, Sculpt *sd, VPaint *vd, struct VPaintData *vpd,
        Object *ob, Mesh *me, Brush *brush, const char symm, const int axis, const int i, const float angle)
{
	SculptSession *ss = ob->sculpt;
	ss->cache->radial_symmetry_pass = i;
	sculpt_cache_calc_brushdata_symm(ss->cache, symm, axis, angle);
	SculptSearchSphereData data;
	PBVHNode **nodes = NULL;
	int totnode;

	/* Build a list of all nodes that are potentially within the brush's area of influence */
	data.ss = ss;
	data.sd = sd;
	data.radius_squared = ss->cache->radius_squared;
	data.original = true;
	BKE_pbvh_search_gather(ss->pbvh, sculpt_search_sphere_cb, &data, &nodes, &totnode);

	sculpt_pbvh_calc_area_normal(brush, ob, nodes, totnode, true, ss->cache->sculpt_normal_symm);

	/* Paint those leaves. */
	vpaint_paint_leaves(C, sd, vd, vpd, ob, me, nodes, totnode);

	if (nodes) {
		MEM_freeN(nodes);
	}
}

static void vpaint_do_radial_symmetry(
        bContext *C, Sculpt *sd, VPaint *vd, struct VPaintData *vpd, Object *ob, Mesh *me,
        Brush *brush, const char symm, const int axis)
{
	for (int i = 1; i < vd->radial_symm[axis - 'X']; i++) {
		const float angle = (2.0 * M_PI) * i / vd->radial_symm[axis - 'X'];
		vpaint_do_paint(C, sd, vd, vpd, ob, me, brush, symm, axis, i, angle);
	}
}

/* near duplicate of: sculpt.c's, 'do_symmetrical_brush_actions' and 'wpaint_do_symmetrical_brush_actions'. */
static void vpaint_do_symmetrical_brush_actions(
        bContext *C, Sculpt *sd, VPaint *vd, struct VPaintData *vpd, Object *ob)
{
	Brush *brush = BKE_paint_brush(&vd->paint);
	Mesh *me = ob->data;
	SculptSession *ss = ob->sculpt;
	StrokeCache *cache = ss->cache;
	const char symm = vd->paint.symmetry_flags & PAINT_SYMM_AXIS_ALL;
	int i = 0;

	/* initial stroke */
	vpaint_do_paint(C, sd, vd, vpd, ob, me, brush, i, 'X', 0, 0);
	vpaint_do_radial_symmetry(C, sd, vd, vpd, ob, me, brush, i, 'X');
	vpaint_do_radial_symmetry(C, sd, vd, vpd, ob, me, brush, i, 'Y');
	vpaint_do_radial_symmetry(C, sd, vd, vpd, ob, me, brush, i, 'Z');

	cache->symmetry = symm;

	/* symm is a bit combination of XYZ - 1 is mirror X; 2 is Y; 3 is XY; 4 is Z; 5 is XZ; 6 is YZ; 7 is XYZ */
	for (i = 1; i <= symm; i++) {
		if (symm & i && (symm != 5 || i != 3) && (symm != 6 || (i != 3 && i != 5))) {
			cache->mirror_symmetry_pass = i;
			cache->radial_symmetry_pass = 0;
			sculpt_cache_calc_brushdata_symm(cache, i, 0, 0);

			if (i & (1 << 0)) {
				vpaint_do_paint(C, sd, vd, vpd, ob, me, brush, i, 'X', 0, 0);
				vpaint_do_radial_symmetry(C, sd, vd, vpd, ob, me, brush, i, 'X');
			}
			if (i & (1 << 1)) {
				vpaint_do_paint(C, sd, vd, vpd, ob, me, brush, i, 'Y', 0, 0);
				vpaint_do_radial_symmetry(C, sd, vd, vpd, ob, me, brush, i, 'Y');
			}
			if (i & (1 << 2)) {
				vpaint_do_paint(C, sd, vd, vpd, ob, me, brush, i, 'Z', 0, 0);
				vpaint_do_radial_symmetry(C, sd, vd, vpd, ob, me, brush, i, 'Z');
			}
		}
	}

	copy_v3_v3(cache->true_last_location, cache->true_location);
	cache->is_last_valid = true;
}

static void vpaint_stroke_update_step(bContext *C, struct PaintStroke *stroke, PointerRNA *itemptr)
{
	Scene *scene = CTX_data_scene(C);
	ToolSettings *ts = CTX_data_tool_settings(C);
	struct VPaintData *vpd = paint_stroke_mode_data(stroke);
	VPaint *vp = ts->vpaint;
	ViewContext *vc = &vpd->vc;
	Object *ob = vc->obact;
	Sculpt *sd = CTX_data_tool_settings(C)->sculpt;

	vwpaint_update_cache_variants(C, vp, ob, itemptr);

	float mat[4][4];
	float mval[2];

	ED_view3d_init_mats_rv3d(ob, vc->rv3d);

	/* load projection matrix */
	mul_m4_m4m4(mat, vc->rv3d->persmat, ob->obmat);

	swap_m4m4(vc->rv3d->persmat, mat);

	vpaint_do_symmetrical_brush_actions(C, sd, vp, vpd, ob);

	swap_m4m4(vc->rv3d->persmat, mat);

	/* calculate pivot for rotation around seletion if needed */
	/* also needed for "View Selected" on last stroke */
	paint_last_stroke_update(scene, vc->ar, mval);

	ED_region_tag_redraw(vc->ar);

	if (vpd->use_fast_update == false) {
		/* recalculate modifier stack to get new colors, slow,
		 * avoid this if we can! */
		DAG_id_tag_update(ob->data, 0);
	}
	else {
		/* If using new VBO drawing, mark mcol as dirty to force colors gpu buffer refresh! */
		ob->derivedFinal->dirty |= DM_DIRTY_MCOL_UPDATE_DRAW;
	}
}

static void vpaint_stroke_done(const bContext *C, struct PaintStroke *stroke)
{
	struct VPaintData *vpd = paint_stroke_mode_data(stroke);
	ViewContext *vc = &vpd->vc;
	Object *ob = vc->obact;

	if (vpd->mlooptag)
		MEM_freeN(vpd->mlooptag);

	WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, ob);

	MEM_freeN(vpd);

	sculpt_cache_free(ob->sculpt->cache);
	ob->sculpt->cache = NULL;
}

static int vpaint_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
	int retval;

	op->customdata = paint_stroke_new(
	        C, op, sculpt_stroke_get_location, vpaint_stroke_test_start,
	        vpaint_stroke_update_step, NULL,
	        vpaint_stroke_done, event->type);

	if ((retval = op->type->modal(C, op, event)) == OPERATOR_FINISHED) {
		paint_stroke_data_free(op);
		return OPERATOR_FINISHED;
	}

	/* add modal handler */
	WM_event_add_modal_handler(C, op);

	OPERATOR_RETVAL_CHECK(retval);
	BLI_assert(retval == OPERATOR_RUNNING_MODAL);

	return OPERATOR_RUNNING_MODAL;
}

static int vpaint_exec(bContext *C, wmOperator *op)
{
	op->customdata = paint_stroke_new(
	        C, op, sculpt_stroke_get_location, vpaint_stroke_test_start,
	        vpaint_stroke_update_step, NULL,
	        vpaint_stroke_done, 0);

	/* frees op->customdata */
	paint_stroke_exec(C, op);

	return OPERATOR_FINISHED;
}

static void vpaint_cancel(bContext *C, wmOperator *op)
{
	Object *ob = CTX_data_active_object(C);
	if (ob->sculpt->cache) {
		sculpt_cache_free(ob->sculpt->cache);
		ob->sculpt->cache = NULL;
	}

	paint_stroke_cancel(C, op);
}

void PAINT_OT_vertex_paint(wmOperatorType *ot)
{
	/* identifiers */
	ot->name = "Vertex Paint";
	ot->idname = "PAINT_OT_vertex_paint";
	ot->description = "Paint a stroke in the active vertex color layer";

	/* api callbacks */
	ot->invoke = vpaint_invoke;
	ot->modal = paint_stroke_modal;
	ot->exec = vpaint_exec;
	ot->poll = vertex_paint_poll;
	ot->cancel = vpaint_cancel;

	/* flags */
	ot->flag = OPTYPE_UNDO | OPTYPE_BLOCKING;

	paint_stroke_operator_properties(ot);
}
