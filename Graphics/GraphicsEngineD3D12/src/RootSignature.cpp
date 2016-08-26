/*     Copyright 2015-2016 Egor Yusov
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

#include "pch.h"

#include "RootSignature.h"
#include "ShaderResourceLayoutD3D12.h"
#include "D3DShaderResourceLoader.h"
#include "ShaderD3D12Impl.h"
#include "CommandContext.h"
#include "RenderDeviceD3D12Impl.h"
#include "TextureD3D12Impl.h"
#include "BufferD3D12Impl.h"
#include "D3D12TypeConversions.h"

namespace Diligent
{

RootSignature::RootParamsManager::RootParamsManager(IMemoryAllocator &MemAllocator):
    m_MemAllocator(MemAllocator),
    m_pMemory(nullptr, STDDeleter<void, IMemoryAllocator>(MemAllocator))
{}

size_t RootSignature::RootParamsManager::GetRequiredMemorySize(Uint32 NumExtraRootTables, Uint32 NumExtraRootViews, Uint32 NumExtraDescriptorRanges)const
{
    return sizeof(RootParameter) * (m_NumRootTables + NumExtraRootTables + m_NumRootViews + NumExtraRootViews) +  sizeof(D3D12_DESCRIPTOR_RANGE) * (m_TotalDescriptorRanges + NumExtraDescriptorRanges);
}

D3D12_DESCRIPTOR_RANGE* RootSignature::RootParamsManager::Extend(Uint32 NumExtraRootTables, Uint32 NumExtraRootViews, Uint32 NumExtraDescriptorRanges, Uint32 RootTableToAddRanges)
{
    VERIFY(NumExtraRootTables > 0 || NumExtraRootViews > 0 || NumExtraDescriptorRanges > 0, "At least one root table, root view or descriptor range must be added" );
    auto MemorySize = GetRequiredMemorySize(NumExtraRootTables, NumExtraRootViews, NumExtraDescriptorRanges);
    VERIFY_EXPR(MemorySize > 0);
    auto *pNewMemory = ALLOCATE(m_MemAllocator, "Memory buffer for root tables, root views & descriptor ranges", MemorySize);
    memset(pNewMemory, 0, MemorySize);

    // Note: this order is more efficient than views->tables->ranges
    auto *pNewRootTables = reinterpret_cast<RootParameter*>(pNewMemory);
    auto *pNewRootViews = pNewRootTables + (m_NumRootTables + NumExtraRootTables);
    auto *pCurrDescriptorRangePtr = reinterpret_cast<D3D12_DESCRIPTOR_RANGE*>(pNewRootViews+m_NumRootViews+NumExtraRootViews);

    // Copy existing root tables to new memory
    for (Uint32 rt = 0; rt < m_NumRootTables; ++rt)
    {
        const auto &SrcTbl = GetRootTable(rt);
        auto &D3D12SrcTbl = static_cast<const D3D12_ROOT_PARAMETER&>(SrcTbl).DescriptorTable;
        auto NumRanges = D3D12SrcTbl.NumDescriptorRanges;
        if(rt == RootTableToAddRanges)
        {
            VERIFY(NumExtraRootTables == 0 || NumExtraRootTables == 1, "Up to one descriptor table can be extended at a time");
            NumRanges += NumExtraDescriptorRanges;
        }
        new(pNewRootTables + rt) RootParameter(SrcTbl, NumRanges, pCurrDescriptorRangePtr);
        pCurrDescriptorRangePtr += NumRanges;
    }

    // Copy existing root view to new memory
    for (Uint32 rv = 0; rv < m_NumRootViews; ++rv)
    {
        const auto &SrcView = GetRootView(rv);
        new(pNewRootViews + rv) RootParameter(SrcView);
    }

    m_pMemory.reset(pNewMemory);
    m_NumRootTables += NumExtraRootTables;
    m_NumRootViews += NumExtraRootViews;
    m_TotalDescriptorRanges += NumExtraDescriptorRanges;
    m_pRootTables = m_NumRootTables != 0 ? pNewRootTables : nullptr;
    m_pRootViews = m_NumRootViews != 0 ?  pNewRootViews : nullptr;

    return pCurrDescriptorRangePtr;
}

void RootSignature::RootParamsManager::AddRootView(D3D12_ROOT_PARAMETER_TYPE ParameterType, Uint32 RootIndex, UINT Register, D3D12_SHADER_VISIBILITY Visibility, SHADER_VARIABLE_TYPE VarType)
{
    auto *pRangePtr = Extend(0, 1, 0);
    VERIFY_EXPR((char*)pRangePtr == (char*)m_pMemory.get() + GetRequiredMemorySize(0, 0, 0));
    new(m_pRootViews + m_NumRootViews-1) RootParameter(ParameterType, RootIndex, Register, 0u, Visibility, VarType);
}

void RootSignature::RootParamsManager::AddRootTable(Uint32 RootIndex, D3D12_SHADER_VISIBILITY Visibility, SHADER_VARIABLE_TYPE VarType, Uint32 NumRangesInNewTable)
{
    auto *pRangePtr = Extend(1, 0, NumRangesInNewTable);
    VERIFY_EXPR( (char*)(pRangePtr + NumRangesInNewTable) == (char*)m_pMemory.get() + GetRequiredMemorySize(0, 0, 0));
    new(m_pRootTables + m_NumRootTables-1) RootParameter(D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE, RootIndex, NumRangesInNewTable, pRangePtr, Visibility, VarType);
}

void RootSignature::RootParamsManager::AddDescriptorRanges(Uint32 RootTableInd, Uint32 NumExtraRanges)
{
    auto *pRangePtr = Extend(0, 0, NumExtraRanges, RootTableInd);
    VERIFY_EXPR( (char*)pRangePtr == (char*)m_pMemory.get() + GetRequiredMemorySize(0, 0, 0));
}


RootSignature::RootSignature() : 
    m_RootParams(GetRawAllocator()),
    m_MemAllocator(GetRawAllocator()),
    m_StaticSamplers( STD_ALLOCATOR_RAW_MEM(StaticSamplerAttribs, GetRawAllocator(), "Allocator for vector<StaticSamplerAttribs>") )
{
    for(size_t s=0; s < SHADER_VARIABLE_TYPE_NUM_TYPES; ++s)
    {
        m_TotalSrvCbvUavSlots[s] = 0;
        m_TotalSamplerSlots[s] = 0;
    }

    for(size_t i=0; i < _countof(m_SrvCbvUavRootTablesMap); ++i)
        m_SrvCbvUavRootTablesMap[i] = InvalidRootTableIndex;
    for(size_t i=0; i < _countof(m_SamplerRootTablesMap); ++i)
        m_SamplerRootTablesMap[i] = InvalidRootTableIndex;

}

static D3D12_SHADER_VISIBILITY ShaderTypeInd2ShaderVisibilityMap[]
{
    D3D12_SHADER_VISIBILITY_VERTEX,     // 0
    D3D12_SHADER_VISIBILITY_PIXEL,      // 1
    D3D12_SHADER_VISIBILITY_GEOMETRY,   // 2
    D3D12_SHADER_VISIBILITY_HULL,       // 3
    D3D12_SHADER_VISIBILITY_DOMAIN,     // 4
    D3D12_SHADER_VISIBILITY_ALL         // 5
};
D3D12_SHADER_VISIBILITY GetShaderVisibility(SHADER_TYPE ShaderType)
{
    auto ShaderInd = GetShaderTypeIndex(ShaderType);
    auto ShaderVisibility = ShaderTypeInd2ShaderVisibilityMap[ShaderInd];
#ifdef _DEBUG
    switch (ShaderType)
    {
        case SHADER_TYPE_VERTEX:    VERIFY_EXPR(ShaderVisibility == D3D12_SHADER_VISIBILITY_VERTEX);    break;
        case SHADER_TYPE_PIXEL:     VERIFY_EXPR(ShaderVisibility == D3D12_SHADER_VISIBILITY_PIXEL);     break;
        case SHADER_TYPE_GEOMETRY:  VERIFY_EXPR(ShaderVisibility == D3D12_SHADER_VISIBILITY_GEOMETRY);  break;
        case SHADER_TYPE_HULL:      VERIFY_EXPR(ShaderVisibility == D3D12_SHADER_VISIBILITY_HULL);      break;
        case SHADER_TYPE_DOMAIN:    VERIFY_EXPR(ShaderVisibility == D3D12_SHADER_VISIBILITY_DOMAIN);    break;
        case SHADER_TYPE_COMPUTE:   VERIFY_EXPR(ShaderVisibility == D3D12_SHADER_VISIBILITY_ALL);       break;
        default: LOG_ERROR("Unknown shader type (", ShaderType, ")"); break;
    }
#endif
    return ShaderVisibility;
}

static SHADER_TYPE ShaderVisibility2ShaderTypeMap[] = 
{
    SHADER_TYPE_COMPUTE,    // D3D12_SHADER_VISIBILITY_ALL	    = 0
    SHADER_TYPE_VERTEX,     // D3D12_SHADER_VISIBILITY_VERTEX	= 1
    SHADER_TYPE_HULL,       // D3D12_SHADER_VISIBILITY_HULL	    = 2
    SHADER_TYPE_DOMAIN,     // D3D12_SHADER_VISIBILITY_DOMAIN	= 3
    SHADER_TYPE_GEOMETRY,   // D3D12_SHADER_VISIBILITY_GEOMETRY	= 4
    SHADER_TYPE_PIXEL       // D3D12_SHADER_VISIBILITY_PIXEL	= 5
};
SHADER_TYPE ShaderTypeFromShaderVisibility(D3D12_SHADER_VISIBILITY ShaderVisibility)
{
    VERIFY_EXPR(ShaderVisibility >= D3D12_SHADER_VISIBILITY_ALL && ShaderVisibility <= D3D12_SHADER_VISIBILITY_PIXEL );
    auto ShaderType = ShaderVisibility2ShaderTypeMap[ShaderVisibility];
#ifdef _DEBUG
    switch (ShaderVisibility)
    {
        case D3D12_SHADER_VISIBILITY_VERTEX:    VERIFY_EXPR(ShaderType == SHADER_TYPE_VERTEX);   break;
        case D3D12_SHADER_VISIBILITY_PIXEL:     VERIFY_EXPR(ShaderType == SHADER_TYPE_PIXEL);    break;
        case D3D12_SHADER_VISIBILITY_GEOMETRY:  VERIFY_EXPR(ShaderType == SHADER_TYPE_GEOMETRY); break;
        case D3D12_SHADER_VISIBILITY_HULL:      VERIFY_EXPR(ShaderType == SHADER_TYPE_HULL);     break;
        case D3D12_SHADER_VISIBILITY_DOMAIN:    VERIFY_EXPR(ShaderType == SHADER_TYPE_DOMAIN);   break;
        case D3D12_SHADER_VISIBILITY_ALL:       VERIFY_EXPR(ShaderType == SHADER_TYPE_COMPUTE);  break;
        default: LOG_ERROR("Unknown shader visibility (", ShaderVisibility, ")"); break;
    }
#endif
    return ShaderType;
}


static D3D12_DESCRIPTOR_HEAP_TYPE RangeType2HeapTypeMap[]
{
    D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, //D3D12_DESCRIPTOR_RANGE_TYPE_SRV	= 0,
    D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, //D3D12_DESCRIPTOR_RANGE_TYPE_UAV	= ( D3D12_DESCRIPTOR_RANGE_TYPE_SRV + 1 ) ,
    D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, //D3D12_DESCRIPTOR_RANGE_TYPE_CBV	= ( D3D12_DESCRIPTOR_RANGE_TYPE_UAV + 1 ) ,
    D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER      //D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER	= ( D3D12_DESCRIPTOR_RANGE_TYPE_CBV + 1 ) 
};
D3D12_DESCRIPTOR_HEAP_TYPE HeapTypeFromRangeType(D3D12_DESCRIPTOR_RANGE_TYPE RangeType)
{
    VERIFY_EXPR(RangeType >= D3D12_DESCRIPTOR_RANGE_TYPE_SRV && RangeType <= D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER);
    auto HeapType = RangeType2HeapTypeMap[RangeType];

#ifdef _DEBUG
    switch (RangeType)
    {
        case D3D12_DESCRIPTOR_RANGE_TYPE_CBV: VERIFY_EXPR(HeapType == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV); break;
        case D3D12_DESCRIPTOR_RANGE_TYPE_SRV: VERIFY_EXPR(HeapType == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV); break;
        case D3D12_DESCRIPTOR_RANGE_TYPE_UAV: VERIFY_EXPR(HeapType == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV); break;
        case D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER: VERIFY_EXPR(HeapType == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER); break;
        default: UNEXPECTED("Unexpected descriptor range type"); break;
    }
#endif
    return HeapType;
}


void RootSignature::InitStaticSampler(SHADER_TYPE ShaderType, const String &TextureName, const D3DShaderResourceAttribs &SamplerAttribs)
{
    auto ShaderVisibility = GetShaderVisibility(ShaderType);
    auto SamplerFound = false;
    for (auto &StSmplr : m_StaticSamplers)
    {
        if (StSmplr.ShaderVisibility == ShaderVisibility &&
            TextureName.compare(StSmplr.SamplerDesc.TextureName) == 0)
        {
            StSmplr.ShaderRegister = SamplerAttribs.BindPoint;
            StSmplr.ArraySize = SamplerAttribs.BindCount;
            StSmplr.RegisterSpace = 0;
            SamplerFound = true;
            break;
        }
    }

    if (!SamplerFound)
    {
        LOG_ERROR("Failed to find static sampler for variable \"", TextureName, '\"')
    }
}

void RootSignature::AllocateResourceSlot(SHADER_TYPE ShaderType, const D3DShaderResourceAttribs &ShaderResAttribs, D3D12_DESCRIPTOR_RANGE_TYPE RangeType, Uint32 &RootIndex, Uint32 &OffsetFromTableStart)
{
    auto ShaderInd = GetShaderTypeIndex(ShaderType);
    auto ShaderVisibility = GetShaderVisibility(ShaderType);
    if (RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_CBV && ShaderResAttribs.BindCount == 1)
    {
        // Allocate CBV directly in the root signature
        RootIndex = m_RootParams.GetNumRootTables() +  m_RootParams.GetNumRootViews();
        OffsetFromTableStart = 0;

        m_RootParams.AddRootView(D3D12_ROOT_PARAMETER_TYPE_CBV, RootIndex, ShaderResAttribs.BindPoint, ShaderVisibility, ShaderResAttribs.GetVariableType());
    }
    else
    {
        // Use the same table for static and mutable resources. Mark table type as static (mutable is an equivalent option)
        auto RootTableType = (ShaderResAttribs.GetVariableType() == SHADER_VARIABLE_TYPE_DYNAMIC) ? SHADER_VARIABLE_TYPE_DYNAMIC : SHADER_VARIABLE_TYPE_STATIC;
        auto TableIndKey = ShaderInd * SHADER_VARIABLE_TYPE_NUM_TYPES + RootTableType;
        auto &RootTableInd = (( RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER ) ? m_SamplerRootTablesMap : m_SrvCbvUavRootTablesMap)[ TableIndKey ];
        if (RootTableInd == InvalidRootTableIndex)
        {
            RootIndex = m_RootParams.GetNumRootTables() +  m_RootParams.GetNumRootViews();
            VERIFY_EXPR(m_RootParams.GetNumRootTables() < 255);
            RootTableInd = static_cast<Uint8>( m_RootParams.GetNumRootTables() );
            m_RootParams.AddRootTable(RootIndex, ShaderVisibility, RootTableType, 1);
        }
        else
        {
            m_RootParams.AddDescriptorRanges(RootTableInd, 1);
        }

        auto &CurrParam = m_RootParams.GetRootTable(RootTableInd);
        RootIndex = CurrParam.GetRootIndex();

        const auto& d3d12RootParam = static_cast<const D3D12_ROOT_PARAMETER&>(CurrParam);

        VERIFY( d3d12RootParam.ShaderVisibility == ShaderVisibility, "Shader visibility is not correct" );
        
        OffsetFromTableStart = CurrParam.GetDescriptorTableSize();

        Uint32 NewDescriptorRangeIndex = d3d12RootParam.DescriptorTable.NumDescriptorRanges-1;
        CurrParam.SetDescriptorRange(NewDescriptorRangeIndex, RangeType, ShaderResAttribs.BindPoint, ShaderResAttribs.BindCount, 0, OffsetFromTableStart);
    }
}


#ifdef _DEBUG
void RootSignature::dbgVerifyRootParameters()const
{
    Uint32 dbgTotalSrvCbvUavSlots = 0;
    Uint32 dbgTotalSamplerSlots = 0;
    for(Uint32 rt = 0; rt < m_RootParams.GetNumRootTables(); ++rt)
    {
        auto &RootTable = m_RootParams.GetRootTable(rt);
        auto &Param = static_cast<const D3D12_ROOT_PARAMETER&>( RootTable );
        VERIFY(Param.ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE, "Root parameter is expected to be a descriptor table");
        auto &Table = Param.DescriptorTable;
        VERIFY(Table.NumDescriptorRanges > 0, "Descriptor table is expected to be non-empty");
        VERIFY(Table.pDescriptorRanges[0].OffsetInDescriptorsFromTableStart == 0, "Descriptor table is expected to start at 0 offset");
        bool IsResourceTable = Table.pDescriptorRanges[0].RangeType != D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
        for (Uint32 r = 0; r < Table.NumDescriptorRanges; ++r)
        {
            const auto &range = Table.pDescriptorRanges[r];
            if(IsResourceTable)
            {
                VERIFY( range.RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_SRV ||
                        range.RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_CBV || 
                        range.RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_UAV, "Resource type is expected to be SRV, CBV or UAV")
                dbgTotalSrvCbvUavSlots += range.NumDescriptors;
            }
            else
            {
                VERIFY(range.RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, "Resource type is expected to be sampler")
                dbgTotalSamplerSlots += range.NumDescriptors;
            }

            if(r>0)
            {
                VERIFY(Table.pDescriptorRanges[r].OffsetInDescriptorsFromTableStart == Table.pDescriptorRanges[r-1].OffsetInDescriptorsFromTableStart+Table.pDescriptorRanges[r-1].NumDescriptors, "Ranges in a descriptor table are expected to be consequtive");
            }
        }
    }

    for(Uint32 rv = 0; rv < m_RootParams.GetNumRootViews(); ++rv)
    {
        auto &RootView = m_RootParams.GetRootView(rv);
        auto &Param = static_cast<const D3D12_ROOT_PARAMETER&>( RootView );
        VERIFY(Param.ParameterType == D3D12_ROOT_PARAMETER_TYPE_CBV, "Root parameter is expected to be a CBV");
    }

    VERIFY(dbgTotalSrvCbvUavSlots == 
                m_TotalSrvCbvUavSlots[SHADER_VARIABLE_TYPE_STATIC] + 
                m_TotalSrvCbvUavSlots[SHADER_VARIABLE_TYPE_MUTABLE] + 
                m_TotalSrvCbvUavSlots[SHADER_VARIABLE_TYPE_DYNAMIC], "Unexpected number of SRV CBV UAV resource slots")
    VERIFY(dbgTotalSamplerSlots == 
                m_TotalSamplerSlots[SHADER_VARIABLE_TYPE_STATIC] +
                m_TotalSamplerSlots[SHADER_VARIABLE_TYPE_MUTABLE] + 
                m_TotalSamplerSlots[SHADER_VARIABLE_TYPE_DYNAMIC], "Unexpected number of sampler slots")
}
#endif

void RootSignature::AllocateStaticSamplers(IShader* const*ppShaders, Uint32 NumShaders)
{
    Uint32 TotalSamplers = 0;
    for(Uint32 s=0;s < NumShaders; ++s)
        TotalSamplers += ppShaders[s]->GetDesc().NumStaticSamplers;
    if (TotalSamplers > 0)
    {
        m_StaticSamplers.reserve(TotalSamplers);
        for(Uint32 sh=0;sh < NumShaders; ++sh)
        {
            const auto &Desc = ppShaders[sh]->GetDesc();
            for(Uint32 sam=0; sam < Desc.NumStaticSamplers; ++sam)
            {
                m_StaticSamplers.emplace_back(Desc.StaticSamplers[sam], GetShaderVisibility(Desc.ShaderType));
            }
        }
        VERIFY_EXPR(m_StaticSamplers.size() == TotalSamplers);
    }
}

void RootSignature::Finalize(ID3D12Device *pd3d12Device)
{
    for(Uint32 rt = 0; rt < m_RootParams.GetNumRootTables(); ++rt)
    {
        auto &RootTbl = m_RootParams.GetRootTable(rt);
        auto &d3d12RootParam = static_cast<const D3D12_ROOT_PARAMETER&>(RootTbl);
        VERIFY_EXPR(d3d12RootParam.ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE);
        
        auto TableSize = RootTbl.GetDescriptorTableSize();
        VERIFY(d3d12RootParam.DescriptorTable.NumDescriptorRanges > 0 && TableSize > 0, "Unexpected empty descriptor table");
        auto IsSamplerTable = d3d12RootParam.DescriptorTable.pDescriptorRanges[0].RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
        auto VarType = RootTbl.GetShaderVariableType();
        (IsSamplerTable ? m_TotalSamplerSlots : m_TotalSrvCbvUavSlots)[VarType] += TableSize;
    }

#ifdef _DEBUG
    dbgVerifyRootParameters();
#endif

    D3D12_ROOT_SIGNATURE_DESC rootSignatureDesc;
    rootSignatureDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    
    auto TotalParams = m_RootParams.GetNumRootTables() + m_RootParams.GetNumRootViews();
    std::vector<D3D12_ROOT_PARAMETER, STDAllocatorRawMem<D3D12_ROOT_PARAMETER> > D3D12Parameters( TotalParams, D3D12_ROOT_PARAMETER(), STD_ALLOCATOR_RAW_MEM(D3D12_ROOT_PARAMETER, GetRawAllocator(), "Allocator for vector<D3D12_ROOT_PARAMETER>") );
    for(Uint32 rt = 0; rt < m_RootParams.GetNumRootTables(); ++rt)
    {
        auto &RootTable = m_RootParams.GetRootTable(rt);
        const D3D12_ROOT_PARAMETER &SrcParam = RootTable;
        VERIFY( SrcParam.ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE && SrcParam.DescriptorTable.NumDescriptorRanges > 0, "Non-empty descriptor table is expected" )
        D3D12Parameters[RootTable.GetRootIndex()] = SrcParam;
    }
    for(Uint32 rv = 0; rv < m_RootParams.GetNumRootViews(); ++rv)
    {
        auto &RootView = m_RootParams.GetRootView(rv);
        const D3D12_ROOT_PARAMETER &SrcParam = RootView;
        VERIFY( SrcParam.ParameterType == D3D12_ROOT_PARAMETER_TYPE_CBV, "Root CBV is expected" )
        D3D12Parameters[RootView.GetRootIndex()] = SrcParam;
    }


    rootSignatureDesc.NumParameters = static_cast<UINT>(D3D12Parameters.size());
    rootSignatureDesc.pParameters = D3D12Parameters.size() ? D3D12Parameters.data() : nullptr;

    UINT TotalD3D12StaticSamplers = 0;
    for(const auto &StSam : m_StaticSamplers)
        TotalD3D12StaticSamplers += StSam.ArraySize;
    rootSignatureDesc.NumStaticSamplers = TotalD3D12StaticSamplers;
    rootSignatureDesc.pStaticSamplers = nullptr;
    std::vector<D3D12_STATIC_SAMPLER_DESC, STDAllocatorRawMem<D3D12_STATIC_SAMPLER_DESC> > D3D12StaticSamplers( STD_ALLOCATOR_RAW_MEM(D3D12_STATIC_SAMPLER_DESC, GetRawAllocator(), "Allocator for vector<D3D12_STATIC_SAMPLER_DESC>") );
    D3D12StaticSamplers.reserve(TotalD3D12StaticSamplers);
    if ( !m_StaticSamplers.empty() )
    {
        for(size_t s=0; s < m_StaticSamplers.size(); ++s)
        {
            const auto &StSmplrDesc = m_StaticSamplers[s];
            const auto &SamDesc = StSmplrDesc.SamplerDesc.Desc;
            for(UINT ArrInd = 0; ArrInd < StSmplrDesc.ArraySize; ++ArrInd)
            {
                D3D12StaticSamplers.emplace_back(
                    D3D12_STATIC_SAMPLER_DESC{
                        FilterTypeToD3D12Filter(SamDesc.MinFilter, SamDesc.MagFilter, SamDesc.MipFilter),
                        TexAddressModeToD3D12AddressMode(SamDesc.AddressU),
                        TexAddressModeToD3D12AddressMode(SamDesc.AddressV),
                        TexAddressModeToD3D12AddressMode(SamDesc.AddressW),
                        SamDesc.MipLODBias,
                        SamDesc.MaxAnisotropy,
                        ComparisonFuncToD3D12ComparisonFunc(SamDesc.ComparisonFunc),
                        BorderColorToD3D12StaticBorderColor(SamDesc.BorderColor),
                        SamDesc.MinLOD,
                        SamDesc.MaxLOD,
                        StSmplrDesc.ShaderRegister + ArrInd,
                        StSmplrDesc.RegisterSpace,
                        StSmplrDesc.ShaderVisibility
                    }
                );
            }
        }
        rootSignatureDesc.pStaticSamplers = D3D12StaticSamplers.data();
        
        // Release static samplers array, we no longer need it
        std::vector<StaticSamplerAttribs, STDAllocatorRawMem<StaticSamplerAttribs> > EmptySamplers( STD_ALLOCATOR_RAW_MEM(StaticSamplerAttribs, GetRawAllocator(), "Allocator for vector<StaticSamplerAttribs>") );
        m_StaticSamplers.swap( EmptySamplers );

        VERIFY_EXPR(D3D12StaticSamplers.size() == TotalD3D12StaticSamplers);
    }
    

	CComPtr<ID3DBlob> signature;
	CComPtr<ID3DBlob> error;
    HRESULT hr = D3D12SerializeRootSignature(&rootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error);
    hr = pd3d12Device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), __uuidof(m_pd3d12RootSignature), reinterpret_cast<void**>( static_cast<ID3D12RootSignature**>(&m_pd3d12RootSignature)));
    CHECK_D3D_RESULT_THROW(hr, "Failed to create root signature")


    bool bHasDynamicResources = m_TotalSrvCbvUavSlots[SHADER_VARIABLE_TYPE_DYNAMIC]!=0 || m_TotalSamplerSlots[SHADER_VARIABLE_TYPE_DYNAMIC]!=0;
    if(bHasDynamicResources)
    {
        CommitDescriptorHandles = &RootSignature::CommitDescriptorHandlesInternal_SMD<false>;
        TransitionAndCommitDescriptorHandles = &RootSignature::CommitDescriptorHandlesInternal_SMD<true>;
    }
    else
    {
        CommitDescriptorHandles = &RootSignature::CommitDescriptorHandlesInternal_SM<false>;
        TransitionAndCommitDescriptorHandles = &RootSignature::CommitDescriptorHandlesInternal_SM<true>;
    }
}

void RootSignature::InitResourceCache(RenderDeviceD3D12Impl *pDeviceD3D12Impl, ShaderResourceCacheD3D12& ResourceCache, IMemoryAllocator &CacheMemAllocator)const
{
    std::vector<Uint32, STDAllocatorRawMem<Uint32> > CacheTableSizes(m_RootParams.GetNumRootTables() + m_RootParams.GetNumRootViews(), 0, STD_ALLOCATOR_RAW_MEM(Uint32, GetRawAllocator(), "Allocator for vector<Uint32>") );
    for(Uint32 rt = 0; rt < m_RootParams.GetNumRootTables(); ++rt)
    {
        auto &RootParam = m_RootParams.GetRootTable(rt);
        CacheTableSizes[RootParam.GetRootIndex()] = RootParam.GetDescriptorTableSize();
    }

    for(Uint32 rv = 0; rv < m_RootParams.GetNumRootViews(); ++rv)
    {
        auto &RootParam = m_RootParams.GetRootView(rv);
        CacheTableSizes[RootParam.GetRootIndex()] = 1;
    }
    ResourceCache.Initialize(CacheMemAllocator, static_cast<Uint32>(CacheTableSizes.size()), CacheTableSizes.data());

    Uint32 TotalSrvCbvUavDescriptors =
                m_TotalSrvCbvUavSlots[SHADER_VARIABLE_TYPE_STATIC] + 
                m_TotalSrvCbvUavSlots[SHADER_VARIABLE_TYPE_MUTABLE];
    Uint32 TotalSamplerDescriptors =
                m_TotalSamplerSlots[SHADER_VARIABLE_TYPE_STATIC] +
                m_TotalSamplerSlots[SHADER_VARIABLE_TYPE_MUTABLE];

    DescriptorHeapAllocation CbcSrvUavHeapSpace, SamplerHeapSpace;
    if(TotalSrvCbvUavDescriptors)
        CbcSrvUavHeapSpace = pDeviceD3D12Impl->AllocateGPUDescriptors(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, TotalSrvCbvUavDescriptors);

    if(TotalSamplerDescriptors)
        SamplerHeapSpace = pDeviceD3D12Impl->AllocateGPUDescriptors(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, TotalSamplerDescriptors);

    Uint32 SrvCbvUavTblStartOffset = 0;
    Uint32 SamplerTblStartOffset = 0;
    for(Uint32 rt = 0; rt < m_RootParams.GetNumRootTables(); ++rt)
    {
        auto &RootParam = m_RootParams.GetRootTable(rt);
        const auto& D3D12RootParam = static_cast<const D3D12_ROOT_PARAMETER&>(RootParam);
        auto &RootTableCache = ResourceCache.GetRootTable(RootParam.GetRootIndex());
        
        SHADER_TYPE dbgShaderType = SHADER_TYPE_UNKNOWN;
#ifdef _DEBUG
        dbgShaderType = ShaderTypeFromShaderVisibility(D3D12RootParam.ShaderVisibility);
#endif
        VERIFY_EXPR( D3D12RootParam.ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE )
        
        auto TableSize = RootParam.GetDescriptorTableSize();
        VERIFY(TableSize > 0, "Unexpected empty descriptor table");

        auto HeapType = HeapTypeFromRangeType(D3D12RootParam.DescriptorTable.pDescriptorRanges[0].RangeType);

#ifdef _DEBUG
        RootTableCache.SetDebugAttribs( TableSize, HeapType, dbgShaderType );
#endif

        // Space for dynamic variables is allocated at every draw call
        if( RootParam.GetShaderVariableType() != SHADER_VARIABLE_TYPE_DYNAMIC )
        {
            if( HeapType == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV )
            {
                RootTableCache.TableStartOffset = SrvCbvUavTblStartOffset;
                SrvCbvUavTblStartOffset += TableSize;
            }
            else
            {
                RootTableCache.TableStartOffset = SamplerTblStartOffset;
                SamplerTblStartOffset += TableSize;
            }
        }
        else
        {
            VERIFY_EXPR(RootTableCache.TableStartOffset == ShaderResourceCacheD3D12::InvalidDescriptorOffset)
        }
    }

#ifdef _DEBUG
    for(Uint32 rv = 0; rv < m_RootParams.GetNumRootViews(); ++rv)
    {
        auto &RootParam = m_RootParams.GetRootView(rv);
        const auto& D3D12RootParam = static_cast<const D3D12_ROOT_PARAMETER&>(RootParam);
        auto &RootTableCache = ResourceCache.GetRootTable(RootParam.GetRootIndex());
        VERIFY_EXPR(RootTableCache.TableStartOffset == ShaderResourceCacheD3D12::InvalidDescriptorOffset)
        
        SHADER_TYPE dbgShaderType = ShaderTypeFromShaderVisibility(D3D12RootParam.ShaderVisibility);
        VERIFY_EXPR(D3D12RootParam.ParameterType == D3D12_ROOT_PARAMETER_TYPE_CBV);
        RootTableCache.SetDebugAttribs( 1, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, dbgShaderType );
    }
#endif
    
    VERIFY_EXPR(SrvCbvUavTblStartOffset == SrvCbvUavTblStartOffset);
    VERIFY_EXPR(SamplerTblStartOffset == SamplerTblStartOffset);

    ResourceCache.SetDescriptorHeapSpace(std::move(CbcSrvUavHeapSpace), std::move(SamplerHeapSpace));
}

const D3D12_RESOURCE_STATES D3D12_RESOURCE_STATE_SHADER_RESOURCE  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

__forceinline
void TransitionResource(CommandContext &Ctx, 
                        ShaderResourceCacheD3D12::Resource &Res,
                        D3D12_DESCRIPTOR_RANGE_TYPE RangeType)
{
    switch (Res.Type)
    {
        case CachedResourceType::CBV:
        {
            VERIFY(RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_CBV, "Unexpected descriptor range type");
            // Not using QueryInterface() for the sake of efficiency
            auto *pBuffToTransition = ValidatedCast<BufferD3D12Impl>(Res.pObject.RawPtr());
            if( !pBuffToTransition->CheckAllStates(D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER) )
                Ctx.TransitionResource(pBuffToTransition, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER );
        }
        break;

        case CachedResourceType::BufSRV:
        {
            VERIFY(RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_SRV, "Unexpected descriptor range type");
            auto *pBuffViewD3D12 = ValidatedCast<BufferViewD3D12Impl>(Res.pObject.RawPtr());
            auto *pBuffToTransition = ValidatedCast<BufferD3D12Impl>(pBuffViewD3D12->GetBuffer());
            if( !pBuffToTransition->CheckAllStates(D3D12_RESOURCE_STATE_SHADER_RESOURCE) )
                Ctx.TransitionResource(pBuffToTransition, D3D12_RESOURCE_STATE_SHADER_RESOURCE );
        }
        break;

        case CachedResourceType::BufUAV:
        {
            VERIFY(RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_UAV, "Unexpected descriptor range type");
            auto *pBuffViewD3D12 = ValidatedCast<BufferViewD3D12Impl>(Res.pObject.RawPtr());
            auto *pBuffToTransition = ValidatedCast<BufferD3D12Impl>(pBuffViewD3D12->GetBuffer());
            if( !pBuffToTransition->CheckAllStates(D3D12_RESOURCE_STATE_UNORDERED_ACCESS) )
                Ctx.TransitionResource(pBuffToTransition, D3D12_RESOURCE_STATE_UNORDERED_ACCESS );
        }
        break;

        case CachedResourceType::TexSRV:
        {
            VERIFY(RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_SRV, "Unexpected descriptor range type");
            auto *pTexViewD3D12 = ValidatedCast<TextureViewD3D12Impl>(Res.pObject.RawPtr());
            auto *pTexToTransition = ValidatedCast<TextureD3D12Impl>(pTexViewD3D12->GetTexture());
            if( !pTexToTransition->CheckAllStates(D3D12_RESOURCE_STATE_SHADER_RESOURCE) )
                Ctx.TransitionResource(pTexToTransition, D3D12_RESOURCE_STATE_SHADER_RESOURCE );
        }
        break;

        case CachedResourceType::TexUAV:
        {
            VERIFY(RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_UAV, "Unexpected descriptor range type");
            auto *pTexViewD3D12 = ValidatedCast<TextureViewD3D12Impl>(Res.pObject.RawPtr());
            auto *pTexToTransition = ValidatedCast<TextureD3D12Impl>(pTexViewD3D12->GetTexture());
            if( !pTexToTransition->CheckAllStates(D3D12_RESOURCE_STATE_UNORDERED_ACCESS) )
                Ctx.TransitionResource(pTexToTransition, D3D12_RESOURCE_STATE_UNORDERED_ACCESS );
        }
        break;

        case CachedResourceType::Sampler:
            VERIFY(RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, "Unexpected descriptor range type");
        break;

        default:
            // Resource not bound
            VERIFY(Res.Type == CachedResourceType::Unknown, "Unexpected resource type") 
            VERIFY(Res.pObject == nullptr && Res.CPUDescriptorHandle.ptr == 0, "Bound resource is unexpected")
    }
}


#ifdef _DEBUG
void DbgVerifyResourceState(ShaderResourceCacheD3D12::Resource &Res,
                            D3D12_DESCRIPTOR_RANGE_TYPE RangeType)
{
    switch (Res.Type)
    {
        case CachedResourceType::CBV:
        {
            VERIFY(RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_CBV, "Unexpected descriptor range type");
            // Not using QueryInterface() for the sake of efficiency
            auto *pBuffToTransition = ValidatedCast<BufferD3D12Impl>(Res.pObject.RawPtr());
            auto State = pBuffToTransition->GetState();
            if( (State & D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER) != D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER )
                LOG_ERROR_MESSAGE("Resource \"", pBuffToTransition->GetDesc().Name, "\" is not in D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER state. Did you forget to call TransitionShaderResources() or specify COMMIT_SHADER_RESOURCES_FLAG_TRANSITION_RESOURCES flag in a call to CommitShaderResources()?" );
        }
        break;

        case CachedResourceType::BufSRV:
        {
            VERIFY(RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_SRV, "Unexpected descriptor range type");
            auto *pBuffViewD3D12 = ValidatedCast<BufferViewD3D12Impl>(Res.pObject.RawPtr());
            auto *pBuffToTransition = ValidatedCast<BufferD3D12Impl>(pBuffViewD3D12->GetBuffer());
            auto State = pBuffToTransition->GetState();
            if( (State & D3D12_RESOURCE_STATE_SHADER_RESOURCE) != D3D12_RESOURCE_STATE_SHADER_RESOURCE )
                LOG_ERROR_MESSAGE("Resource \"", pBuffToTransition->GetDesc().Name, "\" is not in correct state. Did you forget to call TransitionShaderResources() or specify COMMIT_SHADER_RESOURCES_FLAG_TRANSITION_RESOURCES flag in a call to CommitShaderResources()?" );
        }
        break;

        case CachedResourceType::BufUAV:
        {
            VERIFY(RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_UAV, "Unexpected descriptor range type");
            auto *pBuffViewD3D12 = ValidatedCast<BufferViewD3D12Impl>(Res.pObject.RawPtr());
            auto *pBuffToTransition = ValidatedCast<BufferD3D12Impl>(pBuffViewD3D12->GetBuffer());
            auto State = pBuffToTransition->GetState();
            if( (State & D3D12_RESOURCE_STATE_UNORDERED_ACCESS) != D3D12_RESOURCE_STATE_UNORDERED_ACCESS )
                LOG_ERROR_MESSAGE("Resource \"", pBuffToTransition->GetDesc().Name, "\" is not in D3D12_RESOURCE_STATE_UNORDERED_ACCESS state. Did you forget to call TransitionShaderResources() or specify COMMIT_SHADER_RESOURCES_FLAG_TRANSITION_RESOURCES flag in a call to CommitShaderResources()?" );
        }
        break;

        case CachedResourceType::TexSRV:
        {
            VERIFY(RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_SRV, "Unexpected descriptor range type");
            auto *pTexViewD3D12 = ValidatedCast<TextureViewD3D12Impl>(Res.pObject.RawPtr());
            auto *pTexToTransition = ValidatedCast<TextureD3D12Impl>(pTexViewD3D12->GetTexture());
            auto State = pTexToTransition->GetState();
            if( (State & D3D12_RESOURCE_STATE_SHADER_RESOURCE) != D3D12_RESOURCE_STATE_SHADER_RESOURCE )
                LOG_ERROR_MESSAGE("Resource \"", pTexToTransition->GetDesc().Name, "\" is not in correct state. Did you forget to call TransitionShaderResources() or specify COMMIT_SHADER_RESOURCES_FLAG_TRANSITION_RESOURCES flag in a call to CommitShaderResources()?" );
        }
        break;

        case CachedResourceType::TexUAV:
        {
            VERIFY(RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_UAV, "Unexpected descriptor range type");
            auto *pTexViewD3D12 = ValidatedCast<TextureViewD3D12Impl>(Res.pObject.RawPtr());
            auto *pTexToTransition = ValidatedCast<TextureD3D12Impl>(pTexViewD3D12->GetTexture());
            auto State = pTexToTransition->GetState();
            if( (State & D3D12_RESOURCE_STATE_UNORDERED_ACCESS) != D3D12_RESOURCE_STATE_UNORDERED_ACCESS )
                LOG_ERROR_MESSAGE("Resource \"", pTexToTransition->GetDesc().Name, "\" is not in D3D12_RESOURCE_STATE_UNORDERED_ACCESS state. Did you forget to call TransitionShaderResources() or specify COMMIT_SHADER_RESOURCES_FLAG_TRANSITION_RESOURCES flag in a call to CommitShaderResources()?" );
        }
        break;

        case CachedResourceType::Sampler:
            VERIFY(RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, "Unexpected descriptor range type");
        break;

        default:
            // Resource not bound
            VERIFY(Res.Type == CachedResourceType::Unknown, "Unexpected resource type") 
            VERIFY(Res.pObject == nullptr && Res.CPUDescriptorHandle.ptr == 0, "Bound resource is unexpected")
    }
}
#endif

template<class TOperation>
__forceinline void RootSignature::RootParamsManager::ProcessRootTables(TOperation Operation)const
{
    for(Uint32 rt = 0; rt < m_NumRootTables; ++rt)
    {
        auto &RootTable = GetRootTable(rt);
        auto RootInd = RootTable.GetRootIndex();
        const D3D12_ROOT_PARAMETER& D3D12Param = RootTable;

        VERIFY_EXPR(D3D12Param.ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE);

        auto &d3d12Table = D3D12Param.DescriptorTable;
        VERIFY(d3d12Table.NumDescriptorRanges > 0 && RootTable.GetDescriptorTableSize() > 0, "Unexepected empty descriptor table");
        bool IsResourceTable = d3d12Table.pDescriptorRanges[0].RangeType != D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
        D3D12_DESCRIPTOR_HEAP_TYPE dbgHeapType = D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES;
#ifdef _DEBUG
            dbgHeapType = IsResourceTable ? D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV : D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
#endif
        Operation(RootInd, RootTable, D3D12Param, IsResourceTable, dbgHeapType);
    }
}

template<class TOperation>
__forceinline void ProcessCachedTableResources(Uint32 RootInd, 
                                               const D3D12_ROOT_PARAMETER& D3D12Param, 
                                               ShaderResourceCacheD3D12& ResourceCache, 
                                               D3D12_DESCRIPTOR_HEAP_TYPE dbgHeapType, 
                                               TOperation Operation)
{
    for (UINT r = 0; r < D3D12Param.DescriptorTable.NumDescriptorRanges; ++r)
    {
        const auto &range = D3D12Param.DescriptorTable.pDescriptorRanges[r];
        for (UINT d = 0; d < range.NumDescriptors; ++d)
        {
            SHADER_TYPE dbgShaderType = SHADER_TYPE_UNKNOWN;
#ifdef _DEBUG
            dbgShaderType = ShaderTypeFromShaderVisibility(D3D12Param.ShaderVisibility);
            VERIFY(dbgHeapType == HeapTypeFromRangeType(range.RangeType), "Mistmatch between descriptor heap type and descriptor range type");
#endif
            auto OffsetFromTableStart = range.OffsetInDescriptorsFromTableStart + d;
            auto& Res = ResourceCache.GetRootTable(RootInd).GetResource(OffsetFromTableStart, dbgHeapType, dbgShaderType);

            Operation(OffsetFromTableStart, range, Res);
        }
    }
}


template<bool PerformResourceTransitions>
void RootSignature::CommitDescriptorHandlesInternal_SMD(RenderDeviceD3D12Impl *pRenderDeviceD3D12, 
                                                        ShaderResourceCacheD3D12& ResourceCache, 
                                                        CommandContext &Ctx, 
                                                        bool IsCompute)const
{
    auto *pd3d12Device = pRenderDeviceD3D12->GetD3D12Device();

    Uint32 NumDynamicCbvSrvUavDescriptors = m_TotalSrvCbvUavSlots[SHADER_VARIABLE_TYPE_DYNAMIC];
    Uint32 NumDynamicSamplerDescriptors = m_TotalSamplerSlots[SHADER_VARIABLE_TYPE_DYNAMIC];
    VERIFY_EXPR(NumDynamicCbvSrvUavDescriptors > 0 || NumDynamicSamplerDescriptors > 0);

    DescriptorHeapAllocation DynamicCbvSrvUavDescriptors, DynamicSamplerDescriptors;
    if(NumDynamicCbvSrvUavDescriptors)
        DynamicCbvSrvUavDescriptors = Ctx.AllocateDynamicGPUVisibleDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, NumDynamicCbvSrvUavDescriptors);
    if(NumDynamicSamplerDescriptors)
        DynamicSamplerDescriptors = Ctx.AllocateDynamicGPUVisibleDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, NumDynamicSamplerDescriptors);

    CommandContext::ShaderDescriptorHeaps Heaps(ResourceCache.GetSrvCbvUavDescriptorHeap(), ResourceCache.GetSamplerDescriptorHeap());
    if(Heaps.pSamplerHeap == nullptr)
        Heaps.pSamplerHeap = DynamicSamplerDescriptors.GetDescriptorHeap();

    if(Heaps.pSrvCbvUavHeap == nullptr)
        Heaps.pSrvCbvUavHeap = DynamicCbvSrvUavDescriptors.GetDescriptorHeap();

    if(NumDynamicCbvSrvUavDescriptors)
        VERIFY(DynamicCbvSrvUavDescriptors.GetDescriptorHeap() == Heaps.pSrvCbvUavHeap, "Inconsistent CbvSrvUav descriptor heaps" )
    if(NumDynamicSamplerDescriptors)
        VERIFY(DynamicSamplerDescriptors.GetDescriptorHeap() == Heaps.pSamplerHeap, "Inconsistent Sampler descriptor heaps" )

    if(Heaps)
        Ctx.SetDescriptorHeaps(Heaps);

    // Offset to the beginning of the current dynamic CBV_SRV_UAV/SAMPLER table from 
    // the start of the allocation
    Uint32 DynamicCbvSrvUavTblOffset = 0;
    Uint32 DynamicSamplerTblOffset = 0;

    m_RootParams.ProcessRootTables(
        [&](Uint32 RootInd, const RootParameter &RootTable, const D3D12_ROOT_PARAMETER& D3D12Param, bool IsResourceTable, D3D12_DESCRIPTOR_HEAP_TYPE dbgHeapType )
        {
            D3D12_GPU_DESCRIPTOR_HANDLE RootTableGPUDescriptorHandle;
            bool IsDynamicTable = RootTable.GetShaderVariableType() == SHADER_VARIABLE_TYPE_DYNAMIC;
            if (IsDynamicTable)
            {
                if( IsResourceTable )
                    RootTableGPUDescriptorHandle = DynamicCbvSrvUavDescriptors.GetGpuHandle(DynamicCbvSrvUavTblOffset);
                else
                    RootTableGPUDescriptorHandle = DynamicSamplerDescriptors.GetGpuHandle(DynamicSamplerTblOffset);
            }
            else
            {
                RootTableGPUDescriptorHandle = IsResourceTable ? 
                    ResourceCache.GetShaderVisibleTableGPUDescriptorHandle<D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV>(RootInd) : 
                    ResourceCache.GetShaderVisibleTableGPUDescriptorHandle<D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER>(RootInd);
                VERIFY(RootTableGPUDescriptorHandle.ptr != 0, "Unexpected null GPU descriptor handle")
            }

            if(IsCompute)
                Ctx.GetCommandList()->SetComputeRootDescriptorTable(RootInd, RootTableGPUDescriptorHandle);
            else
                Ctx.GetCommandList()->SetGraphicsRootDescriptorTable(RootInd, RootTableGPUDescriptorHandle);

            ProcessCachedTableResources(RootInd, D3D12Param, ResourceCache, dbgHeapType, 
                [&](UINT OffsetFromTableStart, const D3D12_DESCRIPTOR_RANGE &range, ShaderResourceCacheD3D12::Resource &Res)
                {
                    if(PerformResourceTransitions)
                    {
                        TransitionResource(Ctx, Res, range.RangeType);
                    }
#ifdef _DEBUG
                    else
                    {
                        DbgVerifyResourceState(Res, range.RangeType);
                    }
#endif

                    if(IsDynamicTable)
                    {
                        if (IsResourceTable)
                        {
                            if( Res.CPUDescriptorHandle.ptr == 0 )
                                LOG_ERROR_MESSAGE("No valid CbvSrvUav descriptor handle found for root parameter ", RootInd, ", descriptor slot ", OffsetFromTableStart)

                            VERIFY( DynamicCbvSrvUavTblOffset < NumDynamicCbvSrvUavDescriptors, "Not enough space in the descriptor heap allocation")
                            
                            pd3d12Device->CopyDescriptorsSimple(1, DynamicCbvSrvUavDescriptors.GetCpuHandle(DynamicCbvSrvUavTblOffset), Res.CPUDescriptorHandle, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                            ++DynamicCbvSrvUavTblOffset;
                        }
                        else
                        {
                            if( Res.CPUDescriptorHandle.ptr == 0 )
                                LOG_ERROR_MESSAGE("No valid sampler descriptor handle found for root parameter ", RootInd, ", descriptor slot ", OffsetFromTableStart)

                            VERIFY( DynamicSamplerTblOffset < NumDynamicSamplerDescriptors, "Not enough space in the descriptor heap allocation")
                            
                            pd3d12Device->CopyDescriptorsSimple(1, DynamicSamplerDescriptors.GetCpuHandle(DynamicSamplerTblOffset), Res.CPUDescriptorHandle, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
                            ++DynamicSamplerTblOffset;
                        }
                    }
                }
            );
        }
    );
    
    VERIFY_EXPR( DynamicCbvSrvUavTblOffset == NumDynamicCbvSrvUavDescriptors );
    VERIFY_EXPR( DynamicSamplerTblOffset == NumDynamicSamplerDescriptors );
}

template<bool PerformResourceTransitions>
void RootSignature::CommitDescriptorHandlesInternal_SM(RenderDeviceD3D12Impl *pRenderDeviceD3D12, 
                                                       ShaderResourceCacheD3D12& ResourceCache, 
                                                       CommandContext &Ctx, 
                                                       bool IsCompute)const
{
    VERIFY_EXPR(m_TotalSrvCbvUavSlots[SHADER_VARIABLE_TYPE_DYNAMIC] == 0 && m_TotalSamplerSlots[SHADER_VARIABLE_TYPE_DYNAMIC] == 0);

    CommandContext::ShaderDescriptorHeaps Heaps(ResourceCache.GetSrvCbvUavDescriptorHeap(), ResourceCache.GetSamplerDescriptorHeap());
    if(Heaps)
        Ctx.SetDescriptorHeaps(Heaps);

    m_RootParams.ProcessRootTables(
        [&](Uint32 RootInd, const RootParameter &RootTable, const D3D12_ROOT_PARAMETER& D3D12Param, bool IsResourceTable, D3D12_DESCRIPTOR_HEAP_TYPE dbgHeapType )
        {
            VERIFY(RootTable.GetShaderVariableType() != SHADER_VARIABLE_TYPE_DYNAMIC, "Unexpected dynamic resource");

            D3D12_GPU_DESCRIPTOR_HANDLE RootTableGPUDescriptorHandle = IsResourceTable ? 
                ResourceCache.GetShaderVisibleTableGPUDescriptorHandle<D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV>(RootInd) : 
                ResourceCache.GetShaderVisibleTableGPUDescriptorHandle<D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER>(RootInd);
            VERIFY(RootTableGPUDescriptorHandle.ptr != 0, "Unexpected null GPU descriptor handle")

            if(IsCompute)
                Ctx.GetCommandList()->SetComputeRootDescriptorTable(RootInd, RootTableGPUDescriptorHandle);
            else
                Ctx.GetCommandList()->SetGraphicsRootDescriptorTable(RootInd, RootTableGPUDescriptorHandle);

            if(PerformResourceTransitions)
            {
                ProcessCachedTableResources(RootInd, D3D12Param, ResourceCache, dbgHeapType, 
                    [&](UINT OffsetFromTableStart, const D3D12_DESCRIPTOR_RANGE &range, ShaderResourceCacheD3D12::Resource &Res)
                    {
                        TransitionResource(Ctx, Res, range.RangeType);
                    }
                );
            }
#ifdef _DEBUG
            else
            {
                ProcessCachedTableResources(RootInd, D3D12Param, ResourceCache, dbgHeapType, 
                    [&](UINT OffsetFromTableStart, const D3D12_DESCRIPTOR_RANGE &range, ShaderResourceCacheD3D12::Resource &Res)
                    {
                        DbgVerifyResourceState(Res, range.RangeType);
                    }
                );
            }
#endif
        }
    );
}


void RootSignature::TransitionResources(ShaderResourceCacheD3D12& ResourceCache, 
                                        class CommandContext &Ctx)const
{
    m_RootParams.ProcessRootTables(
        [&](Uint32 RootInd, const RootParameter &RootTable, const D3D12_ROOT_PARAMETER& D3D12Param, bool IsResourceTable, D3D12_DESCRIPTOR_HEAP_TYPE dbgHeapType )
        {
            ProcessCachedTableResources(RootInd, D3D12Param, ResourceCache, dbgHeapType, 
                [&](UINT OffsetFromTableStart, const D3D12_DESCRIPTOR_RANGE &range, ShaderResourceCacheD3D12::Resource &Res)
                {
                    TransitionResource(Ctx, Res, range.RangeType);
                }
            );
        }
    );
}


void RootSignature::CommitRootViews(ShaderResourceCacheD3D12& ResourceCache, 
                                    CommandContext &Ctx, 
                                    bool IsCompute,
                                    Uint32 ContextId)const
{
    for(Uint32 rv = 0; rv < m_RootParams.GetNumRootViews(); ++rv)
    {
        auto &RootView = m_RootParams.GetRootView(rv);
        auto RootInd = RootView.GetRootIndex();
       
        SHADER_TYPE dbgShaderType = SHADER_TYPE_UNKNOWN;
#ifdef _DEBUG
        auto &Param = static_cast<const D3D12_ROOT_PARAMETER&>( RootView );
        VERIFY_EXPR(Param.ParameterType == D3D12_ROOT_PARAMETER_TYPE_CBV);
        dbgShaderType = ShaderTypeFromShaderVisibility(Param.ShaderVisibility);
#endif

        auto& Res = ResourceCache.GetRootTable(RootInd).GetResource(0, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, dbgShaderType);
        auto *pBuffToTransition = ValidatedCast<BufferD3D12Impl>(Res.pObject.RawPtr());
        if( !pBuffToTransition->CheckAllStates(D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER) )
            Ctx.TransitionResource(pBuffToTransition, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);

        D3D12_GPU_VIRTUAL_ADDRESS CBVAddress = pBuffToTransition->GetGPUAddress(ContextId);
        if(IsCompute)
            Ctx.GetCommandList()->SetComputeRootConstantBufferView(RootInd, CBVAddress);
        else
            Ctx.GetCommandList()->SetGraphicsRootConstantBufferView(RootInd, CBVAddress);
    }
}

}