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

// tr_main.c -- main control flow for each frame

#include "tr_local.h"
#include "ghoul2/g2_local.h"

trGlobals_t		tr;

static const float s_flipMatrix[16] QALIGN(16) = {
	// convert from our coordinate system (looking down X)
	// to OpenGL's coordinate system (looking down -Z)
	0, 0, -1, 0,
	-1, 0, 0, 0,
	0, 1, 0, 0,
	0, 0, 0, 1
};

refimport_t	ri;

// entities that will have procedurally generated surfaces will just
// point at this for their sorting surface
static surfaceType_t entitySurface = SF_ENTITY;

/*
=================
R_CullLocalBox

Returns CULL_IN, CULL_CLIP, or CULL_OUT
=================
*/
int R_CullLocalBox( const vec3_t bounds[2] ) {
	int			i, j;
	vec3_t		transformed[8];
	float		dists[8];
	vec3_t		v;
	cplane_t	*frust;
	int			anyBack;
	int			front, back;

	if (r_nocull->integer == 1) {
		return CULL_CLIP;
	}

	// transform into world space
	for (i = 0; i < 8; i++) {
		v[0] = bounds[i & 1][0];
		v[1] = bounds[(i >> 1) & 1][1];
		v[2] = bounds[(i >> 2) & 1][2];

		VectorCopy(tr.ori.origin, transformed[i]);
		VectorMA(transformed[i], v[0], tr.ori.axis[0], transformed[i]);
		VectorMA(transformed[i], v[1], tr.ori.axis[1], transformed[i]);
		VectorMA(transformed[i], v[2], tr.ori.axis[2], transformed[i]);
	}

	// check against frustum planes
	anyBack = 0;
	for (i = 0; i < 4; i++) {
		frust = &tr.viewParms.frustum[i];

		front = back = 0;
		for (j = 0; j < 8; j++) {
			dists[j] = DotProduct(transformed[j], frust->normal);
			if (dists[j] > frust->dist) {
				front = 1;
				if (back) {
					break;		// a point is in front
				}
			}
			else {
				back = 1;
			}
		}
		if (!front) {
			// all points were behind one of the planes
			return CULL_OUT;
		}
		anyBack |= back;
	}

	if (!anyBack) {
		return CULL_IN;		// completely inside frustum
	}

	return CULL_CLIP;		// partially clipped
}

/*
** R_CullLocalPointAndRadius
*/
int R_CullLocalPointAndRadius( const vec3_t pt, float radius )
{
	vec3_t transformed;

	R_LocalPointToWorld(pt, transformed);

	return R_CullPointAndRadius(transformed, radius);
}

/*
** R_CullPointAndRadius
*/
int R_CullPointAndRadius( const vec3_t pt, float radius )
{
	int				i;
	float			dist;
	const cplane_t	*frust;
	qboolean		mightBeClipped = qfalse;

	if (r_nocull->integer == 1) {
		return CULL_CLIP;
	}

	// check against frustum planes
	for (i = 0; i < 4; i++)
	{
		frust = &tr.viewParms.frustum[i];

		dist = DotProduct(pt, frust->normal) - frust->dist;
		if (dist < -radius)
		{
			return CULL_OUT;
		}
		else if (dist <= radius)
		{
			mightBeClipped = qtrue;
		}
	}

	if (mightBeClipped)
	{
		return CULL_CLIP;
	}

	return CULL_IN;		// completely inside frustum
}

/*
** R_CullDlight
*/
int R_CullDlight( const dlight_t *dl)
{
	int			i;
	float		dist, dist2;
	cplane_t	*frust;
	qboolean	mightBeClipped = qfalse;

	if (r_nocull->integer)
		return CULL_CLIP;

	if (dl->linear) {
		for (i = 0; i < 4; i++) {
			frust = &tr.viewParms.frustum[i];
			dist = DotProduct(dl->transformed, frust->normal) - frust->dist;
			dist2 = DotProduct(dl->transformed2, frust->normal) - frust->dist;
			if (dist < -dl->radius && dist2 < -dl->radius)
				return CULL_OUT;
			else if (dist <= dl->radius || dist2 <= dl->radius)
				mightBeClipped = qtrue;
		}
	}
	else
		// check against frustum planes
		for (i = 0; i < 4; i++) {
			frust = &tr.viewParms.frustum[i];
			dist = DotProduct(dl->transformed, frust->normal) - frust->dist;
			if (dist < -dl->radius)
				return CULL_OUT;
			else if (dist <= dl->radius)
				mightBeClipped = qtrue;
		}

	if (mightBeClipped)
		return CULL_CLIP;

	return CULL_IN;	// completely inside frustum
}

/*
=================
R_LocalNormalToWorld

=================
*/
static void R_LocalNormalToWorld( const vec3_t local, vec3_t world ) {
	world[0] = local[0] * tr.ori.axis[0][0] + local[1] * tr.ori.axis[1][0] + local[2] * tr.ori.axis[2][0];
	world[1] = local[0] * tr.ori.axis[0][1] + local[1] * tr.ori.axis[1][1] + local[2] * tr.ori.axis[2][1];
	world[2] = local[0] * tr.ori.axis[0][2] + local[1] * tr.ori.axis[1][2] + local[2] * tr.ori.axis[2][2];
}

/*
=================
R_LocalPointToWorld

=================
*/
void R_LocalPointToWorld( const vec3_t local, vec3_t world ) {
	world[0] = local[0] * tr.ori.axis[0][0] + local[1] * tr.ori.axis[1][0] + local[2] * tr.ori.axis[2][0] + tr.ori.origin[0];
	world[1] = local[0] * tr.ori.axis[0][1] + local[1] * tr.ori.axis[1][1] + local[2] * tr.ori.axis[2][1] + tr.ori.origin[1];
	world[2] = local[0] * tr.ori.axis[0][2] + local[1] * tr.ori.axis[1][2] + local[2] * tr.ori.axis[2][2] + tr.ori.origin[2];
}

float preTransEntMatrix[16];

/*
=================
R_WorldNormalToEntity

=================
*/
void R_WorldNormalToEntity( const vec3_t worldvec, vec3_t entvec )
{
	entvec[0] = -worldvec[0] * preTransEntMatrix[0] - worldvec[1] * preTransEntMatrix[4] + worldvec[2] * preTransEntMatrix[8];
	entvec[1] = -worldvec[0] * preTransEntMatrix[1] - worldvec[1] * preTransEntMatrix[5] + worldvec[2] * preTransEntMatrix[9];
	entvec[2] = -worldvec[0] * preTransEntMatrix[2] - worldvec[1] * preTransEntMatrix[6] + worldvec[2] * preTransEntMatrix[10];
}

/*
=================
R_WorldPointToEntity

=================
*/
/*void R_WorldPointToEntity (vec3_t worldvec, vec3_t entvec)
{
	entvec[0] = worldvec[0] * preTransEntMatrix[0] + worldvec[1] * preTransEntMatrix[4] + worldvec[2] * preTransEntMatrix[8]+preTransEntMatrix[12];
	entvec[1] = worldvec[0] * preTransEntMatrix[1] + worldvec[1] * preTransEntMatrix[5] + worldvec[2] * preTransEntMatrix[9]+preTransEntMatrix[13];
	entvec[2] = worldvec[0] * preTransEntMatrix[2] + worldvec[1] * preTransEntMatrix[6] + worldvec[2] * preTransEntMatrix[10]+preTransEntMatrix[14];
}
*/

/*
=================
R_WorldToLocal

=================
*/
void R_WorldToLocal( vec3_t world, vec3_t local ) {
	local[0] = DotProduct(world, tr.ori.axis[0]);
	local[1] = DotProduct(world, tr.ori.axis[1]);
	local[2] = DotProduct(world, tr.ori.axis[2]);
}

/*
==========================
R_TransformModelToClip

==========================
*/
void R_TransformModelToClip( const vec3_t src, const float *modelMatrix, const float *projectionMatrix,
	vec4_t eye, vec4_t dst ) {
	int i;

	for (i = 0; i < 4; i++) {
		eye[i] =
			src[0] * modelMatrix[i + 0 * 4] +
			src[1] * modelMatrix[i + 1 * 4] +
			src[2] * modelMatrix[i + 2 * 4] +
			1 * modelMatrix[i + 3 * 4];
	}

	for (i = 0; i < 4; i++) {
		dst[i] =
			eye[0] * projectionMatrix[i + 0 * 4] +
			eye[1] * projectionMatrix[i + 1 * 4] +
			eye[2] * projectionMatrix[i + 2 * 4] +
			eye[3] * projectionMatrix[i + 3 * 4];
	}
}

/*
==========================
R_TransformModelToClipMVP
==========================
*/
static void R_TransformModelToClipMVP( const vec3_t src, const float *mvp, vec4_t clip ) {
	int i;

	for (i = 0; i < 4; i++) {
		clip[i] =
			src[0] * mvp[i + 0 * 4] +
			src[1] * mvp[i + 1 * 4] +
			src[2] * mvp[i + 2 * 4] +
			1 * mvp[i + 3 * 4];
	}
}

/*
==========================
R_TransformClipToWindow

==========================
*/
void R_TransformClipToWindow( const vec4_t clip, const viewParms_t *view, vec4_t normalized, vec4_t window ) {
	normalized[0] = clip[0] / clip[3];
	normalized[1] = clip[1] / clip[3];
	normalized[2] = (clip[2] + clip[3]) / (2 * clip[3]);

	window[0] = 0.5f * (1.0f + normalized[0]) * view->viewportWidth;
	window[1] = 0.5f * (1.0f + normalized[1]) * view->viewportHeight;
	window[2] = normalized[2];

	window[0] = (int)(window[0] + 0.5);
	window[1] = (int)(window[1] + 0.5);
}

/*
==========================
myGlMultMatrix

==========================
*/
void myGlMultMatrix( const float *a, const float *b, float *out ) {
	int		i, j;

	for (i = 0; i < 4; i++) {
		for (j = 0; j < 4; j++) {
			out[i * 4 + j] =
				a[i * 4 + 0] * b[0 * 4 + j]
				+ a[i * 4 + 1] * b[1 * 4 + j]
				+ a[i * 4 + 2] * b[2 * 4 + j]
				+ a[i * 4 + 3] * b[3 * 4 + j];
		}
	}
}

/*
=================
R_RotateForEntity

Generates an orientation for an entity and viewParms
Does NOT produce any GL calls
Called by both the front end and the back end
=================
*/
void R_RotateForEntity( const trRefEntity_t *ent, const viewParms_t *viewParms,
	orientationr_t *ori ) 
{
	vec3_t	delta;
	float	axisLength;

	if (ent->e.reType != RT_MODEL) {
		*ori = viewParms->world;
		return;
	}

	VectorCopy(ent->e.origin, ori->origin);

	VectorCopy(ent->e.axis[0], ori->axis[0]);
	VectorCopy(ent->e.axis[1], ori->axis[1]);
	VectorCopy(ent->e.axis[2], ori->axis[2]);

	preTransEntMatrix[0] = ori->axis[0][0];
	preTransEntMatrix[4] = ori->axis[1][0];
	preTransEntMatrix[8] = ori->axis[2][0];
	preTransEntMatrix[12] = ori->origin[0];

	preTransEntMatrix[1] = ori->axis[0][1];
	preTransEntMatrix[5] = ori->axis[1][1];
	preTransEntMatrix[9] = ori->axis[2][1];
	preTransEntMatrix[13] = ori->origin[1];

	preTransEntMatrix[2] = ori->axis[0][2];
	preTransEntMatrix[6] = ori->axis[1][2];
	preTransEntMatrix[10] = ori->axis[2][2];
	preTransEntMatrix[14] = ori->origin[2];

	preTransEntMatrix[3] = 0;
	preTransEntMatrix[7] = 0;
	preTransEntMatrix[11] = 0;
	preTransEntMatrix[15] = 1;

	myGlMultMatrix(preTransEntMatrix, viewParms->world.modelMatrix, ori->modelMatrix);

	// calculate the viewer origin in the model's space
	// needed for fog, specular, and environment mapping
	VectorSubtract(viewParms->ori.origin, ori->origin, delta);

	// compensate for scale in the axes if necessary
	if (ent->e.nonNormalizedAxes) {
		axisLength = VectorLength(ent->e.axis[0]);
		if (!axisLength) {
			axisLength = 0;
		}
		else {
			axisLength = 1.0f / axisLength;
		}
	}
	else {
		axisLength = 1.0f;
	}

	ori->viewOrigin[0] = DotProduct(delta, ori->axis[0]) * axisLength;
	ori->viewOrigin[1] = DotProduct(delta, ori->axis[1]) * axisLength;
	ori->viewOrigin[2] = DotProduct(delta, ori->axis[2]) * axisLength;
}

/*
=================
R_RotateForViewer

Sets up the modelview matrix for a given viewParm
=================
*/
static void R_RotateForViewer( void )
{
	float	viewerMatrix[16];
	vec3_t	origin;

	Com_Memset(&tr.ori , 0, sizeof(tr.ori ));
	tr.ori.axis[0][0] = 1;
	tr.ori.axis[1][1] = 1;
	tr.ori.axis[2][2] = 1;
	VectorCopy(tr.viewParms.ori.origin, tr.ori.viewOrigin);

	// transform by the camera placement
	VectorCopy(tr.viewParms.ori.origin, origin);

	viewerMatrix[0] = tr.viewParms.ori.axis[0][0];
	viewerMatrix[4] = tr.viewParms.ori.axis[0][1];
	viewerMatrix[8] = tr.viewParms.ori.axis[0][2];
	viewerMatrix[12] = -origin[0] * viewerMatrix[0] + -origin[1] * viewerMatrix[4] + -origin[2] * viewerMatrix[8];

	viewerMatrix[1] = tr.viewParms.ori.axis[1][0];
	viewerMatrix[5] = tr.viewParms.ori.axis[1][1];
	viewerMatrix[9] = tr.viewParms.ori.axis[1][2];
	viewerMatrix[13] = -origin[0] * viewerMatrix[1] + -origin[1] * viewerMatrix[5] + -origin[2] * viewerMatrix[9];

	viewerMatrix[2] = tr.viewParms.ori.axis[2][0];
	viewerMatrix[6] = tr.viewParms.ori.axis[2][1];
	viewerMatrix[10] = tr.viewParms.ori.axis[2][2];
	viewerMatrix[14] = -origin[0] * viewerMatrix[2] + -origin[1] * viewerMatrix[6] + -origin[2] * viewerMatrix[10];

	viewerMatrix[3] = 0;
	viewerMatrix[7] = 0;
	viewerMatrix[11] = 0;
	viewerMatrix[15] = 1;

	// convert from our coordinate system (looking down X)
	// to OpenGL's coordinate system (looking down -Z)
	myGlMultMatrix(viewerMatrix, s_flipMatrix, tr.ori.modelMatrix);

	tr.viewParms.world = tr.ori ;
}

/*
** SetFarClip
*/
static void R_SetFarClip( void )
{
	float	farthestCornerDistance;
	int		i;

	// if not rendering the world (icons, menus, etc)
	// set a 2k far clip plane
	if ( tr.refdef.rdflags & RDF_NOWORLDMODEL ) {
		if (tr.refdef.rdflags & RDF_AUTOMAP)
		{ //override the zfar then
			tr.viewParms.zFar = 32768.0f;
		}
		else
		{
			tr.viewParms.zFar = 2048.0f;
		}
		return;
	}

	//
	// set far clipping planes dynamically
	//
	farthestCornerDistance = 0;
	for (i = 0; i < 8; i++)
	{
		vec3_t v;
		float distance;

		v[0] = tr.viewParms.visBounds[(i >> 0) & 1][0];
		v[1] = tr.viewParms.visBounds[(i >> 1) & 1][1];
		v[2] = tr.viewParms.visBounds[(i >> 2) & 1][2];

		distance = DistanceSquared(tr.viewParms.ori.origin, v);

		if (distance > farthestCornerDistance)
		{
			farthestCornerDistance = distance;
		}
	}
	
	//tr.viewParms.zFar = sqrt(farthestCornerDistance);

	// Bring in the zFar to the distanceCull distance
	// The sky renders at zFar so need to move it out a little
	// ...and make sure there is a minimum zfar to prevent problems
	tr.viewParms.zFar = Com_Clamp(2048.0f, tr.distanceCull * (1.732), sqrtf(farthestCornerDistance));
}

/*
Set up the culling frustum planes for the current view using the results we got from computing the first two rows of
the projection matrix.
================ =
*/
static void R_SetupFrustum( viewParms_t *dest, const float xmin, const float xmax, 
	const float ymax, const float zProj )
{
	vec3_t		ofsorigin;
	float		oppleg, adjleg, length, max[2] = { xmax, ymax };
	int			i, j;
	cplane_t	*frustum;

	// symmetric case can be simplified
	VectorCopy(dest->ori.origin, ofsorigin);

	frustum = dest->frustum;
	for (i = 0; i < 2; i++) {
		length = sqrt(max[i] * max[i] + zProj * zProj);
		oppleg = max[i] / length;
		adjleg = zProj / length;

		for (j = 0; j < 2; j++, frustum++) {
			VectorScale(dest->ori.axis[0], oppleg, frustum->normal);
			VectorMA(frustum->normal, ((j%2)?-adjleg:adjleg), dest->ori.axis[i+1], frustum->normal);
		}
	}

	for (i = 0; i < 4; i++) {
		dest->frustum[i].type = PLANE_NON_AXIAL;
		dest->frustum[i].dist = DotProduct(ofsorigin, dest->frustum[i].normal);
		SetPlaneSignbits(&dest->frustum[i]);
	}

	// near clipping plane
	VectorCopy(dest->ori.axis[0], dest->frustum[4].normal);
	dest->frustum[4].type = PLANE_NON_AXIAL;
	dest->frustum[4].dist = DotProduct(ofsorigin, dest->frustum[4].normal) + r_znear->value;
	SetPlaneSignbits(&dest->frustum[4]);
}

/*
===============
R_SetupProjection
===============
*/
void R_SetupProjection( viewParms_t *dest, float zProj, qboolean computeFrustum )
{
	float	xmin, xmax, ymin, ymax;
	float	width, height;

	ymax = zProj * tan(dest->fovY * M_PI / 360.0f);
	ymin = -ymax;

	xmax = zProj * tan(dest->fovX * M_PI / 360.0f);
	xmin = -xmax;

	width = xmax - xmin;
	height = ymax - ymin;

	dest->projectionMatrix[0] = 2 * zProj / width;
	dest->projectionMatrix[4] = 0;
	dest->projectionMatrix[8] = (xmax + xmin) / width;
	dest->projectionMatrix[12] = 2 * zProj / width;

	dest->projectionMatrix[1] = 0;
	dest->projectionMatrix[5] = 2 * zProj / height;
	dest->projectionMatrix[9] = (ymax + ymin) / height;	// normally 0
	dest->projectionMatrix[13] = 0;

	dest->projectionMatrix[3] = 0;
	dest->projectionMatrix[7] = 0;
	dest->projectionMatrix[11] = -1;
	dest->projectionMatrix[15] = 0;

	// Now that we have all the data for the projection matrix we can also setup the view frustum.
	if (computeFrustum)
		R_SetupFrustum(dest, xmin, xmax, ymax, zProj);
}

/*
===============
R_SetupProjectionZ
===============
*/
static void R_SetupProjectionZ( viewParms_t *dest ) {

	const float zNear = r_znear->value;
	const float zFar = dest->zFar;
	const float depth = zFar - zNear;

	dest->projectionMatrix[2] = 0;
	dest->projectionMatrix[6] = 0;
#ifdef USE_REVERSED_DEPTH
	dest->projectionMatrix[10] = zNear / depth;
	dest->projectionMatrix[14] = zFar * zNear / depth;
#else
	dest->projectionMatrix[10] = -zFar / depth;
	dest->projectionMatrix[14] = -zFar * zNear / depth;
#endif

	if (dest->portalView != PV_NONE)
	{
		float plane[4], plane2[4];
		vec4_t q, c;

#ifdef USE_REVERSED_DEPTH
		dest->projectionMatrix[10] = -zFar / depth;
		dest->projectionMatrix[14] = -zFar * zNear / depth;
#endif
		// transform portal plane into camera space
		plane[0] = dest->portalPlane.normal[0];
		plane[1] = dest->portalPlane.normal[1];
		plane[2] = dest->portalPlane.normal[2];
		plane[3] = dest->portalPlane.dist;

		plane2[0] = -DotProduct(dest->ori.axis[1], plane);
		plane2[1] = DotProduct(dest->ori.axis[2], plane);
		plane2[2] = -DotProduct(dest->ori.axis[0], plane);
		plane2[3] = DotProduct(plane, dest->ori.origin) - plane[3];

		// Lengyel, Eric. "Modifying the Projection Matrix to Perform Oblique Near-plane Clipping".
		// Terathon Software 3D Graphics Library, 2004. http://www.terathon.com/code/oblique.html
		q[0] = (SGN(plane2[0]) + dest->projectionMatrix[8]) / dest->projectionMatrix[0];
		q[1] = (SGN(plane2[1]) + dest->projectionMatrix[9]) / dest->projectionMatrix[5];
		q[2] = -1.0f;
		q[3] = -dest->projectionMatrix[10] / dest->projectionMatrix[14];
		VectorScale4(plane2, 2.0f / DotProduct4(plane2, q), c);

		dest->projectionMatrix[2] = c[0];
		dest->projectionMatrix[6] = c[1];
		dest->projectionMatrix[10] = c[2];
		dest->projectionMatrix[14] = c[3];

#ifdef USE_REVERSED_DEPTH
		dest->projectionMatrix[2] = -dest->projectionMatrix[2];
		dest->projectionMatrix[6] = -dest->projectionMatrix[6];
		dest->projectionMatrix[10] = -(dest->projectionMatrix[10] + 1.0);
		dest->projectionMatrix[14] = -dest->projectionMatrix[14];
#endif
	}	
}

/*
=================
R_MirrorPoint
=================
*/
static void R_MirrorPoint( const vec3_t in, const orientation_t *surface, const orientation_t *camera, vec3_t out ) {
	int		i;
	vec3_t	local;
	vec3_t	transformed;
	float	d;

	VectorSubtract(in, surface->origin, local);

	VectorClear(transformed);
	for (i = 0; i < 3; i++) {
		d = DotProduct(local, surface->axis[i]);
		VectorMA(transformed, d, camera->axis[i], transformed);
	}

	VectorAdd(transformed, camera->origin, out);
}

static void R_MirrorVector( const vec3_t in, const orientation_t *surface, const orientation_t *camera, vec3_t out ) {
	int		i;
	float	d;

	VectorClear(out);
	for (i = 0; i < 3; i++) {
		d = DotProduct(in, surface->axis[i]);
		VectorMA(out, d, camera->axis[i], out);
	}
}

/*
=============
R_PlaneForSurface
=============
*/
static void R_PlaneForSurface( const surfaceType_t *surfType, cplane_t *plane ) {
	srfTriangles_t	*tri;
	srfPoly_t		*poly;
	drawVert_t		*v1, *v2, *v3;
	vec4_t			plane4;

	if (!surfType) {
		memset(plane, 0, sizeof(*plane));
		plane->normal[0] = 1;
		return;
	}
	switch (*surfType) {
	case SF_FACE:
		*plane = ((srfSurfaceFace_t*)surfType)->plane;
		return;
	case SF_TRIANGLES:
		tri = (srfTriangles_t*)surfType;
		v1 = tri->verts + tri->indexes[0];
		v2 = tri->verts + tri->indexes[1];
		v3 = tri->verts + tri->indexes[2];
		PlaneFromPoints(plane4, v1->xyz, v2->xyz, v3->xyz);
		VectorCopy(plane4, plane->normal);
		plane->dist = plane4[3];
		return;
	case SF_POLY:
		poly = (srfPoly_t*)surfType;
		PlaneFromPoints(plane4, poly->verts[0].xyz, poly->verts[1].xyz, poly->verts[2].xyz);
		VectorCopy(plane4, plane->normal);
		plane->dist = plane4[3];
		return;
	default:
		memset(plane, 0, sizeof(*plane));
		plane->normal[0] = 1;
		return;
	}
}

/*
=================
R_GetPortalOrientation

entityNum is the entity that the portal surface is a part of, which may
be moving and rotating.

Returns qtrue if it should be mirrored
=================
*/
static qboolean R_GetPortalOrientations( const drawSurf_t *drawSurf, int entityNum,
	orientation_t *surface, orientation_t *camera,
	vec3_t pvsOrigin, portalView_t *portalView )
{
	int				i;
	cplane_t		originalPlane, plane;
	trRefEntity_t	*e;
	float			d;
	vec3_t			transformed;

	// create plane axis for the portal we are seeing
	R_PlaneForSurface(drawSurf->surface, &originalPlane);

	// rotate the plane if necessary
	if (entityNum != REFENTITYNUM_WORLD) {
		tr.currentEntityNum = entityNum;
		tr.currentEntity = &tr.refdef.entities[entityNum];

		// get the orientation of the entity
		R_RotateForEntity(tr.currentEntity, &tr.viewParms, &tr.ori);

		// rotate the plane, but keep the non-rotated version for matching
		// against the portalSurface entities
		R_LocalNormalToWorld(originalPlane.normal, plane.normal);
		plane.dist = originalPlane.dist + DotProduct(plane.normal, tr.ori.origin);

		// translate the original plane
		originalPlane.dist = originalPlane.dist + DotProduct(originalPlane.normal, tr.ori.origin);
	}
	else {
		plane = originalPlane;
	}

	VectorCopy(plane.normal, surface->axis[0]);
	PerpendicularVector(surface->axis[1], surface->axis[0]);
	CrossProduct(surface->axis[0], surface->axis[1], surface->axis[2]);

	// locate the portal entity closest to this plane.
	// origin will be the origin of the portal, origin2 will be
	// the origin of the camera
	for (i = 0; i < tr.refdef.num_entities; i++) {
		e = &tr.refdef.entities[i];
		if (e->e.reType != RT_PORTALSURFACE) {
			continue;
		}

		d = DotProduct(e->e.origin, originalPlane.normal) - originalPlane.dist;
		if (d > 64 || d < -64) {
			continue;
		}

		// get the pvsOrigin from the entity
		VectorCopy(e->e.oldorigin, pvsOrigin);

		// if the entity is just a mirror, don't use as a camera point
		if (e->e.oldorigin[0] == e->e.origin[0] &&
			e->e.oldorigin[1] == e->e.origin[1] &&
			e->e.oldorigin[2] == e->e.origin[2]) {
			VectorScale(plane.normal, plane.dist, surface->origin);
			VectorCopy(surface->origin, camera->origin);
			VectorSubtract(vec3_origin, surface->axis[0], camera->axis[0]);
			VectorCopy(surface->axis[1], camera->axis[1]);
			VectorCopy(surface->axis[2], camera->axis[2]);

			*portalView = PV_MIRROR;
			return qtrue;
		}

		// project the origin onto the surface plane to get
		// an origin point we can rotate around
		d = DotProduct(e->e.origin, plane.normal) - plane.dist;
		VectorMA(e->e.origin, -d, surface->axis[0], surface->origin);

		// now get the camera origin and orientation
		VectorCopy(e->e.oldorigin, camera->origin);
		AxisCopy(e->e.axis, camera->axis);
		VectorSubtract(vec3_origin, camera->axis[0], camera->axis[0]);
		VectorSubtract(vec3_origin, camera->axis[1], camera->axis[1]);

		// optionally rotate
		if (e->e.oldframe) {
			// if a speed is specified
			if (e->e.frame) {
				// continuous rotate
				d = (tr.refdef.time / 1000.0f) * e->e.frame;
				VectorCopy(camera->axis[1], transformed);
				RotatePointAroundVector(camera->axis[1], camera->axis[0], transformed, d);
				CrossProduct(camera->axis[0], camera->axis[1], camera->axis[2]);
			}
			else {
				// bobbing rotate, with skinNum being the rotation offset
				d = sin(tr.refdef.time * 0.003f);
				d = e->e.skinNum + d * 4;
				VectorCopy(camera->axis[1], transformed);
				RotatePointAroundVector(camera->axis[1], camera->axis[0], transformed, d);
				CrossProduct(camera->axis[0], camera->axis[1], camera->axis[2]);
			}
		}
		else if (e->e.skinNum) {
			d = e->e.skinNum;
			VectorCopy(camera->axis[1], transformed);
			RotatePointAroundVector(camera->axis[1], camera->axis[0], transformed, d);
			CrossProduct(camera->axis[0], camera->axis[1], camera->axis[2]);
		}
		*portalView = PV_PORTAL;
		return qtrue;
	}

	// if we didn't locate a portal entity, don't render anything.
	// We don't want to just treat it as a mirror, because without a
	// portal entity the server won't have communicated a proper entity set
	// in the snapshot

	// unfortunately, with local movement prediction it is easily possible
	// to see a surface before the server has communicated the matching
	// portal surface entity, so we don't want to print anything here...

	//ri.Printf( PRINT_ALL, "Portal surface without a portal entity\n" );

	return qfalse;
}

static qboolean IsMirror( const drawSurf_t *drawSurf, int entityNum )
{
	int				i;
	cplane_t		originalPlane, plane;
	trRefEntity_t	*e;
	float			d;

	// create plane axis for the portal we are seeing
	R_PlaneForSurface(drawSurf->surface, &originalPlane);

	// rotate the plane if necessary
	if (entityNum != REFENTITYNUM_WORLD)
	{
		tr.currentEntityNum = entityNum;
		tr.currentEntity = &tr.refdef.entities[entityNum];

		// get the orientation of the entity
		R_RotateForEntity(tr.currentEntity, &tr.viewParms, &tr.ori);

		// rotate the plane, but keep the non-rotated version for matching
		// against the portalSurface entities
		R_LocalNormalToWorld(originalPlane.normal, plane.normal);
		plane.dist = originalPlane.dist + DotProduct(plane.normal, tr.ori.origin);

		// translate the original plane
		originalPlane.dist = originalPlane.dist + DotProduct(originalPlane.normal, tr.ori.origin);
	}
	else
	{
		plane = originalPlane;
	}

	// locate the portal entity closest to this plane.
	// origin will be the origin of the portal, origin2 will be
	// the origin of the camera
	for (i = 0; i < tr.refdef.num_entities; i++)
	{
		e = &tr.refdef.entities[i];
		if (e->e.reType != RT_PORTALSURFACE) {
			continue;
		}

		d = DotProduct(e->e.origin, originalPlane.normal) - originalPlane.dist;
		if (d > 64 || d < -64) {
			continue;
		}

		// if the entity is just a mirror, don't use as a camera point
		if (e->e.oldorigin[0] == e->e.origin[0] &&
			e->e.oldorigin[1] == e->e.origin[1] &&
			e->e.oldorigin[2] == e->e.origin[2])
		{
			return qtrue;
		}

		return qfalse;
	}
	return qfalse;
}

/*
** SurfIsOffscreen
**
** Determines if a surface is completely offscreen.
*/
static qboolean SurfIsOffscreen( const drawSurf_t *drawSurf, vec4_t clipDest[128] ) {
	float		shortest = 100000000;
	int			entityNum;
	int			numTriangles;
	shader_t	*shader;
	int			fogNum;
	int			dlighted;
	vec4_t		clip, eye;
	int			i;

	unsigned int pointAnd = (unsigned int)~0;

	R_RotateForViewer();

	R_DecomposeSort(drawSurf->sort, &entityNum, &shader, &fogNum, &dlighted);
	RB_BeginSurface(shader, fogNum);

#ifdef USE_VBO
	tess.allowVBO = qfalse;
#endif

	rb_surfaceTable[*drawSurf->surface](drawSurf->surface);

	assert(tess.numVertexes < 128);

	for (i = 0; i < tess.numVertexes; i++)
	{
		int j;
		unsigned int pointFlags = 0;

		R_TransformModelToClip(tess.xyz[i], tr.ori.modelMatrix, tr.viewParms.projectionMatrix, eye, clip);

		for (j = 0; j < 3; j++)
		{
			if (clip[j] >= clip[3])
			{
				pointFlags |= (1 << (j * 2));
			}
			else if (clip[j] <= -clip[3])
			{
				pointFlags |= (1 << (j * 2 + 1));
			}
		}
		pointAnd &= pointFlags;
	}

	// trivially reject
	if (pointAnd)
	{
		tess.numIndexes = 0;
		return qtrue;
	}

	// determine if this surface is backfaced and also determine the distance
	// to the nearest vertex so we can cull based on portal range.  Culling
	// based on vertex distance isn't 100% correct (we should be checking for
	// range to the surface), but it's good enough for the types of portals
	// we have in the game right now.
	numTriangles = tess.numIndexes / 3;

	for (i = 0; i < tess.numIndexes; i += 3)
	{
		vec3_t normal;
		float dot;
		float len;

		VectorSubtract(tess.xyz[tess.indexes[i]], tr.viewParms.ori.origin, normal);

		len = VectorLengthSquared(normal);			// lose the sqrt
		if (len < shortest)
		{
			shortest = len;
		}

		if ((dot = DotProduct(normal, tess.normal[tess.indexes[i]])) >= 0)
		{
			numTriangles--;
		}
	}
	tess.numIndexes = 0;
	if (!numTriangles)
	{
		return qtrue;
	}

	// mirrors can early out at this point, since we don't do a fade over distance
	// with them (although we could)
	if (IsMirror(drawSurf, entityNum))
	{
		return qfalse;
	}

	if (shortest > (tess.shader->portalRange * tess.shader->portalRange))
	{
		return qtrue;
	}

	return qfalse;
}

#if 0
/*
================
R_GetModelViewBounds
================
*/
static void R_GetModelViewBounds( int *mins, int *maxs )
{
	float			minn[2];
	float			maxn[2];
	float			norm[2];
	float			mvp[16];
	float			dist[4];
	vec4_t			clip;
	int				i, j;

	minn[0] = minn[1] = 1.0;
	maxn[0] = maxn[1] = -1.0;

	// premultiply
	myGlMultMatrix(tr.ori.modelMatrix, tr.viewParms.projectionMatrix, mvp);

	for (i = 0; i < tess.numVertexes; i++) {
		R_TransformModelToClipMVP(tess.xyz[i], mvp, clip);
		if (clip[3] <= 0.0) {
			dist[0] = DotProduct(tess.xyz[i], tr.viewParms.frustum[0].normal) - tr.viewParms.frustum[0].dist; // right
			dist[1] = DotProduct(tess.xyz[i], tr.viewParms.frustum[1].normal) - tr.viewParms.frustum[1].dist; // left
			dist[2] = DotProduct(tess.xyz[i], tr.viewParms.frustum[2].normal) - tr.viewParms.frustum[2].dist; // bottom
			dist[3] = DotProduct(tess.xyz[i], tr.viewParms.frustum[3].normal) - tr.viewParms.frustum[3].dist; // top
			if (dist[0] <= 0 && dist[1] <= 0) {
				if (dist[0] < dist[1]) {
					maxn[0] = 1.0f;
				}
				else {
					minn[0] = -1.0f;
				}
			}
			else {
				if (dist[0] <= 0) maxn[0] = 1.0f;
				if (dist[1] <= 0) minn[0] = -1.0f;
			}
			if (dist[2] <= 0 && dist[3] <= 0) {
				if (dist[2] < dist[3])
					minn[1] = -1.0f;
				else
					maxn[1] = 1.0f;
			}
			else {
				if (dist[2] <= 0) minn[1] = -1.0f;
				if (dist[3] <= 0) maxn[1] = 1.0f;
			}
		}
		else {
			for (j = 0; j < 2; j++) {
				if (clip[j] > clip[3]) clip[j] = clip[3]; else
					if (clip[j] < -clip[3]) clip[j] = -clip[3];
			}
			norm[0] = clip[0] / clip[3];
			norm[1] = clip[1] / clip[3];
			for (j = 0; j < 2; j++) {
				if (norm[j] < minn[j]) minn[j] = norm[j];
				if (norm[j] > maxn[j]) maxn[j] = norm[j];
			}
		}
	}

	mins[0] = (int)(-0.5 + 0.5 * (1.0 + minn[0]) * tr.viewParms.viewportWidth);
	mins[1] = (int)(-0.5 + 0.5 * (1.0 + minn[1]) * tr.viewParms.viewportHeight);
	maxs[0] = (int)(0.5 + 0.5 * (1.0 + maxn[0]) * tr.viewParms.viewportWidth);
	maxs[1] = (int)(0.5 + 0.5 * (1.0 + maxn[1]) * tr.viewParms.viewportHeight);
}
#endif

/*
========================
R_MirrorViewBySurface

Returns qtrue if another view has been rendered
========================
*/
extern int r_numdlights;
static qboolean R_MirrorViewBySurface( const drawSurf_t *drawSurf, int entityNum ) {
	vec4_t			clipDest[128];
	viewParms_t		newParms;
	viewParms_t		oldParms;
	orientation_t	surface, camera;

	// don't recursively mirror
	if (tr.viewParms.portalView != PV_NONE) {
		vk_debug("WARNING: recursive mirror/portal found\n");
		return qfalse;
	}

	if (r_noportals->integer > 1 /*|| (r_fastsky->integer == 1)*/) {
		return qfalse;
	}

	// trivially reject portal/mirror
	if (SurfIsOffscreen(drawSurf, clipDest)) {
		return qfalse;
	}

	// save old viewParms so we can return to it after the mirror view
	oldParms = tr.viewParms;

	newParms = tr.viewParms;
	newParms.portalView = PV_NONE;
	if (!R_GetPortalOrientations(drawSurf, entityNum, &surface, &camera,
		newParms.pvsOrigin, &newParms.portalView)) {
		return qfalse;		// bad portal, no portalentity
	}

#ifdef USE_PMLIGHT
	// create dedicated set for each view
	if (r_numdlights + oldParms.num_dlights <= ARRAY_LEN(backEndData->dlights)) {
		int i;
		newParms.dlights = oldParms.dlights + oldParms.num_dlights;
		newParms.num_dlights = oldParms.num_dlights;
		r_numdlights += oldParms.num_dlights;
		for (i = 0; i < oldParms.num_dlights; i++)
			newParms.dlights[i] = oldParms.dlights[i];
	}
#endif

#if 0
	// causing artifacts with mirrors.
	if (tess.numVertexes > 2 && r_fastsky->integer && vk.fastSky) {
		int mins[2], maxs[2];
		R_GetModelViewBounds(mins, maxs);
		newParms.scissorX = newParms.viewportX + mins[0];
		newParms.scissorY = newParms.viewportY + mins[1];
		newParms.scissorWidth = maxs[0] - mins[0];
		newParms.scissorHeight = maxs[1] - mins[1];
	}
#endif

	R_MirrorPoint(oldParms.ori.origin, &surface, &camera, newParms.ori.origin);

	VectorSubtract(vec3_origin, camera.axis[0], newParms.portalPlane.normal);
	newParms.portalPlane.dist = DotProduct(camera.origin, newParms.portalPlane.normal);

	R_MirrorVector(oldParms.ori.axis[0], &surface, &camera, newParms.ori.axis[0]);
	R_MirrorVector(oldParms.ori.axis[1], &surface, &camera, newParms.ori.axis[1]);
	R_MirrorVector(oldParms.ori.axis[2], &surface, &camera, newParms.ori.axis[2]);

	// OPTIMIZE: restrict the viewport on the mirrored view

	// render the mirror view
	R_RenderView(&newParms);

	tr.viewParms = oldParms;

	return qtrue;
}

/*
=================
R_SpriteFogNum

See if a sprite is inside a fog volume
=================
*/
static int R_SpriteFogNum( const trRefEntity_t *ent ) {
	int		i, j;
	fog_t	*fog;

	if (tr.refdef.rdflags & RDF_NOWORLDMODEL) {
		return 0;
	}

	for (i = 1; i < tr.world->numfogs; i++) {
		fog = &tr.world->fogs[i];
		for (j = 0; j < 3; j++) {
			if (ent->e.origin[j] - ent->e.radius >= fog->bounds[1][j]) {
				break;
			}
			if (ent->e.origin[j] + ent->e.radius <= fog->bounds[0][j]) {
				break;
			}
		}
		if (j == 3) {
			return i;
		}
	}

	return 0;
}

/*
==========================================================================================

DRAWSURF SORTING

==========================================================================================
*/

/*
===============
R_Radix
===============
*/
static QINLINE void R_Radix(int byte, int size, drawSurf_t *source, drawSurf_t *dest)
{
	int           count[256] = { 0 };
	int           index[256];
	int           i;
	unsigned char *sortKey = NULL;
	unsigned char *end = NULL;

	sortKey = ((unsigned char*)&source[0].sort) + byte;
	end = sortKey + (size * sizeof(drawSurf_t));
	for (; sortKey < end; sortKey += sizeof(drawSurf_t))
		++count[*sortKey];

	index[0] = 0;

	for (i = 1; i < 256; ++i)
		index[i] = index[i - 1] + count[i - 1];

	sortKey = ((unsigned char*)&source[0].sort) + byte;
	for (i = 0; i < size; ++i, sortKey += sizeof(drawSurf_t))
		dest[index[*sortKey]++] = source[i];
}

/*
===============
R_RadixSort

Radix sort with 4 byte size buckets
===============
*/
static void R_RadixSort( drawSurf_t *source, int size )
{
	static drawSurf_t scratch[MAX_DRAWSURFS];
#ifdef Q3_LITTLE_ENDIAN
	R_Radix(0, size, source, scratch);
	R_Radix(1, size, scratch, source);
	R_Radix(2, size, source, scratch);
	R_Radix(3, size, scratch, source);
#else
	R_Radix(3, size, source, scratch);
	R_Radix(2, size, scratch, source);
	R_Radix(1, size, source, scratch);
	R_Radix(0, size, scratch, source);
#endif //Q3_LITTLE_ENDIAN
}

#ifdef USE_PMLIGHT
typedef struct litSurf_tape_s {
	struct litSurf_s *first;
	struct litSurf_s *last;
	unsigned count;
} litSurf_tape_t;

// Philip Erdelsky gets all the credit for this one...

static void R_SortLitsurfs( dlight_t *dl )
{
	litSurf_tape_t	tape[4];
	int				base;
	litSurf_t		*p;
	litSurf_t		*next;
	unsigned		block_size;
	litSurf_tape_t	*tape0;
	litSurf_tape_t	*tape1;
	int				dest;
	litSurf_tape_t	*output_tape;
	litSurf_tape_t	*chosen_tape;
	unsigned		n0, n1;

	// distribute the records alternately to tape[0] and tape[1]

	tape[0].count = tape[1].count = 0;
	tape[0].first = tape[1].first = NULL;

	base = 0;
	p = dl->head;

	while (p) {
		next = p->next;
		p->next = tape[base].first;
		tape[base].first = p;
		tape[base].count++;
		p = next;
		base ^= 1;
	}

	// merge from the two active tapes into the two idle ones
	// doubling the number of records and pingponging the tape sets as we go

	block_size = 1;
	for (base = 0; tape[base + 1].count; base ^= 2, block_size <<= 1)
	{
		tape0 = tape + base;
		tape1 = tape + base + 1;
		dest = base ^ 2;

		tape[dest].count = tape[dest + 1].count = 0;
		for (; tape0->count; dest ^= 1)
		{
			output_tape = tape + dest;
			n0 = n1 = block_size;

			while (1)
			{
				if (n0 == 0 || tape0->count == 0)
				{
					if (n1 == 0 || tape1->count == 0)
						break;
					chosen_tape = tape1;
					n1--;
				}
				else if (n1 == 0 || tape1->count == 0)
				{
					chosen_tape = tape0;
					n0--;
				}
				else if (tape0->first->sort > tape1->first->sort)
				{
					chosen_tape = tape1;
					n1--;
				}
				else
				{
					chosen_tape = tape0;
					n0--;
				}
				chosen_tape->count--;
				p = chosen_tape->first;
				chosen_tape->first = p->next;
				if (output_tape->count == 0)
					output_tape->first = p;
				else
					output_tape->last->next = p;
				output_tape->last = p;
				output_tape->count++;
			}
		}
	}

	if (tape[base].count > 1)
		tape[base].last->next = NULL;

	dl->head = tape[base].first;
}

/*
=================
R_AddLitSurf
=================
*/
void R_AddLitSurf( surfaceType_t *surface, shader_t *shader, int fogIndex )
{
	struct litSurf_s *litsurf;

	if (tr.refdef.numLitSurfs >= ARRAY_LEN(backEndData->litSurfs))
		return;

	tr.pc.c_lit_surfs++;

	litsurf = &tr.refdef.litSurfs[tr.refdef.numLitSurfs++];

	litsurf->sort = (shader->sortedIndex << QSORT_SHADERNUM_SHIFT)
		| tr.shiftedEntityNum | (fogIndex << QSORT_FOGNUM_SHIFT);
	litsurf->surface = surface;

	if (!tr.light->head)
		tr.light->head = litsurf;
	if (tr.light->tail)
		tr.light->tail->next = litsurf;

	tr.light->tail = litsurf;
	tr.light->tail->next = NULL;
}

/*
=================
R_DecomposeLitSort
=================
*/
void R_DecomposeLitSort( unsigned sort, int *entityNum, shader_t **shader, int *fogNum ) {
	*fogNum = (sort >> QSORT_FOGNUM_SHIFT) & FOGNUM_MASK;
	*shader = tr.sortedShaders[(sort >> QSORT_SHADERNUM_SHIFT) & SHADERNUM_MASK];
	*entityNum = (sort >> QSORT_REFENTITYNUM_SHIFT) & REFENTITYNUM_MASK;
}

#endif // USE_PMLIGHT


//==========================================================================================

/*
=================
R_AddDrawSurf
=================
*/
void R_AddDrawSurf( surfaceType_t *surface, shader_t *shader,
	int fogIndex, int dlightMap )
{
	int			index;

	if (tr.refdef.rdflags & RDF_NOFOG)
	{
		fogIndex = 0;
	}

	if ((shader->surfaceFlags & SURF_FORCESIGHT) && !(tr.refdef.rdflags & RDF_ForceSightOn))
	{	//if shader is only seen with ForceSight and we don't have ForceSight on, then don't draw
		return;
	}

	// instead of checking for overflow, we just mask the index
	// so it wraps around
	index = tr.refdef.numDrawSurfs & DRAWSURF_MASK;
	// the sort data is packed into a single 32 bit value so it can be
	// compared quickly during the qsorting process
	tr.refdef.drawSurfs[index].sort = (shader->sortedIndex << QSORT_SHADERNUM_SHIFT)
		| tr.shiftedEntityNum | (fogIndex << QSORT_FOGNUM_SHIFT) | (int)dlightMap;
	tr.refdef.drawSurfs[index].surface = surface;
	tr.refdef.numDrawSurfs++;
}

/*
=================
R_DecomposeSort
=================
*/
void R_DecomposeSort( unsigned sort, int *entityNum, shader_t **shader, 
											int *fogNum, int *dlightMap )
{
	*fogNum = (sort >> QSORT_FOGNUM_SHIFT) & 31;
	*shader = tr.sortedShaders[(sort >> QSORT_SHADERNUM_SHIFT) & (MAX_SHADERS - 1)];
	*entityNum = (sort >> QSORT_REFENTITYNUM_SHIFT) & REFENTITYNUM_MASK;
	*dlightMap = sort & 3;
}

/*
=================
R_SortDrawSurfs
=================
*/
void R_SortDrawSurfs( drawSurf_t *drawSurfs, int numDrawSurfs ) {
	shader_t		*shader; 
	int				fogNum;
	int				entityNum;
	int				dlighted;
	int				i;

	// it is possible for some views to not have any surfaces
	if (numDrawSurfs < 1) {
		// we still need to add it for hyperspace cases
		R_AddDrawSurfCmd(drawSurfs, numDrawSurfs);
		return;
	}

	// if we overflowed MAX_DRAWSURFS, the drawsurfs
	// wrapped around in the buffer and we will be missing
	// the first surfaces, not the last ones
	if (numDrawSurfs > MAX_DRAWSURFS) {
		numDrawSurfs = MAX_DRAWSURFS;
	}

	// sort the drawsurfs by sort type, then orientation, then shader
	R_RadixSort(drawSurfs, numDrawSurfs);

	// check for any pass through drawing, which
	// may cause another view to be rendered first
	for (i = 0; i < numDrawSurfs; i++) {
		R_DecomposeSort((drawSurfs + i)->sort, &entityNum, &shader, &fogNum, &dlighted);

		if (shader->sort > SS_PORTAL) {
			break;
		}

		// no shader should ever have this sort type
		if (shader->sort == SS_BAD) {
			Com_Error(ERR_DROP, "Shader '%s'with sort == SS_BAD", shader->name);
		}

		// if the mirror was completely clipped away, we may need to check another surface
		if (R_MirrorViewBySurface((drawSurfs + i), entityNum)) {
			// this is a debug option to see exactly what is being mirrored
			if (r_portalOnly->integer) {
				return;
			}

			if (r_fastsky->integer == 0 || !vk.fastSky) {
				break;	// only one mirror view at a time
			}
		}
	}

#ifdef USE_PMLIGHT
	{
		dlight_t *dl;

		// all the lit surfaces are in a single queue
		// but each light's surfaces are sorted within its subsection
		for (i = 0; i < tr.refdef.num_dlights; ++i) {
			dl = &tr.refdef.dlights[i];
			if (dl->head) {
				R_SortLitsurfs(dl);
			}
		}
	}
#endif // USE_PMLIGHT

	R_AddDrawSurfCmd(drawSurfs, numDrawSurfs);
}

/*
=============
R_AddEntitySurfaces
=============
*/
static void R_AddEntitySurfaces( void ) {
	trRefEntity_t	*ent;
	shader_t		*shader;

	if (!r_drawentities->integer) {
		return;
	}

	for (tr.currentEntityNum = 0;
		tr.currentEntityNum < tr.refdef.num_entities;
		tr.currentEntityNum++) {
		ent = tr.currentEntity = &tr.refdef.entities[tr.currentEntityNum];

		assert(ent->e.renderfx >= 0);
		// preshift the value we are going to OR into the drawsurf sort
		tr.shiftedEntityNum = tr.currentEntityNum << QSORT_REFENTITYNUM_SHIFT;

		//
		// the weapon model must be handled special --
		// we don't want the hacked weapon position showing in
		// mirrors, because the true body position will already be drawn
		//
		if ((ent->e.renderfx & RF_FIRST_PERSON) && (tr.viewParms.portalView != PV_NONE)) {
			continue;
		}

		// simple generated models, like sprites and beams, are not culled
		switch (ent->e.reType) {
		case RT_PORTALSURFACE:
			break;		// don't draw anything
		case RT_SPRITE:
		case RT_BEAM:
		case RT_ORIENTED_QUAD:
		case RT_ELECTRICITY:
		case RT_LINE:
		case RT_ORIENTEDLINE:
		case RT_CYLINDER:
		case RT_SABER_GLOW:
			// self blood sprites, talk balloons, etc should not be drawn in the primary
			// view.  We can't just do this check for all entities, because md3
			// entities may still want to cast shadows from them
			if ((ent->e.renderfx & RF_THIRD_PERSON) && (tr.viewParms.portalView == PV_NONE)) {
				continue;
			}
			shader = R_GetShaderByHandle(ent->e.customShader);
			R_AddDrawSurf(&entitySurface, shader, R_SpriteFogNum(ent), 0);
			break;

		case RT_MODEL:
			// we must set up parts of tr.ori for model culling
			R_RotateForEntity(ent, &tr.viewParms, &tr.ori);

			tr.currentModel = R_GetModelByHandle(ent->e.hModel);
			if (!tr.currentModel) {
				R_AddDrawSurf(&entitySurface, tr.defaultShader, 0, 0);
			}
			else {
				switch (tr.currentModel->type) {
				case MOD_MESH:
					R_AddMD3Surfaces(ent);
					break;
				case MOD_BRUSH:
					R_AddBrushModelSurfaces(ent);
					break;
					/*
					Ghoul2 Insert Start
					*/
				case MOD_MDXM:
					//g2r
					if (ent->e.ghoul2)
					{
						R_AddGhoulSurfaces(ent);
					}
					break;
				case MOD_BAD:		// null model axis
					if ((ent->e.renderfx & RF_THIRD_PERSON) && (tr.viewParms.portalView == PV_NONE)) {
						if (!(ent->e.renderfx & RF_SHADOW_ONLY))
						{
							break;
						}
					}

					if (ent->e.ghoul2 && G2API_HaveWeGhoul2Models(*((CGhoul2Info_v*)ent->e.ghoul2)))
					{
						R_AddGhoulSurfaces(ent);
						break;
					}

					R_AddDrawSurf(&entitySurface, tr.defaultShader, 0, false);
					break;
					/*
					Ghoul2 Insert End
					*/
				default:
					Com_Error(ERR_DROP, "R_AddEntitySurfaces: Bad modeltype");
					break;
				}
			}
			break;

		case RT_ENT_CHAIN:
			shader = R_GetShaderByHandle(ent->e.customShader);
			R_AddDrawSurf(&entitySurface, shader, R_SpriteFogNum(ent), false);
			break;

		default:
			Com_Error(ERR_DROP, "R_AddEntitySurfaces: Bad reType");
		}
	}

}


/*
====================
R_GenerateDrawSurfs
====================
*/
static void R_GenerateDrawSurfs( void ) {
	R_AddWorldSurfaces();

	R_AddPolygonSurfaces();

	// set the projection matrix with the minimum zfar
	// now that we have the world bounded
	// this needs to be done before entities are
	// added, because they use the projection
	// matrix for lod calculation

	// dynamically compute far clip plane distance
	R_SetFarClip();

	// we know the size of the clipping volume. Now set the rest of the projection matrix.
	R_SetupProjectionZ(&tr.viewParms);

	R_AddEntitySurfaces();
}

/*
================
R_RenderView

A view may be either the actual camera view,
or a mirror / remote location
================
*/
void R_RenderView( const viewParms_t *parms ) {
	int		firstDrawSurf;

	if (parms->viewportWidth <= 0 || parms->viewportHeight <= 0) {
		return;
	}

	tr.viewCount++;

	tr.viewParms = *parms;
	tr.viewParms.frameSceneNum = tr.frameSceneNum;
	tr.viewParms.frameCount = tr.frameCount;

	firstDrawSurf = tr.refdef.numDrawSurfs;

	//tr.viewCount++;

	R_RotateForViewer();

	R_SetupProjection(&tr.viewParms, r_zproj->value, qtrue);
	
	R_GenerateDrawSurfs();

	R_SortDrawSurfs(tr.refdef.drawSurfs + firstDrawSurf, tr.refdef.numDrawSurfs - firstDrawSurf);
}