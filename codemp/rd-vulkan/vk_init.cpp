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

#include <stdio.h>
#include "tr_local.h"
#include <algorithm>
#include "../rd-common/tr_common.h"
#include "tr_WorldEffects.h"
#include "qcommon/MiniHeap.h"
#include "ghoul2/g2_local.h"

int vkSamples = VK_SAMPLE_COUNT_1_BIT;
int vkMaxSamples = VK_SAMPLE_COUNT_1_BIT;

static void vk_set_render_scale( void )
{
	if (gls.windowWidth != glConfig.vidWidth || gls.windowHeight != glConfig.vidHeight)
	{
		if (r_renderScale->integer > 0)
		{
			int scaleMode = r_renderScale->integer - 1;
			if (scaleMode & 1)
			{
				// preserve aspect ratio (black bars on sides)
				float windowAspect = (float)gls.windowWidth / (float)gls.windowHeight;
				float renderAspect = (float)glConfig.vidWidth / (float)glConfig.vidHeight;
				if (windowAspect >= renderAspect)
				{
					float scale = (float)gls.windowHeight / (float)glConfig.vidHeight;
					int bias = (gls.windowWidth - scale * (float)glConfig.vidWidth) / 2;
					vk.blitX0 += bias;
				}
				else
				{
					float scale = (float)gls.windowWidth / (float)glConfig.vidWidth;
					int bias = (gls.windowHeight - scale * (float)glConfig.vidHeight) / 2;
					vk.blitY0 += bias;
				}
			}
			// linear filtering
			if (scaleMode & 2)
				vk.blitFilter = GL_LINEAR;
			else
				vk.blitFilter = GL_NEAREST;
		}

		vk.windowAdjusted = qtrue;
	}

	if (r_fbo->integer && r_ext_supersample->integer && !r_renderScale->integer)
	{
		vk.blitFilter = GL_LINEAR;
	}
}

void get_viewport_rect( VkRect2D *r )
{
	if (backEnd.projection2D)
	{
		r->offset.x = 0;
		r->offset.y = 0;
		r->extent.width = vk.renderWidth;
		r->extent.height = vk.renderHeight;
	}
	else
	{
		r->offset.x = backEnd.viewParms.viewportX * vk.renderScaleX;
		r->offset.y = vk.renderHeight - (backEnd.viewParms.viewportY + backEnd.viewParms.viewportHeight) * vk.renderScaleY;
		r->extent.width = (float)backEnd.viewParms.viewportWidth * vk.renderScaleX;
		r->extent.height = (float)backEnd.viewParms.viewportHeight * vk.renderScaleY;
	}
}

void get_viewport( VkViewport *viewport, Vk_Depth_Range depth_range ) {
	VkRect2D r;

	get_viewport_rect(&r);

	viewport->x = (float)r.offset.x;
	viewport->y = (float)r.offset.y;
	viewport->width = (float)r.extent.width;
	viewport->height = (float)r.extent.height;

	switch (depth_range) {
		default:
#ifdef USE_REVERSED_DEPTH
		//case DEPTH_RANGE_NORMAL:
			viewport->minDepth = 0.0f;
			viewport->maxDepth = 1.0f;
			break;
		case DEPTH_RANGE_ZERO:
			viewport->minDepth = 1.0f;
			viewport->maxDepth = 1.0f;
			break;
		case DEPTH_RANGE_ONE:
			viewport->minDepth = 0.0f;
			viewport->maxDepth = 0.0f;
			break;
		case DEPTH_RANGE_WEAPON:
			viewport->minDepth = 0.6f;
			viewport->maxDepth = 1.0f;
			break;
#else
		//case DEPTH_RANGE_NORMAL:
			viewport->minDepth = 0.0f;
			viewport->maxDepth = 1.0f;
			break;
		case DEPTH_RANGE_ZERO:
			viewport->minDepth = 0.0f;
			viewport->maxDepth = 0.0f;
			break;
		case DEPTH_RANGE_ONE:
			viewport->minDepth = 1.0f;
			viewport->maxDepth = 1.0f;
			break;
		case DEPTH_RANGE_WEAPON:
			viewport->minDepth = 0.0f;
			viewport->maxDepth = 0.3f;
			break;
#endif
	}
}

void get_scissor_rect( VkRect2D *r ) {

	if (backEnd.viewParms.portalView != PV_NONE)
	{
		r->offset.x = backEnd.viewParms.scissorX;
		r->offset.y = glConfig.vidHeight - backEnd.viewParms.scissorY - backEnd.viewParms.scissorHeight;
		r->extent.width = backEnd.viewParms.scissorWidth;
		r->extent.height = backEnd.viewParms.scissorHeight;
	}
	else
	{
		get_viewport_rect(r);

		if (r->offset.x < 0)
			r->offset.x = 0;
		if (r->offset.y < 0)
			r->offset.y = 0;

		if (r->offset.x + r->extent.width > glConfig.vidWidth)
			r->extent.width = glConfig.vidWidth - r->offset.x;
		if (r->offset.y + r->extent.height > glConfig.vidHeight)
			r->extent.height = glConfig.vidHeight - r->offset.y;
	}
}

static void vk_render_splash( void )
{
	VkCommandBufferBeginInfo	begin_info;
	VkSubmitInfo				submit_info;
	VkPresentInfoKHR			present_info;
	VkPipelineStageFlags		wait_dst_stage_mask;
	VkImage						imageBuffer;
	image_t						*splashImage;
	VkImageBlit					imageBlit;
	float						ratio;

	ratio = ( (float)( SCREEN_WIDTH * glConfig.vidHeight ) / (float)( SCREEN_HEIGHT * glConfig.vidWidth ) );

	if ( cl_ratioFix->integer && ratio >= 0.74f && ratio <= 0.76f ){
		splashImage = R_FindImageFile("menu/splash_16_9", IMGFLAG_CLAMPTOEDGE);

		if ( !splashImage ){
			splashImage = R_FindImageFile("menu/splash", IMGFLAG_CLAMPTOEDGE);
		}
	}
	else{
		splashImage = R_FindImageFile("menu/splash", IMGFLAG_CLAMPTOEDGE);
	}

	if( !splashImage ){
		return;
	}

	VK_CHECK( qvkWaitForFences( vk.device, 1, &vk.cmd->rendering_finished_fence, VK_TRUE, 1e10 ) );
	VK_CHECK( qvkResetFences( vk.device, 1, &vk.cmd->rendering_finished_fence ) );

	qvkAcquireNextImageKHR( vk.device, vk.swapchain, UINT64_MAX, vk.cmd->image_acquired, VK_NULL_HANDLE, &vk.swapchain_image_index );
	imageBuffer = vk.swapchain_images[vk.swapchain_image_index];

	// begin the command buffer
	Com_Memset( &begin_info, 0, sizeof(begin_info) );
	begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin_info.pNext = NULL;
	begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	begin_info.pInheritanceInfo = NULL;
	VK_CHECK( qvkBeginCommandBuffer( vk.cmd->command_buffer, &begin_info ) );

	vk_record_image_layout_transition( vk.cmd->command_buffer, splashImage->handle,
		VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_IMAGE_LAYOUT_UNDEFINED,
		VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
		VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT );

	vk_record_image_layout_transition( vk.cmd->command_buffer, imageBuffer,
		VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_IMAGE_LAYOUT_UNDEFINED,
		VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
		VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT );

	Com_Memset( &imageBlit, 0, sizeof(imageBlit) );
	imageBlit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	imageBlit.srcSubresource.mipLevel = 0;
	imageBlit.srcSubresource.baseArrayLayer = 0;
	imageBlit.srcSubresource.layerCount = 1;
	imageBlit.srcOffsets[0] = { 0, 0, 0 };
	imageBlit.srcOffsets[1] = { splashImage->width, splashImage->height, 1 };
	imageBlit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	imageBlit.dstSubresource.mipLevel = 0;
	imageBlit.dstSubresource.baseArrayLayer = 0;
	imageBlit.dstSubresource.layerCount = 1;
	imageBlit.dstOffsets[0] = { vk.blitX0, vk.blitY0, 0 };
	imageBlit.dstOffsets[1] = { ( gls.windowWidth - vk.blitX0 ), ( gls.windowHeight - vk.blitY0 ), 1 };

	qvkCmdBlitImage( vk.cmd->command_buffer, splashImage->handle,
		VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, imageBuffer,
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
		&imageBlit, VK_FILTER_LINEAR );

	vk_record_image_layout_transition( vk.cmd->command_buffer, imageBuffer,
		VK_IMAGE_ASPECT_COLOR_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
		vk.queue_family_index, vk.queue_family_index,
		VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT );

	// we can end the command buffer now
	VK_CHECK( qvkEndCommandBuffer( vk.cmd->command_buffer ) );

	wait_dst_stage_mask = VK_PIPELINE_STAGE_TRANSFER_BIT;

	submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit_info.pNext = NULL;
	submit_info.commandBufferCount = 1;
	submit_info.pCommandBuffers = &vk.cmd->command_buffer;
	submit_info.waitSemaphoreCount = 1;
	submit_info.pWaitSemaphores = &vk.cmd->image_acquired;
	submit_info.pWaitDstStageMask = &wait_dst_stage_mask;
	submit_info.signalSemaphoreCount = 1;
	submit_info.pSignalSemaphores = &vk.cmd->rendering_finished;
	VK_CHECK( qvkQueueSubmit( vk.queue, 1, &submit_info, vk.cmd->rendering_finished_fence ) );

	present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	present_info.pNext = NULL;
	present_info.waitSemaphoreCount = 1;
	present_info.pWaitSemaphores = &vk.cmd->rendering_finished;
	present_info.swapchainCount = 1;
	present_info.pSwapchains = &vk.swapchain;
	present_info.pImageIndices = &vk.swapchain_image_index;
	present_info.pResults = NULL;
	qvkQueuePresentKHR( vk.queue, &present_info );

	return;
}

void vk_set_fastsky_color( void ) {
	vec4_t *out;

	switch( r_fastsky->integer ){
		case 1: out = &colorBlack; break;
		case 2: out = &colorRed; break;
		case 3: out = &colorGreen; break;
		case 4: out = &colorBlue; break;
		case 5: out = &colorYellow; break;
		case 6: out = &colorOrange; break;
		case 7: out = &colorMagenta; break;
		case 8: out = &colorCyan; break;
		case 9: out = &colorWhite; break;
		case 10: out = &colorLtGrey; break;
		case 11: out = &colorMdGrey; break;
		case 12: out = &colorDkGrey; break;
		case 13: out = &colorLtBlue; break;
		case 14: out = &colorDkBlue; break;
		default: out = &colorBlack;
	}

	tr.fastskyColor = out;
}

void vk_create_window( void ) {
	//R_Set2DRatio();

	if (glConfig.vidWidth == 0)
	{
		windowDesc_t windowDesc = { GRAPHICS_API_VULKAN };

		glConfig.deviceSupportsGamma = qfalse;
		window = ri.WIN_Init(&windowDesc, &glConfig);

		if (r_ignorehwgamma->integer)
			glConfig.deviceSupportsGamma = qfalse;

		gls.windowWidth = glConfig.vidWidth;
		gls.windowHeight = glConfig.vidHeight;

		gls.captureWidth = glConfig.vidWidth;
		gls.captureHeight = glConfig.vidHeight;

		//ri.CL_SetScaling(1.0, glConfig.vidWidth, glConfig.vidHeight);	// consolefont and avi capture

		if (r_fbo->integer)
		{
			if (r_renderScale->integer){
				glConfig.vidWidth = r_renderWidth->integer;
				glConfig.vidHeight = r_renderHeight->integer;
			}

			gls.captureWidth = glConfig.vidWidth;
			gls.captureHeight = glConfig.vidHeight;
		
			//ri.CL_SetScaling(1.0, gls.captureWidth, gls.captureHeight);	// consolefont and avi capture

			if (r_ext_supersample->integer){
				glConfig.vidWidth *= 2;
				glConfig.vidHeight *= 2;

				//ri.CL_SetScaling(2.0, gls.captureWidth, gls.captureHeight);	// consolefont and avi capture
			}
		}

		vk_initialize();

		gls.initTime = ri.Milliseconds();
	}

	if ( !vk.active && vk.instance ){
		// might happen after REF_KEEP_WINDOW
		vk_initialize();
		gls.initTime = ri.Milliseconds();
	}
	if ( vk.active ) {
		vk_init_descriptors();
	}
	else {
		ri.Error( ERR_FATAL, "Recursive error during Vulkan initialization" );
	}

	glState.glStateBits = GLS_DEPTHTEST_DISABLE | GLS_DEPTHMASK_TRUE;

	tr.inited = qtrue;
}

static void vk_initTextureCompression( void )
{
	if ( r_ext_compressed_textures->integer )
	{
		VkFormatProperties formatProps;
		qvkGetPhysicalDeviceFormatProperties( vk.physical_device, VK_FORMAT_BC3_UNORM_BLOCK, &formatProps );
		if ( formatProps.linearTilingFeatures && formatProps.optimalTilingFeatures )
		{
			vk.compressed_format = VK_FORMAT_BC3_UNORM_BLOCK; //GL_COMPRESSED_RGBA_S3TC_DXT5_EXT
		}
	}
}

void vk_initialize( void )
{
	VkPhysicalDeviceProperties props;

	vk_init_library();

	qvkGetDeviceQueue( vk.device, vk.queue_family_index, 0, &vk.queue );

	vk_get_vulkan_properties(&props);

	// Command buffer
	vk.cmd_index = 0;
	//vk.cmd = vk.tess + vk.cmd_index;
	vk.cmd = &vk.tess[vk.cmd_index];

	// Memory alignment
	vk.uniform_alignment = props.limits.minUniformBufferOffsetAlignment;
	vk.uniform_item_size = PAD( sizeof(vkUniform_t), vk.uniform_alignment );
	vk.storage_alignment = MAX( props.limits.minStorageBufferOffsetAlignment, sizeof(uint32_t) ); //for flare visibility tests

	// maxTextureSize must not exceed IMAGE_CHUNK_SIZE
	glConfig.maxTextureSize = MIN( props.limits.maxImageDimension2D, log2pad( sqrtf( IMAGE_CHUNK_SIZE / 4 ), 0 ) );
	if (glConfig.maxTextureSize > MAX_TEXTURE_SIZE)
		glConfig.maxTextureSize = MAX_TEXTURE_SIZE; // ResampleTexture() relies on that maximum

	// default chunk size, may be doubled on demand
	vk.image_chunk_size = IMAGE_CHUNK_SIZE; 

	// maxActiveTextures must not exceed MAX_TEXTURE_UNITS
	if ( props.limits.maxPerStageDescriptorSamplers != 0xFFFFFFFF )
		glConfig.maxActiveTextures = props.limits.maxPerStageDescriptorSamplers;
	else
		glConfig.maxActiveTextures = props.limits.maxBoundDescriptorSets;

	if ( glConfig.maxActiveTextures > MAX_TEXTURE_UNITS )
		glConfig.maxActiveTextures = MAX_TEXTURE_UNITS;

	vk.maxBoundDescriptorSets = props.limits.maxBoundDescriptorSets;
	vk.maxAnisotropy = props.limits.maxSamplerAnisotropy;
	vk.maxLod = 1 + Q_log2( glConfig.maxTextureSize );

	ri.Printf( PRINT_ALL, "\nVK_MAX_TEXTURE_SIZE: %d\n", glConfig.maxTextureSize );
	ri.Printf( PRINT_ALL, "VK_MAX_TEXTURE_UNITS: %d\n", glConfig.maxActiveTextures );

	vk_initTextureCompression();

	vk.xscale2D = glConfig.vidWidth * ( 1.0 / 640.0 );
	vk.yscale2D = glConfig.vidHeight * ( 1.0 / 480.0 );

	vk.windowAdjusted = qfalse;
	vk.blitFilter = GL_NEAREST;
	vk.blitX0 = vk.blitY0 = 0;

	vk_set_render_scale();

	if ( r_fbo->integer )
		vk.fboActive = qtrue;		

	//if (r_ext_multisample->integer && !r_ext_supersample->integer)
	if ( r_ext_multisample->integer )
		vk.msaaActive = qtrue;

	// MSAA
	vkMaxSamples = MIN( props.limits.sampledImageColorSampleCounts, props.limits.sampledImageDepthSampleCounts);

	if ( vk.msaaActive ) {
		VkSampleCountFlags mask = vkMaxSamples;
		vkSamples = MAX( log2pad( r_ext_multisample->integer, 1 ), VK_SAMPLE_COUNT_2_BIT );
		while ( vkSamples > mask )
			vkSamples >>= 1;
	}
	else {
		vkSamples = VK_SAMPLE_COUNT_1_BIT;
	}

	ri.Printf( PRINT_ALL, "MSAA max: %dx, using %dx\n", vkMaxSamples, vkSamples );

	// Anisotropy
	ri.Printf( PRINT_ALL, "Anisotropy max: %dx, using %dx\n\n", r_ext_max_anisotropy->integer, r_ext_texture_filter_anisotropic->integer );
		
	// Bloom
	if ( vk.fboActive && r_bloom->integer )
		vk.bloomActive = qtrue;

	// Dynamic glow
	if( vk.fboActive && glConfig.maxActiveTextures >= 4 && r_DynamicGlow->integer )
		vk.dglowActive = qtrue;

	// Screenmap
	vk.screenMapSamples = MIN(vkMaxSamples, VK_SAMPLE_COUNT_4_BIT);
	vk.screenMapWidth = (float)glConfig.vidWidth / 16.0;
	vk.screenMapHeight = (float)glConfig.vidHeight / 16.0;	

	if ( vk.screenMapWidth < 4 )
		vk.screenMapWidth = 4;	
	
	if ( vk.screenMapHeight < 4 )
		vk.screenMapHeight = 4;

	vk_create_sync_primitives();
	vk_create_command_pool();
	vk_create_command_buffer();
	vk_create_descriptor_layout();
	vk_create_pipeline_layout();

	vk.geometry_buffer_size_new = VERTEX_BUFFER_SIZE;
	vk_create_vertex_buffer( vk.geometry_buffer_size_new );
	vk_create_storage_buffer( MAX_FLARES * vk.storage_alignment );
	vk_create_shader_modules();

	{
		VkPipelineCacheCreateInfo ci;
		Com_Memset(&ci, 0, sizeof(ci));
		ci.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
		VK_CHECK(qvkCreatePipelineCache(vk.device, &ci, VK_NULL_HANDLE, &vk.pipelineCache));
	}

	vk.renderPassIndex = RENDER_PASS_MAIN; // default render pass
	vk.initSwapchainLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

	vk_create_swapchain( vk.physical_device, vk.device, vk.surface, vk.present_format, &vk.swapchain );
	vk_texture_mode( r_textureMode->string, qtrue );
	vk_render_splash();
	vk_create_attachments();
	vk_create_render_passes();
	vk_create_framebuffers();

	vk.active = qtrue;
}

// Shutdown vulkan subsystem by releasing resources acquired by Vk_Instance.
void vk_shutdown( void )
{
    ri.Printf( PRINT_ALL, "vk_shutdown()\n" );

	if (!qvkQueuePresentKHR) {// not fully initialized
		goto __cleanup;
	}

	vk_destroy_framebuffers();
	vk_destroy_pipelines( qtrue ); // reset counter
	vk_destroy_render_passes();
	vk_destroy_attachments();
	vk_destroy_swapchain();

	if (vk.pipelineCache != VK_NULL_HANDLE) {
		qvkDestroyPipelineCache(vk.device, vk.pipelineCache, NULL);
		vk.pipelineCache = VK_NULL_HANDLE;
	}

	qvkDestroyCommandPool(vk.device, vk.command_pool, NULL);

	qvkDestroyDescriptorPool(vk.device, vk.descriptor_pool, NULL);

	qvkDestroyDescriptorSetLayout(vk.device, vk.set_layout_sampler, NULL);
	qvkDestroyDescriptorSetLayout(vk.device, vk.set_layout_uniform, NULL);
	qvkDestroyDescriptorSetLayout(vk.device, vk.set_layout_storage, NULL);

	qvkDestroyPipelineLayout(vk.device, vk.pipeline_layout, NULL);
	qvkDestroyPipelineLayout(vk.device, vk.pipeline_layout_post_process, NULL);
	qvkDestroyPipelineLayout(vk.device, vk.pipeline_layout_blend, NULL);

#ifdef USE_VBO	
	vk_release_vbo();
#endif

	vk_release_geometry_buffers();
    vk_destroy_sync_primitives();
    
	// storage buffer
	qvkDestroyBuffer(vk.device, vk.storage.buffer, NULL);
	qvkFreeMemory(vk.device, vk.storage.memory, NULL);

    vk_destroy_shader_modules();

__cleanup:
	if (vk.device != VK_NULL_HANDLE)
		qvkDestroyDevice(vk.device, NULL);

	if (vk.surface != VK_NULL_HANDLE)
		qvkDestroySurfaceKHR(vk.instance, vk.surface, NULL);

#ifdef USE_VK_VALIDATION
	if (qvkDestroyDebugReportCallbackEXT && vk.debug_callback)
		qvkDestroyDebugReportCallbackEXT(vk.instance, vk.debug_callback, NULL);
#endif

	if (vk.instance != VK_NULL_HANDLE)
		qvkDestroyInstance(vk.instance, NULL);

	Com_Memset(&vk, 0, sizeof(vk));

	vk_deinit_library();

	vk.active = qfalse;
}