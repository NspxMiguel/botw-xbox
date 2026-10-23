#pragma once

// Capture the production root-signature description on hosts without the Windows SDK.
using HRESULT = int;
using UINT = unsigned;
struct ID3DBlob;
enum D3D12_ROOT_PARAMETER_TYPE
{
	D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE,
	D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS,
	D3D12_ROOT_PARAMETER_TYPE_CBV
};
enum D3D12_SHADER_VISIBILITY
{
	D3D12_SHADER_VISIBILITY_ALL,
	D3D12_SHADER_VISIBILITY_VERTEX,
	D3D12_SHADER_VISIBILITY_PIXEL,
	D3D12_SHADER_VISIBILITY_GEOMETRY
};
enum D3D12_DESCRIPTOR_RANGE_TYPE
{
	D3D12_DESCRIPTOR_RANGE_TYPE_CBV,
	D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
	D3D12_DESCRIPTOR_RANGE_TYPE_UAV,
	D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER
};
using D3D12_DESCRIPTOR_RANGE_FLAGS = unsigned;
inline constexpr unsigned D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE = 1;
inline constexpr unsigned D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE = 2;
inline constexpr unsigned D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE = 2;
inline constexpr unsigned D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT = 1;
inline constexpr unsigned D3D_ROOT_SIGNATURE_VERSION_1_1 = 2;
struct D3D12_DESCRIPTOR_RANGE1
{
	D3D12_DESCRIPTOR_RANGE_TYPE RangeType{};
	UINT NumDescriptors{}, BaseShaderRegister{}, RegisterSpace{};
	D3D12_DESCRIPTOR_RANGE_FLAGS Flags{};
	UINT OffsetInDescriptorsFromTableStart{};
};
struct D3D12_ROOT_PARAMETER1
{
	D3D12_ROOT_PARAMETER_TYPE ParameterType{};
	struct
	{
		UINT NumDescriptorRanges{};
		const D3D12_DESCRIPTOR_RANGE1 *pDescriptorRanges{};
	} DescriptorTable;
	struct
	{
		UINT ShaderRegister{}, RegisterSpace{}, Flags{};
	} Descriptor;
	struct
	{
		UINT ShaderRegister{}, RegisterSpace{}, Num32BitValues{};
	} Constants;
	D3D12_SHADER_VISIBILITY ShaderVisibility{};
};
struct D3D12_VERSIONED_ROOT_SIGNATURE_DESC
{
	unsigned Version{};
	struct
	{
		UINT NumParameters{};
		const D3D12_ROOT_PARAMETER1 *pParameters{};
		UINT NumStaticSamplers{};
		const void *pStaticSamplers{};
		unsigned Flags{};
	} Desc_1_1;
};
HRESULT D3D12SerializeVersionedRootSignature(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *, ID3DBlob **, ID3DBlob **);
