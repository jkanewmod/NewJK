/*
===========================================================================
Copyright (C) 1999 - 2005, Id Software, Inc.
Copyright (C) 2000 - 2013, Raven Software, Inc.
Copyright (C) 2001 - 2013, Activision, Inc.
Copyright (C) 2013 - 2015, OpenJK contributors

This file is part of the OpenJK source code.

OpenJK is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License version 2 as
published by the Free Software Foundation.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, see <http://www.gnu.org/licenses/>.
===========================================================================
*/

#include "tr_local.h"
#include "tr_WorldEffects.h"

backEndData_t	*backEndData;
backEndState_t	backEnd;

//bool tr_stencilled = false;
//extern qboolean tr_distortionPrePost;
//extern qboolean tr_distortionNegate;
//extern void RB_CaptureScreenImage(void);
//extern void RB_DistortionFill(void);

#define	MAC_EVENT_PUMP_MSEC		5

#if 0
//used by RF_DISTORTION
static inline bool R_WorldCoordToScreenCoordFloat( vec3_t worldCoord, float *x, float *y )
{
	int	xcenter, ycenter;
	vec3_t	local, transformed;
	vec3_t	vfwd;
	vec3_t	vright;
	vec3_t	vup;
	float xzi;
	float yzi;

	xcenter = glConfig.vidWidth / 2;
	ycenter = glConfig.vidHeight / 2;

	//AngleVectors (tr.refdef.viewangles, vfwd, vright, vup);
	VectorCopy(tr.refdef.viewaxis[0], vfwd);
	VectorCopy(tr.refdef.viewaxis[1], vright);
	VectorCopy(tr.refdef.viewaxis[2], vup);

	VectorSubtract (worldCoord, tr.refdef.vieworg, local);

	transformed[0] = DotProduct(local,vright);
	transformed[1] = DotProduct(local,vup);
	transformed[2] = DotProduct(local,vfwd);

	// Make sure Z is not negative.
	if(transformed[2] < 0.01)
	{
		return false;
	}

	xzi = xcenter / transformed[2] * (90.0/tr.refdef.fov_x);
	yzi = ycenter / transformed[2] * (90.0/tr.refdef.fov_y);

	*x = xcenter + xzi * transformed[0];
	*y = ycenter - yzi * transformed[1];

	return true;
}

//used by RF_DISTORTION
static inline bool R_WorldCoordToScreenCoord( vec3_t worldCoord, int *x, int *y )
{
	float	xF, yF;
	bool retVal = R_WorldCoordToScreenCoordFloat( worldCoord, &xF, &yF );
	*x = (int)xF;
	*y = (int)yF;
	return retVal;
}

//number of possible surfs we can postrender.
//note that postrenders lack much of the optimization that the standard sort-render crap does,
//so it's slower.
#define MAX_POST_RENDERS	128

typedef struct postRender_s {
	int			fogNum;
	int			entNum;
	int			dlighted;
	int			depthRange;
	drawSurf_t	*drawSurf;
	shader_t	*shader;
	qboolean	eValid;
} postRender_t;

static postRender_t g_postRenders[MAX_POST_RENDERS];
static int g_numPostRenders = 0;
#endif

/*
================
RB_Hyperspace

A player has predicted a teleport, but hasn't arrived yet
================
*/
static void RB_Hyperspace(void) {
	color4ub_t c;

	if ( !backEnd.isHyperspace ) {
		// do initialization shit
	}

	if ( tess.shader != tr.whiteShader ) {
		RB_EndSurface();
		vk_set_2d();
		RB_BeginSurface( tr.whiteShader, 0 );
	}

#ifdef USE_VBO
	VBO_UnBind();
#endif

	vk_set_2d();

	c[0] = c[1] = c[2] = ( backEnd.refdef.time & 255 );
	c[3] = 255;

	RB_AddQuadStamp2( backEnd.refdef.x, backEnd.refdef.y, backEnd.refdef.width, backEnd.refdef.height,
		0.0, 0.0, 0.0, 0.0, c );

	RB_EndSurface();

	tess.numIndexes = 0;
	tess.numVertexes = 0;

	backEnd.isHyperspace = qtrue;
}

#ifdef USE_PMLIGHT
static void RB_LightingPass(void);
#endif

/*
=================
RB_BeginDrawingView

Any mirrored or portaled views have already been drawn, so prepare
to actually render the visible surfaces for this view
=================
*/
static void RB_BeginDrawingView( void ) {

	// sync with gl if needed
	if ( r_finish->integer == 1 && !glState.finishCalled ) {
		vk_queue_wait_idle();

		glState.finishCalled = qtrue;
	} else if ( r_finish->integer == 0 ) {
		glState.finishCalled = qtrue;
	}

	// we will need to change the projection matrix before drawing
	// 2D images again
	backEnd.projection2D = qfalse;

	// force depth range and viewport/scissor updates
	vk.cmd->depth_range = DEPTH_RANGE_COUNT;

	// ensures that depth writes are enabled for the depth clear
	//vk_clear_depthstencil_attachments(r_shadows->integer == 2 ? qtrue : qfalse);
	vk_clear_depthstencil_attachments(qtrue);

	if ( backEnd.refdef.rdflags & RDF_HYPERSPACE ) {
		RB_Hyperspace();

		backEnd.projection2D = qfalse;

		// force depth range and viewport/scissor updates
		vk.cmd->depth_range = DEPTH_RANGE_COUNT;
	} else {
		backEnd.isHyperspace = qfalse;
	}

	glState.faceCulling = -1;		// force face culling to set next time

	// we will only draw a sun if there was sky rendered in this view
	backEnd.skyRenderedThisView = qfalse;
}

// newjk: file-scope fullbright state, set near the top of RB_StageIteratorGeneric and cleared at
// the very bottom. Any early return between the two leaks the last entity's flat color onto
// everything drawn afterwards.
bool forceFullbright = false, forceFullbrightWhiteShader = false;
vec3_t forceFullbrightColor = { 0, 0, 0 };
extern std::vector<trRefEntity_t *> forceWhiteEnts;

// newjk: wraps RB_EndSurface so the per-weapon-model fullbright override
// (r_fullbrightcolor_weapon_*) is applied to the batch before it is drawn. Flushing with a bare
// RB_EndSurface() anywhere in the draw loop bypasses this and draws that batch uncolored.
static void DoEndSurface(const char *name, bool canFullbright) {
	if (!VALIDSTRING(name)) {
		RB_EndSurface();
		return;
	}

	if (!strncmp(name, "models/weapons2/", 16) && *(name + 16)) {
		const char *weapon = name + 16;
		const cvar_t *weaponCvar;
		if (!strncmp(weapon, "sab", 3))
			weaponCvar = r_fullbrightcolor_weapon_sab;
		else if (!strncmp(weapon, "blaster_p", 9) || !strncmp(weapon, "bri", 3))
			weaponCvar = r_fullbrightcolor_weapon_pis;
		else if (!strncmp(weapon, "blaster_r", 9))
			weaponCvar = r_fullbrightcolor_weapon_bla;
		else if (!strncmp(weapon, "disr", 4))
			weaponCvar = r_fullbrightcolor_weapon_dis;
		else if (!strncmp(weapon, "bow", 3))
			weaponCvar = r_fullbrightcolor_weapon_bow;
		else if (!strncmp(weapon, "hea", 3))
			weaponCvar = r_fullbrightcolor_weapon_rep;
		else if (!strncmp(weapon, "dem", 3))
			weaponCvar = r_fullbrightcolor_weapon_dem;
		else if (!strncmp(weapon, "gol", 3))
			weaponCvar = r_fullbrightcolor_weapon_gol;
		else if (!strncmp(weapon, "mer", 3))
			weaponCvar = r_fullbrightcolor_weapon_roc;
		else if (!strncmp(weapon, "con", 3))
			weaponCvar = r_fullbrightcolor_weapon_con;
		else if (!strncmp(weapon, "the", 3))
			weaponCvar = r_fullbrightcolor_weapon_the;
		else if (!strncmp(weapon, "las", 3))
			weaponCvar = r_fullbrightcolor_weapon_min;
		else if (!strncmp(weapon, "det", 3))
			weaponCvar = r_fullbrightcolor_weapon_dpk;
		else if (!strncmp(weapon, "stu", 3))
			weaponCvar = r_fullbrightcolor_weapon_stu;
		else
			weaponCvar = nullptr;

		float r, g, b;
		int usingWhiteTextureInt;

		if (weaponCvar && VALIDSTRING(weaponCvar->string) && canFullbright && sscanf(weaponCvar->string, "%f %f %f %d", &r, &g, &b, &usingWhiteTextureInt) == 4) {
			float ambientLight[3], directedLight[3], lightDir[3];
			VectorCopy(backEnd.currentEntity->ambientLight, ambientLight);
			VectorCopy(backEnd.currentEntity->directedLight, directedLight);
			VectorCopy(backEnd.currentEntity->lightDir, lightDir);

			forceFullbrightColor[0] = backEnd.currentEntity->ambientLight[0] = backEnd.currentEntity->directedLight[0] = r;
			forceFullbrightColor[1] = backEnd.currentEntity->ambientLight[1] = backEnd.currentEntity->directedLight[1] = g;
			forceFullbrightColor[2] = backEnd.currentEntity->ambientLight[2] = backEnd.currentEntity->directedLight[2] = b;
			r = backEnd.currentEntity->e.shaderRGBA[0] / 255.0f;
			g = backEnd.currentEntity->e.shaderRGBA[1] / 255.0f;
			b = backEnd.currentEntity->e.shaderRGBA[2] / 255.0f;
			int ambientLightInt = backEnd.currentEntity->ambientLightInt;
			((byte *)&backEnd.currentEntity->ambientLightInt)[0] = Q_ftol(r * backEnd.currentEntity->ambientLight[0]);
			((byte *)&backEnd.currentEntity->ambientLightInt)[1] = Q_ftol(g * backEnd.currentEntity->ambientLight[1]);
			((byte *)&backEnd.currentEntity->ambientLightInt)[2] = Q_ftol(b * backEnd.currentEntity->ambientLight[2]);
			((byte *)&backEnd.currentEntity->ambientLightInt)[3] = backEnd.currentEntity->e.shaderRGBA[3];
			VectorCopy(tr.sunDirection, backEnd.currentEntity->lightDir);

			forceFullbright = true;
			if (usingWhiteTextureInt)
				forceFullbrightWhiteShader = true;
			RB_EndSurface();
			forceFullbright = forceFullbrightWhiteShader = false;

			VectorCopy(ambientLight, backEnd.currentEntity->ambientLight);
			VectorCopy(directedLight, backEnd.currentEntity->directedLight);
			VectorCopy(lightDir, backEnd.currentEntity->lightDir);
			backEnd.currentEntity->ambientLightInt = ambientLightInt;
		}
		else {
			RB_EndSurface();
		}
	}
	else {
		RB_EndSurface();
	}
}

/*
==================
RB_RenderDrawSurfList
==================
*/
void RB_RenderDrawSurfList( drawSurf_t *drawSurfs, int numDrawSurfs ) {
	shader_t		*shader, *oldShader;
	int				i, fogNum, oldFogNum, entityNum, oldEntityNum, dlighted, oldDlighted, reType, oldReType;
	Vk_Depth_Range	depthRange;
	drawSurf_t		*drawSurf;
	unsigned int	oldSort;
	float			oldShaderSort, originalTime;
	CBoneCache		*oldBoneCache = nullptr;

#ifdef USE_VANILLA_SHADOWFINISH
	qboolean		didShadowPass;

	if ( backEnd.isGlowPass )
	{ //only shadow on initial passes
		didShadowPass = true;
	}
#endif

	// save original time for entity shader offsets
	originalTime = backEnd.refdef.floatTime;

	oldEntityNum			= -1;
	backEnd.currentEntity	= &tr.worldEntity;
	oldShader				= NULL;
	oldSort					= MAX_UINT;
	oldShaderSort			= -1;
	depthRange				= DEPTH_RANGE_NORMAL;
	oldFogNum				= -1;
	oldDlighted				= qfalse;
	oldReType				= -1;
	qboolean				push_constant;
#ifdef USE_VANILLA_SHADOWFINISH
	didShadowPass			= qfalse;
#endif

	backEnd.pc.c_surfaces	+= numDrawSurfs;

	for (i = 0, drawSurf = drawSurfs; i < numDrawSurfs; i++, drawSurf++)
	{
		R_DecomposeSort(drawSurf->sort, &entityNum, &shader, &fogNum, &dlighted);

		if (vk.renderPassIndex == RENDER_PASS_SCREENMAP && entityNum != REFENTITYNUM_WORLD && backEnd.refdef.entities[entityNum].e.renderfx & RF_DEPTHHACK) {
			continue;
		}

		// check if we have amy dynamic glow surfaces before dglow pass
		if( !backEnd.hasGlowSurfaces && vk.dglowActive && !backEnd.isGlowPass && shader->hasGlow )
			backEnd.hasGlowSurfaces = qtrue;

		// if we're rendering glowing objects, but this shader has no stages with glow, skip it!
		if ( backEnd.isGlowPass && !shader->hasGlow )
		{
			shader = oldShader;
			entityNum = oldEntityNum;
			fogNum = oldFogNum;
			dlighted = oldDlighted;
			continue;
		}

		if ( vk.vboGhoul2Active && *drawSurf->surface == SF_MDX )
		{
			if ( ((CRenderableSurface*)drawSurf->surface)->boneCache != oldBoneCache )
			{
				/*
					this flush has to go through DoEndSurface for the same reason the shader
					boundary and the final flush do - it is where the per-weapon-model fullbright
					override is applied. A bone-cache change forces a mid-batch flush, and a bare
					RB_EndSurface here drew the batch uncolored, after which the boundary's own
					DoEndSurface found nothing left to draw. Whether a bone-cache change lands
					inside a weapon's batch depends on draw order, which is why the color came
					and went with camera position. Only reachable with r_vbo_models 1.
				*/
				// newjk: flush through DoEndSurface so the per-weapon fullbright override is applied
				DoEndSurface( tess.shader ? tess.shader->name : NULL,
					oldEntityNum >= 0 ? !(backEnd.refdef.entities[oldEntityNum].e.renderfx & RF_ISCORPSE) : true );
				RB_BeginSurface( shader, fogNum );
				oldBoneCache = ((CRenderableSurface*)drawSurf->surface)->boneCache;
				vk.cmd->bones_ubo_offset = RB_GetBoneUboOffset((CRenderableSurface*)drawSurf->surface);
			}
		}

		if (drawSurf->sort == oldSort && backEnd.refractionFill == shader->useDistortion ) {
			// fast path, same as previous sort
			rb_surfaceTable[*drawSurf->surface](drawSurf->surface);
			continue;
		}

		//oldSort = drawSurf->sort;

		//
		// change the tess parameters if needed
		// a "entityMergable" shader is a shader that can have surfaces from seperate
		// entities merged into a single batch, like smoke and blood puff sprites

		push_constant = qfalse;

		reType = (entityNum == REFENTITYNUM_WORLD) ? -1 : backEnd.refdef.entities[entityNum].e.reType;

		//if (((oldSort ^ drawSurf->sort) & ~QSORT_REFENTITYNUM_MASK) || !shader->entityMergable) {
		if ( shader != oldShader || fogNum != oldFogNum || dlighted != oldDlighted
			|| ( entityNum != oldEntityNum && ( !tess.entityMergable || reType != oldReType ) ) )
		{
			// newjk: flush through DoEndSurface so the per-weapon fullbright override is applied
			DoEndSurface( oldShader ? oldShader->name : NULL, oldShader ? !(backEnd.refdef.entities[oldEntityNum].e.renderfx & RF_ISCORPSE) : true );
#ifdef USE_PMLIGHT
#define INSERT_POINT SS_FOG
			if (backEnd.refdef.numLitSurfs && oldShaderSort < INSERT_POINT && shader->sort >= INSERT_POINT) {
				RB_LightingPass();

				oldEntityNum = -1; // force matrix setup
			}
			oldShaderSort = shader->sort;
#endif

#ifdef USE_VANILLA_SHADOWFINISH
			if (!didShadowPass && shader && shader->sort > SS_BANNER)
			{
				RB_ShadowFinish();
				didShadowPass = qtrue;
			}
#endif
			RB_BeginSurface(shader, fogNum);
			oldShader = shader;
			oldFogNum = fogNum;
			oldDlighted = dlighted;
			oldReType = reType;

			push_constant = qtrue;
		}

		oldSort = drawSurf->sort;

		//
		// change the modelview matrix if needed
		//
		if (entityNum != oldEntityNum)
		{
			depthRange = DEPTH_RANGE_NORMAL;

			if (entityNum != REFENTITYNUM_WORLD)
			{
				backEnd.currentEntity = &backEnd.refdef.entities[entityNum];
				backEnd.refdef.floatTime = originalTime - backEnd.currentEntity->e.shaderTime;

				// set up the transformation matrix
				R_RotateForEntity(backEnd.currentEntity, &backEnd.viewParms, &backEnd.ori );

				if ( backEnd.currentEntity->e.renderfx & RF_NODEPTH ) {
					// No depth at all, very rare but some things for seeing through walls
					depthRange = DEPTH_RANGE_ZERO;
				}

				if (backEnd.currentEntity->e.renderfx & RF_DEPTHHACK) {
					// hack the depth range to prevent view model from poking into walls
					depthRange = DEPTH_RANGE_WEAPON;
				}
			}
			else
			{
				backEnd.currentEntity = &tr.worldEntity;
				backEnd.refdef.floatTime = originalTime;
				backEnd.ori  = backEnd.viewParms.world;
			}

			// we have to reset the shaderTime as well otherwise image animations on
			// the world (like water) continue with the wrong frame
			tess.shaderTime = backEnd.refdef.floatTime - tess.shader->timeOffset;

			vk_set_depthrange( depthRange );

			if ( push_constant ) {
				Com_Memcpy(vk_world.modelview_transform, backEnd.ori.modelViewMatrix, 64);
				vk_update_mvp(NULL);
			}

			oldEntityNum = entityNum;
		}

		qboolean isDistortionShader = (qboolean)
			((shader->useDistortion == qtrue) || (backEnd.currentEntity && backEnd.currentEntity->e.renderfx & RF_DISTORTION));

		if ( backEnd.refractionFill != isDistortionShader ) {
			if ( vk.refractionActive && vk.renderPassIndex != RENDER_PASS_REFRACTION && !backEnd.hasRefractionSurfaces )
				backEnd.hasRefractionSurfaces = qtrue;

			// skip refracted surfaces in main pass, 
			// and non-refracted surfaces in refraction pass 
			continue;	
		}

		// add the triangles for this surface
		rb_surfaceTable[*drawSurf->surface](drawSurf->surface);
	}

	/*
		draw the contents of the last shader batch - through DoEndSurface like every other batch
		in the loop above, not bare RB_EndSurface. DoEndSurface is where the per-weapon-model
		fullbright override (cg_fullbrightWeapons) is applied, so flushing the final batch
		without it means whichever surface happens to sort last never gets its override color.
		Sort order shifts with view angle and distance, which is why a world weapon pickup
		flickered between its override color and its normal skin as the camera moved, and why a
		held weapon could end up wearing its owner's entity tint instead of its own override.
	*/
	if (oldShader != NULL) {
		// newjk: flush through DoEndSurface so the per-weapon fullbright override is applied
		DoEndSurface( oldShader->name, oldEntityNum >= 0 ? !(backEnd.refdef.entities[oldEntityNum].e.renderfx & RF_ISCORPSE) : true );
	}

	backEnd.refdef.floatTime = originalTime;

	// go back to the world modelview matrix
	Com_Memcpy(vk_world.modelview_transform, backEnd.viewParms.world.modelViewMatrix, 64);
	//vk_update_mvp();
	vk_set_depthrange(DEPTH_RANGE_NORMAL);

#ifdef USE_VANILLA_SHADOWFINISH
	if (!didShadowPass)
	{
		RB_ShadowFinish();
		didShadowPass = qtrue;
	}
#endif
}

#ifdef USE_PMLIGHT
/*
=================
RB_BeginDrawingLitView
=================
*/
static void RB_BeginDrawingLitSurfs( void )
{
	// we will need to change the projection matrix before drawing
	// 2D images again
	backEnd.projection2D = qfalse;

	// we will only draw a sun if there was sky rendered in this view
	backEnd.skyRenderedThisView = qfalse;

	// force depth range and viewport/scissor updates
	vk.cmd->depth_range = DEPTH_RANGE_COUNT;

	glState.faceCulling = -1;		// force face culling to set next time
}

/*
==================
RB_RenderLitSurfList
==================
*/
static void RB_RenderLitSurfList( dlight_t *dl ) {
	shader_t		*shader, *oldShader;
	int				fogNum;
	int				entityNum, oldEntityNum;
	Vk_Depth_Range	depthRange;
	const litSurf_t *litSurf;
	unsigned int	oldSort;
	double			originalTime; // -EC-

	// save original time for entity shader offsets
	originalTime = backEnd.refdef.floatTime;

	// draw everything
	oldEntityNum			= -1;
	backEnd.currentEntity	= &tr.worldEntity;
	oldShader				= NULL;
	oldSort					= MAX_UINT;
	depthRange				= DEPTH_RANGE_NORMAL;

	tess.dlightUpdateParams = qtrue;

	for (litSurf = dl->head; litSurf; litSurf = litSurf->next) {
		//if ( litSurf->sort == sort ) {
		if (litSurf->sort == oldSort) {
			// fast path, same as previous sort
			rb_surfaceTable[*litSurf->surface](litSurf->surface);
			continue;
		}

		R_DecomposeLitSort(litSurf->sort, &entityNum, &shader, &fogNum);

		if (vk.renderPassIndex == RENDER_PASS_SCREENMAP && entityNum != REFENTITYNUM_WORLD && backEnd.refdef.entities[entityNum].e.renderfx & RF_DEPTHHACK) {
			continue;
		}

		// anything BEFORE opaque is sky/portal, anything AFTER it should never have been added
		//assert( shader->sort == SS_OPAQUE );
		// !!! but MIRRORS can trip that assert, so just do this for now
		//if ( shader->sort < SS_OPAQUE )
		//	continue;

		//
		// change the tess parameters if needed
		// a "entityMergable" shader is a shader that can have surfaces from seperate
		// entities merged into a single batch, like smoke and blood puff sprites
		if (((oldSort ^ litSurf->sort) & ~QSORT_REFENTITYNUM_MASK) || !tess.entityMergable) {
			if (oldShader != NULL) {
				RB_EndSurface();
			}
			RB_BeginSurface(shader, fogNum);
			oldShader = shader;
		}

		oldSort = litSurf->sort;

		//
		// change the modelview matrix if needed
		//
		if (entityNum != oldEntityNum) {
			depthRange = DEPTH_RANGE_NORMAL;

			if (entityNum != REFENTITYNUM_WORLD) {
				backEnd.currentEntity = &backEnd.refdef.entities[entityNum];

				/*if (backEnd.currentEntity->intShaderTime)
					backEnd.refdef.floatTime = originalTime - (double)(backEnd.currentEntity->e.shaderTime.i) * 0.001;
				else*/
				backEnd.refdef.floatTime = originalTime - (double)backEnd.currentEntity->e.shaderTime;

				// set up the transformation matrix
				R_RotateForEntity(backEnd.currentEntity, &backEnd.viewParms, &backEnd.ori );

				if ( backEnd.currentEntity->e.renderfx & RF_NODEPTH ) {
					// No depth at all, very rare but some things for seeing through walls
					depthRange = DEPTH_RANGE_ZERO;
				}

				if (backEnd.currentEntity->e.renderfx & RF_DEPTHHACK) {
					// hack the depth range to prevent view model from poking into walls
					depthRange = DEPTH_RANGE_WEAPON;
				}
			}
			else {
				backEnd.currentEntity = &tr.worldEntity;
				backEnd.refdef.floatTime = originalTime;
				backEnd.ori = backEnd.viewParms.world;
			}

			// we have to reset the shaderTime as well otherwise image animations on
			// the world (like water) continue with the wrong frame
			tess.shaderTime = backEnd.refdef.floatTime - tess.shader->timeOffset;

			// set up the dynamic lighting
			R_TransformDlights(1, dl, &backEnd.ori );
			tess.dlightUpdateParams = qtrue;

			vk_set_depthrange( depthRange );

			Com_Memcpy(vk_world.modelview_transform, backEnd.ori.modelViewMatrix, 64);
			vk_update_mvp(NULL);

			oldEntityNum = entityNum;
		}

		// add the triangles for this surface
		rb_surfaceTable[*litSurf->surface](litSurf->surface);
	}

	// draw the contents of the last shader batch
	if (oldShader != NULL) {
		RB_EndSurface();
	}

	backEnd.refdef.floatTime = originalTime;

	// go back to the world modelview matrix
	Com_Memcpy(vk_world.modelview_transform, backEnd.viewParms.world.modelViewMatrix, 64);
	//vk_update_mvp();

	vk_set_depthrange(DEPTH_RANGE_NORMAL);
}
#endif // USE_PMLIGHT

/*
=============
RE_StretchRaw

FIXME: not exactly backend
Stretches a raw 32 bit power of 2 bitmap image over the given screen rectangle.
Used for cinematics.
=============
*/
void RE_StretchRaw ( int x, int y, int w, int h, int cols, int rows, const byte *data, int client, qboolean dirty )
{
	int			i, j;
	int			start, end;

	if (!tr.registered) {
		return;
	}

	start = 0;
	if (r_speeds->integer) {
		start = ri.Milliseconds() * ri.Cvar_VariableValue("timescale");
	}

	// make sure rows and cols are powers of 2
	for (i = 0; (1 << i) < cols; i++)
	{
		;
	}
	for (j = 0; (1 << j) < rows; j++)
	{
		;
	}

	if ((1 << i) != cols || (1 << j) != rows) {
		Com_Error(ERR_DROP, "Draw_StretchRaw: size not a power of 2: %i by %i", cols, rows);
	}

	RE_UploadCinematic( cols, rows, (byte*)data, client, dirty );

	if (r_speeds->integer) {
		end = ri.Milliseconds() * ri.Cvar_VariableValue("timescale");
		ri.Printf(PRINT_ALL, "RE_UploadCinematic( %i, %i ): %i msec\n", cols, rows, end - start);
	}

	tr.cinematicShader->stages[0]->bundle[0].image[0] = tr.scratchImage[client];
	RE_StretchPic(x, y, w, h, 0.5f / cols, 0.5f / rows, 1.0f - 0.5f / cols, 1.0f - 0.5 / rows, tr.cinematicShader->index);
}

/*
=============
RB_SetColor

=============
*/
const void	*RB_SetColor( const void *data ) {
	const setColorCommand_t	*cmd;

	cmd = (const setColorCommand_t *)data;

	backEnd.color2D[0] = cmd->color[0] * 255;
	backEnd.color2D[1] = cmd->color[1] * 255;
	backEnd.color2D[2] = cmd->color[2] * 255;
	backEnd.color2D[3] = cmd->color[3] * 255;

	return (const void *)(cmd + 1);
}

/*
=============
RB_StretchPic
=============
*/
const void *RB_StretchPic ( const void *data ) {
	const stretchPicCommand_t	*cmd;
	shader_t *shader;

	cmd = (const stretchPicCommand_t *)data;

	shader = cmd->shader;
	if ( shader != tess.shader ) {
		RB_EndSurface();
		backEnd.currentEntity = &backEnd.entity2D;
		vk_set_2d(); // set correct shader time before RB_BeginSurface() on 3D->2D transition
		RB_BeginSurface( shader, 0 );
	}

#ifdef USE_VBO
	VBO_UnBind();
#endif

	vk_set_2d();

	if ( vk.bloomActive ) {
		vk_bloom();
	}

	RB_AddQuadStamp2( cmd->x, cmd->y, cmd->w, cmd->h, cmd->s1, cmd->t1,
		cmd->s2, cmd->t2, backEnd.color2D );

	return (const void *)(cmd + 1);
}

/*
=============
RB_DrawRotatePic
=============
*/
const void *RB_RotatePic ( const void *data )
{
	const rotatePicCommand_t	*cmd;
	image_t *image;
	shader_t *shader;

	cmd = (const rotatePicCommand_t *)data;

	shader = cmd->shader;
	image = shader->stages[0]->bundle[0].image[0];

	if ( image ) {
		shader = cmd->shader;
		if ( shader != tess.shader ) {
			RB_EndSurface();
			backEnd.currentEntity = &backEnd.entity2D;
			vk_set_2d(); // set correct shader time before RB_BeginSurface() on 3D->2D transition
			RB_BeginSurface( shader, 0 );
		}

		vk_set_2d();

		RB_CHECKOVERFLOW( 4, 6 );
		int numVerts = tess.numVertexes;
		int numIndexes = tess.numIndexes;

		float angle = DEG2RAD( cmd-> a );
		float s = sinf( angle );
		float c = cosf( angle );

		matrix3_t m = {
			{ c, s, 0.0f },
			{ -s, c, 0.0f },
			{ cmd->x + cmd->w, cmd->y, 1.0f }
		};

		tess.numVertexes += 4;
		tess.numIndexes += 6;

		tess.indexes[ numIndexes + 0 ] = numVerts + 3;
		tess.indexes[ numIndexes + 1 ] = numVerts + 0;
		tess.indexes[ numIndexes + 2 ] = numVerts + 2;
		tess.indexes[ numIndexes + 3 ] = numVerts + 2;
		tess.indexes[ numIndexes + 4 ] = numVerts + 0;
		tess.indexes[ numIndexes + 5 ] = numVerts + 1;

		byteAlias_t *baDest = NULL, *baSource = (byteAlias_t *)&backEnd.color2D;
		baDest = (byteAlias_t *)&tess.vertexColors[numVerts + 0]; baDest->ui = baSource->ui;
		baDest = (byteAlias_t *)&tess.vertexColors[numVerts + 1]; baDest->ui = baSource->ui;
		baDest = (byteAlias_t *)&tess.vertexColors[numVerts + 2]; baDest->ui = baSource->ui;
		baDest = (byteAlias_t *)&tess.vertexColors[numVerts + 3]; baDest->ui = baSource->ui;

		tess.xyz[ numVerts + 0 ][0] = m[0][0] * (-cmd->w) + m[2][0];
		tess.xyz[ numVerts + 0 ][1] = m[0][1] * (-cmd->w) + m[2][1];
		tess.xyz[ numVerts + 0 ][2] = 0;

		tess.xyz[ numVerts + 1 ][0] = m[2][0];
		tess.xyz[ numVerts + 1 ][1] = m[2][1];
		tess.xyz[ numVerts + 1 ][2] = 0;

		tess.xyz[ numVerts + 2 ][0] = m[1][0] * (cmd->h) + m[2][0];
		tess.xyz[ numVerts + 2 ][1] = m[1][1] * (cmd->h) + m[2][1];
		tess.xyz[ numVerts + 2 ][2] = 0;

		tess.xyz[ numVerts + 3 ][0] = m[0][0] * (-cmd->w) + m[1][0] * (cmd->h) + m[2][0];
		tess.xyz[ numVerts + 3 ][1] = m[0][1] * (-cmd->w) + m[1][1] * (cmd->h) + m[2][1];
		tess.xyz[ numVerts + 3 ][2] = 0;

		tess.texCoords[0][ numVerts + 0 ][0] = cmd->s1;
		tess.texCoords[0][ numVerts + 0 ][1] = cmd->t1;
		tess.texCoords[0][ numVerts + 1 ][0] = cmd->s2;
		tess.texCoords[0][ numVerts + 1 ][1] = cmd->t1;
		tess.texCoords[0][ numVerts + 2 ][0] = cmd->s2;
		tess.texCoords[0][ numVerts + 2 ][1] = cmd->t2;
		tess.texCoords[0][ numVerts + 3 ][0] = cmd->s1;
		tess.texCoords[0][ numVerts + 3 ][1] = cmd->t2;

		return (const void *)(cmd + 1);

	}

	return (const void *)(cmd + 1);
}

/*
=============
RB_DrawRotatePic2
=============
*/
const void *RB_RotatePic2 ( const void *data )
{
	const rotatePicCommand_t	*cmd;
	image_t *image;
	shader_t *shader;

	cmd = (const rotatePicCommand_t *)data;

	shader = cmd->shader;

	if ( shader->numUnfoggedPasses )
	{
		image = shader->stages[0]->bundle[0].image[0];

		if ( image )
		{
			shader = cmd->shader;
			if ( shader != tess.shader ) {
				RB_EndSurface();
				backEnd.currentEntity = &backEnd.entity2D;
				vk_set_2d(); // set correct shader time before RB_BeginSurface() on 3D->2D transition
				RB_BeginSurface( shader, 0 );
			}

			vk_set_2d();

			RB_CHECKOVERFLOW( 4, 6 );
			int numVerts = tess.numVertexes;
			int numIndexes = tess.numIndexes;

			float angle = DEG2RAD( cmd-> a );
			float s = sinf( angle );
			float c = cosf( angle );

			matrix3_t m = {
				{ c, s, 0.0f },
				{ -s, c, 0.0f },
				{ cmd->x, cmd->y, 1.0f }
			};

			tess.numVertexes += 4;
			tess.numIndexes += 6;

			tess.indexes[ numIndexes + 0 ] = numVerts + 3;
			tess.indexes[ numIndexes + 1 ] = numVerts + 0;
			tess.indexes[ numIndexes + 2 ] = numVerts + 2;
			tess.indexes[ numIndexes + 3 ] = numVerts + 2;
			tess.indexes[ numIndexes + 4 ] = numVerts + 0;
			tess.indexes[ numIndexes + 5 ] = numVerts + 1;

			byteAlias_t *baDest = NULL, *baSource = (byteAlias_t *)&backEnd.color2D;
			baDest = (byteAlias_t *)&tess.vertexColors[numVerts + 0]; baDest->ui = baSource->ui;
			baDest = (byteAlias_t *)&tess.vertexColors[numVerts + 1]; baDest->ui = baSource->ui;
			baDest = (byteAlias_t *)&tess.vertexColors[numVerts + 2]; baDest->ui = baSource->ui;
			baDest = (byteAlias_t *)&tess.vertexColors[numVerts + 3]; baDest->ui = baSource->ui;

			tess.xyz[ numVerts + 0 ][0] = m[0][0] * (-cmd->w * 0.5f) + m[1][0] * (-cmd->h * 0.5f) + m[2][0];
			tess.xyz[ numVerts + 0 ][1] = m[0][1] * (-cmd->w * 0.5f) + m[1][1] * (-cmd->h * 0.5f) + m[2][1];
			tess.xyz[ numVerts + 0 ][2] = 0;

			tess.xyz[ numVerts + 1 ][0] = m[0][0] * (cmd->w * 0.5f) + m[1][0] * (-cmd->h * 0.5f) + m[2][0];
			tess.xyz[ numVerts + 1 ][1] = m[0][1] * (cmd->w * 0.5f) + m[1][1] * (-cmd->h * 0.5f) + m[2][1];
			tess.xyz[ numVerts + 1 ][2] = 0;

			tess.xyz[ numVerts + 2 ][0] = m[0][0] * (cmd->w * 0.5f) + m[1][0] * (cmd->h * 0.5f) + m[2][0];
			tess.xyz[ numVerts + 2 ][1] = m[0][1] * (cmd->w * 0.5f) + m[1][1] * (cmd->h * 0.5f) + m[2][1];
			tess.xyz[ numVerts + 2 ][2] = 0;

			tess.xyz[ numVerts + 3 ][0] = m[0][0] * (-cmd->w * 0.5f) + m[1][0] * (cmd->h * 0.5f) + m[2][0];
			tess.xyz[ numVerts + 3 ][1] = m[0][1] * (-cmd->w * 0.5f) + m[1][1] * (cmd->h * 0.5f) + m[2][1];
			tess.xyz[ numVerts + 3 ][2] = 0;

			tess.texCoords[0][ numVerts + 0 ][0] = cmd->s1;
			tess.texCoords[0][ numVerts + 0 ][1] = cmd->t1;
			tess.texCoords[0][ numVerts + 1 ][0] = cmd->s2;
			tess.texCoords[0][ numVerts + 1 ][1] = cmd->t1;
			tess.texCoords[0][ numVerts + 2 ][0] = cmd->s2;
			tess.texCoords[0][ numVerts + 2 ][1] = cmd->t2;
			tess.texCoords[0][ numVerts + 3 ][0] = cmd->s1;
			tess.texCoords[0][ numVerts + 3 ][1] = cmd->t2;

			return (const void *)(cmd + 1);
		}
	}

	return (const void *)(cmd + 1);
}

#ifdef USE_PMLIGHT
static void RB_LightingPass( void )
{
	dlight_t* dl;
	int	i;

#ifdef USE_VBO
	//VBO_Flush();
	//tess.allowVBO = qfalse; // for now
#endif

	tess.dlightPass = qtrue;

	for (i = 0; i < backEnd.viewParms.num_dlights; i++)
	{
		dl = &backEnd.viewParms.dlights[i];
		if (dl->head)
		{
			tess.light = dl;
			RB_RenderLitSurfList(dl);
		}
	}

	tess.dlightPass = qfalse;

	backEnd.viewParms.num_dlights = 0;
}
#endif

static void vk_update_camera_constants( const trRefdef_t *refdef, const viewParms_t *viewParms ) 
{
	// set
	vkUniformCamera_t uniform = {};

	Com_Memcpy( uniform.viewOrigin, refdef->vieworg, sizeof( vec3_t) );
	uniform.viewOrigin[3] = refdef->floatTime;

	/*
	const float* p = viewParms->projectionMatrix;
	float proj[16];
	Com_Memcpy(proj, p, 64);

	proj[5] = -p[5];
	//myGlMultMatrix(vk_world.modelview_transform, proj, uniform.mvp);
	myGlMultMatrix(viewParms->world.modelViewMatrix, proj, uniform.mvp);
	*/

	vk.cmd->camera_ubo_offset = vk_append_uniform( &uniform, sizeof(uniform), vk.uniform_camera_item_size );
}

static void vk_update_entity_light_constants( vkUniformEntity_t &uniform, const trRefEntity_t *refEntity ) 
{
	static const float normalizeFactor = 1.0f / 255.0f;

	VectorScale(refEntity->ambientLight, normalizeFactor, uniform.ambientLight);
	VectorScale(refEntity->directedLight, normalizeFactor, uniform.directedLight);
	VectorCopy(refEntity->lightDir, uniform.lightOrigin);

	uniform.lightOrigin[3] = 0.0f;
}

static void vk_update_entity_matrix_constants( vkUniformEntity_t &uniform, const trRefEntity_t *refEntity ) 
{
	orientationr_t ori;

	// backend ref cant be right
	/*if ( refEntity == &tr.worldEntity ) {
		ori = backEnd.viewParms.world;
		Matrix16Identity( uniform.modelMatrix );
	}else{
		R_RotateForEntity( refEntity, &backEnd.viewParms, &ori );
		Matrix16Copy( ori.modelMatrix, uniform.modelMatrix );
	}*/

	R_RotateForEntity(refEntity, &backEnd.viewParms, &ori);
	Matrix16Copy(ori.modelMatrix, uniform.modelMatrix);
	VectorCopy(ori.viewOrigin, uniform.localViewOrigin);

	Com_Memcpy( &uniform.localViewOrigin, ori.viewOrigin, sizeof( vec3_t) );
	uniform.localViewOrigin[3] = 0.0f;
}

static void vk_update_entity_constants( const trRefdef_t *refdef ) {
	uint32_t i;
	Com_Memset( vk.cmd->entity_ubo_offset, 0, sizeof(vk.cmd->entity_ubo_offset) );

	for ( i = 0; i < refdef->num_entities; i++ ) {
		trRefEntity_t *ent = &refdef->entities[i];

		R_SetupEntityLighting( refdef, ent );

		vkUniformEntity_t uniform = {};
		vk_update_entity_light_constants( uniform, ent );
		vk_update_entity_matrix_constants( uniform, ent );

		vk.cmd->entity_ubo_offset[i] = vk_append_uniform( &uniform, sizeof(uniform), vk.uniform_entity_item_size );
	}

	const trRefEntity_t *ent = &tr.worldEntity;
	vkUniformEntity_t uniform = {};
	vk_update_entity_light_constants( uniform, ent );
	vk_update_entity_matrix_constants( uniform, ent );

	vk.cmd->entity_ubo_offset[REFENTITYNUM_WORLD] = vk_append_uniform( &uniform, sizeof(uniform), vk.uniform_entity_item_size );
}

static void vk_update_ghoul2_constants( const trRefdef_t *refdef ) {
	uint32_t i;

	if ( !vk.vboGhoul2Active )
		return;

	for ( i = 0; i < refdef->num_entities; i++ )
	{
		const trRefEntity_t *ent = &refdef->entities[i];
		if (ent->e.reType != RT_MODEL)
			continue;

		model_t *model = R_GetModelByHandle(ent->e.hModel);
		if (!model)
			continue;

		switch (model->type)
		{
		case MOD_MDXM:
		case MOD_BAD:
		{
			// Transform Bones and upload them
			RB_TransformBones( ent, refdef );
		}
		break;

		default:
			break;
		}
	}

}

static void vk_update_fog_constants(const trRefdef_t* refdef)
{
	uint32_t i;
	size_t size;
	vkUniformFog_t uniform = {};

	uniform.num_fogs = tr.world ? ( tr.world->numfogs - 1 ) : 0;

	size = sizeof(vec4_t);

	for ( i = 0; i < MIN(uniform.num_fogs, 16); ++i )
	{
		const fog_t *fog = tr.world->fogs + i + 1;
		vkUniformFogEntry_t *fogData = uniform.fogs + i;

		VectorCopy4( fog->surface, fogData->plane );
		VectorCopy4( fog->color, fogData->color );
		fogData->depthToOpaque = sqrtf(-logf(1.0f / 255.0f)) / fog->parms.depthForOpaque;
		fogData->hasPlane = fog->hasSurface;
	}

	size += (i * sizeof(vkUniformFogEntry_t));

	vk.cmd->fogs_ubo_offset = vk_append_uniform( &uniform, size, vk.uniform_fogs_item_size );
}

static void RB_UpdateUniformConstants( const trRefdef_t *refdef, const viewParms_t *viewParms ) 
{
	vk_update_camera_constants( refdef, viewParms );

	if ( vk.vboGhoul2Active ) 
	{
		vk_update_entity_constants( refdef );
		vk_update_ghoul2_constants( refdef );
	}

	vk_update_fog_constants( refdef );
}

/*
=============
RB_DrawSurfs

=============
*/
const void	*RB_DrawSurfs( const void *data ) {
	const drawSurfsCommand_t	*cmd;

	RB_EndSurface(); // finish any 2D drawing if needed

	cmd = (const drawSurfsCommand_t *)data;

	backEnd.refdef = cmd->refdef;
	backEnd.viewParms = cmd->viewParms;

	backEnd.hasGlowSurfaces = qfalse;
	backEnd.isGlowPass = qfalse;

	backEnd.hasRefractionSurfaces = qfalse;

#ifdef USE_VBO
	VBO_UnBind();
#endif

	RB_UpdateUniformConstants( &backEnd.refdef, &backEnd.viewParms );

	// clear the z buffer, set the modelview, etc
	RB_BeginDrawingView();

	RB_RenderDrawSurfList( cmd->drawSurfs, cmd->numDrawSurfs );

#ifdef USE_VBO
	VBO_UnBind();
#endif

	if ( r_drawSun->integer ) {
		RB_DrawSun( 0.1f, tr.sunShader );
	}

#ifndef USE_VANILLA_SHADOWFINISH
	RB_ShadowFinish();
#endif

	RB_RenderFlares();

#ifdef USE_PMLIGHT
	if ( backEnd.refdef.numLitSurfs ) {
		RB_BeginDrawingLitSurfs();
		RB_LightingPass();
	}
#endif

	// draw main system development information (surface outlines, etc)
	R_DebugGraphics();

	if ( cmd->refdef.switchRenderPass ) {
		vk_end_render_pass();
		vk_begin_main_render_pass();
		backEnd.screenMapDone = qtrue;
	}

	// refraction / distortion pass
	if ( backEnd.hasRefractionSurfaces ) {
		vk_end_render_pass();
	
		// extract/copy offscreen color attachment to make it usable as input
		vk_refraction_extract();

		backEnd.refractionFill = qtrue;	
		vk_begin_post_refraction_extract_render_pass();

		RB_RenderDrawSurfList( cmd->drawSurfs, cmd->numDrawSurfs );
		backEnd.refractionFill = qfalse;
	}

	// checked in previous RB_RenderDrawSurfList() if there is at least one glowing surface
	if ( vk.dglowActive && !( backEnd.refdef.rdflags & RDF_NOWORLDMODEL ) && backEnd.hasGlowSurfaces )
	{
		vk_end_render_pass();

		backEnd.isGlowPass = qtrue;
		vk_begin_dglow_extract_render_pass();

		RB_RenderDrawSurfList( cmd->drawSurfs, cmd->numDrawSurfs );

		vk_begin_dglow_blur();
		backEnd.isGlowPass = qfalse;
	}

	//TODO Maybe check for rdf_noworld stuff but q3mme has full 3d ui
	backEnd.doneSurfaces = qtrue; // for bloom

	return (const void*)(cmd + 1);
}

/*
=============
RB_DrawBuffer

=============
*/
const void	*RB_DrawBuffer( const void *data ) {
	const drawBufferCommand_t	*cmd;

	cmd = (const drawBufferCommand_t *)data;

	vk_begin_frame();

	vk_set_depthrange(DEPTH_RANGE_NORMAL);

	// force depth range and viewport/scissor updates
	vk.cmd->depth_range = DEPTH_RANGE_COUNT;

	if ( r_clear->integer && vk.clearAttachment ) {
		const vec4_t color = { 1, 0, 0.5, 1 };

		backEnd.projection2D = qtrue; // to ensure we have viewport that occupies entire window
		vk_clear_color_attachments( color );
		backEnd.projection2D = qfalse;
	}
	return (const void *)(cmd + 1);
}

/*
=============
RB_SwapBuffers

=============
*/
const void	*RB_SwapBuffers( const void *data ) {
	const swapBuffersCommand_t	*cmd;

	// finish any 2D drawing if needed
	RB_EndSurface();

	ResetGhoul2RenderableSurfaceHeap();

	// texture swapping test
	if ( r_showImages->integer ) {
		RB_ShowImages(tr.images.items, tr.images.count);
	}

	cmd = (const swapBuffersCommand_t *)data;

	tr.needScreenMap = 0;

	vk_end_frame();

	if ( backEnd.doneSurfaces && !glState.finishCalled ) {
		vk_queue_wait_idle();
	}

	if (backEnd.screenshotMask && vk.cmd->waitForFence) {
		if (backEnd.screenshotMask & SCREENSHOT_TGA && backEnd.screenshotTGA[0]) {
			R_TakeScreenshot(0, 0, gls.captureWidth, gls.captureHeight, backEnd.screenshotTGA);
			if (!backEnd.screenShotTGAsilent) {
				// newjk: [skipnotify] keeps this out of the notify area, and so out of the next shot
				ri.Printf(PRINT_ALL, "[skipnotify]Wrote %s\n", backEnd.screenshotTGA);
			}
		}
		if (backEnd.screenshotMask & SCREENSHOT_JPG && backEnd.screenshotJPG[0]) {
			R_TakeScreenshotJPEG(0, 0, gls.captureWidth, gls.captureHeight, backEnd.screenshotJPG);
			if (!backEnd.screenShotJPGsilent) {
				// newjk: [skipnotify] keeps this out of the notify area, and so out of the next shot
				ri.Printf(PRINT_ALL, "[skipnotify]Wrote %s\n", backEnd.screenshotJPG);
			}
		}
		if (backEnd.screenshotMask & SCREENSHOT_PNG && backEnd.screenshotPNG[0]) {
			R_TakeScreenshotPNG(0, 0, gls.captureWidth, gls.captureHeight, backEnd.screenshotPNG);
			if (!backEnd.screenShotPNGsilent) {
				// newjk: [skipnotify] keeps this out of the notify area, and so out of the next shot
				ri.Printf(PRINT_ALL, "[skipnotify]Wrote %s\n", backEnd.screenshotPNG);
			}
		}
		if (backEnd.screenshotMask & SCREENSHOT_AVI) {
			RB_TakeVideoFrameCmd(&backEnd.vcmd);
		}

		backEnd.screenshotJPG[0] = '\0';
		backEnd.screenshotTGA[0] = '\0';
		backEnd.screenshotPNG[0] = '\0';
		backEnd.screenshotMask = 0;
	}

	vk_present_frame();

	backEnd.projection2D = qfalse;
	backEnd.doneSurfaces = qfalse;
	backEnd.doneBloom = qfalse;
	//backEnd.drawConsole = qfalse;

	return (const void *)(cmd + 1);
}

const void	*RB_WorldEffects( const void *data )
{
	const drawBufferCommand_t	*cmd;

	cmd = (const drawBufferCommand_t *)data;

	// Always flush the tess buffer
	if ( tess.shader && tess.numIndexes )
		RB_EndSurface();

	RB_RenderWorldEffects();

	if ( tess.shader )
		RB_BeginSurface( tess.shader, tess.fogNum );

	return (const void *)(cmd + 1);
}

/*
=============
RB_ClearColor
=============
*/
static const void *RB_ClearColor( const void *data )
{
	const clearColorCommand_t* cmd = (const clearColorCommand_t*)data;

	backEnd.projection2D = qtrue;

	if ( r_fastsky->integer )
		vk_clear_color_attachments( (float*)tr.clearColor );
	else
		vk_clear_color_attachments( (float*)tr.world->fogs[tr.world->globalFog].color );

	backEnd.projection2D = qfalse;

	return (const void*)(cmd + 1);
}

/*
====================
RB_ExecuteRenderCommands
====================
*/
extern const void *R_DrawWireframeAutomap( const void *data ); //tr_world.cpp
void RB_ExecuteRenderCommands( const void *data ) {
	int		t1, t2;

	// Once the device is lost there is no command buffer to record into - vk_begin_frame bails out
	// before beginning one - so recording anything here would be issuing commands against a buffer
	// that was never started. Drop the whole command list; the client is about to end the session.
	if ( vk.deviceLost )
		return;

	t1 = ri.Milliseconds()*ri.Cvar_VariableValue( "timescale" );

	while ( 1 ) {
		data = PADP(data, sizeof(void *));

		switch ( *(const int *)data ) {
		case RC_SET_COLOR:
			data = RB_SetColor( data );
			break;
		case RC_STRETCH_PIC:
			data = RB_StretchPic( data );
			break;
		case RC_ROTATE_PIC:
			data = RB_RotatePic( data );
			break;
		case RC_ROTATE_PIC2:
			data = RB_RotatePic2( data );
			break;
		case RC_DRAW_SURFS:
			data = RB_DrawSurfs( data );
			break;
		case RC_DRAW_BUFFER:
			data = RB_DrawBuffer( data );
			break;
		case RC_SWAP_BUFFERS:
			data = RB_SwapBuffers( data );
			break;
		case RC_VIDEOFRAME:
			data = RB_TakeVideoFrameCmd( data );
			break;
		case RC_WORLD_EFFECTS:
			data = RB_WorldEffects( data );
			break;
		case RC_AUTO_MAP:
			data = R_DrawWireframeAutomap(data);
			break;
		case RC_CLEARCOLOR:
			data = RB_ClearColor(data);
			break;
		case RC_END_OF_LIST:
		default:
			// stop rendering
			vk_end_frame();
			t2 = ri.Milliseconds()*ri.Cvar_VariableValue( "timescale" );
			backEnd.pc.msec = t2 - t1;
			return;
		}
	}

}
