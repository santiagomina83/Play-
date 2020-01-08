#include "GSH_VulkanDraw.h"
#include "GSH_VulkanMemoryUtils.h"
#include "MemStream.h"
#include "vulkan/StructDefs.h"
#include "vulkan/Utils.h"
#include "nuanceur/Builder.h"
#include "nuanceur/generators/SpirvShaderGenerator.h"
#include "../GSHandler.h"
#include "../GsPixelFormats.h"

using namespace GSH_Vulkan;

#define VERTEX_ATTRIB_LOCATION_POSITION 0
#define VERTEX_ATTRIB_LOCATION_DEPTH 1
#define VERTEX_ATTRIB_LOCATION_COLOR 2
#define VERTEX_ATTRIB_LOCATION_TEXCOORD 3

#define DESCRIPTOR_LOCATION_BUFFER_MEMORY 0
#define DESCRIPTOR_LOCATION_IMAGE_CLUT 1
#define DESCRIPTOR_LOCATION_IMAGE_SWIZZLETABLE_TEX 2
#define DESCRIPTOR_LOCATION_IMAGE_SWIZZLETABLE_FB 3

static void MakeLinearZOrtho(float* matrix, float left, float right, float bottom, float top)
{
	matrix[0] = 2.0f / (right - left);
	matrix[1] = 0;
	matrix[2] = 0;
	matrix[3] = 0;

	matrix[4] = 0;
	matrix[5] = 2.0f / (top - bottom);
	matrix[6] = 0;
	matrix[7] = 0;

	matrix[8] = 0;
	matrix[9] = 0;
	matrix[10] = 1;
	matrix[11] = 0;

	matrix[12] = -(right + left) / (right - left);
	matrix[13] = -(top + bottom) / (top - bottom);
	matrix[14] = 0;
	matrix[15] = 1;
}

#define DRAW_AREA_SIZE 1024
#define MAX_VERTEX_COUNT 1024

#define DEPTH_MAX (4294967296.0f)

CDraw::CDraw(const ContextPtr& context, const FrameCommandBufferPtr& frameCommandBuffer)
    : m_context(context)
    , m_frameCommandBuffer(frameCommandBuffer)
    , m_pipelineCache(context->device)
{
	CreateRenderPass();
	CreateDrawImage();
	CreateFramebuffer();

	m_vertexBuffer = Framework::Vulkan::CBuffer(
	    m_context->device, m_context->physicalDeviceMemoryProperties,
	    VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, sizeof(PRIM_VERTEX) * MAX_VERTEX_COUNT);

	auto result = m_context->device.vkMapMemory(m_context->device, m_vertexBuffer.GetMemory(),
	                                            0, VK_WHOLE_SIZE, 0, reinterpret_cast<void**>(&m_vertexBufferPtr));
	CHECKVULKANERROR(result);

	MakeLinearZOrtho(m_pushConstants.projMatrix, 0, DRAW_AREA_SIZE, 0, DRAW_AREA_SIZE);

	m_pipelineCaps <<= 0;
}

CDraw::~CDraw()
{
	m_context->device.vkUnmapMemory(m_context->device, m_vertexBuffer.GetMemory());
	m_context->device.vkDestroyFramebuffer(m_context->device, m_framebuffer, nullptr);
	m_context->device.vkDestroyRenderPass(m_context->device, m_renderPass, nullptr);
	m_context->device.vkDestroyImageView(m_context->device, m_drawImageView, nullptr);
}

void CDraw::SetPipelineCaps(const PIPELINE_CAPS& caps)
{
	bool changed = static_cast<uint64>(caps) != static_cast<uint64>(m_pipelineCaps);
	if(!changed) return;
	FlushVertices();
	m_pipelineCaps = caps;
}

void CDraw::SetFramebufferParams(uint32 addr, uint32 width)
{
	bool changed =
	    (m_pushConstants.fbBufAddr != addr) ||
	    (m_pushConstants.fbBufWidth != width);
	if(!changed) return;
	FlushVertices();
	m_pushConstants.fbBufAddr = addr;
	m_pushConstants.fbBufWidth = width;
}

void CDraw::SetDepthbufferParams(uint32 addr, uint32 width)
{
	bool changed =
	    (m_pushConstants.depthBufAddr != addr) ||
	    (m_pushConstants.depthBufWidth != width);
	if(!changed) return;
	FlushVertices();
	m_pushConstants.depthBufAddr = addr;
	m_pushConstants.depthBufWidth = width;
}

void CDraw::SetTextureParams(uint32 bufAddr, uint32 bufWidth, uint32 width, uint32 height)
{
	bool changed =
	    (m_pushConstants.texBufAddr != bufAddr) ||
	    (m_pushConstants.texBufWidth != bufWidth) ||
	    (m_pushConstants.texWidth != width) ||
	    (m_pushConstants.texHeight != height);
	if(!changed) return;
	FlushVertices();
	m_pushConstants.texBufAddr = bufAddr;
	m_pushConstants.texBufWidth = bufWidth;
	m_pushConstants.texWidth = width;
	m_pushConstants.texHeight = height;
}

void CDraw::SetScissor(uint32 scissorX, uint32 scissorY, uint32 scissorWidth, uint32 scissorHeight)
{
	bool changed =
	    (m_scissorX != scissorX) ||
	    (m_scissorY != scissorY) ||
	    (m_scissorWidth != scissorWidth) ||
	    (m_scissorHeight != scissorHeight);
	if(!changed) return;
	FlushVertices();
	m_scissorX = scissorX;
	m_scissorY = scissorY;
	m_scissorWidth = scissorWidth;
	m_scissorHeight = scissorHeight;
}

void CDraw::AddVertices(const PRIM_VERTEX* vertexBeginPtr, const PRIM_VERTEX* vertexEndPtr)
{
	auto amount = vertexEndPtr - vertexBeginPtr;
	if((m_passVertexEnd + amount) > MAX_VERTEX_COUNT)
	{
		m_frameCommandBuffer->Flush();
		assert((m_passVertexEnd + amount) <= MAX_VERTEX_COUNT);
	}
	memcpy(m_vertexBufferPtr + m_passVertexEnd, vertexBeginPtr, amount * sizeof(PRIM_VERTEX));
	m_passVertexEnd += amount;
}

void CDraw::FlushVertices()
{
	uint32 vertexCount = m_passVertexEnd - m_passVertexStart;
	if(vertexCount == 0) return;

	auto commandBuffer = m_frameCommandBuffer->GetCommandBuffer();

	//Find pipeline and create it if we've never encountered it before
	auto drawPipeline = m_pipelineCache.TryGetPipeline(m_pipelineCaps);
	if(!drawPipeline)
	{
		drawPipeline = m_pipelineCache.RegisterPipeline(m_pipelineCaps, CreateDrawPipeline(m_pipelineCaps));
	}

	{
		VkViewport viewport = {};
		viewport.width = DRAW_AREA_SIZE;
		viewport.height = DRAW_AREA_SIZE;
		viewport.maxDepth = 1.0f;
		m_context->device.vkCmdSetViewport(commandBuffer, 0, 1, &viewport);

		VkRect2D scissor = {};
		scissor.offset.x = m_scissorX;
		scissor.offset.y = m_scissorY;
		scissor.extent.width = m_scissorWidth;
		scissor.extent.height = m_scissorHeight;
		m_context->device.vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
	}

	auto renderPassBeginInfo = Framework::Vulkan::RenderPassBeginInfo();
	renderPassBeginInfo.renderPass = m_renderPass;
	renderPassBeginInfo.renderArea.extent.width = DRAW_AREA_SIZE;
	renderPassBeginInfo.renderArea.extent.height = DRAW_AREA_SIZE;
	renderPassBeginInfo.framebuffer = m_framebuffer;
	m_context->device.vkCmdBeginRenderPass(commandBuffer, &renderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);

	auto descriptorSetCaps = make_convertible<DESCRIPTORSET_CAPS>(0);
	descriptorSetCaps.hasTexture = m_pipelineCaps.hasTexture;
	descriptorSetCaps.framebufferFormat = m_pipelineCaps.framebufferFormat;
	descriptorSetCaps.depthbufferFormat = m_pipelineCaps.depthbufferFormat;
	descriptorSetCaps.textureFormat = m_pipelineCaps.textureFormat;
	
	auto descriptorSet = PrepareDescriptorSet(drawPipeline->descriptorSetLayout, descriptorSetCaps);

	m_context->device.vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, drawPipeline->pipelineLayout,
	                                          0, 1, &descriptorSet, 0, nullptr);

	m_context->device.vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, drawPipeline->pipeline);

	VkDeviceSize vertexBufferOffset = (m_passVertexStart * sizeof(PRIM_VERTEX));
	VkBuffer vertexBuffer = m_vertexBuffer;
	m_context->device.vkCmdBindVertexBuffers(commandBuffer, 0, 1, &vertexBuffer, &vertexBufferOffset);

	m_context->device.vkCmdPushConstants(commandBuffer, drawPipeline->pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
	                                     0, sizeof(DRAW_PIPELINE_PUSHCONSTANTS), &m_pushConstants);

	assert((vertexCount % 3) == 0);
	m_context->device.vkCmdDraw(commandBuffer, vertexCount, 1, 0, 0);

	m_context->device.vkCmdEndRenderPass(commandBuffer);

	m_passVertexStart = m_passVertexEnd;
}

void CDraw::ResetDescriptorSets()
{
	m_descriptorSetCache.clear();
}

void CDraw::PreFlushFrameCommandBuffer()
{
	FlushVertices();
}

void CDraw::PostFlushFrameCommandBuffer()
{
	m_passVertexStart = m_passVertexEnd = 0;
}

VkDescriptorSet CDraw::PrepareDescriptorSet(VkDescriptorSetLayout descriptorSetLayout, const DESCRIPTORSET_CAPS& caps)
{
	auto descriptorSetIterator = m_descriptorSetCache.find(caps);
	if(descriptorSetIterator != std::end(m_descriptorSetCache))
	{
		return descriptorSetIterator->second;
	}

	auto result = VK_SUCCESS;
	VkDescriptorSet descriptorSet = VK_NULL_HANDLE;

	//Allocate descriptor set
	{
		auto setAllocateInfo = Framework::Vulkan::DescriptorSetAllocateInfo();
		setAllocateInfo.descriptorPool = m_context->descriptorPool;
		setAllocateInfo.descriptorSetCount = 1;
		setAllocateInfo.pSetLayouts = &descriptorSetLayout;

		result = m_context->device.vkAllocateDescriptorSets(m_context->device, &setAllocateInfo, &descriptorSet);
		CHECKVULKANERROR(result);
	}

	//Update descriptor set
	{
		VkDescriptorBufferInfo descriptorMemoryBufferInfo = {};
		descriptorMemoryBufferInfo.buffer = m_context->memoryBuffer;
		descriptorMemoryBufferInfo.range = VK_WHOLE_SIZE;

		VkDescriptorImageInfo descriptorClutImageInfo = {};
		descriptorClutImageInfo.imageView = m_context->clutImageView;
		descriptorClutImageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

		VkDescriptorImageInfo descriptorTexSwizzleTableImageInfo = {};
		descriptorTexSwizzleTableImageInfo.imageView = m_context->GetSwizzleTable(caps.textureFormat);
		descriptorTexSwizzleTableImageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

		VkDescriptorImageInfo descriptorFbSwizzleTableImageInfo = {};
		descriptorFbSwizzleTableImageInfo.imageView = m_context->GetSwizzleTable(caps.framebufferFormat);
		descriptorFbSwizzleTableImageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

		std::vector<VkWriteDescriptorSet> writes;

		{
			auto writeSet = Framework::Vulkan::WriteDescriptorSet();
			writeSet.dstSet = descriptorSet;
			writeSet.dstBinding = DESCRIPTOR_LOCATION_BUFFER_MEMORY;
			writeSet.descriptorCount = 1;
			writeSet.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
			writeSet.pBufferInfo = &descriptorMemoryBufferInfo;
			writes.push_back(writeSet);
		}

		{
			auto writeSet = Framework::Vulkan::WriteDescriptorSet();
			writeSet.dstSet = descriptorSet;
			writeSet.dstBinding = DESCRIPTOR_LOCATION_IMAGE_SWIZZLETABLE_FB;
			writeSet.descriptorCount = 1;
			writeSet.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
			writeSet.pImageInfo = &descriptorFbSwizzleTableImageInfo;
			writes.push_back(writeSet);
		}

		if(caps.hasTexture)
		{
			{
				auto writeSet = Framework::Vulkan::WriteDescriptorSet();
				writeSet.dstSet = descriptorSet;
				writeSet.dstBinding = DESCRIPTOR_LOCATION_IMAGE_SWIZZLETABLE_TEX;
				writeSet.descriptorCount = 1;
				writeSet.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
				writeSet.pImageInfo = &descriptorTexSwizzleTableImageInfo;
				writes.push_back(writeSet);
			}

			if(CGsPixelFormats::IsPsmIDTEX(caps.textureFormat))
			{
				auto writeSet = Framework::Vulkan::WriteDescriptorSet();
				writeSet.dstSet = descriptorSet;
				writeSet.dstBinding = DESCRIPTOR_LOCATION_IMAGE_CLUT;
				writeSet.descriptorCount = 1;
				writeSet.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
				writeSet.pImageInfo = &descriptorClutImageInfo;
				writes.push_back(writeSet);
			}
		}

		m_context->device.vkUpdateDescriptorSets(m_context->device, writes.size(), writes.data(), 0, nullptr);
	}

	m_descriptorSetCache.insert(std::make_pair(caps, descriptorSet));

	return descriptorSet;
}

void CDraw::CreateFramebuffer()
{
	assert(m_renderPass != VK_NULL_HANDLE);
	assert(m_framebuffer == VK_NULL_HANDLE);

	auto frameBufferCreateInfo = Framework::Vulkan::FramebufferCreateInfo();
	frameBufferCreateInfo.renderPass = m_renderPass;
	frameBufferCreateInfo.width = DRAW_AREA_SIZE;
	frameBufferCreateInfo.height = DRAW_AREA_SIZE;
	frameBufferCreateInfo.layers = 1;
	frameBufferCreateInfo.attachmentCount = 1;
	frameBufferCreateInfo.pAttachments = &m_drawImageView;

	auto result = m_context->device.vkCreateFramebuffer(m_context->device, &frameBufferCreateInfo, nullptr, &m_framebuffer);
	CHECKVULKANERROR(result);
}

void CDraw::CreateRenderPass()
{
	assert(m_renderPass == VK_NULL_HANDLE);

	auto result = VK_SUCCESS;

	VkAttachmentDescription colorAttachment = {};
	colorAttachment.format = VK_FORMAT_R8G8B8A8_UNORM;
	colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
	colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	colorAttachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	colorAttachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

	VkAttachmentReference colorRef = {};
	colorRef.attachment = 0;
	colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

	VkSubpassDescription subpass = {};
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.pColorAttachments = &colorRef;
	subpass.colorAttachmentCount = 1;

	auto renderPassCreateInfo = Framework::Vulkan::RenderPassCreateInfo();
	renderPassCreateInfo.subpassCount = 1;
	renderPassCreateInfo.pSubpasses = &subpass;
	renderPassCreateInfo.attachmentCount = 1;
	renderPassCreateInfo.pAttachments = &colorAttachment;

	result = m_context->device.vkCreateRenderPass(m_context->device, &renderPassCreateInfo, nullptr, &m_renderPass);
	CHECKVULKANERROR(result);
}

PIPELINE CDraw::CreateDrawPipeline(const PIPELINE_CAPS& caps)
{
	PIPELINE drawPipeline;

	auto vertexShader = CreateVertexShader();
	auto fragmentShader = CreateFragmentShader(caps);

	auto result = VK_SUCCESS;

	{
		std::vector<VkDescriptorSetLayoutBinding> setLayoutBindings;

		{
			VkDescriptorSetLayoutBinding setLayoutBinding = {};
			setLayoutBinding.binding = DESCRIPTOR_LOCATION_BUFFER_MEMORY;
			setLayoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
			setLayoutBinding.descriptorCount = 1;
			setLayoutBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
			setLayoutBindings.push_back(setLayoutBinding);
		}

		{
			VkDescriptorSetLayoutBinding setLayoutBinding = {};
			setLayoutBinding.binding = DESCRIPTOR_LOCATION_IMAGE_SWIZZLETABLE_FB;
			setLayoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
			setLayoutBinding.descriptorCount = 1;
			setLayoutBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
			setLayoutBindings.push_back(setLayoutBinding);
		}

		if(caps.hasTexture)
		{
			{
				VkDescriptorSetLayoutBinding setLayoutBinding = {};
				setLayoutBinding.binding = DESCRIPTOR_LOCATION_IMAGE_SWIZZLETABLE_TEX;
				setLayoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
				setLayoutBinding.descriptorCount = 1;
				setLayoutBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
				setLayoutBindings.push_back(setLayoutBinding);
			}

			if(CGsPixelFormats::IsPsmIDTEX(caps.textureFormat))
			{
				VkDescriptorSetLayoutBinding setLayoutBinding = {};
				setLayoutBinding.binding = DESCRIPTOR_LOCATION_IMAGE_CLUT;
				setLayoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
				setLayoutBinding.descriptorCount = 1;
				setLayoutBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
				setLayoutBindings.push_back(setLayoutBinding);
			}
		}

		auto setLayoutCreateInfo = Framework::Vulkan::DescriptorSetLayoutCreateInfo();
		setLayoutCreateInfo.bindingCount = static_cast<uint32>(setLayoutBindings.size());
		setLayoutCreateInfo.pBindings = setLayoutBindings.data();

		result = m_context->device.vkCreateDescriptorSetLayout(m_context->device, &setLayoutCreateInfo, nullptr, &drawPipeline.descriptorSetLayout);
		CHECKVULKANERROR(result);
	}

	{
		VkPushConstantRange pushConstantInfo = {};
		pushConstantInfo.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
		pushConstantInfo.offset = 0;
		pushConstantInfo.size = sizeof(DRAW_PIPELINE_PUSHCONSTANTS);

		auto pipelineLayoutCreateInfo = Framework::Vulkan::PipelineLayoutCreateInfo();
		pipelineLayoutCreateInfo.pushConstantRangeCount = 1;
		pipelineLayoutCreateInfo.pPushConstantRanges = &pushConstantInfo;
		pipelineLayoutCreateInfo.setLayoutCount = 1;
		pipelineLayoutCreateInfo.pSetLayouts = &drawPipeline.descriptorSetLayout;

		result = m_context->device.vkCreatePipelineLayout(m_context->device, &pipelineLayoutCreateInfo, nullptr, &drawPipeline.pipelineLayout);
		CHECKVULKANERROR(result);
	}

	auto inputAssemblyInfo = Framework::Vulkan::PipelineInputAssemblyStateCreateInfo();
	inputAssemblyInfo.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

	std::vector<VkVertexInputAttributeDescription> vertexAttributes;

	{
		VkVertexInputAttributeDescription vertexAttributeDesc = {};
		vertexAttributeDesc.format = VK_FORMAT_R32G32_SFLOAT;
		vertexAttributeDesc.offset = offsetof(PRIM_VERTEX, x);
		vertexAttributeDesc.location = VERTEX_ATTRIB_LOCATION_POSITION;
		vertexAttributes.push_back(vertexAttributeDesc);
	}

	{
		VkVertexInputAttributeDescription vertexAttributeDesc = {};
		vertexAttributeDesc.format = VK_FORMAT_R32_UINT;
		vertexAttributeDesc.offset = offsetof(PRIM_VERTEX, z);
		vertexAttributeDesc.location = VERTEX_ATTRIB_LOCATION_DEPTH;
		vertexAttributes.push_back(vertexAttributeDesc);
	}

	{
		VkVertexInputAttributeDescription vertexAttributeDesc = {};
		vertexAttributeDesc.format = VK_FORMAT_R8G8B8A8_UNORM;
		vertexAttributeDesc.offset = offsetof(PRIM_VERTEX, color);
		vertexAttributeDesc.location = VERTEX_ATTRIB_LOCATION_COLOR;
		vertexAttributes.push_back(vertexAttributeDesc);
	}

	{
		VkVertexInputAttributeDescription vertexAttributeDesc = {};
		vertexAttributeDesc.format = VK_FORMAT_R32G32B32_SFLOAT;
		vertexAttributeDesc.offset = offsetof(PRIM_VERTEX, s);
		vertexAttributeDesc.location = VERTEX_ATTRIB_LOCATION_TEXCOORD;
		vertexAttributes.push_back(vertexAttributeDesc);
	}

	VkVertexInputBindingDescription binding = {};
	binding.stride = sizeof(PRIM_VERTEX);
	binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

	auto vertexInputInfo = Framework::Vulkan::PipelineVertexInputStateCreateInfo();
	vertexInputInfo.vertexBindingDescriptionCount = 1;
	vertexInputInfo.pVertexBindingDescriptions = &binding;
	vertexInputInfo.vertexAttributeDescriptionCount = static_cast<uint32>(vertexAttributes.size());
	vertexInputInfo.pVertexAttributeDescriptions = vertexAttributes.data();

	auto rasterStateInfo = Framework::Vulkan::PipelineRasterizationStateCreateInfo();
	rasterStateInfo.polygonMode = VK_POLYGON_MODE_FILL;
	rasterStateInfo.cullMode = VK_CULL_MODE_NONE;
	rasterStateInfo.lineWidth = 1.0f;

	// Our attachment will write to all color channels, but no blending is enabled.
	VkPipelineColorBlendAttachmentState blendAttachment = {};
	blendAttachment.colorWriteMask = 0xf;

	auto colorBlendStateInfo = Framework::Vulkan::PipelineColorBlendStateCreateInfo();
	colorBlendStateInfo.attachmentCount = 1;
	colorBlendStateInfo.pAttachments = &blendAttachment;

	auto viewportStateInfo = Framework::Vulkan::PipelineViewportStateCreateInfo();
	viewportStateInfo.viewportCount = 1;
	viewportStateInfo.scissorCount = 1;

	auto depthStencilStateInfo = Framework::Vulkan::PipelineDepthStencilStateCreateInfo();

	auto multisampleStateInfo = Framework::Vulkan::PipelineMultisampleStateCreateInfo();
	multisampleStateInfo.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	static const VkDynamicState dynamicStates[] =
	    {
	        VK_DYNAMIC_STATE_VIEWPORT,
	        VK_DYNAMIC_STATE_SCISSOR,
	    };
	auto dynamicStateInfo = Framework::Vulkan::PipelineDynamicStateCreateInfo();
	dynamicStateInfo.pDynamicStates = dynamicStates;
	dynamicStateInfo.dynamicStateCount = sizeof(dynamicStates) / sizeof(dynamicStates[0]);

	VkPipelineShaderStageCreateInfo shaderStages[2] =
	    {
	        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
	        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
	    };

	shaderStages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	shaderStages[0].module = vertexShader;
	shaderStages[0].pName = "main";
	shaderStages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	shaderStages[1].module = fragmentShader;
	shaderStages[1].pName = "main";

	auto pipelineCreateInfo = Framework::Vulkan::GraphicsPipelineCreateInfo();
	pipelineCreateInfo.stageCount = 2;
	pipelineCreateInfo.pStages = shaderStages;
	pipelineCreateInfo.pInputAssemblyState = &inputAssemblyInfo;
	pipelineCreateInfo.pVertexInputState = &vertexInputInfo;
	pipelineCreateInfo.pRasterizationState = &rasterStateInfo;
	pipelineCreateInfo.pColorBlendState = &colorBlendStateInfo;
	pipelineCreateInfo.pViewportState = &viewportStateInfo;
	pipelineCreateInfo.pDepthStencilState = &depthStencilStateInfo;
	pipelineCreateInfo.pMultisampleState = &multisampleStateInfo;
	pipelineCreateInfo.pDynamicState = &dynamicStateInfo;
	pipelineCreateInfo.renderPass = m_renderPass;
	pipelineCreateInfo.layout = drawPipeline.pipelineLayout;

	result = m_context->device.vkCreateGraphicsPipelines(m_context->device, VK_NULL_HANDLE, 1, &pipelineCreateInfo, nullptr, &drawPipeline.pipeline);
	CHECKVULKANERROR(result);

	return drawPipeline;
}

Framework::Vulkan::CShaderModule CDraw::CreateVertexShader()
{
	using namespace Nuanceur;

	auto b = CShaderBuilder();

	{
		//Vertex Inputs
		auto inputPosition = CFloat4Lvalue(b.CreateInput(Nuanceur::SEMANTIC_POSITION));
		auto inputDepth = CUint4Lvalue(b.CreateInputUint(Nuanceur::SEMANTIC_TEXCOORD, VERTEX_ATTRIB_LOCATION_DEPTH - 1));
		auto inputColor = CFloat4Lvalue(b.CreateInput(Nuanceur::SEMANTIC_TEXCOORD, VERTEX_ATTRIB_LOCATION_COLOR - 1));
		auto inputTexCoord = CFloat4Lvalue(b.CreateInput(Nuanceur::SEMANTIC_TEXCOORD, VERTEX_ATTRIB_LOCATION_TEXCOORD - 1));

		//Outputs
		auto outputPosition = CFloat4Lvalue(b.CreateOutput(Nuanceur::SEMANTIC_SYSTEM_POSITION));
		auto outputDepth = CFloat4Lvalue(b.CreateOutput(Nuanceur::SEMANTIC_TEXCOORD, 1));
		auto outputColor = CFloat4Lvalue(b.CreateOutput(Nuanceur::SEMANTIC_TEXCOORD, 2));
		auto outputTexCoord = CFloat4Lvalue(b.CreateOutput(Nuanceur::SEMANTIC_TEXCOORD, 3));

		//Constants
		auto projMatrix = CMatrix44Value(b.CreateUniformMatrix("g_projMatrix", Nuanceur::UNIFORM_UNIT_PUSHCONSTANT));

		outputPosition = projMatrix * NewFloat4(inputPosition->xyz(), NewFloat(b, 1.0f));
		outputDepth = ToFloat(inputDepth) / NewFloat4(b, DEPTH_MAX, DEPTH_MAX, DEPTH_MAX, DEPTH_MAX);
		outputColor = inputColor->xyzw();
		outputTexCoord = inputTexCoord->xyzw();
	}

	Framework::CMemStream shaderStream;
	Nuanceur::CSpirvShaderGenerator::Generate(shaderStream, b, Nuanceur::CSpirvShaderGenerator::SHADER_TYPE_VERTEX);
	shaderStream.Seek(0, Framework::STREAM_SEEK_SET);
	return Framework::Vulkan::CShaderModule(m_context->device, shaderStream);
}

static Nuanceur::CFloat4Rvalue GetTextureColor(Nuanceur::CShaderBuilder& b, uint32 textureFormat, uint32 clutFormat,
	Nuanceur::CInt2Value texelPos, Nuanceur::CArrayUintValue memoryBuffer, Nuanceur::CImageUint2DValue clutImage,
	Nuanceur::CImageUint2DValue texSwizzleTable, Nuanceur::CIntValue texBufAddress, Nuanceur::CIntValue texBufWidth)
{
	using namespace Nuanceur;

	switch(textureFormat)
	{
	default:
		assert(false);
	case CGSHandler::PSMCT32:
	{
		auto texAddress = CMemoryUtils::GetPixelAddress<CGsPixelFormats::STORAGEPSMCT32>(
			b, texSwizzleTable, texBufAddress, texBufWidth, texelPos);
		auto texPixel = CMemoryUtils::Memory_Read32(b, memoryBuffer, texAddress);
		return CMemoryUtils::PSM32ToVec4(b, texPixel);
	}
	case CGSHandler::PSMCT16S:
	{
		auto texAddress = CMemoryUtils::GetPixelAddress<CGsPixelFormats::STORAGEPSMCT16>(
			b, texSwizzleTable, texBufAddress, texBufWidth, texelPos);
		auto texPixel = CMemoryUtils::Memory_Read16(b, memoryBuffer, texAddress);
		return CMemoryUtils::PSM16ToVec4(b, texPixel);
	}
	case CGSHandler::PSMT8:
	{
		assert(clutFormat == CGSHandler::PSMCT32);
		auto texAddress = CMemoryUtils::GetPixelAddress<CGsPixelFormats::STORAGEPSMT8>(
			b, texSwizzleTable, texBufAddress, texBufWidth, texelPos);
		auto texPixel = CMemoryUtils::Memory_Read8(b, memoryBuffer, texAddress);
		auto clutIndexLo = NewInt2(ToInt(texPixel), NewInt(b, 0));
		auto clutIndexHi = NewInt2(ToInt(texPixel) + NewInt(b, 0x100), NewInt(b, 0));
		auto clutPixelLo = Load(clutImage, clutIndexLo)->x();
		auto clutPixelHi = Load(clutImage, clutIndexHi)->x();
		auto clutPixel = clutPixelLo | (clutPixelHi << NewUint(b, 16));
		return CMemoryUtils::PSM32ToVec4(b, clutPixel);
	}
	case CGSHandler::PSMT4:
	{
		assert(clutFormat == CGSHandler::PSMCT32);
		auto texAddress = CMemoryUtils::GetPixelAddress_PSMT4(
			b, texSwizzleTable, texBufAddress, texBufWidth, texelPos);
		auto texPixel = CMemoryUtils::Memory_Read4(b, memoryBuffer, texAddress);
//		auto clutIndexLo = NewInt2(ToInt(texPixel), NewInt(b, 0));
//		auto clutIndexHi = NewInt2(ToInt(texPixel) + NewInt(b, 0x100), NewInt(b, 0));
//		auto clutPixelLo = Load(clutImage, clutIndexLo)->x();
//		auto clutPixelHi = Load(clutImage, clutIndexHi)->x();
//		auto clutPixel = clutPixelLo | (clutPixelHi << NewUint(b, 16));
//		return CMemoryUtils::PSM32ToVec4(b, clutPixel);
		auto test = texPixel * NewUint(b, 0x10);
		return CMemoryUtils::PSM32ToVec4(b, test | NewUint(b, 0xFF000000));
	}
	}
}

static Nuanceur::CFloat3Rvalue GetAlphaABD(Nuanceur::CShaderBuilder& b, uint32 alphaABD,
	Nuanceur::CFloat4Value srcColor, Nuanceur::CFloat4Value dstColor)
{
	switch(alphaABD)
	{
	default:
		assert(false);
	case CGSHandler::ALPHABLEND_ABD_CS:
		return srcColor->xyz();
	case CGSHandler::ALPHABLEND_ABD_CD:
		return dstColor->xyz();
	case CGSHandler::ALPHABLEND_ABD_ZERO:
		return NewFloat3(b, 0, 0, 0);
	}
}


static Nuanceur::CFloat3Rvalue GetAlphaC(Nuanceur::CShaderBuilder& b, uint32 alphaC,
	Nuanceur::CFloat4Value srcColor, Nuanceur::CFloat4Value dstColor)
{
	switch(alphaC)
	{
	default:
		assert(false);
	case CGSHandler::ALPHABLEND_C_AS:
		return srcColor->www();
	case CGSHandler::ALPHABLEND_C_AD:
		return dstColor->www();
	}
}

Framework::Vulkan::CShaderModule CDraw::CreateFragmentShader(const PIPELINE_CAPS& caps)
{
	using namespace Nuanceur;

	auto b = CShaderBuilder();

	{
		//Inputs
		auto inputPosition = CFloat4Lvalue(b.CreateInput(Nuanceur::SEMANTIC_SYSTEM_POSITION));
		auto inputDepth = CFloat4Lvalue(b.CreateInput(Nuanceur::SEMANTIC_TEXCOORD, 1));
		auto inputColor = CFloat4Lvalue(b.CreateInput(Nuanceur::SEMANTIC_TEXCOORD, 2));
		auto inputTexCoord = CFloat4Lvalue(b.CreateInput(Nuanceur::SEMANTIC_TEXCOORD, 3));

		//Outputs
		auto outputColor = CFloat4Lvalue(b.CreateOutput(Nuanceur::SEMANTIC_SYSTEM_COLOR));

		auto memoryBuffer = CArrayUintValue(b.CreateUniformArrayUint("memoryBuffer", DESCRIPTOR_LOCATION_BUFFER_MEMORY));
		auto clutImage = CImageUint2DValue(b.CreateImage2DUint(DESCRIPTOR_LOCATION_IMAGE_CLUT));
		auto texSwizzleTable = CImageUint2DValue(b.CreateImage2DUint(DESCRIPTOR_LOCATION_IMAGE_SWIZZLETABLE_TEX));
		auto fbSwizzleTable = CImageUint2DValue(b.CreateImage2DUint(DESCRIPTOR_LOCATION_IMAGE_SWIZZLETABLE_FB));

		//Push constants
		auto projMatrix = CMatrix44Value(b.CreateUniformMatrix("g_projMatrix", Nuanceur::UNIFORM_UNIT_PUSHCONSTANT));
		auto fbDepthParams = CInt4Lvalue(b.CreateUniformInt4("fbDepthParams", Nuanceur::UNIFORM_UNIT_PUSHCONSTANT));
		auto texParams = CInt4Lvalue(b.CreateUniformInt4("texParams", Nuanceur::UNIFORM_UNIT_PUSHCONSTANT));

		auto fbBufAddress = fbDepthParams->x();
		auto fbBufWidth = fbDepthParams->y();

		auto texBufAddress = texParams->x();
		auto texBufWidth = texParams->y();
		auto texSize = texParams->zw();

		auto srcDepth = ToUint(inputDepth->x() * NewFloat(b, DEPTH_MAX));

		//TODO: Try vectorized shift
		//auto imageColor = ToUint(inputColor * NewFloat4(b, 255.f, 255.f, 255.f, 255.f));

		auto textureColor = CFloat4Lvalue(b.CreateTemporary());
		textureColor = NewFloat4(b, 1, 1, 1, 1);

		if(caps.hasTexture)
		{
			auto texelPos = ToInt(inputTexCoord->xy() / inputTexCoord->zz() * ToFloat(texSize));
			textureColor = GetTextureColor(b, caps.textureFormat, caps.clutFormat, texelPos,
				memoryBuffer, clutImage, texSwizzleTable, texBufAddress, texBufWidth);

			//Modulate
			//TODO: Proper multiply & clamping
			textureColor = textureColor * inputColor * NewFloat4(b, 2, 2, 2, 2);
			textureColor = Clamp(textureColor, NewFloat4(b, 0, 0, 0, 0), NewFloat4(b, 1, 1, 1, 1));
		}
		else
		{
			textureColor = inputColor->xyzw();
		}

		auto screenPos = ToInt(inputPosition->xy());

		auto fbAddress = CIntLvalue(b.CreateTemporaryInt());

		switch(caps.framebufferFormat)
		{
		default:
			assert(false);
		case CGSHandler::PSMCT32:
		case CGSHandler::PSMCT24:
			fbAddress = CMemoryUtils::GetPixelAddress<CGsPixelFormats::STORAGEPSMCT32>(
			    b, fbSwizzleTable, fbBufAddress, fbBufWidth, screenPos);
			break;
		case CGSHandler::PSMCT16S:
			fbAddress = CMemoryUtils::GetPixelAddress<CGsPixelFormats::STORAGEPSMCT16>(
			    b, fbSwizzleTable, fbBufAddress, fbBufWidth, screenPos);
			break;
		}

		BeginInvocationInterlock(b);

		auto dstColor = CFloat4Lvalue(b.CreateTemporary());

		bool needsDstColor = (caps.hasAlphaBlending != 0);
		if(needsDstColor)
		{
			switch(caps.framebufferFormat)
			{
			default:
				assert(false);
			case CGSHandler::PSMCT32:
				{
					auto dstPixel = CMemoryUtils::Memory_Read32(b, memoryBuffer, fbAddress);
					dstColor = CMemoryUtils::PSM32ToVec4(b, dstPixel);
				}
				break;
			case CGSHandler::PSMCT16S:
				{
					auto dstPixel = CMemoryUtils::Memory_Read16(b, memoryBuffer, fbAddress);
					dstColor = CMemoryUtils::PSM16ToVec4(b, dstPixel);
				}
				break;
			}
		}

		if(caps.hasAlphaBlending)
		{
			//Blend
			auto alphaA = GetAlphaABD(b, caps.alphaA, textureColor, dstColor);
			auto alphaB = GetAlphaABD(b, caps.alphaB, textureColor, dstColor);
			auto alphaC = GetAlphaC(b, caps.alphaC, textureColor, dstColor);
			auto alphaD = GetAlphaABD(b, caps.alphaD, textureColor, dstColor);

			auto blendedColor = ((alphaA - alphaB) * alphaC * NewFloat3(b, 2, 2, 2)) + alphaD;
			auto finalColor = NewFloat4(blendedColor, textureColor->w());
			dstColor = Clamp(finalColor, NewFloat4(b, 0, 0, 0, 0), NewFloat4(b, 1, 1, 1, 1));
		}
		else
		{
			dstColor = textureColor->xyzw();
		}

		//Write
		switch(caps.framebufferFormat)
		{
		default:
			assert(false);
		case CGSHandler::PSMCT32:
			{
				auto dstPixel = CMemoryUtils::Vec4ToPSM32(b, dstColor);
				CMemoryUtils::Memory_Write32(b, memoryBuffer, fbAddress, dstPixel);
			}
			break;
		case CGSHandler::PSMCT16S:
			{
				auto dstPixel = CMemoryUtils::Vec4ToPSM16(b, dstColor);
				CMemoryUtils::Memory_Write16(b, memoryBuffer, fbAddress, dstPixel);
			}
			break;
		}

		EndInvocationInterlock(b);

		outputColor = dstColor->xyzw();
	}

	Framework::CMemStream shaderStream;
	Nuanceur::CSpirvShaderGenerator::Generate(shaderStream, b, Nuanceur::CSpirvShaderGenerator::SHADER_TYPE_FRAGMENT);
	shaderStream.Seek(0, Framework::STREAM_SEEK_SET);
	return Framework::Vulkan::CShaderModule(m_context->device, shaderStream);
}

void CDraw::CreateDrawImage()
{
	//This image is needed for MoltenVK/Metal which seem to discard pixels
	//that don't write to any color attachment

	m_drawImage = Framework::Vulkan::CImage(m_context->device, m_context->physicalDeviceMemoryProperties,
	                                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_FORMAT_R8G8B8A8_UNORM, DRAW_AREA_SIZE, DRAW_AREA_SIZE);

	m_drawImage.SetLayout(m_context->queue, m_context->commandBufferPool,
	                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);

	m_drawImageView = m_drawImage.CreateImageView();
}
