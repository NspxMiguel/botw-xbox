// SPDX-License-Identifier: MIT
#ifdef ENABLE_D3D12
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12Fsr.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12Renderer.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12SwapChain.h"
#include "fsr1_source.h"
#include <d3dcompiler.h>

namespace
{
const char *wrapper = R"hlsl(
Texture2D<float4> Source : register(t0);
SamplerState ClampSampler : register(s0);
cbuffer Params : register(b0) { float2 inputSize; float2 outputSize; float2 origin; float sharpness; };
AF4 FsrEasuRF(AF2 p) { return Source.GatherRed(ClampSampler, p); }
AF4 FsrEasuGF(AF2 p) { return Source.GatherGreen(ClampSampler, p); }
AF4 FsrEasuBF(AF2 p) { return Source.GatherBlue(ClampSampler, p); }
AF4 FsrRcasLoadF(ASU2 p) { return Source.Load(int3(clamp(p, int2(0,0), int2(outputSize)-1), 0)); }
void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}
float4 VS(uint id : SV_VertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2,-2) + float2(-1,1), 0, 1);
}
float4 EASU(float4 position : SV_Position) : SV_Target {
    AU4 c0, c1, c2, c3;
    FsrEasuCon(c0,c1,c2,c3,inputSize.x,inputSize.y,inputSize.x,inputSize.y,outputSize.x,outputSize.y);
    AF3 color;
    FsrEasuF(color, AU2(position.xy), c0,c1,c2,c3);
    return float4(color,1);
}
float4 RCAS(float4 position : SV_Position) : SV_Target {
    AU4 con;
    FsrRcasCon(con,sharpness);
    AF3 color;
    FsrRcasF(color.r,color.g,color.b, AU2(position.xy-origin),con);
    return float4(color,1);
}
)hlsl";
ComPtr<ID3DBlob> compile(const std::string &source, const char *entry, const char *profile)
{
	ComPtr<ID3DBlob> code, errors;
	HRESULT hr = D3DCompile(source.data(), source.size(), "AMD_FSR1", nullptr, nullptr, entry, profile,
							D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
	if (FAILED(hr))
		throw std::runtime_error(
			errors ? std::string(static_cast<const char *>(errors->GetBufferPointer()), errors->GetBufferSize())
				   : "FSR shader compilation failed");
	return code;
}
} // namespace
D3D12Fsr::D3D12Fsr(D3D12Renderer &r) : renderer(r)
{
	auto *device = r.GetDevice();
	D3D12_DESCRIPTOR_RANGE range{};
	range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	range.NumDescriptors = 1;
	range.OffsetInDescriptorsFromTableStart = D3D12Const::kStageTableOffsetSRV;
	D3D12_ROOT_PARAMETER params[2]{};
	params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	params[0].Constants.Num32BitValues = 7;
	params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	params[1].DescriptorTable = {1, &range};
	D3D12_STATIC_SAMPLER_DESC sampler{};
	sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
	sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
	sampler.MaxAnisotropy = 1;
	sampler.MaxLOD = D3D12_FLOAT32_MAX;
	sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	D3D12_ROOT_SIGNATURE_DESC desc{};
	desc.NumParameters = 2;
	desc.pParameters = params;
	desc.NumStaticSamplers = 1;
	desc.pStaticSamplers = &sampler;
	ComPtr<ID3DBlob> blob, errors;
	D3D12_ThrowIfFailed(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors),
						"FSR root serialization");
	D3D12_ThrowIfFailed(
		device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root)),
		"FSR root");
	std::string source = "#define A_GPU 1\n#define A_HLSL 1\n#define FSR_EASU_F 1\n#define FSR_RCAS_F 1\n";
	for (const char *chunk : kFsrMath)
		source += chunk;
	for (const char *chunk : kFsrAlgorithm)
		source += chunk;
	source += wrapper;
	auto vs = compile(source, "VS", "vs_5_0");
	auto makePipeline = [&](const char *entry, ComPtr<ID3D12PipelineState> &pipeline) {
		auto ps = compile(source, entry, "ps_5_0");
		D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
		pso.pRootSignature = root.Get();
		pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
		pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
		for (auto &target : pso.BlendState.RenderTarget)
		{
			target.SrcBlend = target.SrcBlendAlpha = D3D12_BLEND_ONE;
			target.DestBlend = target.DestBlendAlpha = D3D12_BLEND_ZERO;
			target.BlendOp = target.BlendOpAlpha = D3D12_BLEND_OP_ADD;
			target.LogicOp = D3D12_LOGIC_OP_NOOP;
		}
		pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
		pso.SampleMask = UINT_MAX;
		pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
		pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
		pso.RasterizerState.DepthClipEnable = TRUE;
		pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
		pso.DepthStencilState.StencilReadMask = pso.DepthStencilState.StencilWriteMask = 0xff;
		pso.DepthStencilState.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
		pso.DepthStencilState.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
		pso.DepthStencilState.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
		pso.DepthStencilState.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
		pso.DepthStencilState.BackFace = pso.DepthStencilState.FrontFace;
		pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		pso.NumRenderTargets = 1;
		pso.RTVFormats[0] = D3D12SwapChain::kFormat;
		pso.SampleDesc.Count = 1;
		D3D12_ThrowIfFailed(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&pipeline)), "FSR pipeline");
	};
	makePipeline("EASU", easu);
	makePipeline("RCAS", rcas);
	inputRTV = r.GetStagingRTVHeap().Allocate();
	scaledRTV = r.GetStagingRTVHeap().Allocate();
	inputSRV = r.GetStagingViewHeap().Allocate();
	scaledSRV = r.GetStagingViewHeap().Allocate();
}
D3D12Fsr::~D3D12Fsr()
{
	renderer.ReleaseResourceDeferred(std::move(input));
	renderer.ReleaseResourceDeferred(std::move(scaled));
	renderer.GetStagingRTVHeap().Free(inputRTV);
	renderer.GetStagingRTVHeap().Free(scaledRTV);
	renderer.GetStagingViewHeap().Free(inputSRV);
	renderer.GetStagingViewHeap().Free(scaledSRV);
}
void D3D12Fsr::ResizeTargets(uint32 iw, uint32 ih, uint32 ow, uint32 oh)
{
	if (iw != inWidth || ih != inHeight || ow != outWidth || oh != outHeight)
	{
		// Called between frames. SRVs are copied into submission-owned GPU tables;
		// RTV descriptors are consumed by recording calls. Retire only the resources
		// by submission fence, so replacing these CPU staging views needs no idle wait.
		auto create = [&](uint32 w, uint32 h, ComPtr<ID3D12Resource> &texture, D3D12_CPU_DESCRIPTOR_HANDLE rtv,
						  D3D12_CPU_DESCRIPTOR_HANDLE srv) {
			D3D12_RESOURCE_DESC d{};
			d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			d.Width = w;
			d.Height = h;
			d.DepthOrArraySize = d.MipLevels = d.SampleDesc.Count = 1;
			d.Format = D3D12SwapChain::kFormat;
			d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
			auto heap = D3D12_HeapProps(D3D12_HEAP_TYPE_DEFAULT);
			renderer.ReleaseResourceDeferred(std::move(texture));
			D3D12_ThrowIfFailed(renderer.GetDevice()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d,
																			  D3D12_RESOURCE_STATE_COMMON, nullptr,
																			  IID_PPV_ARGS(&texture)),
								"FSR texture");
			renderer.GetDevice()->CreateRenderTargetView(texture.Get(), nullptr, rtv);
			renderer.GetDevice()->CreateShaderResourceView(texture.Get(), nullptr, srv);
		};
		create(iw, ih, input, inputRTV, inputSRV);
		create(ow, oh, scaled, scaledRTV, scaledSRV);
		inWidth = iw;
		inHeight = ih;
		outWidth = ow;
		outHeight = oh;
		inputState = scaledState = D3D12_RESOURCE_STATE_COMMON;
		cemuLog_log(LogType::Force, "D3D12: AMD FSR1 EASU + RCAS: {}x{} -> {}x{}", iw, ih, ow, oh);
	}
}
void D3D12Fsr::BeginInput()
{
	renderer.TransitionResource(input.Get(), inputState, D3D12_RESOURCE_STATE_RENDER_TARGET);
	renderer.FlushBarriers();
}
void D3D12Fsr::Draw(D3D12SwapChain &swapChain, sint32 x, sint32 y, uint32 w, uint32 h, bool clear)
{
	auto *cmd = renderer.GetCommandList();
	renderer.TransitionResource(input.Get(), inputState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	renderer.TransitionResource(scaled.Get(), scaledState, D3D12_RESOURCE_STATE_RENDER_TARGET);
	renderer.FlushBarriers();
	renderer.BindRootSignatureAndHeaps();
	cmd->SetGraphicsRootSignature(root.Get());
	// AMD's 0.2-stop default applies only to this upscaling path.
	struct Params
	{
		float iw, ih, ow, oh, x, y, sharpness;
	} params{float(inWidth), float(inHeight), float(w), float(h), 0, 0, 0.2f};
	auto draw = [&](D3D12_CPU_DESCRIPTOR_HANDLE srv, D3D12_CPU_DESCRIPTOR_HANDLE rtv, ID3D12PipelineState *pipeline,
					float vx, float vy) {
		D3D12StageTableDesc table;
		table.srv[0] = srv;
		auto gpu = renderer.WriteStageTable(table);
		cmd->SetGraphicsRoot32BitConstants(0, 7, &params, 0);
		cmd->SetGraphicsRootDescriptorTable(1, gpu);
		cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
		D3D12_VIEWPORT vp{vx, vy, float(w), float(h), 0, 1};
		D3D12_RECT scissor{0, 0, LONG(pipeline == easu.Get() ? w : swapChain.GetWidth()),
						   LONG(pipeline == easu.Get() ? h : swapChain.GetHeight())};
		cmd->RSSetViewports(1, &vp);
		cmd->RSSetScissorRects(1, &scissor);
		cmd->SetPipelineState(pipeline);
		cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		cmd->DrawInstanced(3, 1, 0, 0);
	};
	draw(inputSRV, scaledRTV, easu.Get(), 0, 0);
	renderer.TransitionResource(scaled.Get(), scaledState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	renderer.TransitionResource(swapChain.GetCurrentBackbuffer(), swapChain.GetCurrentBackbufferState(),
								D3D12_RESOURCE_STATE_RENDER_TARGET);
	renderer.FlushBarriers();
	if (clear)
	{
		const float black[4] = {0, 0, 0, 1};
		cmd->ClearRenderTargetView(swapChain.GetCurrentRTV(), black, 0, nullptr);
	}
	params.x = float(x);
	params.y = float(y);
	draw(scaledSRV, swapChain.GetCurrentRTV(), rcas.Get(), float(x), float(y));
	renderer.InvalidateDrawState();
}
#endif
