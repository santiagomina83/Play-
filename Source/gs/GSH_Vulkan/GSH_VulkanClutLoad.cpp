#include "GSH_VulkanClutLoad.h"
#include "GSH_VulkanMemoryUtils.h"
#include "../GsPixelFormats.h"
#include "MemStream.h"
#include "nuanceur/Builder.h"
#include "nuanceur/generators/SpirvShaderGenerator.h"
#include "vulkan/StructDefs.h"

using namespace GSH_Vulkan;

#define DESCRIPTOR_LOCATION_MEMORY 0
#define DESCRIPTOR_LOCATION_CLUT 1
#define DESCRIPTOR_LOCATION_SWIZZLETABLE 2

CClutLoad::CClutLoad(const ContextPtr& context, const FrameCommandBufferPtr& frameCommandBuffer)
    : m_context(context)
    , m_frameCommandBuffer(frameCommandBuffer)
    , m_pipelines(context->device)
{
}

void CClutLoad::DoClutLoad(const CGSHandler::TEX0& tex0)
{
	auto caps = make_convertible<PIPELINE_CAPS>(0);
	caps.idx8 = CGsPixelFormats::IsPsmIDTEX8(tex0.nPsm) ? 1 : 0;
	caps.csm = tex0.nCSM;
	caps.cpsm = tex0.nCPSM;

	auto loadPipeline = m_pipelines.TryGetPipeline(caps);
	if(!loadPipeline)
	{
		loadPipeline = m_pipelines.RegisterPipeline(caps, CreateLoadPipeline(caps));
	}

	LOAD_PARAMS loadParams;
	loadParams.clutBufPtr = tex0.GetCLUTPtr();
	loadParams.csa = tex0.nCSA;

	auto swizzleTable = m_context->GetSwizzleTable(tex0.nCPSM);
	auto descriptorSet = PrepareDescriptorSet(loadPipeline->descriptorSetLayout, swizzleTable);
	auto commandBuffer = m_frameCommandBuffer->GetCommandBuffer();

	m_context->device.vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, loadPipeline->pipelineLayout, 0, 1, &descriptorSet, 0, nullptr);
	m_context->device.vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, loadPipeline->pipeline);
	m_context->device.vkCmdPushConstants(commandBuffer, loadPipeline->pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(LOAD_PARAMS), &loadParams);
	m_context->device.vkCmdDispatch(commandBuffer, 1, 1, 1);
}

VkDescriptorSet CClutLoad::PrepareDescriptorSet(VkDescriptorSetLayout descriptorSetLayout, VkImageView swizzleTable)
{
	VkResult result = VK_SUCCESS;
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
		std::vector<VkWriteDescriptorSet> writes;

		VkDescriptorBufferInfo descriptorMemoryBufferInfo = {};
		descriptorMemoryBufferInfo.buffer = m_context->memoryBuffer;
		descriptorMemoryBufferInfo.range = VK_WHOLE_SIZE;

		VkDescriptorImageInfo descriptorClutImageInfo = {};
		descriptorClutImageInfo.imageView = m_context->clutImageView;
		descriptorClutImageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

		VkDescriptorImageInfo descriptorSwizzleTableImageInfo = {};
		descriptorSwizzleTableImageInfo.imageView = swizzleTable;
		descriptorSwizzleTableImageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

		//Memory Image Descriptor
		{
			auto writeSet = Framework::Vulkan::WriteDescriptorSet();
			writeSet.dstSet = descriptorSet;
			writeSet.dstBinding = DESCRIPTOR_LOCATION_MEMORY;
			writeSet.descriptorCount = 1;
			writeSet.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
			writeSet.pBufferInfo = &descriptorMemoryBufferInfo;
			writes.push_back(writeSet);
		}

		//CLUT Image Descriptor
		{
			auto writeSet = Framework::Vulkan::WriteDescriptorSet();
			writeSet.dstSet = descriptorSet;
			writeSet.dstBinding = DESCRIPTOR_LOCATION_CLUT;
			writeSet.descriptorCount = 1;
			writeSet.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
			writeSet.pImageInfo = &descriptorClutImageInfo;
			writes.push_back(writeSet);
		}

		//Swizzle Table Descriptor
		{
			auto writeSet = Framework::Vulkan::WriteDescriptorSet();
			writeSet.dstSet = descriptorSet;
			writeSet.dstBinding = DESCRIPTOR_LOCATION_SWIZZLETABLE;
			writeSet.descriptorCount = 1;
			writeSet.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
			writeSet.pImageInfo = &descriptorSwizzleTableImageInfo;
			writes.push_back(writeSet);
		}

		m_context->device.vkUpdateDescriptorSets(m_context->device, writes.size(), writes.data(), 0, nullptr);
	}

	return descriptorSet;
}

Framework::Vulkan::CShaderModule CClutLoad::CreateLoadShader(const PIPELINE_CAPS& caps)
{
	using namespace Nuanceur;

	auto b = CShaderBuilder();

	if(caps.idx8)
	{
		b.SetMetadata(CShaderBuilder::METADATA_LOCALSIZE_X, 16);
		b.SetMetadata(CShaderBuilder::METADATA_LOCALSIZE_Y, 16);
	}
	else
	{
		b.SetMetadata(CShaderBuilder::METADATA_LOCALSIZE_X, 8);
		b.SetMetadata(CShaderBuilder::METADATA_LOCALSIZE_Y, 2);
	}

	{
		auto inputInvocationId = CInt4Lvalue(b.CreateInputInt(Nuanceur::SEMANTIC_SYSTEM_GIID));
		auto memoryBuffer = CArrayUintValue(b.CreateUniformArrayUint("memoryBuffer", DESCRIPTOR_LOCATION_MEMORY));
		auto clutImage = CImageUint2DValue(b.CreateImage2DUint(DESCRIPTOR_LOCATION_CLUT));
		auto swizzleTable = CImageUint2DValue(b.CreateImage2DUint(DESCRIPTOR_LOCATION_SWIZZLETABLE));

		auto loadParams = CInt4Lvalue(b.CreateUniformInt4("loadParams", Nuanceur::UNIFORM_UNIT_PUSHCONSTANT));
		auto clutBufPtr = loadParams->x();
		auto csa = loadParams->y();

		auto colorPos = inputInvocationId->xy();
		auto colorPixel = CUintLvalue(b.CreateTemporaryUint());
		auto clutIndex = CIntLvalue(b.CreateTemporaryInt());

		switch(caps.cpsm)
		{
		case CGSHandler::PSMCT32:
		{
			auto colorAddress = CMemoryUtils::GetPixelAddress<CGsPixelFormats::STORAGEPSMCT32>(
			    b, swizzleTable, clutBufPtr, NewInt(b, 64), colorPos);
			colorPixel = CMemoryUtils::Memory_Read32(b, memoryBuffer, colorAddress);
		}
		break;
		default:
			assert(false);
			break;
		}

		if(caps.idx8)
		{
			clutIndex = colorPos->x() + (colorPos->y() * NewInt(b, 16));
			clutIndex = (clutIndex & NewInt(b, ~0x18)) | ((clutIndex & NewInt(b, 0x08)) << NewInt(b, 1)) | ((clutIndex & NewInt(b, 0x10)) >> NewInt(b, 1));
		}
		else
		{
			clutIndex = colorPos->x() + (colorPos->y() * NewInt(b, 8));
			clutIndex = clutIndex + (csa * NewInt(b, 16));
		}

		switch(caps.cpsm)
		{
		case CGSHandler::PSMCT32:
		{
			auto colorPixelLo = (colorPixel)&NewUint(b, 0xFFFF);
			auto colorPixelHi = (colorPixel >> NewUint(b, 16)) & NewUint(b, 0xFFFF);
			auto clutIndexLo = NewInt2(clutIndex, NewInt(b, 0));
			auto clutIndexHi = NewInt2(clutIndex + NewInt(b, 0x100), NewInt(b, 0));
			Store(clutImage, clutIndexLo, NewUint4(colorPixelLo, NewUint3(b, 0, 0, 0)));
			Store(clutImage, clutIndexHi, NewUint4(colorPixelHi, NewUint3(b, 0, 0, 0)));
		}
		break;
		default:
			assert(false);
			break;
		}
	}

	Framework::CMemStream shaderStream;
	Nuanceur::CSpirvShaderGenerator::Generate(shaderStream, b, Nuanceur::CSpirvShaderGenerator::SHADER_TYPE_COMPUTE);
	shaderStream.Seek(0, Framework::STREAM_SEEK_SET);
	return Framework::Vulkan::CShaderModule(m_context->device, shaderStream);
}

PIPELINE CClutLoad::CreateLoadPipeline(const PIPELINE_CAPS& caps)
{
	PIPELINE loadPipeline;

	auto loadShader = CreateLoadShader(caps);

	VkResult result = VK_SUCCESS;

	{
		std::vector<VkDescriptorSetLayoutBinding> bindings;

		//GS memory
		{
			VkDescriptorSetLayoutBinding binding = {};
			binding.binding = DESCRIPTOR_LOCATION_MEMORY;
			binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
			binding.descriptorCount = 1;
			binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
			bindings.push_back(binding);
		}

		//CLUT buffer
		{
			VkDescriptorSetLayoutBinding binding = {};
			binding.binding = DESCRIPTOR_LOCATION_CLUT;
			binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
			binding.descriptorCount = 1;
			binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
			bindings.push_back(binding);
		}

		//Swizzle table
		{
			VkDescriptorSetLayoutBinding binding = {};
			binding.binding = DESCRIPTOR_LOCATION_SWIZZLETABLE;
			binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
			binding.descriptorCount = 1;
			binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
			bindings.push_back(binding);
		}

		auto createInfo = Framework::Vulkan::DescriptorSetLayoutCreateInfo();
		createInfo.bindingCount = bindings.size();
		createInfo.pBindings = bindings.data();
		result = m_context->device.vkCreateDescriptorSetLayout(m_context->device, &createInfo, nullptr, &loadPipeline.descriptorSetLayout);
		CHECKVULKANERROR(result);
	}

	{
		VkPushConstantRange pushConstantInfo = {};
		pushConstantInfo.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
		pushConstantInfo.offset = 0;
		pushConstantInfo.size = sizeof(LOAD_PARAMS);

		auto pipelineLayoutCreateInfo = Framework::Vulkan::PipelineLayoutCreateInfo();
		pipelineLayoutCreateInfo.pushConstantRangeCount = 1;
		pipelineLayoutCreateInfo.pPushConstantRanges = &pushConstantInfo;
		pipelineLayoutCreateInfo.setLayoutCount = 1;
		pipelineLayoutCreateInfo.pSetLayouts = &loadPipeline.descriptorSetLayout;

		result = m_context->device.vkCreatePipelineLayout(m_context->device, &pipelineLayoutCreateInfo, nullptr, &loadPipeline.pipelineLayout);
		CHECKVULKANERROR(result);
	}

	{
		auto createInfo = Framework::Vulkan::ComputePipelineCreateInfo();
		createInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		createInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
		createInfo.stage.pName = "main";
		createInfo.stage.module = loadShader;
		createInfo.layout = loadPipeline.pipelineLayout;

		result = m_context->device.vkCreateComputePipelines(m_context->device, VK_NULL_HANDLE, 1, &createInfo, nullptr, &loadPipeline.pipeline);
		CHECKVULKANERROR(result);
	}

	return loadPipeline;
}
