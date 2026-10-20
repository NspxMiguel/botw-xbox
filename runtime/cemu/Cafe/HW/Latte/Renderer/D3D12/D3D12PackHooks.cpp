#ifdef ENABLE_D3D12
#include "D3D12PackHooks.h"
#include "D3D12ShaderCompiler.h"

namespace d3d12_packs
{
namespace
{
bool compile_shader(const packs::ShaderReplacement &shader, std::string &error)
{
	D3D12Const::Stage stage;
	switch (shader.stage)
	{
	case packs::ShaderStage::vertex:
		stage = D3D12Const::Stage::Vertex;
		break;
	case packs::ShaderStage::geometry:
		stage = D3D12Const::Stage::Geometry;
		break;
	case packs::ShaderStage::pixel:
		stage = D3D12Const::Stage::Pixel;
		break;
	default:
		error = "Unsupported shader stage";
		return false;
	}
	std::vector<uint8> bytecode;
	D3D12BindingRemap remap;
	return D3D12ShaderCompiler::CompileGLSL(stage, shader.source, bytecode, remap, error);
}
} // namespace
packs::RendererHooks &hooks()
{
	static PackHooks instance{compile_shader};
	return instance;
}
} // namespace d3d12_packs
#endif
