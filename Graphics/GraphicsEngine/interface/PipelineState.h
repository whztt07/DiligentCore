/*     Copyright 2015-2018 Egor Yusov
 *  
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 * 
 *     http://www.apache.org/licenses/LICENSE-2.0
 * 
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT OF ANY PROPRIETARY RIGHTS.
 *
 *  In no event and under no legal theory, whether in tort (including negligence), 
 *  contract, or otherwise, unless required by applicable law (such as deliberate 
 *  and grossly negligent acts) or agreed to in writing, shall any Contributor be
 *  liable for any damages, including any direct, indirect, special, incidental, 
 *  or consequential damages of any character arising as a result of this License or 
 *  out of the use or inability to use the software (including but not limited to damages 
 *  for loss of goodwill, work stoppage, computer failure or malfunction, or any and 
 *  all other commercial damages or losses), even if such Contributor has been advised 
 *  of the possibility of such damages.
 */

#pragma once

/// \file
/// Definition of the Diligent::IRenderDevice interface and related data structures

#include "../../../Primitives/interface/Object.h"
#include "../../../Platforms/interface/PlatformDefinitions.h"
#include "GraphicsTypes.h"
#include "BlendState.h"
#include "RasterizerState.h"
#include "DepthStencilState.h"
#include "InputLayout.h"
#include "ShaderResourceBinding.h"

namespace Diligent
{

/// Primitive topology type

/// [D3D12_PRIMITIVE_TOPOLOGY_TYPE]: https://msdn.microsoft.com/en-us/library/windows/desktop/dn770385(v=vs.85).aspx
/// This enumeration describes primitive topology type. It generally mirrors [D3D12_PRIMITIVE_TOPOLOGY_TYPE] enumeration.
/// The enumeration is used by GraphicsPipelineDesc to describe how the pipeline interprets 
/// geometry or hull shader input primitives
enum PRIMITIVE_TOPOLOGY_TYPE : Int32
{
    /// Topology is not defined
    PRIMITIVE_TOPOLOGY_TYPE_UNDEFINED	= 0,

    /// Interpret the input primitive as a point.
    PRIMITIVE_TOPOLOGY_TYPE_POINT,

    /// Interpret the input primitive as a line.
    PRIMITIVE_TOPOLOGY_TYPE_LINE,

    /// Interpret the input primitive as a triangle.
    PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE,

    /// Interpret the input primitive as a control point patch.
    PRIMITIVE_TOPOLOGY_TYPE_PATCH,

    /// Total number of topology types
    PRIMITIVE_TOPOLOGY_TYPE_NUM_TYPES
};
    
/// Sample description

/// This structure is used by GraphicsPipelineDesc to describe multisampling parameters
struct SampleDesc
{
    /// Sample count
    Uint32 Count;

    /// Quality
    Uint32 Quality;

    SampleDesc() : 
        Count(1),
        Quality(0)
    {}
};


/// Graphics pipeline state description

/// This structure describes the graphics pipeline state and is part of the PipelineStateDesc structure.
struct GraphicsPipelineDesc
{
    /// Vertex shader to be used with the pipeline
    IShader *pVS = nullptr;

    /// Pixel shader to be used with the pipeline
    IShader *pPS = nullptr;

    /// Domain shader to be used with the pipeline
    IShader *pDS = nullptr;

    /// Hull shader to be used with the pipeline
    IShader *pHS = nullptr;

    /// Geometry shader to be used with the pipeline
    IShader *pGS = nullptr;
    
    //D3D12_STREAM_OUTPUT_DESC StreamOutput;
    
    /// Blend state description
    BlendStateDesc BlendDesc;

    /// 32-bit sample mask that determines which samples get updated 
    /// in all the active render targets. A sample mask is always applied; 
    /// it is independent of whether multisampling is enabled, and does not 
    /// depend on whether an application uses multisample render targets.
    Uint32 SampleMask = 0xFFFFFFFF;

    /// Rasterizer state description
    RasterizerStateDesc RasterizerDesc;

    /// Depth-stencil state description
    DepthStencilStateDesc DepthStencilDesc;

    /// Input layout
    InputLayoutDesc InputLayout;
    //D3D12_INDEX_BUFFER_STRIP_CUT_VALUE IBStripCutValue;

    /// Primitive topology type
    PRIMITIVE_TOPOLOGY_TYPE PrimitiveTopologyType = PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;

    /// Number of render targets in the RTVFormats member
    Uint32 NumRenderTargets = 0;

    /// Render target formats
    TEXTURE_FORMAT RTVFormats[8];

    /// Depth-stencil format
    TEXTURE_FORMAT DSVFormat;

    /// Multisampling parameters
    SampleDesc SmplDesc;

    /// Node mask.
    Uint32 NodeMask = 0;

    //D3D12_CACHED_PIPELINE_STATE CachedPSO;
    //D3D12_PIPELINE_STATE_FLAGS Flags;

    GraphicsPipelineDesc()
    {
        for(size_t rt = 0; rt < _countof(RTVFormats); ++rt)
            RTVFormats[rt] = TEX_FORMAT_UNKNOWN;
        DSVFormat = TEX_FORMAT_UNKNOWN;
    };
};


/// Compute pipeline state description

/// This structure describes the compute pipeline state and is part of the PipelineStateDesc structure.
struct ComputePipelineDesc
{
    /// Compute shader to be used with the pipeline
    IShader *pCS;
    ComputePipelineDesc() : 
        pCS(nullptr)
    {}
};

/// Pipeline state description
struct PipelineStateDesc : DeviceObjectAttribs
{
    /// Flag indicating if pipeline state is a compute pipeline state
    bool IsComputePipeline;

    /// Graphics pipeline state description. This memeber is ignored if IsComputePipeline == True
    GraphicsPipelineDesc GraphicsPipeline;

    /// Compute pipeline state description. This memeber is ignored if IsComputePipeline == False
    ComputePipelineDesc ComputePipeline;

    /// Shader resource binding allocation granularity
    
    /// This member defines allocation granularity for internal resources required by the shader resource
    /// binding object instances.
    Uint32 SRBAllocationGranularity;

    PipelineStateDesc() : 
        IsComputePipeline(false),
        SRBAllocationGranularity(1)
    {}
};

// {06084AE5-6A71-4FE8-84B9-395DD489A28C}
static constexpr INTERFACE_ID IID_PipelineState =
{ 0x6084ae5, 0x6a71, 0x4fe8, { 0x84, 0xb9, 0x39, 0x5d, 0xd4, 0x89, 0xa2, 0x8c } };

/**
  * Pipeline state interface
  */
class IPipelineState : public IDeviceObject
{
public:
    /// Queries the specific interface, see IObject::QueryInterface() for details
    virtual void QueryInterface( const INTERFACE_ID &IID, IObject **ppInterface ) = 0;

    /// Returns the blend state description used to create the object
    virtual const PipelineStateDesc& GetDesc()const = 0;

    /// Binds resources for all shaders in the pipeline state

    /// \param [in] pResourceMapping - Pointer to the resource mapping interface.
    /// \param [in] Flags - Additional flags. See Diligent::BIND_SHADER_RESOURCES_FLAGS.
    /// \remarks For older OpenGL devices that do not support program pipelines 
    ///          (OpenGL4.1-, OpenGLES3.0-). This function is the only way to bind
    ///          shader resources.
    virtual void BindShaderResources( IResourceMapping *pResourceMapping, Uint32 Flags ) = 0;

    /// Creates a shader resource binding object

    /// \param [out] ppShaderResourceBinding - memory location where pointer to the new shader resource
    ///                                        binding object is written.
    virtual void CreateShaderResourceBinding( IShaderResourceBinding **ppShaderResourceBinding ) = 0;

    /// Checks if this pipeline state object is compatible with another PSO

    /// If two pipeline state objects are compatible, they can use shader resource binding
    /// objects interchangebly, i.e. SRBs created by one PSO can be committed
    /// when another PSO is bound.
    /// \param [in] pPSO - Pointer to the pipeline state object to check compatibility with
    /// \return     true if this PSO is compatbile with pPSO. false otherwise.
    /// \remarks    The function only checks that shader resource layouts are compatible, but 
    ///             does not check if resource types match. For instance, if a pixel shader in one PSO
    ///             uses a texture at slot 0, and a pixel shader in another PSO uses texture array at slot 0,
    ///             the pipelines will be compatible. However, if you try to use SRB object from the first pipeline
    ///             to commit resources for the second pipeline, a runtime error will occur.\n
    ///             The function only checks compatibility of shader resource layouts. It does not take
    ///             into account vertex shader input layout, number of outputs, etc.
    virtual bool IsCompatibleWith(const IPipelineState *pPSO)const = 0;
};

}
