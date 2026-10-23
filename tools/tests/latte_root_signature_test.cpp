#include "Cafe/HW/Latte/Renderer/D3D12/D3D12Common.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12RootSignature.h"
#include <iostream>

HRESULT D3D12SerializeVersionedRootSignature(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *desc, ID3DBlob **, ID3DBlob **)
{
	using namespace D3D12Const;
	const auto &signature = desc->Desc_1_1;
	assert(signature.NumParameters == kRootParamCount);
	const auto *params = signature.pParameters;
	uint32 dwords = 0;
	for (uint32 i = 0; i < signature.NumParameters; ++i)
	{
		const auto &p = params[i];
		switch (p.ParameterType)
		{
		case D3D12_ROOT_PARAMETER_TYPE_CBV:
			dwords += 2;
			break;
		case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE:
			++dwords;
			break;
		case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS:
			dwords += p.Constants.Num32BitValues;
			break;
		}
	}
	assert(dwords == kRootSignatureDwords && dwords == 62 && dwords <= 64);
	const D3D12_SHADER_VISIBILITY visibility[] = {D3D12_SHADER_VISIBILITY_VERTEX, D3D12_SHADER_VISIBILITY_PIXEL, D3D12_SHADER_VISIBILITY_GEOMETRY};
	for (uint32 s = 0; s < (uint32)Stage::Count; ++s)
	{
		const auto stage = (Stage)s;
		const auto &table = params[RootParamStageTable(stage)];
		assert(table.ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE);
		assert(table.ShaderVisibility == visibility[s]);
		assert(table.DescriptorTable.NumDescriptorRanges == 3);
		const auto *ranges = table.DescriptorTable.pDescriptorRanges;
		assert(ranges[0].RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_CBV);
		assert(ranges[0].BaseShaderRegister == RootCBVCount(stage));
		assert(ranges[0].RegisterSpace == RegisterSpace(stage, BindingClass::CBV));
		assert(ranges[0].NumDescriptors + RootCBVCount(stage) == kMaxCBVsPerStage);
		assert(ranges[0].OffsetInDescriptorsFromTableStart == 0);
		assert(ranges[1].OffsetInDescriptorsFromTableStart == ranges[0].NumDescriptors);
		assert(ranges[1].OffsetInDescriptorsFromTableStart == StageTableOffsetSRV(stage));
		assert(ranges[2].OffsetInDescriptorsFromTableStart == StageTableOffsetUAV(stage));
		assert(ranges[2].OffsetInDescriptorsFromTableStart + ranges[2].NumDescriptors == StageTableSize(stage));
		for (uint32 i = 0; i < RootCBVCount(stage); ++i)
		{
			const auto &cbv = params[RootParamStageRootCBV(stage, i)];
			assert(cbv.ParameterType == D3D12_ROOT_PARAMETER_TYPE_CBV);
			assert(cbv.ShaderVisibility == visibility[s]);
			assert(cbv.Descriptor.ShaderRegister == i);
			assert(cbv.Descriptor.RegisterSpace == ranges[0].RegisterSpace);
			assert(cbv.Descriptor.Flags == D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE);
		}
		const auto &samplers = params[RootParamStageSamplers(stage)];
		assert(samplers.ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE);
		assert(samplers.DescriptorTable.pDescriptorRanges[0].RegisterSpace == RegisterSpace(stage, BindingClass::SRV));
	}
	assert(params[kRootParamRuntimeData].Constants.RegisterSpace == kSpaceRuntimeData);
	assert(params[kRootParamRuntimeData].Constants.Num32BitValues == kRuntimeDataDwords);
	assert(params[kRootParamPushConstants].Constants.RegisterSpace == kSpacePushConstants);
	assert(params[kRootParamPushConstants].Constants.Num32BitValues == kPushConstantDwords);
	assert(RootCBVCount(Stage::Count) == 0);
	assert(StageTableSize(Stage::Count) == kStageTableSize);
	assert(StageTableOffsetSRV(Stage::Count) == kStageTableOffsetSRV);
	return 0;
}

int main()
{
	assert(D3D12_SerializeCemuRootSignature(nullptr, nullptr) == 0);
	std::cout << "Production root signature tests passed (62 DWORDs, stage visibility, register split, internal layout).\n";
}
