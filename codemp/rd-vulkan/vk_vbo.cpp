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

#ifdef USE_VBO

/*

General concept of this VBO implementation is to store all possible static data
(vertexes,colors,tex.coords[0..1],normals) in device-local memory
and accessing it via indexes ONLY.

Static data in current meaning is a world surfaces whose shader data
can be evaluated at map load time.

Every static surface gets unique item index which will be added to queue
instead of tesselation like for regular surfaces. Using items queue also
eleminates run-time tesselation limits.

When it is time to render - we sort queued items to get longest possible
index sequence run to check if its long enough i.e. worth issuing a draw call.
So long device-local index runs are rendered via multiple draw calls,
all remaining short index sequences are grouped together into single
host-visible index buffer which is finally rendered via single draw call.

*/

#define MAX_VBO_STAGES MAX_SHADER_STAGES

#define MIN_IBO_RUN 320

//[ibo]: [index0][index1][index2]
//[vbo]: [index0][vertex0...][index1][vertex1...][index2][vertex2...]

typedef struct vbo_item_s {
	int			index_offset;  // device-local, relative to current shader
	int			soft_offset;   // host-visible, absolute
	int			num_indexes;
	int			num_vertexes;
} vbo_item_t;

typedef struct ibo_item_s {
	int offset;
	int length;
} ibo_item_t;

typedef struct vbo_s {
	byte *vbo_buffer;
	int vbo_offset;
	int vbo_size;

	byte *ibo_buffer;
	int ibo_offset;
	int ibo_size;

	uint32_t soft_buffer_indexes;
	uint32_t soft_buffer_offset;

	ibo_item_t *ibo_items;
	int ibo_items_count;

	vbo_item_t *items;
	int items_count;

	int *items_queue;
	int items_queue_count;

} vbo_t;

static vbo_t world_vbo;

void VBO_Cleanup(void);

static qboolean isStaticRGBgen(colorGen_t cgen)
{
	switch (cgen)
	{
	case CGEN_BAD:
	case CGEN_IDENTITY_LIGHTING:	// tr.identityLight
	case CGEN_IDENTITY:				// always (1,1,1,1)
	case CGEN_ENTITY:				// grabbed from entity's modulate field
	case CGEN_ONE_MINUS_ENTITY:		// grabbed from 1 - entity.modulate
	case CGEN_EXACT_VERTEX:			// tess.vertexColors
	case CGEN_VERTEX:				// tess.vertexColors * tr.identityLight
	case CGEN_ONE_MINUS_VERTEX:
		// case CGEN_WAVEFORM:			// programmatically generated
	case CGEN_LIGHTING_DIFFUSE:
		//case CGEN_FOG:				// standard fog
	case CGEN_CONST:				// fixed color
		return qtrue;
	default:
		return qfalse;
	}
}

static qboolean isStaticTCgen(const shaderStage_t *stage, int bundle)
{
	switch (stage->bundle[bundle].tcGen)
	{
	case TCGEN_BAD:
	case TCGEN_IDENTITY:	// clear to 0,0
	case TCGEN_LIGHTMAP:
	case TCGEN_TEXTURE:
		//case TCGEN_ENVIRONMENT_MAPPED:
		//case TCGEN_ENVIRONMENT_MAPPED_FP:
		//case TCGEN_FOG:
	case TCGEN_VECTOR:		// S and T from world coordinates
		return qtrue;
	case TCGEN_ENVIRONMENT_MAPPED:
		if (bundle == 0 && (stage->tessFlags & TESS_ENV))
			return qtrue;
		else
			return qfalse;
	default:
		return qfalse;
	}
}

static qboolean isStaticTCmod(const textureBundle_t *bundle)
{
	texMod_t type;
	int i;

	for (i = 0; i < bundle->numTexMods; i++) {
		type = bundle->texMods[i].type;
		if (type != TMOD_NONE && type != TMOD_SCALE && type != TMOD_TRANSFORM) {
			return qfalse;
		}
	}

	return qtrue;
}

static qboolean isStaticAgen(alphaGen_t agen)
{
	switch (agen)
	{
	case AGEN_IDENTITY:
	case AGEN_SKIP:
	case AGEN_ENTITY:
	case AGEN_ONE_MINUS_ENTITY:
	case AGEN_VERTEX:
	case AGEN_ONE_MINUS_VERTEX:
		//case AGEN_LIGHTING_SPECULAR:
		//case AGEN_WAVEFORM:
		//case AGEN_PORTAL:
	case AGEN_CONST:
		return qtrue;
	default:
		return qfalse;
	}
}

/*
=============
isStaticShader

Decide if we can put surface in static vbo
=============
*/
static qboolean isStaticShader(shader_t *shader)
{
	const shaderStage_t* stage;
	int i, b, svarsSize;

	if (shader->isStaticShader)
		return qtrue;

	if (shader->isSky || shader->remappedShader)
		return qfalse;

	if (shader->numDeforms || shader->numUnfoggedPasses > MAX_VBO_STAGES)
		return qfalse;

	svarsSize = 0;

	for (i = 0; i < shader->numUnfoggedPasses; i++)
	{
		stage = shader->stages[i];
		if (!stage || !stage->active)
			break;
		if (stage->depthFragment)
			return qfalse;
		for (b = 0; b < NUM_TEXTURE_BUNDLES; b++) {
			if (!isStaticTCmod(&stage->bundle[b]))
				return qfalse;
			if (!isStaticTCgen(stage, b))
				return qfalse;
			if (stage->bundle[b].adjustColorsForFog != ACFF_NONE)
				return qfalse;
			if (!isStaticRGBgen(stage->bundle[b].rgbGen))
				return qfalse;
			if (!isStaticAgen(stage->bundle[b].alphaGen))
				return qfalse;
		}
		if (stage->tessFlags & TESS_RGBA0)
			svarsSize += sizeof(color4ub_t);
		if (stage->tessFlags & TESS_RGBA1)
			svarsSize += sizeof(color4ub_t);
		if (stage->tessFlags & TESS_RGBA2)
			svarsSize += sizeof(color4ub_t);
		if (stage->tessFlags & TESS_ST0)
			svarsSize += sizeof(vec2_t);
		if (stage->tessFlags & TESS_ST1)
			svarsSize += sizeof(vec2_t);
		if (stage->tessFlags & TESS_ST2)
			svarsSize += sizeof(vec2_t);
	}

	if (i == 0)
		return qfalse;

	shader->isStaticShader = qtrue;

	// TODO: alloc separate structure?
	shader->svarsSize = svarsSize;
	shader->iboOffset = -1;
	shader->vboOffset = -1;
	shader->curIndexes = 0;
	shader->curVertexes = 0;
	shader->numIndexes = 0;
	shader->numVertexes = 0;

	return qtrue;
}

static void VBO_AddGeometry(vbo_t *vbo, vbo_item_t *vi, shaderCommands_t *input)
{
	uint32_t size, offs;
	uint32_t offs_st[NUM_TEXTURE_BUNDLES];
	uint32_t offs_cl[NUM_TEXTURE_BUNDLES];
	int i;

	offs_st[0] = offs_st[1] = offs_st[2] = 0;
	offs_cl[0] = offs_cl[1] = offs_cl[2] = 0;

	if (input->shader->iboOffset == -1 || input->shader->vboOffset == -1) {

		// allocate indexes
		input->shader->iboOffset = vbo->vbo_offset;
		vbo->vbo_offset += input->shader->numIndexes * sizeof(input->indexes[0]);

		// allocate xyz + normals + svars
		input->shader->vboOffset = vbo->vbo_offset;
		vbo->vbo_offset += input->shader->numVertexes * (sizeof(input->xyz[0]) + sizeof(input->normal[0]) + input->shader->svarsSize);

		// go to normals offset
		input->shader->normalOffset = input->shader->vboOffset + input->shader->numVertexes * sizeof(input->xyz[0]);

		// go to first color offset
		offs = input->shader->normalOffset + input->shader->numVertexes * sizeof(input->normal[0]);

		for (i = 0; i < MAX_VBO_STAGES; i++)
		{
			shaderStage_t *pStage = input->xstages[i];
			if (!pStage)
				break;

			if (pStage->tessFlags & TESS_RGBA0) {
				offs_cl[0] = offs;
				pStage->rgb_offset[0] = offs; offs += input->shader->numVertexes * sizeof(color4ub_t);
			}
			else {
				pStage->rgb_offset[0] = offs_cl[0];
			}

			if (pStage->tessFlags & TESS_RGBA1) {
				offs_cl[1] = offs;
				pStage->rgb_offset[1] = offs; offs += input->shader->numVertexes * sizeof(color4ub_t);
			}
			else {
				pStage->rgb_offset[1] = offs_cl[1];
			}

			if (pStage->tessFlags & TESS_RGBA2) {
				offs_cl[2] = offs;
				pStage->rgb_offset[2] = offs; offs += input->shader->numVertexes * sizeof(color4ub_t);
			}
			else {
				pStage->rgb_offset[2] = offs_cl[2];
			}

			if (pStage->tessFlags & TESS_ST0) {
				offs_st[0] = offs;
				pStage->tex_offset[0] = offs; offs += input->shader->numVertexes * sizeof(vec2_t);
			}
			else {
				pStage->tex_offset[0] = offs_st[0];
			}
			if (pStage->tessFlags & TESS_ST1) {
				offs_st[1] = offs;
				pStage->tex_offset[1] = offs; offs += input->shader->numVertexes * sizeof(vec2_t);
			}
			else {
				pStage->tex_offset[1] = offs_st[1];
			}
			if (pStage->tessFlags & TESS_ST2) {
				offs_st[2] = offs;
				pStage->tex_offset[2] = offs; offs += input->shader->numVertexes * sizeof(vec2_t);
			}
			else {
				pStage->tex_offset[2] = offs_st[2];
			}
		}

		input->shader->curVertexes = 0;
		input->shader->curIndexes = 0;
	}

	// shift indexes relative to current shader
	for (i = 0; i < input->numIndexes; i++)
		input->indexes[i] += input->shader->curVertexes;

	if (vi->index_offset == -1) // one-time initialization
	{
		// initialize geometry offsets relative to current shader
		vi->index_offset = input->shader->curIndexes;
		vi->soft_offset = vbo->ibo_offset;
	}

	offs = input->shader->iboOffset + input->shader->curIndexes * sizeof(input->indexes[0]);
	size = input->numIndexes * sizeof(input->indexes[0]);
	if (offs + size > vbo->vbo_size) {
		ri.Error(ERR_DROP, "Index0 overflow");
	}
	memcpy(vbo->vbo_buffer + offs, input->indexes, size);

	// fill soft buffer too
	if (vbo->ibo_offset + size > vbo->ibo_size) {
		ri.Error(ERR_DROP, "Index1 overflow");
	}
	memcpy(vbo->ibo_buffer + vbo->ibo_offset, input->indexes, size);
	vbo->ibo_offset += size;
	//Com_Printf( "i offs=%i size=%i\n", offs, size );

	// vertexes
	offs = input->shader->vboOffset + input->shader->curVertexes * sizeof(input->xyz[0]);
	size = input->numVertexes * sizeof(input->xyz[0]);
	if (offs + size > vbo->vbo_size) {
		ri.Error(ERR_DROP, "Vertex overflow");
	}
	//Com_Printf( "v offs=%i size=%i\n", offs, size );
	memcpy(vbo->vbo_buffer + offs, input->xyz, size);

	// normals
	offs = input->shader->normalOffset + input->shader->curVertexes * sizeof(input->normal[0]);
	size = input->numVertexes * sizeof(input->normal[0]);
	if (offs + size > vbo->vbo_size) {
		ri.Error(ERR_DROP, "Normals overflow");
	}
	//Com_Printf( "v offs=%i size=%i\n", offs, size );
	memcpy(vbo->vbo_buffer + offs, input->normal, size);

	vi->num_indexes += input->numIndexes;
	vi->num_vertexes += input->numVertexes;
}

static void VBO_AddStageColors(vbo_t *vbo, const int stage, const shaderCommands_t *input, const int bundle)
{
	const int offs = input->xstages[stage]->rgb_offset[bundle] + input->shader->curVertexes * sizeof(color4ub_t);
	const int size = input->numVertexes * sizeof(color4ub_t);

	memcpy(vbo->vbo_buffer + offs, input->svars.colors[bundle], size);
}

static void VBO_AddStageTxCoords(vbo_t *vbo, const int stage, const shaderCommands_t *input, const int bundle)
{
	const int offs = input->xstages[stage]->tex_offset[bundle] + input->shader->curVertexes * sizeof(vec2_t);
	const int size = input->numVertexes * sizeof(vec2_t);

	memcpy(vbo->vbo_buffer + offs, input->svars.texcoordPtr[bundle], size);
}

void VBO_PushData( int itemIndex, shaderCommands_t *input)
{
	const shaderStage_t *pStage;
	vbo_t *vbo = &world_vbo;
	vbo_item_t *vi = vbo->items + itemIndex;
	int i;
	VBO_AddGeometry(vbo, vi, input);
	for (i = 0; i < MAX_VBO_STAGES; i++)
	{
		pStage = input->xstages[i];
		if (!pStage)
			break;

		if (pStage->tessFlags & TESS_RGBA0)
		{
			ComputeColors(0, tess.svars.colors[0], pStage, 0);
			VBO_AddStageColors(vbo, i, input, 0);
		}
		if (pStage->tessFlags & TESS_RGBA1)
		{
			ComputeColors(1, tess.svars.colors[1], pStage, 0);
			VBO_AddStageColors(vbo, i, input, 1);
		}
		if (pStage->tessFlags & TESS_RGBA2)
		{
			ComputeColors(2, tess.svars.colors[2], pStage, 0);
			VBO_AddStageColors(vbo, i, input, 2);
		}

		if (pStage->tessFlags & TESS_ST0)
		{
			ComputeTexCoords(0, &pStage->bundle[0]);
			VBO_AddStageTxCoords(vbo, i, input, 0);
		}
		if (pStage->tessFlags & TESS_ST1)
		{
			ComputeTexCoords(1, &pStage->bundle[1]);
			VBO_AddStageTxCoords(vbo, i, input, 1);
		}
		if (pStage->tessFlags & TESS_ST2)
		{
			ComputeTexCoords(2, &pStage->bundle[2]);
			VBO_AddStageTxCoords(vbo, i, input, 2);
		}
	}
	input->shader->curVertexes += input->numVertexes;
	input->shader->curIndexes += input->numIndexes;

	//Com_Printf( "%s: vert %i (of %i), ind %i (of %i)\n", input->shader->name, 
	//	input->shader->curVertexes, input->shader->numVertexes,
	//	input->shader->curIndexes, input->shader->numIndexes );
}

void VBO_UnBind(void)
{
	tess.vbo_world_index = 0;
}

static int surfSortFunc(const void *a, const void *b)
{
	const msurface_t **sa = (const msurface_t **)a;
	const msurface_t **sb = (const msurface_t **)b;
	return (*sa)->shader - (*sb)->shader;
}

static void initItem(vbo_item_t *item)
{
	item->num_vertexes = 0;
	item->num_indexes = 0;

	item->index_offset = -1;
	item->soft_offset = -1;
}

#ifdef USE_VBO_GHOUL2

static inline float G2_GetVertBoneWeightNotSlow( const mdxmVertex_t *vert, const int index )
{
	int weight = vert->BoneWeightings[index];
	weight |= ( vert->uiNmWeightsAndBoneIndexes >> ( iG2_BONEWEIGHT_TOPBITS_SHIFT + ( index * 2 ) ) ) & iG2_BONEWEIGHT_TOPBITS_AND;

	return fG2_BONEWEIGHT_RECIPROCAL_MULT * weight;
}

static void vk_release_model_vbo( uint32_t index )
{
	if ( !tr.vbos[index] )
		return;

	if ( tr.vbos[index]->buffer )
		qvkDestroyBuffer( vk.device, tr.vbos[index]->buffer, NULL );
	
	if ( tr.vbos[index]->memory )
		qvkFreeMemory( vk.device, tr.vbos[index]->memory, NULL );

	tr.vbos[index]->memory = VK_NULL_HANDLE;
	tr.vbos[index]->buffer = VK_NULL_HANDLE;
}

static void vk_release_model_ibo( uint32_t index )
{
	if ( !tr.ibos[index] )
		return;

	if ( tr.ibos[index]->buffer )
		qvkDestroyBuffer( vk.device, tr.ibos[index]->buffer, NULL );
	
	if ( tr.ibos[index]->memory )
		qvkFreeMemory( vk.device, tr.ibos[index]->memory, NULL );

	tr.ibos[index]->memory = VK_NULL_HANDLE;
	tr.ibos[index]->buffer = VK_NULL_HANDLE;
}

void vk_release_model_vbo( void ) {
	uint32_t i;

	for ( i = 0 ; i < tr.numVBOs; i++ ) {
		vk_release_model_vbo( i );
		vk_release_model_ibo( i );
	}

	tr.numVBOs = 0;
	tr.numIBOs = 0;
}

IBO_t *R_CreateIBO( const char *name, const byte *vbo_data, int vbo_size )
{
	VkMemoryRequirements vb_mem_reqs;
	VkMemoryAllocateInfo alloc_info;
	VkBufferCreateInfo desc;
	VkDeviceSize vertex_buffer_offset;
	VkDeviceSize allocationSize;
	uint32_t memory_type_bits;
	VkBuffer staging_vertex_buffer;
	VkDeviceMemory staging_buffer_memory;
	VkCommandBuffer command_buffer;
	VkBufferCopy copyRegion[1];
	void *data;

	IBO_t          *ibo;

	if ( tr.numIBOs == MAX_VBOS ) {
		ri.Error( ERR_DROP, "R_CreateVBO: MAX_VBOS hit");

	}
	vk_release_model_ibo( tr.numIBOs );

	ibo = tr.ibos[tr.numIBOs] = (IBO_t *)ri.Hunk_Alloc(sizeof(*ibo), h_low);

	desc.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	desc.pNext = NULL;
	desc.flags = 0;
	desc.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	desc.queueFamilyIndexCount = 0;
	desc.pQueueFamilyIndices = NULL;

	// device-local buffer
	desc.size = vbo_size;
	desc.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
	VK_CHECK( qvkCreateBuffer( vk.device, &desc, NULL, &tr.ibos[tr.numIBOs]->buffer ) );
	
	// staging buffer
	desc.size = vbo_size;
	desc.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
	VK_CHECK(qvkCreateBuffer(vk.device, &desc, NULL, &staging_vertex_buffer));

	// memory requirements
	qvkGetBufferMemoryRequirements( vk.device, tr.ibos[tr.numIBOs]->buffer, &vb_mem_reqs );
	vertex_buffer_offset = 0;
	allocationSize = vertex_buffer_offset + vb_mem_reqs.size;
	memory_type_bits = vb_mem_reqs.memoryTypeBits;

	alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	alloc_info.pNext = NULL;
	alloc_info.allocationSize = allocationSize;
	alloc_info.memoryTypeIndex = vk_find_memory_type(memory_type_bits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	VK_CHECK(qvkAllocateMemory( vk.device, &alloc_info, NULL, &tr.ibos[tr.numIBOs]->memory));
	qvkBindBufferMemory( vk.device, tr.ibos[tr.numIBOs]->buffer, tr.ibos[tr.numIBOs]->memory, vertex_buffer_offset );
	// staging buffers

	// memory requirements
	qvkGetBufferMemoryRequirements(vk.device, staging_vertex_buffer, &vb_mem_reqs);
	vertex_buffer_offset = 0;
	allocationSize = vertex_buffer_offset + vb_mem_reqs.size;
	memory_type_bits = vb_mem_reqs.memoryTypeBits;

	alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	alloc_info.pNext = NULL;
	alloc_info.allocationSize = allocationSize;
	alloc_info.memoryTypeIndex = vk_find_memory_type(memory_type_bits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	VK_CHECK(qvkAllocateMemory(vk.device, &alloc_info, NULL, &staging_buffer_memory));
	qvkBindBufferMemory(vk.device, staging_vertex_buffer, staging_buffer_memory, vertex_buffer_offset);

	VK_CHECK(qvkMapMemory(vk.device, staging_buffer_memory, 0, VK_WHOLE_SIZE, 0, &data));
	memcpy((byte*)data + vertex_buffer_offset, vbo_data, vbo_size);
	qvkUnmapMemory(vk.device, staging_buffer_memory);

	command_buffer = vk_begin_command_buffer();
	copyRegion[0].srcOffset = 0;
	copyRegion[0].dstOffset = 0;
	copyRegion[0].size = vbo_size;
	qvkCmdCopyBuffer( command_buffer, staging_vertex_buffer, tr.ibos[tr.numIBOs]->buffer, 1, &copyRegion[0] );
	vk_end_command_buffer(command_buffer);

	qvkDestroyBuffer(vk.device, staging_vertex_buffer, NULL);
	qvkFreeMemory(vk.device, staging_buffer_memory, NULL);

	VK_SET_OBJECT_NAME( tr.ibos[tr.numIBOs]->buffer, va( "static IBO[2] %s", name ), VK_DEBUG_REPORT_OBJECT_TYPE_BUFFER_EXT );
	VK_SET_OBJECT_NAME( tr.ibos[tr.numIBOs]->memory, va( "static IBO[2] memory %s", name ), VK_DEBUG_REPORT_OBJECT_TYPE_DEVICE_MEMORY_EXT );

	tr.numIBOs++;

	return ibo;
}

VBO_t *R_CreateVBO( const char *name, const byte *vbo_data, int vbo_size )
{
	VkMemoryRequirements vb_mem_reqs;
	VkMemoryAllocateInfo alloc_info;
	VkBufferCreateInfo desc;
	VkDeviceSize vertex_buffer_offset;
	VkDeviceSize allocationSize;
	uint32_t memory_type_bits;
	VkBuffer staging_vertex_buffer;
	VkDeviceMemory staging_buffer_memory;
	VkCommandBuffer command_buffer;
	VkBufferCopy copyRegion[1];
	void *data;

	VBO_t          *vbo;

	if ( tr.numVBOs == MAX_VBOS ) {
		ri.Error( ERR_DROP, "R_CreateVBO: MAX_VBOS hit");

	}

	vk_release_model_vbo( tr.numVBOs );

	vbo = tr.vbos[tr.numVBOs] = (VBO_t *)ri.Hunk_Alloc(sizeof(*vbo), h_low);

	desc.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	desc.pNext = NULL;
	desc.flags = 0;
	desc.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	desc.queueFamilyIndexCount = 0;
	desc.pQueueFamilyIndices = NULL;

	// device-local buffer
	desc.size = vbo_size;
	desc.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
	VK_CHECK( qvkCreateBuffer( vk.device, &desc, NULL, &tr.vbos[tr.numVBOs]->buffer ) );
	
	// staging buffer
	desc.size = vbo_size;
	desc.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
	VK_CHECK(qvkCreateBuffer(vk.device, &desc, NULL, &staging_vertex_buffer));

	// memory requirements
	qvkGetBufferMemoryRequirements( vk.device, tr.vbos[tr.numVBOs]->buffer, &vb_mem_reqs );
	vertex_buffer_offset = 0;
	allocationSize = vertex_buffer_offset + vb_mem_reqs.size;
	memory_type_bits = vb_mem_reqs.memoryTypeBits;

	alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	alloc_info.pNext = NULL;
	alloc_info.allocationSize = allocationSize;
	alloc_info.memoryTypeIndex = vk_find_memory_type(memory_type_bits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	VK_CHECK(qvkAllocateMemory( vk.device, &alloc_info, NULL, &tr.vbos[tr.numVBOs]->memory));
	qvkBindBufferMemory( vk.device, tr.vbos[tr.numVBOs]->buffer, tr.vbos[tr.numVBOs]->memory, vertex_buffer_offset );
	// staging buffers

	// memory requirements
	qvkGetBufferMemoryRequirements(vk.device, staging_vertex_buffer, &vb_mem_reqs);
	vertex_buffer_offset = 0;
	allocationSize = vertex_buffer_offset + vb_mem_reqs.size;
	memory_type_bits = vb_mem_reqs.memoryTypeBits;

	alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	alloc_info.pNext = NULL;
	alloc_info.allocationSize = allocationSize;
	alloc_info.memoryTypeIndex = vk_find_memory_type(memory_type_bits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	VK_CHECK(qvkAllocateMemory(vk.device, &alloc_info, NULL, &staging_buffer_memory));
	qvkBindBufferMemory(vk.device, staging_vertex_buffer, staging_buffer_memory, vertex_buffer_offset);

	VK_CHECK(qvkMapMemory(vk.device, staging_buffer_memory, 0, VK_WHOLE_SIZE, 0, &data));
	memcpy((byte*)data + vertex_buffer_offset, vbo_data, vbo_size);
	qvkUnmapMemory(vk.device, staging_buffer_memory);

	command_buffer = vk_begin_command_buffer();
	copyRegion[0].srcOffset = 0;
	copyRegion[0].dstOffset = 0;
	copyRegion[0].size = vbo_size;
	qvkCmdCopyBuffer( command_buffer, staging_vertex_buffer, tr.vbos[tr.numVBOs]->buffer, 1, &copyRegion[0] );
	vk_end_command_buffer(command_buffer);

	qvkDestroyBuffer(vk.device, staging_vertex_buffer, NULL);
	qvkFreeMemory(vk.device, staging_buffer_memory, NULL);

	VK_SET_OBJECT_NAME( tr.vbos[tr.numVBOs]->buffer, va( "static VBO[2] %s", name ), VK_DEBUG_REPORT_OBJECT_TYPE_BUFFER_EXT );
	VK_SET_OBJECT_NAME( tr.vbos[tr.numVBOs]->memory, va( "static VBO[2] memory %s", name ), VK_DEBUG_REPORT_OBJECT_TYPE_DEVICE_MEMORY_EXT );

	vbo->index = tr.numVBOs++;
	vbo->index++;
	return vbo;
}

/*
static void VBO_CalculateTangentsMDXM( const mdxmSurface_t *surf, vec4_t *tangentsf )
{
	vec3_t	*xyz0, *xyz1, *xyz2;
	vec2_t	*st0, *st1, *st2;
	vec3_t	*normal0, *normal1, *normal2;
	float	*qtangent0, *qtangent1, *qtangent2;
	int		i, i0, i1, i2;
	vec3_t	tangent, binormal;

	mdxmTriangle_t *triangle = (mdxmTriangle_t *)((byte *)surf + surf->ofsTriangles);
	mdxmVertex_t *vert = (mdxmVertex_t*)( (byte*)surf + surf->ofsVerts );
	mdxmVertexTexCoord_t *tc = (mdxmVertexTexCoord_t*)(vert + surf->numVerts);
	
	// revisit this
	for ( i = 0; i < surf->numTriangles; i++ ) 
	{
		i0 = triangle[i].indexes[0];
		i1 = triangle[i].indexes[1];
		i2 = triangle[i].indexes[2];

		if ( i0 >= surf->numVerts || i1 >= surf->numVerts || i2 >= surf->numVerts )
			continue;

		xyz0 = &vert[i0].vertCoords;
		xyz1 = &vert[i1].vertCoords;
		xyz2 = &vert[i2].vertCoords;

		normal0 = &vert[i0].normal;
		normal1 = &vert[i1].normal;
		normal2 = &vert[i2].normal;

		st0 = &tc[i0].texCoords;
		st1 = &tc[i1].texCoords;
		st2 = &tc[i2].texCoords;

		qtangent0 = *(tangentsf + i0);
		qtangent1 = *(tangentsf + i1);
		qtangent2 = *(tangentsf + i2);
		
		R_CalcTangents( tangent, binormal,
			(float*)xyz0, (float*)xyz1, (float*)xyz2, 
			(float*)st0, (float*)st1, (float*)st2 );

		if ( tangent[0] == 0.0f && tangent[1] == 0.0f && tangent[2] == 0.0f ){
			continue;
		}

		R_TBNtoQtangents( tangent, binormal, (float*)normal0, qtangent0 );
		R_TBNtoQtangents( tangent, binormal, (float*)normal1, qtangent1 );
		R_TBNtoQtangents( tangent, binormal, (float*)normal2, qtangent2 );
	}
}
*/

typedef struct mdxm_attributes_s {
	vec4_t	*verts;
	vec4_t	*normals;
	vec2_t	*texcoords;
	byte	*weights;
	byte	*bonerefs;
	vec4_t	*tangents;
} mdxm_attributes_t;

typedef struct mdv_attributes_s {
	vec4_t *verts;
	vec4_t *normals;
	vec2_t *texcoords;
	vec4_t *tangents;
} mdv_attributes_t;

// in vulkan, pipelines like fog are created before a model is loaded
// therefor the stride is undefined in vk_push_bind() during creation
// use these methods to calculate the strides beforehand.
// apply attribute type changes from R_BuildMDXM or R_BuildMD3 here as well
int get_mdxm_stride( void ) {
	int stride = 0;

	if ( vk.ghoul2_vbo_stride )
		return vk.ghoul2_vbo_stride;

	const mdxm_attributes_t attr = {};

	stride += sizeof (*attr.verts);
	stride += sizeof (*attr.normals);
	stride += sizeof (*attr.texcoords);
	stride += sizeof (*attr.bonerefs) * 4;
	stride += sizeof (*attr.weights) * 4;
	stride += sizeof (*attr.tangents);
	
	vk.ghoul2_vbo_stride = stride;

	return stride;
}

int get_mdv_stride( void ) {
	int stride = 0;

	if ( vk.mdv_vbo_stride )
		return vk.mdv_vbo_stride;

	const mdv_attributes_t attr = {};

	stride += sizeof(*attr.verts);
	stride += sizeof(*attr.normals);
	stride += sizeof(*attr.texcoords);
	stride += sizeof(*attr.tangents);
	
	vk.mdv_vbo_stride = stride;

	return stride;
}


void R_BuildMDXM( model_t *mod, mdxmHeader_t *mdxm )
{
	if( !vk.vboGhoul2Active )
		return;

	mdxmVBOModel_t		*vboModel;
	mdxmSurface_t		*surf;
	mdxmLOD_t			*lod;
	uint32_t			i, n, k, w;

	lod = (mdxmLOD_t *)( (byte *)mdxm + mdxm->ofsLODs );
	mod->data.glm->vboModels = (mdxmVBOModel_t *)ri.Hunk_Alloc( sizeof (mdxmVBOModel_t) * mdxm->numLODs, h_low );

	for ( i = 0; i < mdxm->numLODs; i++ )
	{
		vboModel = &mod->data.glm->vboModels[i];
		mdxmVBOMesh_t *vboMeshes;

		vboModel->numVBOMeshes = mdxm->numSurfaces;
		vboModel->vboMeshes = (mdxmVBOMesh_t *)ri.Hunk_Alloc( sizeof (mdxmVBOMesh_t) * (mdxm->numSurfaces ), h_low );
		vboMeshes = vboModel->vboMeshes;
		
		surf = (mdxmSurface_t *)( (byte *)lod + sizeof (mdxmLOD_t) + ( mdxm->numSurfaces * sizeof (mdxmLODSurfOffset_t) ) );		
		
		mdxm_attributes_t attr = {};

		byte *data;
		int dataSize = 0;
		int ofsPosition, ofsNormals, ofsTexcoords, ofsBoneRefs, ofsWeights, ofsTangents;
		int stride = 0;
		int numVerts = 0;
		int numTriangles = 0;

		// +1 to add total vertex count
		int *baseVertexes = (int *)ri.Hunk_AllocateTempMemory (sizeof (int) * (mdxm->numSurfaces + 1));
		int *indexOffsets = (int *)ri.Hunk_AllocateTempMemory (sizeof (int) * mdxm->numSurfaces);


		// Calculate the required size of the vertex buffer.
		for ( n = 0; n < mdxm->numSurfaces; n++ )
		{
			baseVertexes[n] = numVerts;
			indexOffsets[n] = numTriangles * 3;

			numVerts += surf->numVerts;
			numTriangles += surf->numTriangles;

			surf = (mdxmSurface_t *)((byte *)surf + surf->ofsEnd);
		}

		baseVertexes[mdxm->numSurfaces] = numVerts;

		dataSize += numVerts * sizeof (*attr.verts);
		dataSize += numVerts * sizeof (*attr.normals);
		dataSize += numVerts * sizeof (*attr.texcoords);
		dataSize += numVerts * sizeof (*attr.weights) * 4;
		dataSize += numVerts * sizeof (*attr.bonerefs) * 4;
		dataSize += numVerts * sizeof (*attr.tangents);

		//dataSize = PAD(dataSize, 32);

		// Allocate and write to memory
		data = (byte *)ri.Hunk_AllocateTempMemory (dataSize);

		ofsPosition = stride;
		attr.verts = (vec4_t *)(data + ofsPosition);
		stride += sizeof (*attr.verts);

		ofsNormals = stride;
		attr.normals = (vec4_t *)(data + ofsNormals);
		stride += sizeof (*attr.normals);

		ofsTexcoords = stride;
		attr.texcoords = (vec2_t *)(data + ofsTexcoords);
		stride += sizeof (*attr.texcoords);

		ofsBoneRefs = stride;
		attr.bonerefs = data + ofsBoneRefs;
		stride += sizeof (*attr.bonerefs) * 4;

		ofsWeights = stride;
		attr.weights = data + ofsWeights;
		stride += sizeof (*attr.weights) * 4;

		ofsTangents = stride;
		attr.tangents = (vec4_t *)(data + ofsTangents);
		stride += sizeof (*attr.tangents);

		// Fill in the index buffer and compute tangents
		uint32_t *indices = (uint32_t *)ri.Hunk_AllocateTempMemory(sizeof(uint32_t) * numTriangles * 3);
		uint32_t *index = indices;
		vec4_t *tangentsf = (vec4_t *)ri.Hunk_AllocateTempMemory(sizeof(vec4_t) * numVerts);

		surf = (mdxmSurface_t *)((byte *)lod + sizeof(mdxmLOD_t) + (mdxm->numSurfaces * sizeof(mdxmLODSurfOffset_t)));

		for ( n = 0; n < mdxm->numSurfaces; n++ )
		{
			mdxmTriangle_t *t = (mdxmTriangle_t *)((byte *)surf + surf->ofsTriangles);

			for ( k = 0; k < surf->numTriangles; k++, index += 3 )
			{
				index[0] = t[k].indexes[0] + baseVertexes[n];
				assert( index[0] >= 0 && index[0] < numVerts );

				index[1] = t[k].indexes[1] + baseVertexes[n];
				assert( index[1] >= 0 && index[1] < numVerts );

				index[2] = t[k].indexes[2] + baseVertexes[n];
				assert( index[2] >= 0 && index[2] < numVerts );
			}

			// Build tangent space
			//VBO_CalculateTangentsMDXM( surf, tangentsf + baseVertexes[n] );

			surf = (mdxmSurface_t *)((byte *)surf + surf->ofsEnd);
		}

		assert( index == (indices + numTriangles * 3) );

		surf = (mdxmSurface_t *)((byte *)lod + sizeof (mdxmLOD_t) + (mdxm->numSurfaces * sizeof (mdxmLODSurfOffset_t)));

		for ( n = 0; n < mdxm->numSurfaces; n++ )
		{
			// Positions and normals
			mdxmVertex_t *v = (mdxmVertex_t *)((byte *)surf + surf->ofsVerts);
			int *boneRef = (int *)((byte *)surf + surf->ofsBoneReferences);

			for ( k = 0; k < surf->numVerts; k++ )
			{
				VectorCopy( v[k].vertCoords, *attr.verts );
				VectorCopy( v[k].normal, *attr.normals );

				attr.verts = (vec4_t *)((byte *)attr.verts + stride);
				attr.normals = (vec4_t *)((byte *)attr.normals + stride);
			}

			// Weights
			for ( k = 0; k < surf->numVerts; k++ )
			{
				int numWeights = G2_GetVertWeights(&v[k]);
				int lastWeight = 255;
				int lastInfluence = numWeights - 1;
				for ( w = 0; w < lastInfluence; w++ )
				{
					float weight = G2_GetVertBoneWeightNotSlow( &v[k], w );
					int packedIndex = G2_GetVertBoneIndex( &v[k], w );

					attr.weights[w] = (byte)(weight * 255.0f);		
					attr.bonerefs[w] = boneRef[packedIndex];

					lastWeight -= attr.weights[w];
				}

				assert(lastWeight > 0);

				// Ensure that all the weights add up to 1.0
				attr.weights[lastInfluence] = lastWeight;
				int packedIndex = G2_GetVertBoneIndex(&v[k], lastInfluence);
				attr.bonerefs[lastInfluence] = boneRef[packedIndex];

				// Fill in the rest of the info with zeroes.
				for ( w = numWeights; w < 4; w++ )
				{
					attr.weights[w] = 0;
					attr.bonerefs[w] = 0;
				}

				attr.weights += stride;
				attr.bonerefs += stride;
			}

			// Texture coordinates
			mdxmVertexTexCoord_t *tc = (mdxmVertexTexCoord_t *)(v + surf->numVerts);
			for ( k = 0; k < surf->numVerts; k++ )
			{
				(*attr.texcoords)[0] = tc[k].texCoords[0];
				(*attr.texcoords)[1] = tc[k].texCoords[1];

				attr.texcoords = (vec2_t *)((byte *)attr.texcoords + stride);
			}

			for ( k = 0; k < surf->numVerts; k++ )
			{
				VectorCopy4( (float*)(tangentsf + baseVertexes[n] + k), *attr.tangents );
				attr.tangents = (vec4_t *)((byte *)attr.tangents + stride);
			}

			surf = (mdxmSurface_t *)((byte *)surf + surf->ofsEnd);
		}

		assert ((byte *)attr.verts == (data + dataSize));

		const char *modelName = strrchr( mdxm->name, '/' );
		if ( modelName == NULL )
			modelName = mdxm->name;

		VBO_t *vbo = R_CreateVBO( modelName, data, dataSize );
		IBO_t *ibo = R_CreateIBO( modelName, (byte *)indices, sizeof(uint32_t) * numTriangles * 3 );

		ri.Hunk_FreeTempMemory ( data );
		ri.Hunk_FreeTempMemory ( tangentsf );
		ri.Hunk_FreeTempMemory ( indices );

		vbo->offsets[0] = ofsPosition;
		vbo->offsets[5] = ofsNormals;
		vbo->offsets[2] = ofsTexcoords;
		vbo->offsets[8] = ofsBoneRefs;
		vbo->offsets[9] = ofsWeights;
		//vbo->offsets[8] = ofsTangents;

		surf = (mdxmSurface_t *)((byte *)lod + sizeof (mdxmLOD_t) + (mdxm->numSurfaces * sizeof (mdxmLODSurfOffset_t)));

		for ( n = 0; n < mdxm->numSurfaces; n++ )
		{
			vboMeshes[n].vbo = vbo;
			vboMeshes[n].ibo = ibo;

			vboMeshes[n].indexOffset = indexOffsets[n];
			vboMeshes[n].minIndex = baseVertexes[n];
			vboMeshes[n].maxIndex = baseVertexes[n + 1] - 1;
			vboMeshes[n].numVertexes = surf->numVerts;
			vboMeshes[n].numIndexes = surf->numTriangles * 3;

			surf = (mdxmSurface_t *)((byte *)surf + surf->ofsEnd);
		}

		vboModel->vbo = vbo;
		vboModel->ibo = ibo;

		ri.Hunk_FreeTempMemory ( indexOffsets );
		ri.Hunk_FreeTempMemory ( baseVertexes );

		// find the next LOD
		lod = (mdxmLOD_t *)( (byte *)lod + lod->ofsEnd );
	}

	return;
}
#endif

/*
static void VBO_CalculateTangentsMD3( const mdvSurface_t *surf, vec4_t *tangentsf )
{
	mdvVertex_t	*dv0, *dv1, *dv2;
	float		*st0, *st1, *st2;
	float	*qtangent0, *qtangent1, *qtangent2;
	int			i, i0, i1, i2;
	vec3_t		tangent, binormal;

	for ( i = 0; i < surf->numIndexes; i += 3 ) 
	{
		i0 = surf->indexes[ i + 0 ];
		i1 = surf->indexes[ i + 1 ];
		i2 = surf->indexes[ i + 2 ];

		if ( i0 >= surf->numVerts || i1 >= surf->numVerts || i2 >= surf->numVerts )
			continue;

		dv0 = &surf->verts[i0];
		dv1 = &surf->verts[i1];
		dv2 = &surf->verts[i2];

		st0 = surf->st[i0].st;
		st1 = surf->st[i1].st;
		st2 = surf->st[i2].st;

		qtangent0 = *(tangentsf + i0);
		qtangent1 = *(tangentsf + i1);
		qtangent2 = *(tangentsf + i2);

		R_CalcTangents( tangent, binormal,
			dv0->xyz, dv1->xyz, dv2->xyz, 
			st0, st1, st2 );

		R_TBNtoQtangents( tangent, binormal, dv0->normal, qtangent0 );
		R_TBNtoQtangents( tangent, binormal, dv1->normal, qtangent1 );
		R_TBNtoQtangents( tangent, binormal, dv2->normal, qtangent2 );
	}
}
*/

void R_BuildMD3( model_t *mod, mdvModel_t *mdvModel ) 
{
	mdvVertex_t    *v;
	mdvSt_t        *st;
	mdvSurface_t   *surf;
	srfVBOMDVMesh_t *vboSurf;

	uint32_t		i, j, k;

	mdvModel->numVBOSurfaces = mdvModel->numSurfaces;
	mdvModel->vboSurfaces = (srfVBOMDVMesh_t *)ri.Hunk_Alloc(sizeof(*mdvModel->vboSurfaces) * mdvModel->numSurfaces, h_low);

	if ( !mdvModel->numSurfaces )
		return;

	vboSurf = mdvModel->vboSurfaces;
	surf = mdvModel->surfaces;

	mdv_attributes_t attr = {};

	byte *data;
	int dataSize = 0;
	int ofsPosition, ofsNormals, ofsTexcoords, ofsTangents;
	int stride = 0;
	int numVerts = 0;
	int numIndexes = 0;

	// +1 to add total vertex count
	int *baseVertexes = (int *)ri.Hunk_AllocateTempMemory(sizeof(int) * (mdvModel->numSurfaces + 1));
	int *indexOffsets = (int *)ri.Hunk_AllocateTempMemory(sizeof(int) * mdvModel->numSurfaces);

	// Calculate the required size of the vertex buffer.
	for (int n = 0; n < mdvModel->numSurfaces; n++, surf++)
	{
		baseVertexes[n] = numVerts;
		indexOffsets[n] = numIndexes;

		numVerts += surf->numVerts;
		numIndexes += surf->numIndexes;
	}
	baseVertexes[mdvModel->numSurfaces] = numVerts;

	dataSize += numVerts * sizeof(*attr.verts);
	dataSize += numVerts * sizeof(*attr.normals);
	dataSize += numVerts * sizeof(*attr.texcoords);
	dataSize += numVerts * sizeof(*attr.tangents);

	// Allocate and write to memory
	data = (byte *)ri.Hunk_AllocateTempMemory(dataSize);

	ofsPosition = stride;
	attr.verts = (vec4_t *)(data + ofsPosition);
	stride += sizeof(*attr.verts);

	ofsNormals = stride;
	attr.normals = (vec4_t *)(data + ofsNormals);
	stride += sizeof(*attr.normals);

	ofsTexcoords = stride;
	attr.texcoords = (vec2_t *)(data + ofsTexcoords);
	stride += sizeof(*attr.texcoords);

	ofsTangents = stride;
	attr.tangents = (vec4_t *)(data + ofsTangents);
	stride += sizeof(*attr.tangents);

	// Fill in the index buffer and compute tangents
	glIndex_t *indices = (glIndex_t *)ri.Hunk_AllocateTempMemory(sizeof(glIndex_t) * numIndexes);
	glIndex_t *index = indices;

	surf = mdvModel->surfaces;
	for (i = 0; i < mdvModel->numSurfaces; i++, surf++)
	{
		vec4_t *tangentsf = (vec4_t *)ri.Hunk_AllocateTempMemory(sizeof(vec4_t) * surf->numVerts);
		//VBO_CalculateTangentsMD3( surf, tangentsf + 0 );

		for ( k = 0; k < surf->numIndexes; k++)
		{
			*index = surf->indexes[k] + baseVertexes[i];
			assert(*index >= 0 && *index < numVerts);
			index++;
		}

		v = surf->verts;
		for ( j = 0; j < surf->numVerts; j++, v++ )
		{
			VectorCopy(v->xyz, *attr.verts);
			VectorCopy(v->normal, *attr.normals);
			VectorCopy4( (float*)(tangentsf + j), *attr.tangents );

			//*normals = R_VboPackNormal(v->normal);
			//*tangents = tangentsf[j];

			attr.verts = (vec4_t *)((byte *)attr.verts + stride);
			attr.normals = (vec4_t *)((byte *)attr.normals + stride);
			attr.tangents = (vec4_t *)((byte *)attr.tangents + stride);
		}
		ri.Hunk_FreeTempMemory(tangentsf);

		st = surf->st;
		for ( j = 0; j < surf->numVerts; j++, st++ ) 
		{
			(*attr.texcoords)[0] = st->st[0];
			(*attr.texcoords)[1] = st->st[1];

			attr.texcoords = (vec2_t *)((byte *)attr.texcoords + stride);
		}
	}

	assert((byte *)attr.verts == (data + dataSize));

	VBO_t *vbo = R_CreateVBO( mod->name, data, dataSize );
	IBO_t *ibo = R_CreateIBO( mod->name, (byte *)indices, sizeof(glIndex_t) * numIndexes );

	ri.Hunk_FreeTempMemory(data);
	ri.Hunk_FreeTempMemory(indices);

	vbo->offsets[0] = ofsPosition;
	vbo->offsets[5] = ofsNormals;
	vbo->offsets[2] = ofsTexcoords;
	vbo->offsets[8] = ofsTangents;

	surf = mdvModel->surfaces;
	for ( i = 0; i < mdvModel->numSurfaces; i++, surf++, vboSurf++ )
	{
		vboSurf->surfaceType = SF_VBO_MDVMESH;
		vboSurf->mdvModel = mdvModel;
		vboSurf->mdvSurface = surf;
		vboSurf->vbo = vbo;
		vboSurf->ibo = ibo;

		vboSurf->indexOffset = indexOffsets[i];
		vboSurf->minIndex = baseVertexes[i];
		vboSurf->maxIndex = baseVertexes[i + 1] - 1;
		vboSurf->numVerts = surf->numVerts;
		vboSurf->numIndexes = surf->numIndexes;
	}

	ri.Hunk_FreeTempMemory(indexOffsets);
	ri.Hunk_FreeTempMemory(baseVertexes);
}


void R_BuildWorldVBO(msurface_t *surf, int surfCount)
{
	vbo_t *vbo = &world_vbo;
	msurface_t **surfList;
	srfSurfaceFace_t *face;
	srfTriangles_t *tris;
	srfGridMesh_t *grid;
	msurface_t *sf;
	int ibo_size;
	int vbo_size;
	int i, n;

	int numStaticSurfaces = 0;
	int numStaticIndexes = 0;
	int numStaticVertexes = 0;

	if ( !vk.vboWorldActive )
		return;

	if (glConfig.maxActiveTextures < 3) {
		ri.Printf(PRINT_WARNING, "... not enough texture units for VBO\n");
		return;
	}

	VBO_Cleanup();
	
	Com_Memset( vbo, 0, sizeof( *vbo ) );

	vbo_size = 0;

	// initial scan to count surfaces/indexes/vertexes for memory allocation
	for (i = 0, sf = surf; i < surfCount; i++, sf++) {
		face = (srfSurfaceFace_t *)sf->data;
		if (face->surfaceType == SF_FACE && isStaticShader(sf->shader)) {
			face->vboItemIndex = ++numStaticSurfaces;
			numStaticVertexes += face->numPoints;
			numStaticIndexes += face->numIndices;

			vbo_size += face->numPoints * (sf->shader->svarsSize + sizeof(tess.xyz[0]) + sizeof(tess.normal[0]));
			sf->shader->numVertexes += face->numPoints;
			sf->shader->numIndexes += face->numIndices;
			continue;
		}
		tris = (srfTriangles_t *)sf->data;
		if (tris->surfaceType == SF_TRIANGLES && isStaticShader(sf->shader)) {
			tris->vboItemIndex = ++numStaticSurfaces;
			numStaticVertexes += tris->numVerts;
			numStaticIndexes += tris->numIndexes;

			vbo_size += tris->numVerts * (sf->shader->svarsSize + sizeof(tess.xyz[0]) + sizeof(tess.normal[0]));
			sf->shader->numVertexes += tris->numVerts;
			sf->shader->numIndexes += tris->numIndexes;
			continue;
		}
		grid = (srfGridMesh_t *)sf->data;
		if (grid->surfaceType == SF_GRID && isStaticShader(sf->shader)) {
			grid->vboItemIndex = ++numStaticSurfaces;
			RB_SurfaceGridEstimate(grid, &grid->vboExpectVertices, &grid->vboExpectIndices);
			numStaticVertexes += grid->vboExpectVertices;
			numStaticIndexes += grid->vboExpectIndices;

			vbo_size += grid->vboExpectVertices * (sf->shader->svarsSize + sizeof(tess.xyz[0]) + sizeof(tess.normal[0]));
			sf->shader->numVertexes += grid->vboExpectVertices;
			sf->shader->numIndexes += grid->vboExpectIndices;
			continue;
		}
	}

	if (numStaticSurfaces == 0) {
		ri.Printf(PRINT_ALL, "...no static surfaces for VBO\n");
		return;
	}

	vbo_size = PAD(vbo_size, 32);

	ibo_size = numStaticIndexes * sizeof(tess.indexes[0]);
	ibo_size = PAD(ibo_size, 32);

	// 0 item is unused
	vbo->items = (vbo_item_t*)ri.Hunk_Alloc((numStaticSurfaces + 1) * sizeof(vbo_item_t), h_low);
	vbo->items_count = numStaticSurfaces;

	// last item will be used for run length termination
	vbo->items_queue = (int*)ri.Hunk_Alloc((numStaticSurfaces + 1) * sizeof(int), h_low);
	vbo->items_queue_count = 0;

	ri.Printf(PRINT_ALL, "...found %i VBO surfaces (%i vertexes, %i indexes)\n",
		numStaticSurfaces, numStaticVertexes, numStaticIndexes);

	//Com_Printf( S_COLOR_CYAN "VBO size: %i\n", vbo_size );
	//Com_Printf( S_COLOR_CYAN "IBO size: %i\n", ibo_size );

	// vertex buffer
	vbo_size += ibo_size;
	vbo->vbo_buffer = (byte*)ri.Hunk_AllocateTempMemory(vbo_size);
	vbo->vbo_offset = 0;
	vbo->vbo_size = vbo_size;

	// index buffer
	vbo->ibo_buffer = (byte*)ri.Hunk_Alloc(ibo_size, h_low);
	vbo->ibo_offset = 0;
	vbo->ibo_size = ibo_size;

	// ibo runs buffer
	vbo->ibo_items = (ibo_item_t*)ri.Hunk_Alloc(((numStaticIndexes / MIN_IBO_RUN) + 1) * sizeof(ibo_item_t), h_low);
	vbo->ibo_items_count = 0;

	surfList = (msurface_t**)ri.Hunk_AllocateTempMemory(numStaticSurfaces * sizeof(msurface_t*));

	for (i = 0, n = 0, sf = surf; i < surfCount; i++, sf++) {
		face = (srfSurfaceFace_t *)sf->data;
		if (face->surfaceType == SF_FACE && face->vboItemIndex) {
			surfList[n++] = sf;
			continue;
		}
		tris = (srfTriangles_t *)sf->data;
		if (tris->surfaceType == SF_TRIANGLES && tris->vboItemIndex) {
			surfList[n++] = sf;
			continue;
		}
		grid = (srfGridMesh_t *)sf->data;
		if (grid->surfaceType == SF_GRID && grid->vboItemIndex) {
			surfList[n++] = sf;
			continue;
		}
	}

	if (n != numStaticSurfaces) {
		ri.Error(ERR_DROP, "Invalid VBO surface count");
	}

	// sort surfaces by shader
	qsort(surfList, numStaticSurfaces, sizeof(surfList[0]), surfSortFunc);

	tess.numIndexes = 0;
	tess.numVertexes = 0;

	Com_Memset(&backEnd.viewParms, 0, sizeof(backEnd.viewParms));
	backEnd.currentEntity = &tr.worldEntity;

	for (i = 0; i < numStaticSurfaces; i++)
	{
		sf = surfList[i];
		face = (srfSurfaceFace_t *)sf->data;
		tris = (srfTriangles_t *)sf->data;
		grid = (srfGridMesh_t *)sf->data;
		if (face->surfaceType == SF_FACE)
			face->vboItemIndex = i + 1;
		else if (tris->surfaceType == SF_TRIANGLES) {
			tris->vboItemIndex = i + 1;
		}
		else if (grid->surfaceType == SF_GRID) {
			grid->vboItemIndex = i + 1;
		}
		else {
			ri.Error(ERR_DROP, "Unexpected surface type");
		}
		initItem(vbo->items + i + 1);
		RB_BeginSurface(sf->shader, 0);
		tess.allowVBO = qfalse; // block execution of VBO path as we need to tesselate geometry
#ifdef USE_TESS_NEEDS_NORMAL
		tess.needsNormal = qtrue;
#endif
#ifdef USE_TESS_NEEDS_ST2
		tess.needsST2 = qtrue;
#endif
		// tesselate
		rb_surfaceTable[*sf->data](sf->data); // VBO_PushData() may be called multiple times there
		// setup colors and texture coordinates
		VBO_PushData( i + 1, &tess);
		if (grid->surfaceType == SF_GRID) {
			vbo_item_t *vi = vbo->items + i + 1;
			if (vi->num_vertexes != grid->vboExpectVertices || vi->num_indexes != grid->vboExpectIndices) {
				ri.Error(ERR_DROP, "Unexpected grid vertexes/indexes count");
			}
		}
		tess.numIndexes = 0;
		tess.numVertexes = 0;
	}

	ri.Hunk_FreeTempMemory(surfList);

	//__fail:
	vk_alloc_vbo( "world", vbo->vbo_buffer, vbo->vbo_size );

	//if ( err == GL_OUT_OF_MEMORY )
	//	ri.Printf( PRINT_WARNING, "%s: out of memory\n", __func__ );
	//else
	//	ri.Printf( PRINT_ERROR, "%s: error %i\n", __func__, err );
#if 0
	// reset vbo markers
	for (i = 0, sf = surf; i < surfCount; i++, sf++) {
		face = (srfBsp_t *)sf->data;
		if (face->surfaceType == SF_FACE) {
			face->vboItemIndex = 0;
			continue;
		}
		tris = (srfBsp_t *)sf->data;
		if (tris->surfaceType == SF_TRIANGLES) {
			tris->vboItemIndex = 0;
			continue;
		}
		grid = (srfBsp_t *)sf->data;
		if (grid->surfaceType == SF_GRID) {
			grid->vboItemIndex = 0;
			continue;
		}
	}
#endif

	// release host memory
	ri.Hunk_FreeTempMemory(vbo->vbo_buffer);
	vbo->vbo_buffer = NULL;

	// release GPU resources
	//VBO_Cleanup();
}

void VBO_Cleanup(void)
{
	int i;

	memset( &world_vbo, 0, sizeof(vbo_t) );

	for ( i = 0; i < tr.numShaders; i++ )
	{
		tr.shaders[i]->isStaticShader = qfalse;
		tr.shaders[i]->iboOffset = -1;
		tr.shaders[i]->vboOffset = -1;
	}
}

/*
=============
qsort_int
=============
*/
static void qsort_int(int *a, const int n) {
	int temp, m;
	int i, j;

	if (n < 32) { // CUTOFF
		for (i = 1; i < n + 1; i++) {
			j = i;
			while (j > 0 && a[j] < a[j - 1]) {
				temp = a[j];
				a[j] = a[j - 1];
				a[j - 1] = temp;
				j--;
			}
		}
		return;
	}

	i = 0;
	j = n;
	m = a[n >> 1];

	do {
		while (a[i] < m) i++;
		while (a[j] > m) j--;
		if (i <= j) {
			temp = a[i];
			a[i] = a[j];
			a[j] = temp;
			i++;
			j--;
		}
	} while (i <= j);

	if (j > 0) qsort_int(a, j);
	if (n > i) qsort_int(a + i, n - i);
}

static int run_length(const int *a, int from, int to, int *count)
{
	vbo_t *vbo = &world_vbo;;
	int i, n, cnt;
	for (cnt = 0, n = 1, i = from; i < to; i++, n++)
	{
		cnt += vbo->items[a[i]].num_indexes;
		if (a[i] + 1 != a[i + 1])
			break;
	}
	*count = cnt;
	return n;
}

void VBO_QueueItem(int itemIndex)
{
	vbo_t *vbo = &world_vbo;;

	if (vbo->items_queue_count < vbo->items_count)
	{
		vbo->items_queue[vbo->items_queue_count++] = itemIndex;
	}
	else
	{
		ri.Error(ERR_DROP, "VBO queue overflow");
	}

}

void VBO_ClearQueue(void)
{
	vbo_t *vbo = &world_vbo;
	vbo->items_queue_count = 0;
}

void VBO_Flush(void)
{
	if (!tess.vbo_world_index)
		return;

	RB_EndSurface();
	tess.vbo_world_index = 0;
	RB_BeginSurface(tess.shader, tess.fogNum);
}

static void VBO_AddItemDataToSoftBuffer(int itemIndex)
{
	vbo_t *vbo = &world_vbo;
	const vbo_item_t *vi = vbo->items + itemIndex;

	const uint32_t offset = vk_tess_index(vi->num_indexes, vbo->ibo_buffer + vi->soft_offset);

	if (vbo->soft_buffer_indexes == 0)
	{
		// start recording into host-visible memory
		vbo->soft_buffer_offset = offset;
	}

	vbo->soft_buffer_indexes += vi->num_indexes;
}

static void VBO_AddItemRangeToIBOBuffer(int offset, int length)
{
	vbo_t *vbo = &world_vbo;;
	ibo_item_t *it;

	it = vbo->ibo_items + vbo->ibo_items_count++;

	it->offset = offset;
	it->length = length;
}

void VBO_RenderIBOItems(void)
{
	const vbo_t *vbo = &world_vbo;
	int i;

	// from device-local memory
	if (vbo->ibo_items_count)
	{
		vk_bind_index_buffer(vk.vbo.vertex_buffer, tess.shader->iboOffset);

		for (i = 0; i < vbo->ibo_items_count; i++)
		{
			qvkCmdDrawIndexed(vk.cmd->command_buffer, vbo->ibo_items[i].length, 1, vbo->ibo_items[i].offset, 0, 0);
		}
	}

	// from host-visible memory
	if (vbo->soft_buffer_indexes)
	{
		vk_bind_index_buffer(vk.cmd->vertex_buffer, vbo->soft_buffer_offset);

		qvkCmdDrawIndexed(vk.cmd->command_buffer, vbo->soft_buffer_indexes, 1, 0, 0, 0);
	}
}

void VBO_PrepareQueues(void)
{
	vbo_t *vbo = &world_vbo;
	int i, item_run, index_run, n;
	const int *a;

	vbo->items_queue[vbo->items_queue_count] = 0; // terminate run

	// sort items so we can scan for longest runs
	if (vbo->items_queue_count > 1)
		qsort_int(vbo->items_queue, vbo->items_queue_count - 1);

	vbo->soft_buffer_indexes = 0;
	vbo->ibo_items_count = 0;

	a = vbo->items_queue;
	i = 0;
	while (i < vbo->items_queue_count)
	{
		item_run = run_length(a, i, vbo->items_queue_count, &index_run);
		if (index_run < MIN_IBO_RUN)
		{
			for (n = 0; n < item_run; n++)
				VBO_AddItemDataToSoftBuffer(a[i + n]);
		}
		else
		{
			vbo_item_t *start = vbo->items + a[i];
			vbo_item_t *end = vbo->items + a[i + item_run - 1];
			n = (end->index_offset - start->index_offset) + end->num_indexes;
			VBO_AddItemRangeToIBOBuffer(start->index_offset, n);
		}
		i += item_run;
	}
}


void vk_release_world_vbo( void )
{
	if ( vk.vbo.vertex_buffer )
		qvkDestroyBuffer( vk.device, vk.vbo.vertex_buffer, NULL );
	vk.vbo.vertex_buffer = VK_NULL_HANDLE;

	if ( vk.vbo.buffer_memory )
		qvkFreeMemory( vk.device, vk.vbo.buffer_memory, NULL );
	vk.vbo.buffer_memory = VK_NULL_HANDLE;
}

qboolean vk_alloc_vbo( const char *name, const byte *vbo_data, int vbo_size )
{
	VkMemoryRequirements vb_mem_reqs;
	VkMemoryAllocateInfo alloc_info;
	VkBufferCreateInfo desc;
	VkDeviceSize vertex_buffer_offset;
	VkDeviceSize allocationSize;
	uint32_t memory_type_bits;
	VkBuffer staging_vertex_buffer;
	VkDeviceMemory staging_buffer_memory;
	VkCommandBuffer command_buffer;
	VkBufferCopy copyRegion[1];
	void *data;

	vk_release_world_vbo();

	desc.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	desc.pNext = NULL;
	desc.flags = 0;
	desc.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	desc.queueFamilyIndexCount = 0;
	desc.pQueueFamilyIndices = NULL;

	// device-local buffer
	desc.size = vbo_size;
	desc.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
	VK_CHECK( qvkCreateBuffer( vk.device, &desc, NULL, &vk.vbo.vertex_buffer ) );

	// staging buffer
	desc.size = vbo_size;
	desc.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
	VK_CHECK(qvkCreateBuffer(vk.device, &desc, NULL, &staging_vertex_buffer));

	// memory requirements
	qvkGetBufferMemoryRequirements( vk.device, vk.vbo.vertex_buffer, &vb_mem_reqs );
	vertex_buffer_offset = 0;
	allocationSize = vertex_buffer_offset + vb_mem_reqs.size;
	memory_type_bits = vb_mem_reqs.memoryTypeBits;

	alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	alloc_info.pNext = NULL;
	alloc_info.allocationSize = allocationSize;
	alloc_info.memoryTypeIndex = vk_find_memory_type(memory_type_bits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	VK_CHECK(qvkAllocateMemory( vk.device, &alloc_info, NULL, &vk.vbo.buffer_memory));
	qvkBindBufferMemory( vk.device, vk.vbo.vertex_buffer, vk.vbo.buffer_memory, vertex_buffer_offset );

	// staging buffers

	// memory requirements
	qvkGetBufferMemoryRequirements(vk.device, staging_vertex_buffer, &vb_mem_reqs);
	vertex_buffer_offset = 0;
	allocationSize = vertex_buffer_offset + vb_mem_reqs.size;
	memory_type_bits = vb_mem_reqs.memoryTypeBits;

	alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	alloc_info.pNext = NULL;
	alloc_info.allocationSize = allocationSize;
	alloc_info.memoryTypeIndex = vk_find_memory_type(memory_type_bits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	VK_CHECK(qvkAllocateMemory(vk.device, &alloc_info, NULL, &staging_buffer_memory));
	qvkBindBufferMemory(vk.device, staging_vertex_buffer, staging_buffer_memory, vertex_buffer_offset);

	VK_CHECK(qvkMapMemory(vk.device, staging_buffer_memory, 0, VK_WHOLE_SIZE, 0, &data));
	memcpy((byte*)data + vertex_buffer_offset, vbo_data, vbo_size);
	qvkUnmapMemory(vk.device, staging_buffer_memory);

	command_buffer = vk_begin_command_buffer();
	copyRegion[0].srcOffset = 0;
	copyRegion[0].dstOffset = 0;
	copyRegion[0].size = vbo_size;
	qvkCmdCopyBuffer( command_buffer, staging_vertex_buffer, vk.vbo.vertex_buffer, 1, &copyRegion[0] );

	vk_end_command_buffer(command_buffer);

	qvkDestroyBuffer(vk.device, staging_vertex_buffer, NULL);
	qvkFreeMemory(vk.device, staging_buffer_memory, NULL);

	VK_SET_OBJECT_NAME( vk.vbo.vertex_buffer, va( "static VBO %s", name ), VK_DEBUG_REPORT_OBJECT_TYPE_BUFFER_EXT );
	VK_SET_OBJECT_NAME( vk.vbo.buffer_memory, va( "static VBO memory %s", name ), VK_DEBUG_REPORT_OBJECT_TYPE_DEVICE_MEMORY_EXT );

	return qtrue;
}
#endif