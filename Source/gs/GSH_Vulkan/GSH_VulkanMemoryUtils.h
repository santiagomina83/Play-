#pragma once

#include "nuanceur/Builder.h"

namespace GSH_Vulkan
{
	class CMemoryUtils
	{
	public:
		static Nuanceur::CUintRvalue Memory_Read32(Nuanceur::CShaderBuilder&, Nuanceur::CImageUint2DValue, Nuanceur::CIntValue);

		static void Memory_Write32(Nuanceur::CShaderBuilder&, Nuanceur::CImageUint2DValue, Nuanceur::CIntValue, Nuanceur::CUintValue);

		static Nuanceur::CFloat4Rvalue PSM32ToVec4(Nuanceur::CShaderBuilder&, Nuanceur::CUintValue);

		static Nuanceur::CUintRvalue Vec4ToPSM32(Nuanceur::CShaderBuilder&, Nuanceur::CFloat4Value);
	};
}
