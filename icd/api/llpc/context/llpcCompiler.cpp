/*
 *******************************************************************************
 *
 * Copyright (c) 2016-2017 Advanced Micro Devices, Inc. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 ******************************************************************************/

/**
 ***********************************************************************************************************************
 * @file  llpcCompiler.cpp
 * @brief LLPC source file: contains implementation of class Llpc::Compiler.
 ***********************************************************************************************************************
 */
#define DEBUG_TYPE "llpc-compiler"

#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/Mutex.h"
#include "llvm/Support/MutexGuard.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/TargetSelect.h"

#include <sstream>
#include "spirv.hpp"
#include "SPIRV.h"
#include "SPIRVInternal.h"
#include "llpcCodeGenManager.h"
#include "llpcCompiler.h"
#include "llpcComputeContext.h"
#include "llpcContext.h"
#include "llpcCopyShader.h"
#include "llpcGfx6Chip.h"
#ifdef LLPC_BUILD_GFX9
#include "llpcGfx9Chip.h"
#endif
#include "llpcGraphicsContext.h"
#include "llpcElf.h"
#include "llpcFile.h"
#include "llpcPatch.h"
#ifdef LLPC_BUILD_GFX9
#include "llpcShaderMerger.h"
#endif
#include "llpcSpirvLower.h"
#include "llpcVertexFetch.h"

#ifdef LLPC_ENABLE_SPIRV_OPT
    #define SPVGEN_STATIC_LIB 1
    #include "spvgen.h"
#endif

using namespace llvm;
using namespace SPIRV;

namespace llvm
{

namespace cl
{

// -pipeline-dump-dir: directory where pipeline info are dumped
static opt<std::string> PipelineDumpDir("pipeline-dump-dir",
                                        desc("Directory where pipeline shader info are dumped"),
                                        value_desc("directory"),
                                        init("."));

// -enable-pipeline-dump: enable pipeline info dump
opt<bool> EnablePipelineDump("enable-pipeline-dump", desc("Enable pipeline info dump"), init(false));

// --disable-WIP-features: disable those work-in-progress features
static opt<bool> DisableWipFeatures("disable-WIP-features",
                                   desc("Disable those work-in-progress features"),
                                   init(false));

// -enable-time-profiler: enable time profiler for various compilation phases
static opt<bool> EnableTimeProfiler("enable-time-profiler",
                                    desc("Enable time profiler for various compilation phases"),
                                    init(false));

// -shader-cache-mode: shader cache mode:
// 0 - Disable
// 1 - Runtime cache
// 2 - Cache to disk
static opt<uint32_t> ShaderCacheMode("shader-cache-mode",
                                     desc("Shader cache mode, 0 - disable, 1 - runtime cache, 2 - cache to disk "),
                                     init(0));

// -executable-name: executable file name
static opt<std::string> ExecutableName("executable-name",
                                       desc("Executable file name"),
                                       value_desc("filename"),
                                       init("amdllpc"));

// -shader-replace-mode: shader replacement mode
// 0 - Disable
// 1 - Replacement based on shader hash
// 2 - Replacement based on both shader hash and pipeline hash
static opt<uint32_t> ShaderReplaceMode("shader-replace-mode",
                                       desc("Shader replacement mode, "
                                            "0 - disable, "
                                            "1 - replacement based on shader hash, "
                                            "2 - replacement based on both shader hash and pipeline hash"),
                                       init(0));

// -shader-replace-dir: directory to store the files used in shader replacement
static opt<std::string> ShaderReplaceDir("shader-replace-dir",
                                         desc("Directory to store the files used in shader replacement"),
                                         value_desc("dir"),
                                         init("."));

// -shader-replace-pipeline-hashes: a collection of pipeline hashes, specifying shader replacement is operated on which pipelines
static opt<std::string> ShaderReplacePipelineHashes("shader-replace-pipeline-hashes",
                                                    desc("A collection of pipeline hashes, specifying shader "
                                                         "replacement is operated on which pipelines"),
                                                    value_desc("hashes with comma as separator"),
                                                    init(""));

// -disable-gs-onchip: disable geometry shader on-chip mode
opt<bool> DisableGsOnChip("disable-gs-onchip",
                          desc("Disable geometry shader on-chip mode"),
                          init(true));

// -enable-spirv-opt: enable optimization for SPIR-V binary
opt<bool> EnableSpirvOpt("enable-spirv-opt", desc("Enable optimization for SPIR-V binary"), init(false));

// -auto-layout-desc
extern opt<bool> AutoLayoutDesc;

} // cl

} // llvm

namespace Llpc
{

// Time profiling result
TimeProfileResult g_timeProfileResult = {};

// Enumerates modes used in shader replacement
enum ShaderReplaceMode
{
    ShaderReplaceDisable            = 0, // Disabled
    ShaderReplaceShaderHash         = 1, // Replacement based on shader hash
    ShaderReplaceShaderPipelineHash = 2, // Replacement based on both shader and pipeline hash
};

static const uint8_t GlslNullFsEmuLib[]=
{
    #include "generate/g_llpcGlslNullFsEmuLib.h"
};

static ManagedStatic<sys::Mutex> s_compilerMutex;

uint32_t Compiler::m_instanceCount = 0;

// =====================================================================================================================
// Handler for LLVM fatal error.
static void FatalErrorHandler(
    void*               userData,       // [in] An argument which will be passed to the installed error handler
    const std::string&  reason,         // Error reason
    bool                gen_crash_diag) // Whether diagnostic should be generated
{
    LLPC_ERRS("LLVM FATAL ERROR:" << reason << "\n");
#if LLPC_ENABLE_EXCEPTION
    throw("LLVM fatal error");
#endif
}

// =====================================================================================================================
// Creates LLPC compiler from the specified info.
Result VKAPI_CALL ICompiler::Create(
    GfxIpVersion      gfxIp,        // Graphics IP version
    uint32_t          optionCount,  // Count of compilation-option strings
    const char*const* options,      // [in] An array of compilation-option strings
    ICompiler**       ppCompiler)   // [out] Pointer to the created LLPC compiler object
{
    Result result = Result::Success;

    const char* pClient = options[0];
    bool ignoreErrors = (strcmp(pClient, VkIcdName) == 0);

    raw_null_ostream nullStream;

    MutexGuard lock(*s_compilerMutex);

    if (Compiler::GetInstanceCount() == 0)
    {
        // LLVM command options can't be parsed multiple times
        if (cl::ParseCommandLineOptions(optionCount,
                                        options,
                                        "AMD LLPC compiler",
                                        ignoreErrors ? &nullStream : nullptr) == false)
        {
            result = Result::ErrorInvalidValue;
        }

        // LLVM fatal error handler only can be installed once.
        if (result == Result::Success)
        {
            install_fatal_error_handler(FatalErrorHandler);
        }
    }

    if (result == Result::Success)
    {
        *ppCompiler = new Compiler(pClient, gfxIp);
        LLPC_ASSERT(*ppCompiler != nullptr);
    }
    else
    {
       *ppCompiler = nullptr;
       result = Result::ErrorInvalidValue;
    }
    return result;
}

// =====================================================================================================================
// Checks whether a vertex attribute format is supported by fetch shader.
bool VKAPI_CALL ICompiler::IsVertexFormatSupported(
    VkFormat format)   // Vertex attribute format
{
    auto pInfo = VertexFetch::GetVertexFormatInfo(format);
    return ((pInfo->dfmt == BUF_DATA_FORMAT_INVALID) && (pInfo->numChannels == 0)) ? false : true;
}

// =====================================================================================================================
Compiler::Compiler(
    const char*       pClient,      // [in] Name of the client who calls LLPC
    GfxIpVersion      gfxIp)        // Graphics IP version info
    :
    m_pClientName(pClient),
    m_gfxIp(gfxIp)
{
    if (m_instanceCount == 0)
    {
        RedirectLogOutput(false);

        // Initialize map table of register names
        if (EnableOuts() || cl::EnablePipelineDump)
        {
            if (gfxIp.major <= 8)
            {
                Gfx6::InitRegisterNameMap(gfxIp);
            }
            else
            {
#ifdef LLPC_BUILD_GFX9
                Gfx9::InitRegisterNameMap(gfxIp);
#endif
            }
        }

        // Initialize LLVM target: AMDGPU
        LLVMInitializeAMDGPUTargetInfo();
        LLVMInitializeAMDGPUTarget();
        LLVMInitializeAMDGPUTargetMC();
        LLVMInitializeAMDGPUAsmPrinter();
        LLVMInitializeAMDGPUAsmParser();
        LLVMInitializeAMDGPUDisassembler();

        InitOptimizer();
#ifdef LLPC_ENABLE_SPIRV_OPT
        InitSpvGen();
#endif
    }

    // Initialize shader cache
    ShaderCacheCreateInfo    createInfo = {};
    ShaderCacheAuxCreateInfo auxCreateInfo = {};
    uint32_t shaderCacheMode = cl::ShaderCacheMode;
    auxCreateInfo.shaderCacheMode = static_cast<ShaderCacheMode>(shaderCacheMode);
    auxCreateInfo.gfxIp           = m_gfxIp;
    auxCreateInfo.pExecutableName = cl::ExecutableName.c_str();
    auxCreateInfo.pCacheFilePath  = getenv("AMD_SHADER_DISK_CACHE_PATH");
    if (auxCreateInfo.pCacheFilePath == nullptr)
    {
#ifdef WIN_OS
        auxCreateInfo.pCacheFilePath  = getenv("LOCALAPPDATA");
#else
        auxCreateInfo.pCacheFilePath  = getenv("HOME");
#endif
    }

    m_shaderCache.Init(&createInfo, &auxCreateInfo);
    InitGpuProperty();
    ++m_instanceCount;

    // Create one context at initialization time
    auto pContext = AcquireContext();
    ReleaseContext(pContext);
}

// =====================================================================================================================
Compiler::~Compiler()
{
    bool shutdown = false;
    {
        // Free context pool
        MutexGuard lock(m_contextPoolMutex);
        for (auto pContext : m_contextPool)
        {
            LLPC_ASSERT(pContext->IsInUse() == false);
            delete pContext;
        }
        m_contextPool.clear();
    }

    if (strcmp(m_pClientName, VkIcdName) == 0)
    {
        // NOTE: Skip subsequent cleanup work for Vulkan ICD. The work will be done by system itself
        return;
    }

    {
        // s_compilerMutex is managed by ManagedStatic, it can't be accessed after llvm_shutdown
        MutexGuard lock(*s_compilerMutex);
        -- m_instanceCount;
        if (m_instanceCount == 0)
        {
            RedirectLogOutput(true);
            shutdown = true;
        }
    }

    if (shutdown)
    {
        llvm_shutdown();
    }
}

// =====================================================================================================================
// Destroys the pipeline compiler.
void Compiler::Destroy()
{
    delete this;
}

// =====================================================================================================================
// Builds shader module from the specified info.
Result Compiler::BuildShaderModule(
    const ShaderModuleBuildInfo* pShaderInfo,   // [in] Info to build this shader module
    ShaderModuleBuildOut*        pShaderOut     // [out] Output of building this shader module
    ) const
{
    Result result = Result::Success;

    // Currently, copy SPIR-V binary as output shader module data
    size_t allocSize = sizeof(ShaderModuleData) + pShaderInfo->shaderBin.codeSize;
    void* pAllocBuf = nullptr;
    BinaryType binType = BinaryType::Spirv;

    // Check the type of input shader binary
    if (IsSpirvBinary(&pShaderInfo->shaderBin))
    {
        binType = BinaryType::Spirv;
    }
    else if (IsLlvmBitcode(&pShaderInfo->shaderBin))
    {
        binType = BinaryType::LlvmBc;
    }
    else
    {
        result = Result::ErrorInvalidShader;
    }

    if (result == Result::Success)
    {
        if (pShaderInfo->pfnOutputAlloc != nullptr)
        {
            pAllocBuf = pShaderInfo->pfnOutputAlloc(pShaderInfo->pInstance,
                                                    pShaderInfo->pUserData,
                                                    allocSize);
            result = (pAllocBuf != nullptr) ? Result::Success : Result::ErrorOutOfMemory;
        }
        else
        {
            // Allocator is not specified
            result = Result::ErrorInvalidPointer;
        }
    }

    if (result == Result::Success)
    {
        ShaderModuleData* pModuleData = reinterpret_cast<ShaderModuleData*>(pAllocBuf);

        pModuleData->binType = binType;
        pModuleData->binCode.codeSize = pShaderInfo->shaderBin.codeSize;
        pModuleData->hash = Md5::GenerateHashFromBuffer(pShaderInfo->shaderBin.pCode, pShaderInfo->shaderBin.codeSize);

        if (cl::EnablePipelineDump)
        {
            DumpSpirvBinary(cl::PipelineDumpDir.c_str(), &pShaderInfo->shaderBin, &pModuleData->hash);
        }

        void* pCode = VoidPtrInc(pAllocBuf, sizeof(ShaderModuleData));
        memcpy(pCode, pShaderInfo->shaderBin.pCode, pShaderInfo->shaderBin.codeSize);
        pModuleData->binCode.pCode = pCode;

        pShaderOut->pModuleData = pModuleData;
    }

    return result;
}

// =====================================================================================================================
// Build graphics pipeline from the specified info.
Result Compiler::BuildGraphicsPipeline(
    const GraphicsPipelineBuildInfo* pPipelineInfo, // [in] Info to build this graphics pipeline
    GraphicsPipelineBuildOut*        pPipelineOut)  // [out] Output of building this graphics pipeline
{
    Result           result  = Result::Success;
    CacheEntryHandle hEntry  = nullptr;
    void*            pElf    = nullptr;
    size_t           elfSize = 0;
    ElfPackage       pipelineElf;

    const PipelineShaderInfo* shaderInfo[ShaderStageGfxCount] =
    {
        &pPipelineInfo->vs,
        &pPipelineInfo->tcs,
        &pPipelineInfo->tes,
        &pPipelineInfo->gs,
        &pPipelineInfo->fs,
    };

    for (uint32_t i = 0; (i < ShaderStageGfxCount) && (result == Result::Success); ++i)
    {
        result = ValidatePipelineShaderInfo(static_cast<ShaderStage>(i), shaderInfo[i]);
    }

    Md5::Hash hash = {};
    hash = GenerateHashForGraphicsPipeline(pPipelineInfo);

    // Do shader replacement if it's enabled
    bool ShaderReplaced = false;
    const ShaderModuleData* restoreModuleData[ShaderStageGfxCount] = {};
    if (cl::ShaderReplaceMode != ShaderReplaceDisable)
    {
        char pipelineHash[64];
        int32_t length = snprintf(pipelineHash, 64, "0x%016llX", Md5::Compact64(&hash));
        LLPC_ASSERT(length >= 0);

        bool hashMatch = true;
        if (cl::ShaderReplaceMode == ShaderReplaceShaderPipelineHash)
        {
            std::string pipelineReplacementHashes = cl::ShaderReplacePipelineHashes;
            hashMatch = (pipelineReplacementHashes.find(pipelineHash) != std::string::npos);

            if (hashMatch)
            {
                LLPC_OUTS("// Shader replacement for graphics pipeline: " << pipelineHash << "\n");
            }
        }

        if (hashMatch)
        {
            for (uint32_t stage = 0; stage < ShaderStageGfxCount; ++stage)
            {
                const ShaderModuleData* pOrigModuleData =
                    reinterpret_cast<const ShaderModuleData*>(shaderInfo[stage]->pModuleData);
                if (pOrigModuleData != nullptr)
                {
                    ShaderModuleData *pModuleData = nullptr;
                    if (ReplaceShader(pOrigModuleData, &pModuleData) == Result::Success)
                    {

                        ShaderReplaced = true;
                        restoreModuleData[stage] = pOrigModuleData;
                        const_cast<PipelineShaderInfo*>(shaderInfo[stage])->pModuleData = pModuleData;

                        char shaderHash[64] = {};
                        int32_t length = snprintf(shaderHash, 64, "0x%016llX", Md5::Compact64(&restoreModuleData[stage]->hash));
                        LLPC_ASSERT(length >= 0);
                        LLPC_OUTS("// Shader replacement for shader: " << shaderHash << ", in pipeline: " << pipelineHash << "\n");
                    }
                }
            }

            if (ShaderReplaced)
            {
                // Update pipeline hash after shader replacement
                hash = GenerateHashForGraphicsPipeline(pPipelineInfo);
            }
        }
    }

    GraphicsContext graphicsContext(m_gfxIp, &m_gpuProperty, pPipelineInfo, &hash);

    if ((result == Result::Success) && EnableOuts())
    {
        LLPC_OUTS("===============================================================================\n");
        LLPC_OUTS("// LLPC calculated hash results (graphics pipline)\n");
        LLPC_OUTS("PIPE : " << format("0x%016llX", Md5::Compact64(&hash)) << "\n");
        for (uint32_t stage = 0; stage < ShaderStageGfxCount; ++stage)
        {
            const ShaderModuleData* pModuleData =
                reinterpret_cast<const ShaderModuleData*>(shaderInfo[stage]->pModuleData);
            if (pModuleData != nullptr)
            {
                LLPC_OUTS(format("%-4s : ", GetShaderStageAbbreviation(static_cast<ShaderStage>(stage), true)) <<
                          format("0x%016llX", Md5::Compact64(&pModuleData->hash)) << "\n");
            }
        }
        LLPC_OUTS("\n");
    }

    raw_fd_ostream* pPipelineDumpFile = nullptr;

    if ((result == Result::Success) && cl::EnablePipelineDump)
    {
        pPipelineDumpFile = CreatePipelineDumpFile(cl::PipelineDumpDir.c_str(), nullptr, pPipelineInfo, &hash);
        if (pPipelineDumpFile != nullptr)
        {
            DumpGraphicsPipelineInfo(pPipelineDumpFile, pPipelineInfo);
        }
    }

    ShaderEntryState cacheEntryState = ShaderEntryState::New;
    ShaderCache* pShaderCache = (pPipelineInfo->pShaderCache != nullptr) ?
                                    static_cast<ShaderCache*>(pPipelineInfo->pShaderCache) :
                                    &m_shaderCache;
    if (cl::ShaderCacheMode == ShaderCacheForceInternalCacheOnDisk)
    {
        pShaderCache = &m_shaderCache;
    }

    if (result == Result::Success)
    {
        if (ShaderReplaced)
        {
            cacheEntryState = ShaderEntryState::Compiling;
        }
        else
        {
            cacheEntryState = pShaderCache->FindShader(hash, true, &hEntry);
            if (cacheEntryState == ShaderEntryState::Ready)
            {
                result = pShaderCache->RetrieveShader(hEntry, &pElf, &elfSize);
                // Re-try if shader cache return error unknown
                if (result == Result::ErrorUnknown)
                {
                    result = Result::Success;
                    hEntry = nullptr;
                    cacheEntryState = ShaderEntryState::Compiling;
                }
            }
        }
    }

    if (cacheEntryState == ShaderEntryState::Compiling)
    {
        bool skipLower = false;
        bool skipPatch = false;

        BinaryType binType = BinaryType::Unknown;

        Module* modules[ShaderStageGfxCount] = {};
        std::unique_ptr<Module> bitcodes[ShaderStageGfxCount];

        Context* pContext = AcquireContext();
        pContext->AttachPipelineContext(&graphicsContext);

        // Translate SPIR-V binary to machine-independent LLVM module
        for (uint32_t stage = 0; (stage < ShaderStageGfxCount) && (result == Result::Success); ++stage)
        {
            const PipelineShaderInfo* pShaderInfo = shaderInfo[stage];
            if (pShaderInfo->pModuleData == nullptr)
            {
                continue;
            }

            if (cl::DisableWipFeatures &&
                ((stage == ShaderStageTessControl) || (stage == ShaderStageTessEval) || (stage == ShaderStageGeometry)))
            {
                result = Result::Unsupported;
                LLPC_ERRS("Unsupported shader stage.\n")
                continue;
            }

            Module* pModule = nullptr;

            const ShaderModuleData* pModuleData = reinterpret_cast<const ShaderModuleData*>(pShaderInfo->pModuleData);
            // Binary type must same for all shader stages
            LLPC_ASSERT((binType == BinaryType::Unknown) || (pModuleData->binType == binType));
            binType = pModuleData->binType;
            if (binType == BinaryType::Spirv)
            {
                TimeProfiler timeProfiler(&g_timeProfileResult.translateTime);
                result = TranslateSpirvToLlvm(&pModuleData->binCode,
                                              static_cast<ShaderStage>(stage),
                                              pShaderInfo->pEntryTarget,
                                              pShaderInfo->pSpecializatonInfo,
                                              pContext,
                                              &pModule);
            }
            else if (binType == BinaryType::LlvmBc)
            {
                // Skip lower and patch phase if input is LLVM IR
                skipLower = true;
                skipPatch = true;
                bitcodes[stage] = pContext->LoadLibary(&pModuleData->binCode);
                pModule = bitcodes[stage].get();
            }
            else
            {
                LLPC_NEVER_CALLED();
            }

            // Verify this LLVM module
            if (result == Result::Success)
            {
                LLPC_OUTS("===============================================================================\n");
                LLPC_OUTS("// LLPC SPIRV-to-LLVM translation results (" <<
                          GetShaderStageName(static_cast<ShaderStage>(stage)) << " shader)\n");
                LLPC_OUTS(*pModule);
                LLPC_OUTS("\n");
                std::string errMsg;
                raw_string_ostream errStream(errMsg);
                if (verifyModule(*pModule, &errStream))
                {
                    LLPC_ERRS("Fails to verify module after translation (" <<
                              GetShaderStageName(static_cast<ShaderStage>(stage)) << " shader): " <<
                              errStream.str() << "\n");
                    result = Result::ErrorInvalidShader;
                }
            }

            // Do SPIR-V lowering operations for this LLVM module
            if ((result == Result::Success) && (skipLower == false))
            {
                TimeProfiler timeProfiler(&g_timeProfileResult.lowerTime);
                result = SpirvLower::Run(pModule);
                if (result != Result::Success)
                {
                    LLPC_ERRS("Fails to do SPIR-V lowering operations (" <<
                              GetShaderStageName(static_cast<ShaderStage>(stage)) << " shader)\n");
                }
                else
                {
                    LLPC_OUTS("===============================================================================\n");
                    LLPC_OUTS("// LLPC SPIRV-lowering results (" <<
                              GetShaderStageName(static_cast<ShaderStage>(stage)) << " shader)\n");
                    LLPC_OUTS(*pModule);
                    LLPC_OUTS("\n");
                }
            }

            modules[stage] = pModule;
        }

        // Build null fragment shader if necessary
        std::unique_ptr<Module> pNullFsModule;
        if ((result == Result::Success) && (cl::AutoLayoutDesc == false) && (modules[ShaderStageFragment] == nullptr))
        {
            TimeProfiler timeProfiler(&g_timeProfileResult.lowerTime);
            result = BuildNullFs(pContext, pNullFsModule);
            if (result == Result::Success)
            {
                modules[ShaderStageFragment] = pNullFsModule.get();
            }
            else
            {
                LLPC_ERRS("Fails to build a LLVM module for null fragment shader\n");
            }
        }

        // Do LLVM module pacthing (preliminary patch work)
        for (int32_t stage = ShaderStageGfxCount - 1; (stage >= 0) && (result == Result::Success); --stage)
        {
            Module* pModule = modules[stage];
            if ((pModule == nullptr) || skipPatch)
            {
                continue;
            }

            TimeProfiler timeProfiler(&g_timeProfileResult.patchTime);
            result = Patch::PreRun(pModule);
            if (result != Result::Success)
            {
                LLPC_ERRS("Fails to do preliminary patch work for LLVM module (" <<
                          GetShaderStageName(static_cast<ShaderStage>(stage)) << " shader)\n");
            }
        }

        // Determine whether or not GS on-chip mode is valid for this pipeline
        if ((result == Result::Success) && (cl::DisableGsOnChip == false) && (modules[ShaderStageGeometry] != nullptr))
        {
            bool gsOnChip = pContext->CanGsOnChip();
            pContext->SetGsOnChip(gsOnChip);
        }

#ifdef LLPC_BUILD_GFX9
        // Do user data node merge for merged shader
        if ((result == Result::Success) && (m_gfxIp.major >= 9))
        {
            pContext->DoUserDataNodeMerge();
        }
#endif

        // Do LLVM module patching (main patch work)
        for (int32_t stage = ShaderStageGfxCount - 1; (stage >= 0) && (result == Result::Success); --stage)
        {
            Module* pModule = modules[stage];
            if ((pModule == nullptr) || skipPatch)
            {
                continue;
            }

            TimeProfiler timeProfiler(&g_timeProfileResult.patchTime);
            result = Patch::Run(pModule);
            if (result != Result::Success)
            {
                LLPC_ERRS("Fails to patch LLVM module and link it with external library (" <<
                          GetShaderStageName(static_cast<ShaderStage>(stage)) << " shader)\n");
            }
            else
            {
                LLPC_OUTS("===============================================================================\n");
                LLPC_OUTS("// LLPC patching results (" <<
                          GetShaderStageName(static_cast<ShaderStage>(stage)) << " shader)\n");
                LLPC_OUTS(*pModule);
                LLPC_OUTS("\n");
            }
        }

#ifdef LLPC_BUILD_GFX9
        // Do shader merge operations
        if ((result == Result::Success) && (m_gfxIp.major >= 9))
        {
            const bool hasVs  = (modules[ShaderStageVertex] != nullptr);
            const bool hasTcs = (modules[ShaderStageTessControl] != nullptr);

            const bool hasTs = ((modules[ShaderStageTessControl] != nullptr) ||
                                (modules[ShaderStageTessEval] != nullptr));
            const bool hasGs = (modules[ShaderStageGeometry] != nullptr);

            ShaderMerger shaderMerger(pContext);

            if (hasTs && (hasVs || hasTcs))
            {
                // Ls-HS merged shader should be present
                Module* pLsModule = modules[ShaderStageVertex];
                Module* pHsModule = modules[ShaderStageTessControl];

                Module* pLsHsModule = nullptr;

                TimeProfiler timeProfiler(&g_timeProfileResult.patchTime);
                result = shaderMerger.BuildLsHsMergedShader(pLsModule, pHsModule, &pLsHsModule);

                if (result != Result::Success)
                {
                    LLPC_ERRS("Fails to build LS-HS merged shader\n");
                }
                else
                {
                    LLPC_OUTS("===============================================================================\n");
                    LLPC_OUTS("// LLPC shader merge results (LS-HS)\n");
                    LLPC_OUTS(*pLsHsModule);
                    LLPC_OUTS("\n");
                }

                // NOTE: After LS and HS are merged, LS and HS are destroy. And new LS-HS merged shader is treated
                // as tessellation control shader.
                modules[ShaderStageVertex] = nullptr;
                modules[ShaderStageTessControl] = pLsHsModule;
            }

            if (hasGs)
            {
                // ES-GS merged shader should be present
                Module* pEsModule = modules[hasTs ? ShaderStageTessEval : ShaderStageVertex];
                Module* pGsModule = modules[ShaderStageGeometry];

                Module* pEsGsModule = nullptr;

                TimeProfiler timeProfiler(&g_timeProfileResult.patchTime);
                result = shaderMerger.BuildEsGsMergedShader(pEsModule, pGsModule, &pEsGsModule);

                if (result != Result::Success)
                {
                    LLPC_ERRS("Fails to build ES-GS merged shader\n");
                }
                else
                {
                    LLPC_OUTS("===============================================================================\n");
                    LLPC_OUTS("// LLPC shader merge results (ES-GS)\n");
                    LLPC_OUTS(*pEsGsModule);
                    LLPC_OUTS("\n");
                }

                // NOTE: After ES and GS are merged, ES and GS are destroy. And new ES-GS merged shader is treated
                // as geometry shader.
                modules[hasTs ? ShaderStageTessEval : ShaderStageVertex] = nullptr;
                modules[ShaderStageGeometry] = pEsGsModule;
            }
        }
#endif

        // Generate GPU ISA codes
        std::vector<ElfPackage> shaderElfs;
        for (uint32_t stage = 0; (stage < ShaderStageGfxCount) && (result == Result::Success); ++stage)
        {
            Module* pModule = modules[stage];
            if (pModule == nullptr)
            {
                continue;
            }

            ElfPackage shaderElf;
            raw_svector_ostream elfStream(shaderElf);
            std::string errMsg;

            TimeProfiler timeProfiler(&g_timeProfileResult.codeGenTime);
            result = CodeGenManager::GenerateCode(pModule, elfStream, errMsg);
            if (result != Result::Success)
            {
                LLPC_ERRS("Fails to generate GPU ISA codes (" <<
                          GetShaderStageName(static_cast<ShaderStage>(stage)) << " shader) :" <<
                          errMsg << "\n");
            }
            else
            {
                shaderElfs.push_back(shaderElf);
            }
        }

        // Build copy shader if necessary (has geometry shader)
        if ((result == Result::Success) && (modules[ShaderStageGeometry] != nullptr))
        {
            ElfPackage shaderElf;

            TimeProfiler timeProfiler(&g_timeProfileResult.codeGenTime);
            result = BuildCopyShader(pContext, &shaderElf);
            if (result != Result::Success)
            {
                LLPC_ERRS("Fails to build a LLVM module and generate GPU ISA codes for copy shader\n");
            }
            else
            {
                shaderElfs.push_back(shaderElf);
            }
        }

        // Clean up modules
        if (pNullFsModule != nullptr)
        {
            modules[ShaderStageFragment] = nullptr;
        }

        for (uint32_t stage = 0; stage < ShaderStageGfxCount; ++stage)
        {
            if (bitcodes[stage] != nullptr)
            {
                LLPC_ASSERT(bitcodes[stage].get() == modules[stage]);
                modules[stage] = nullptr;
                bitcodes[stage] = nullptr;
            }
            else
            {
                delete modules[stage];
                modules[stage] = nullptr;
            }
        }

        // Fill pipeline building output
        if (result == Result::Success)
        {
            TimeProfiler timeProfiler(&g_timeProfileResult.codeGenTime);
            result = CodeGenManager::FinalizeElf(pContext, shaderElfs.data(), shaderElfs.size(), &pipelineElf);
            elfSize = pipelineElf.size();
            pElf = pipelineElf.data();
        }

        if ((ShaderReplaced == false) && (hEntry != nullptr))
        {
            if (result == Result::Success)
            {
                LLPC_ASSERT(elfSize > 0);
                pShaderCache->InsertShader(hEntry, pElf, elfSize);
            }
            else
            {
                pShaderCache->ResetShader(hEntry);
            }
        }

        ReleaseContext(pContext);
    }

    if (result == Result::Success)
    {
        void* pAllocBuf = nullptr;
        if (pPipelineInfo->pfnOutputAlloc != nullptr)
        {
            pAllocBuf = pPipelineInfo->pfnOutputAlloc(pPipelineInfo->pInstance, pPipelineInfo->pUserData, elfSize);
        }
        else
        {
            // Allocator is not specified
            result = Result::ErrorInvalidPointer;
        }

        uint8_t* pCode = static_cast<uint8_t*>(pAllocBuf);
        memcpy(pCode, pElf, elfSize);

        pPipelineOut->pipelineBin.codeSize = elfSize;
        pPipelineOut->pipelineBin.pCode = pCode;
    }

    if (pPipelineDumpFile != nullptr)
    {
        if (result == Result::Success)
        {
            DumpPipelineBinary(pPipelineDumpFile,
                               m_gfxIp,
                               &pPipelineOut->pipelineBin);
        }

        DestroyPipelineDumpFile(pPipelineDumpFile);
    }

    // Free shader replacement allocations and restore original shader module
    if (cl::ShaderReplaceMode != ShaderReplaceDisable)
    {
        for (uint32_t stage = 0; stage < ShaderStageGfxCount; ++stage)
        {
            if (restoreModuleData[stage] != nullptr)
            {
                delete reinterpret_cast<const char*>(shaderInfo[stage]->pModuleData);
                const_cast<PipelineShaderInfo*>(shaderInfo[stage])->pModuleData = restoreModuleData[stage];
            }
        }
    }

    if (cl::EnableTimeProfiler)
    {
        DumpTimeProfilingResult(&hash);
    }

    return result;
}

// =====================================================================================================================
// Build compute pipeline from the specified info.
Result Compiler::BuildComputePipeline(
    const ComputePipelineBuildInfo* pPipelineInfo,  // [in] Info to build this compute pipeline
    ComputePipelineBuildOut*        pPipelineOut)   // [out] Output of building this compute pipeline
{
    CacheEntryHandle hEntry = nullptr;
    void*            pElf    = nullptr;
    size_t           elfSize = 0;
    ElfPackage       pipelineElf;

    Result result = ValidatePipelineShaderInfo(ShaderStageCompute, &pPipelineInfo->cs);

    Md5::Hash hash = {};
    hash = GenerateHashForComputePipeline(pPipelineInfo);

    // Do shader replacement if it's enabled
    bool ShaderReplaced = false;
    const ShaderModuleData* pRestoreModuleData = nullptr;
    if (cl::ShaderReplaceMode != ShaderReplaceDisable)
    {
        char pipelineHash[64];
        int32_t length = snprintf(pipelineHash, 64, "0x%016llX", Md5::Compact64(&hash));
        LLPC_ASSERT(length >= 0);

        bool hashMatch = true;
        if (cl::ShaderReplaceMode == ShaderReplaceShaderPipelineHash)
        {
            std::string pipelineReplacementHashes = cl::ShaderReplacePipelineHashes;
            hashMatch = (pipelineReplacementHashes.find(pipelineHash) != std::string::npos);

            if (hashMatch)
            {
                LLPC_OUTS("// Shader replacement for compute pipeline: " << pipelineHash << "\n");
            }
        }

        if (hashMatch)
        {
            const ShaderModuleData* pOrigModuleData =
                reinterpret_cast<const ShaderModuleData*>(pPipelineInfo->cs.pModuleData);
            if (pOrigModuleData != nullptr)
            {
                ShaderModuleData *pModuleData = nullptr;
                if (ReplaceShader(pOrigModuleData, &pModuleData) == Result::Success)
                {
                    ShaderReplaced = true;
                    pRestoreModuleData = pOrigModuleData;
                    const_cast<PipelineShaderInfo*>(&pPipelineInfo->cs)->pModuleData = pModuleData;

                    char shaderHash[64];
                    int32_t length = snprintf(shaderHash, 64, "0x%016llX", Md5::Compact64(&pRestoreModuleData->hash));
                    LLPC_ASSERT(length >= 0);
                    LLPC_OUTS("// Shader replacement for shader: " << shaderHash << ", in pipeline: " << pipelineHash << "\n");
                }
            }

            if (ShaderReplaced)
            {
                // Update pipeline hash after shader replacement
                hash = GenerateHashForComputePipeline(pPipelineInfo);
            }
        }
    }

    ComputeContext computeContext(m_gfxIp, &m_gpuProperty, pPipelineInfo, &hash);

    if ((result == Result::Success) && EnableOuts())
    {
        const ShaderModuleData* pModuleData = reinterpret_cast<const ShaderModuleData*>(pPipelineInfo->cs.pModuleData);
        LLPC_OUTS("===============================================================================\n");
        LLPC_OUTS("// LLPC calculated hash results (compute pipline)\n");
        LLPC_OUTS("PIPE : " << format("0x%016llX", Md5::Compact64(&hash)) << "\n");
        LLPC_OUTS(format("%-4s : ", GetShaderStageAbbreviation(ShaderStageCompute, true)) <<
                  format("0x%016llX", Md5::Compact64(&pModuleData->hash)) << "\n");
        LLPC_OUTS("\n");
    }

    raw_fd_ostream* pPipelineDumpFile = nullptr;
    if ((result == Result::Success) && cl::EnablePipelineDump)
    {
        pPipelineDumpFile = CreatePipelineDumpFile(cl::PipelineDumpDir.c_str(), pPipelineInfo, nullptr, &hash);
        if (pPipelineDumpFile != nullptr)
        {
            DumpComputePipelineInfo(pPipelineDumpFile, pPipelineInfo);
        }
    }

    ShaderEntryState cacheEntryState = ShaderEntryState::New;
    ShaderCache* pShaderCache = (pPipelineInfo->pShaderCache != nullptr) ?
                                    static_cast<ShaderCache*>(pPipelineInfo->pShaderCache) :
                                    &m_shaderCache;
    if (cl::ShaderCacheMode == ShaderCacheForceInternalCacheOnDisk)
    {
        pShaderCache = &m_shaderCache;
    }

    if (result == Result::Success)
    {
        if (ShaderReplaced)
        {
            cacheEntryState = ShaderEntryState::Compiling;
        }
        else
        {
            cacheEntryState = pShaderCache->FindShader(hash, true, &hEntry);
            if (cacheEntryState == ShaderEntryState::Ready)
            {
                result = pShaderCache->RetrieveShader(hEntry, &pElf, &elfSize);
                // Re-try if shader cache return error unknown
                if (result == Result::ErrorUnknown)
                {
                    result = Result::Success;
                    hEntry = nullptr;
                    cacheEntryState = ShaderEntryState::Compiling;
                }
            }
        }
    }

    if (cacheEntryState == ShaderEntryState::Compiling)
    {
        bool skipPatch = false;
        Module* pModule = nullptr;
        std::unique_ptr<Module> bitcode;

        Context* pContext = AcquireContext();
        pContext->AttachPipelineContext(&computeContext);

        // Translate SPIR-V binary to machine-independent LLVM module
        const ShaderModuleData* pModuleData = reinterpret_cast<const ShaderModuleData*>(pPipelineInfo->cs.pModuleData);
        if (pModuleData != nullptr)
        {
            if (pModuleData->binType == BinaryType::Spirv)
            {
                TimeProfiler timeProfiler(&g_timeProfileResult.translateTime);

                result = TranslateSpirvToLlvm(&pModuleData->binCode,
                                              ShaderStageCompute,
                                              pPipelineInfo->cs.pEntryTarget,
                                              pPipelineInfo->cs.pSpecializatonInfo,
                                              pContext,
                                              &pModule);

                // Verify this LLVM module
                if (result == Result::Success)
                {
                    LLPC_OUTS("===============================================================================\n");
                    LLPC_OUTS("// LLPC SPIRV-to-LLVM translation results (" <<
                              GetShaderStageName(ShaderStageCompute) << " shader)\n");
                    LLPC_OUTS(*pModule);
                    LLPC_OUTS("\n");
                    std::string errMsg;
                    raw_string_ostream errStream(errMsg);
                    if (verifyModule(*pModule, &errStream))
                    {
                        LLPC_ERRS("Fails to verify module after translation: (" <<
                                  GetShaderStageName(ShaderStageCompute) << " shader) :" << errStream.str() << "\n");
                        result = Result::ErrorInvalidShader;
                    }
                }

                // Do SPIR-V lowering operations for this LLVM module
                if (result == Result::Success)
                {
                    TimeProfiler timeProfiler(&g_timeProfileResult.lowerTime);
                    result = SpirvLower::Run(pModule);
                    if (result != Result::Success)
                    {
                        LLPC_ERRS("Fails to do SPIR-V lowering operations (" <<
                                  GetShaderStageName(ShaderStageCompute) << " shader)\n");
                    }
                    else
                    {
                        LLPC_OUTS("===============================================================================\n");
                        LLPC_OUTS("// LLPC SPIRV-lowering results (" <<
                                  GetShaderStageName(ShaderStageCompute) << " shader)\n");
                        LLPC_OUTS(*pModule);
                        LLPC_OUTS("\n");
                    }
                }
            }
            else
            {
                // TODO: Handle other binary types.
                LLPC_NOT_IMPLEMENTED();
            }
        }
        else if (pModuleData->binType == BinaryType::LlvmBc)
        {
            // Skip lower and patch phase if input is LLVM IR
            skipPatch = true;
            bitcode = pContext->LoadLibary(&pModuleData->binCode);
            pModule = bitcode.get();
        }
        else
        {
            LLPC_NEVER_CALLED();
        }

        // Do LLVM module pacthing and generate GPU ISA codes
        ElfPackage shaderElf;
        if (result == Result::Success)
        {
            LLPC_ASSERT(pModule != nullptr);

            // Preliminary patch work
            if (skipPatch == false)
            {
                TimeProfiler timeProfiler(&g_timeProfileResult.patchTime);
                result = Patch::PreRun(pModule);
            }

            if (result != Result::Success)
            {
                LLPC_ERRS("Fails to do preliminary patch work for LLVM module (" <<
                          GetShaderStageName(ShaderStageCompute) << " shader)\n");
            }

            // Main patch work
            if (result == Result::Success)
            {
                if (skipPatch == false)
                {
                    TimeProfiler timeProfiler(&g_timeProfileResult.patchTime);
                    result = Patch::Run(pModule);
                }

                if (result != Result::Success)
                {
                    LLPC_ERRS("Fails to patch LLVM module and link it with external library (" <<
                              GetShaderStageName(ShaderStageCompute) << " shader)\n");
                }
                else
                {
                    LLPC_OUTS("===============================================================================\n");
                    LLPC_OUTS("// LLPC patching result (" <<
                              GetShaderStageName(ShaderStageCompute) << " shader)\n");
                    LLPC_OUTS(*pModule);
                    LLPC_OUTS("\n");
                }
            }

            if (result == Result::Success)
            {
                TimeProfiler timeProfiler(&g_timeProfileResult.codeGenTime);
                raw_svector_ostream elfStream(shaderElf);
                std::string errMsg;
                result = CodeGenManager::GenerateCode(pModule, elfStream, errMsg);
                if (result != Result::Success)
                {
                    LLPC_ERRS("Fails to generate GPU ISA codes (" <<
                              GetShaderStageName(ShaderStageCompute) << " shader) : " << errMsg << "\n");
                }
            }
        }

        if (bitcode != nullptr)
        {
            LLPC_ASSERT(bitcode.get() == pModule);
            bitcode = nullptr;
            pModule = nullptr;
        }
        else
        {
            delete pModule;
            pModule = nullptr;
        }

        // Fill pipeline building output
        if (result == Result::Success)
        {
            TimeProfiler timeProfiler(&g_timeProfileResult.codeGenTime);
            result = CodeGenManager::FinalizeElf(pContext, &shaderElf, 1, &pipelineElf);
            pElf = pipelineElf.data();
            elfSize = pipelineElf.size();
        }

        if ((ShaderReplaced == false) && (hEntry != nullptr))
        {
            if (result == Result::Success)
            {
                LLPC_ASSERT(elfSize > 0);
                pShaderCache->InsertShader(hEntry, pElf, elfSize);
            }
            else
            {
                pShaderCache->ResetShader(hEntry);
            }
        }

        ReleaseContext(pContext);
    }

    if (result == Result::Success)
    {
        void* pAllocBuf = nullptr;
        if (pPipelineInfo->pfnOutputAlloc != nullptr)
        {
            pAllocBuf = pPipelineInfo->pfnOutputAlloc(pPipelineInfo->pInstance, pPipelineInfo->pUserData, elfSize);
        }
        else
        {
            // Allocator is not specified
            result = Result::ErrorInvalidPointer;
        }

        uint8_t* pCode = static_cast<uint8_t*>(pAllocBuf);
        memcpy(pCode, pElf, elfSize);

        pPipelineOut->pipelineBin.codeSize = elfSize;
        pPipelineOut->pipelineBin.pCode = pCode;
    }

    if (pPipelineDumpFile != nullptr)
    {
        if (result == Result::Success)
        {
            DumpPipelineBinary(pPipelineDumpFile,
                                m_gfxIp,
                                &pPipelineOut->pipelineBin);
        }

        DestroyPipelineDumpFile(pPipelineDumpFile);
    }

    // Free shader replacement allocations and restore original shader module
    if (cl::ShaderReplaceMode != ShaderReplaceDisable)
    {
        if (pRestoreModuleData != nullptr)
        {
            delete reinterpret_cast<const char*>(pPipelineInfo->cs.pModuleData);
            const_cast<PipelineShaderInfo*>(&pPipelineInfo->cs)->pModuleData = pRestoreModuleData;
        }
    }

    if (cl::EnableTimeProfiler)
    {
        DumpTimeProfilingResult(&hash);
    }

    return result;
}

// =====================================================================================================================
// Does shader replacement if it is feasible (files used by replacement exist as expected).
Result Compiler::ReplaceShader(
    const ShaderModuleData*     pOrigModuleData,    // [in] Original shader module
    ShaderModuleData**          ppModuleData        // [out] Resuling shader module after shader replacement
    ) const
{
    uint64_t shaderHash = Md5::Compact64(&pOrigModuleData->hash);
    char fileName[64];
    int32_t length = snprintf(fileName, 64, "Shader_0x%016llX_replace.spv", shaderHash);
    LLPC_ASSERT(length >= 0);
    std::string replaceFileName = cl::ShaderReplaceDir;
    replaceFileName += "/";
    replaceFileName += fileName;

    Result result = File::Exists(replaceFileName.c_str()) ? Result::Success : Result::ErrorUnavailable;
    if (result == Result::Success)
    {
        File shaderFile;
        result = shaderFile.Open(replaceFileName.c_str(), FileAccessRead | FileAccessBinary);
        if (result == Result::Success)
        {
            size_t binSize = File::GetFileSize(replaceFileName.c_str());

            void *pAllocBuf = new char[binSize + sizeof(ShaderModuleData)];
            ShaderModuleData *pModuleData = reinterpret_cast<ShaderModuleData*>(pAllocBuf);
            pAllocBuf = VoidPtrInc(pAllocBuf, sizeof(ShaderModuleData));

            void* pShaderBin = pAllocBuf;
            shaderFile.Read(pShaderBin, binSize, nullptr);

            pModuleData->binType = pOrigModuleData->binType;
            pModuleData->binCode.codeSize = binSize;
            pModuleData->binCode.pCode = pShaderBin;
            pModuleData->hash = Md5::GenerateHashFromBuffer(pShaderBin, binSize);

            *ppModuleData = pModuleData;

            shaderFile.Close();
        }
    }

    return result;
}

// =====================================================================================================================
// Translates SPIR-V binary to machine-independent LLVM module.
Result Compiler::TranslateSpirvToLlvm(
    const BinaryData*           pSpirvBin,           // [in] SPIR-V binary
    ShaderStage                 shaderStage,         // Shader stage
    const char*                 pEntryTarget,        // [in] SPIR-V entry point
    const VkSpecializationInfo* pSpecializationInfo, // [in] Specialization info
    LLVMContext*                pContext,            // [in] LLPC pipeline context
    Module**                    ppModule             // [out] Created LLVM module after this translation
    ) const
{
    Result result = Result::Success;

    BinaryData  optSpirvBin = {};

    if (OptimizeSpirv(pSpirvBin, &optSpirvBin) == Result::Success)
    {
        pSpirvBin = &optSpirvBin;
    }

    std::string spirvCode(static_cast<const char*>(pSpirvBin->pCode), pSpirvBin->codeSize);
    std::istringstream spirvStream(spirvCode);
    std::string errMsg;
    SPIRVSpecConstMap specConstMap;

    // Build specialization constant map
    if (pSpecializationInfo != nullptr)
    {
        for (uint32_t i = 0; i < pSpecializationInfo->mapEntryCount; ++i)
        {
            SPIRVSpecConstEntry specConstEntry  = {};
            auto pMapEntry = &pSpecializationInfo->pMapEntries[i];
            specConstEntry.DataSize= pMapEntry->size;
            specConstEntry.Data = VoidPtrInc(pSpecializationInfo->pData, pMapEntry->offset);
            specConstMap[pMapEntry->constantID] = specConstEntry;
        }
    }

    if (ReadSPIRV(*pContext,
                    spirvStream,
                    static_cast<spv::ExecutionModel>(shaderStage),
                    pEntryTarget,
                    specConstMap,
                    *ppModule,
                    errMsg) == false)
    {
        LLPC_ERRS("Fails to translate SPIR-V to LLVM (" <<
                    GetShaderStageName(static_cast<ShaderStage>(shaderStage)) << " shader): " << errMsg << "\n");
        result = Result::ErrorInvalidShader;
    }

    CleanOptimizedSpirv(&optSpirvBin);

    return result;
}

// =====================================================================================================================
// Optimizes SPIR-V binary
Result Compiler::OptimizeSpirv(
    const BinaryData* pSpirvBinIn,     // [in] Input SPIR-V binary
    BinaryData*       pSpirvBinOut     // [out] Optimized SPIR-V binary
    ) const
{
    bool success = false;
    uint32_t optBinSize = 0;
    void* pOptBin = nullptr;

#ifdef LLPC_ENABLE_SPIRV_OPT
    if (cl::EnableSpirvOpt)
    {
        char logBuf[4096] = {};
        success = spvOptimizeSpirv(pSpirvBinIn->codeSize,
                                   pSpirvBinIn->pCode,
                                   0,
                                   nullptr,
                                   &optBinSize,
                                   &pOptBin,
                                   4096,
                                   logBuf);
        if (success == false)
        {
            LLPC_ERRS(logBuf);
        }
    }
#endif

    if (success)
    {
        pSpirvBinOut->codeSize = optBinSize;
        pSpirvBinOut->pCode = pOptBin;
    }
    else
    {
        pSpirvBinOut->codeSize = 0;
        pSpirvBinOut->pCode = nullptr;
    }
    return success ? Result::Success : Result::ErrorInvalidShader;
}

// =====================================================================================================================
// Cleanup work for SPIR-V binary, freeing the allocated buffer by OptimizeSpirv()
void Compiler::CleanOptimizedSpirv(
    BinaryData* pSpirvBin   // [in] Optimized SPIR-V binary
    ) const
{
#ifdef LLPC_ENABLE_SPIRV_OPT
    if (pSpirvBin->pCode)
    {
        spvFreeBuffer(const_cast<void*>(pSpirvBin->pCode));
    }
#endif
}

// =====================================================================================================================
// Gets hash code from graphics pipline build info.
uint64_t Compiler::GetGraphicsPipelineHash(
    const GraphicsPipelineBuildInfo* pPipelineInfo  // [in] Info to build a graphics pipeline
    ) const
{
    Md5::Hash hash = GenerateHashForGraphicsPipeline(pPipelineInfo);
    return Md5::Compact64(&hash);
}

// =====================================================================================================================
// Gets hash code from graphics pipline build info.
uint64_t Compiler::GetComputePipelineHash(
    const ComputePipelineBuildInfo* pPipelineInfo  // [in] Info to build a compute pipeline
    ) const
{
    Md5::Hash hash = GenerateHashForComputePipeline(pPipelineInfo);
    return Md5::Compact64(&hash);
}

// =====================================================================================================================
// Builds MD5 hash code from graphics pipline build info.
Md5::Hash Compiler::GenerateHashForGraphicsPipeline(
    const GraphicsPipelineBuildInfo* pPipeline  // [in] Info to build a graphics pipeline
    ) const
{
    Md5::Context checksumCtx = {};
    Md5::Hash    hash        = {};

    Md5::Init(&checksumCtx);

    UpdateHashForPipelineShaderInfo(ShaderStageVertex, &pPipeline->vs, &checksumCtx);
    UpdateHashForPipelineShaderInfo(ShaderStageTessControl, &pPipeline->tcs, &checksumCtx);
    UpdateHashForPipelineShaderInfo(ShaderStageTessEval, &pPipeline->tes, &checksumCtx);
    UpdateHashForPipelineShaderInfo(ShaderStageGeometry, &pPipeline->gs, &checksumCtx);
    UpdateHashForPipelineShaderInfo(ShaderStageFragment, &pPipeline->fs, &checksumCtx);

    if ((pPipeline->pVertexInput != nullptr) && (pPipeline->pVertexInput->vertexBindingDescriptionCount > 0))
    {
        auto pVertexInput = pPipeline->pVertexInput;
        Md5::Update(&checksumCtx, pVertexInput->vertexBindingDescriptionCount);
        Md5::Update(&checksumCtx,
                    pVertexInput->pVertexBindingDescriptions,
                    sizeof(VkVertexInputBindingDescription) * pVertexInput->vertexBindingDescriptionCount);
        Md5::Update(&checksumCtx, pVertexInput->vertexAttributeDescriptionCount);
        Md5::Update(&checksumCtx,
                    pVertexInput->pVertexAttributeDescriptions,
                    sizeof(VkVertexInputAttributeDescription) * pVertexInput->vertexAttributeDescriptionCount);
    }
    auto pIaState = &pPipeline->iaState;
    Md5::Update(&checksumCtx, pIaState->topology);
    Md5::Update(&checksumCtx, pIaState->patchControlPoints);
    Md5::Update(&checksumCtx, pIaState->deviceIndex);
    Md5::Update(&checksumCtx, pIaState->disableVertexReuse);

    auto pVpState = &pPipeline->vpState;
    Md5::Update(&checksumCtx, pVpState->depthClipEnable);

    auto pRsState = &pPipeline->rsState;
    Md5::Update(&checksumCtx, pRsState->rasterizerDiscardEnable);
    if (pRsState->perSampleShading)
    {
        Md5::Update(&checksumCtx, pRsState->perSampleShading);
    }
    Md5::Update(&checksumCtx, pRsState->numSamples);
    Md5::Update(&checksumCtx, pRsState->samplePatternIdx);
    Md5::Update(&checksumCtx, pRsState->usrClipPlaneMask);

    auto pCbState = &pPipeline->cbState;
    Md5::Update(&checksumCtx, pCbState->alphaToCoverageEnable);
    Md5::Update(&checksumCtx, pCbState->dualSourceBlendEnable);
    for (uint32_t i = 0; i < MaxColorTargets; ++i)
    {
        if (pCbState->target[i].format != VK_FORMAT_UNDEFINED)
        {
            Md5::Update(&checksumCtx, pCbState->target[i].format);
            Md5::Update(&checksumCtx, pCbState->target[i].blendEnable);
            Md5::Update(&checksumCtx, pCbState->target[i].blendSrcAlphaToColor);
        }
    }

    Md5::Final(&checksumCtx, &hash);

    return hash;
}

// =====================================================================================================================
// Builds MD5 hash code from compute pipline build info.
Md5::Hash Compiler::GenerateHashForComputePipeline(
    const ComputePipelineBuildInfo* pPipeline   // [in] Info to build a compute pipeline
    ) const
{
    Md5::Context checksumCtx = {};
    Md5::Hash    hash       = {};

    Md5::Init(&checksumCtx);

    UpdateHashForPipelineShaderInfo(ShaderStageCompute, &pPipeline->cs, &checksumCtx);

    Md5::Final(&checksumCtx, &hash);

    return hash;
}

// =====================================================================================================================
// Updates MD5 hash code context for pipeline shader stage.
void Compiler::UpdateHashForPipelineShaderInfo(
    ShaderStage               stage,           // shader stage
    const PipelineShaderInfo* pShaderInfo,     // [in] Shader info in specified shader stage
    Md5::Context*             pChecksumCtx     // [in,out] MD5 hash code context
    ) const
{
    if (pShaderInfo->pModuleData)
    {
        const ShaderModuleData* pModuleData = reinterpret_cast<const ShaderModuleData*>(pShaderInfo->pModuleData);
        Md5::Update(pChecksumCtx, stage);
        Md5::Update(pChecksumCtx, pModuleData->hash);

        if (pShaderInfo->pEntryTarget)
        {
            size_t entryNameLen = strlen(pShaderInfo->pEntryTarget);
            Md5::Update(pChecksumCtx, pShaderInfo->pEntryTarget, entryNameLen);
        }

        if ((pShaderInfo->pSpecializatonInfo) && (pShaderInfo->pSpecializatonInfo->mapEntryCount > 0))
        {
            auto pSpecializatonInfo = pShaderInfo->pSpecializatonInfo;
            Md5::Update(pChecksumCtx, pSpecializatonInfo->mapEntryCount);
            Md5::Update(pChecksumCtx,
                        pSpecializatonInfo->pMapEntries,
                        sizeof(VkSpecializationMapEntry) * pSpecializatonInfo->mapEntryCount);
            Md5::Update(pChecksumCtx, pSpecializatonInfo->dataSize);
            Md5::Update(pChecksumCtx, pSpecializatonInfo->pData, pSpecializatonInfo->dataSize);
        }

        if (pShaderInfo->descriptorRangeValueCount > 0)
        {
            Md5::Update(pChecksumCtx, pShaderInfo->descriptorRangeValueCount);
            for (uint32_t i = 0; i < pShaderInfo->descriptorRangeValueCount; ++i)
            {
                auto pDescriptorRangeValue = &pShaderInfo->pDescriptorRangeValues[i];
                Md5::Update(pChecksumCtx, pDescriptorRangeValue->type);
                Md5::Update(pChecksumCtx, pDescriptorRangeValue->set);
                Md5::Update(pChecksumCtx, pDescriptorRangeValue->binding);
                Md5::Update(pChecksumCtx, pDescriptorRangeValue->arraySize);

                // TODO: We should query descriptor size from patch
                const uint32_t DescriptorSize = 16;
                LLPC_ASSERT(pDescriptorRangeValue->type == ResourceMappingNodeType::DescriptorSampler);
                Md5::Update(pChecksumCtx,
                            pDescriptorRangeValue->pValue,
                            pDescriptorRangeValue->arraySize * DescriptorSize);
            }
        }

        if (pShaderInfo->userDataNodeCount > 0)
        {
            for (uint32_t i = 0; i < pShaderInfo->userDataNodeCount; ++i)
            {
                auto pUserDataNode = &pShaderInfo->pUserDataNodes[i];
                UpdateHashForResourceMappingNode(pUserDataNode, pChecksumCtx);
            }
        }
    }
}

// =====================================================================================================================
// Updates MD5 hash code context for resource mapping node.
//
// NOTE: This function will be called recusively if node's type is "DescriptorTableVaPtr"
void Compiler::UpdateHashForResourceMappingNode(
    const ResourceMappingNode* pUserDataNode,    // [in] Resource mapping node
    Md5::Context*              pChecksumCtx      // [in,out] MD5 hash code context
    ) const
{
    Md5::Update(pChecksumCtx, pUserDataNode->type);
    Md5::Update(pChecksumCtx, pUserDataNode->sizeInDwords);
    Md5::Update(pChecksumCtx, pUserDataNode->offsetInDwords);

    switch (pUserDataNode->type)
    {
    case ResourceMappingNodeType::DescriptorResource:
    case ResourceMappingNodeType::DescriptorSampler:
    case ResourceMappingNodeType::DescriptorCombinedTexture:
    case ResourceMappingNodeType::DescriptorTexelBuffer:
    case ResourceMappingNodeType::DescriptorBuffer:
    case ResourceMappingNodeType::DescriptorFmask:
    case ResourceMappingNodeType::DescriptorBufferCompact:
        {
            Md5::Update(pChecksumCtx, pUserDataNode->srdRange);
            break;
        }
    case ResourceMappingNodeType::DescriptorTableVaPtr:
        {
            for (uint32_t i = 0; i < pUserDataNode->tablePtr.nodeCount; ++i)
            {
                UpdateHashForResourceMappingNode(&pUserDataNode->tablePtr.pNext[i],pChecksumCtx);
            }
            break;
        }
    case ResourceMappingNodeType::IndirectUserDataVaPtr:
        {
            Md5::Update(pChecksumCtx, pUserDataNode->userDataPtr);
            break;
        }
    case ResourceMappingNodeType::PushConst:
        {
            // Do nothing for push constant
            break;
        }
    default:
        {
            LLPC_NEVER_CALLED();
            break;
        }
    }
}

// =====================================================================================================================
// Checks whether fields in pipeline shader info are valid.
Result Compiler::ValidatePipelineShaderInfo(
    ShaderStage               shaderStage,    // Shader stage
    const PipelineShaderInfo* pShaderInfo     // [in] Pipeline shader info
    ) const
{
    Result result = Result::Success;
    const ShaderModuleData* pModuleData = reinterpret_cast<const ShaderModuleData*>(pShaderInfo->pModuleData);
    if (pModuleData != nullptr)
    {
        if (pModuleData->binType == BinaryType::Spirv)
        {
            auto pSpirvBin = &pModuleData->binCode;
            if (pShaderInfo->pEntryTarget != nullptr)
            {
                uint32_t stageMask = GetStageMaskFromSpirvBinary(pSpirvBin, pShaderInfo->pEntryTarget);

                if ((stageMask & ShaderStageToMask(shaderStage)) == 0)
                {
                    LLPC_ERRS("Fail to find entry-point " << pShaderInfo->pEntryTarget << " for " <<
                              GetShaderStageName(shaderStage) << " shader\n");
                    result = Result::ErrorInvalidShader;
                }
            }
            else
            {
                LLPC_ERRS("Missing entry-point name for " << GetShaderStageName(shaderStage) << " shader\n");
                result = Result::ErrorInvalidShader;
            }

            if (cl::DisableWipFeatures)
            {
                if (VerifySpirvBinary(pSpirvBin) != Result::Success)
                {
                    LLPC_ERRS("Unsupported op codes are found in " << GetShaderStageName(shaderStage) << " shader\n");
                    result = Result::Unsupported;
                }
            }
        }
        else if (pModuleData->binType == BinaryType::LlvmBc)
        {
            // Do nothing if input is LLVM IR
        }
        else
        {
            LLPC_ERRS("Invalid shader binary type for " << GetShaderStageName(shaderStage) << " shader\n");
            result = Result::ErrorInvalidShader;
        }
    }

    return result;
}

// =====================================================================================================================
// Builds LLVM module for null fragment shader.
Result Compiler::BuildNullFs(
    Context*                 pContext,     // [in] LLPC context
    std::unique_ptr<Module>& pNullFsModule // [out] Null fragment shader module
    ) const
{
    Result result = Result::Success;

    auto pMemBuffer = MemoryBuffer::getMemBuffer(StringRef(reinterpret_cast<const char*>(&GlslNullFsEmuLib[0]),
                                                 sizeof(GlslNullFsEmuLib)),
                                                 "",
                                                 false);

    Expected<std::unique_ptr<Module>> moduleOrErr = getLazyBitcodeModule(pMemBuffer->getMemBufferRef(), *pContext);
    if (!moduleOrErr)
    {
        Error error = moduleOrErr.takeError();
        LLPC_ERRS("Fails to load LLVM bitcode (null fragment shader)\n");
        result = Result::ErrorInvalidShader;
    }
    else
    {
        if (llvm::Error errCode = (*moduleOrErr)->materializeAll())
        {
            LLPC_ERRS("Fails to materialize (null fragment shader)\n");
            result = Result::ErrorInvalidShader;
        }
    }

    if (result == Result::Success)
    {
        pNullFsModule = std::move(*moduleOrErr);
        GraphicsContext* pGraphicsContext = static_cast<GraphicsContext*>(pContext->GetPipelineContext());
        pGraphicsContext->InitShaderInfoForNullFs();
    }

    return result;
}

// =====================================================================================================================
// Builds LLVM module for copy shader and generates GPU ISA codes accordingly.
Result Compiler::BuildCopyShader(
    Context*         pContext,      // [in] LLPC context
    ElfPackage*      pCopyShaderElf // [out] ELF package for copy shader
    ) const
{
    CopyShader copyShader(pContext);
    return copyShader.Run(pCopyShaderElf);
}

// =====================================================================================================================
// Creates shader cache object with the requested properties.
Result Compiler::CreateShaderCache(
    const ShaderCacheCreateInfo* pCreateInfo,    // [in] Shader cache create info
    IShaderCache**               ppShaderCache)  // [out] Shader cache object
{
    Result result = Result::Success;

    ShaderCacheAuxCreateInfo auxCreateInfo = {};
    auxCreateInfo.shaderCacheMode = ShaderCacheMode::ShaderCacheEnableRuntime;
    auxCreateInfo.gfxIp           = m_gfxIp;

    ShaderCache* pShaderCache = new ShaderCache();

    if (pShaderCache != nullptr)
    {
        result = pShaderCache->Init(pCreateInfo, &auxCreateInfo);
        if (result != Result::Success)
        {
            pShaderCache->Destroy();
            pShaderCache = nullptr;
        }
    }
    else
    {
        result = Result::ErrorOutOfMemory;
    }

    *ppShaderCache = pShaderCache;
    return result;
}

// =====================================================================================================================
// Initialize GPU property.
void Compiler::InitGpuProperty()
{
    // Initial settings (could be adjusted later according to graphics IP version info)
    m_gpuProperty.waveSize = 64;
    m_gpuProperty.ldsSizePerCu = (m_gfxIp.major > 6) ? 65536 : 32768;
    m_gpuProperty.ldsSizePerThreadGroup = 32 * 1024;
    m_gpuProperty.numShaderEngines = 4;

    //TODO: Setup gsPrimBufferDepth from hardware config option, will be done in another change.
    m_gpuProperty.gsPrimBufferDepth = 0x100;

    m_gpuProperty.maxUserDataCount = (m_gfxIp.major >= 9) ? 32 : 16;

    if (m_gfxIp.major <= 8)
    {
        // TODO: Accept gsOnChipDefaultPrimsPerSubgroup from panel option
        m_gpuProperty.gsOnChipDefaultPrimsPerSubgroup   = 64;
        // TODO: Accept gsOnChipDefaultLdsSizePerSubgroup from panel option
        m_gpuProperty.gsOnChipDefaultLdsSizePerSubgroup = 8192;
        m_gpuProperty.ldsSizeDwordGranularity           = 128;
    }

    if (m_gfxIp.major == 6)
    {
        m_gpuProperty.numShaderEngines = (m_gfxIp.stepping == 0) ? 2 : 1;
    }
    else if (m_gfxIp.major == 7)
    {
        if (m_gfxIp.stepping == 0)
        {
            m_gpuProperty.numShaderEngines = 2;
        }
        else if (m_gfxIp.stepping == 1)
        {
            m_gpuProperty.numShaderEngines = 4;
        }
        else
        {
            m_gpuProperty.numShaderEngines = 1;
        }
    }
    else if (m_gfxIp.major == 8)
    {
        // TODO: polaris11 and polaris12 is 2, but we can't identify them by GFX IP now.
        m_gpuProperty.numShaderEngines = ((m_gfxIp.minor == 1) || (m_gfxIp.stepping <= 1)) ? 1 : 4;
    }
    else if (m_gfxIp.major == 9)
    {
#ifdef LLPC_BUILD_GFX9
        if (m_gfxIp.stepping == 0)
        {
            m_gpuProperty.numShaderEngines = 4;
        }
#else
        LLPC_NOT_IMPLEMENTED();
#endif
    }
    else
    {
        LLPC_NOT_IMPLEMENTED();
    }
}

// =====================================================================================================================
// Acquires a free context from context pool.
Context* Compiler::AcquireContext()
{
    Context* pFreeContext = nullptr;

    MutexGuard lock(m_contextPoolMutex);

    // Try to find a free context from pool first
    for (auto pContext : m_contextPool)
    {
        if (pContext->IsInUse() == false)
        {
            pFreeContext = pContext;
            pFreeContext->SetInUse(true);
        }
    }

    if (pFreeContext == nullptr)
    {
        // Create a new one if we fail to find an available one
        pFreeContext = new Context(m_gfxIp);
        pFreeContext->SetInUse(true);
        m_contextPool.push_back(pFreeContext);
    }

    LLPC_ASSERT(pFreeContext != nullptr);
    return pFreeContext;
}

// =====================================================================================================================
// Releases LLPC context.
void Compiler::ReleaseContext(
    Context* pContext)    // [in] LLPC context
{
    MutexGuard lock(m_contextPoolMutex);
    pContext->SetInUse(false);
}

// =====================================================================================================================
// Dumps the result of time profile.
void Compiler::DumpTimeProfilingResult(
    const Md5::Hash* pHash)   // [in] Pipeline hash
{
    int64_t fre = {};
    fre = GetPerfFrequency();
    char shaderHash[64] = {};
    int32_t length = snprintf(shaderHash, 64, "0x%016llX", Md5::Compact64(pHash));
    // NOTE: To get correct profile result, we have to disable general info output, so we have to output time profile
    // result to LLPC_ERRS
    LLPC_ERRS("Time Profiling Results(General): "
              << "Hash = " << shaderHash << ", "
              << "Translate = " << float(g_timeProfileResult.translateTime) / fre << ", "
              << "SPIR-V Lower = " << float(g_timeProfileResult.lowerTime) / fre << ", "
              << "LLVM Patch = " << float(g_timeProfileResult.patchTime) / fre << ", "
              << "Code Generation = " << float(g_timeProfileResult.codeGenTime) / fre << "\n");

    LLPC_ERRS("Time Profiling Results(Special): "
              << "SPIR-V Lower (Optimization) = " << float(g_timeProfileResult.lowerOptTime) / fre << ", "
              << "LLVM Patch (Lib Link) = " << float(g_timeProfileResult.patchLinkTime) / fre << "\n");
}

// =====================================================================================================================
// Dumps graphics pipeline.
void Compiler::DumpGraphicsPipeline(
    const GraphicsPipelineBuildInfo* pPipelineInfo // [in] Info to build this graphics pipeline
    ) const
{
    auto hash = GenerateHashForGraphicsPipeline(pPipelineInfo);
    auto pPipelineDumpFile = CreatePipelineDumpFile(cl::PipelineDumpDir.c_str(), nullptr, pPipelineInfo, &hash);
    if (pPipelineDumpFile != nullptr)
    {
        DumpGraphicsPipelineInfo(pPipelineDumpFile, pPipelineInfo);
        DestroyPipelineDumpFile(pPipelineDumpFile);
    }
}

// =====================================================================================================================
// Dumps compute pipeline.
void Compiler::DumpComputePipeline(
    const ComputePipelineBuildInfo* pPipelineInfo // [in] Info to build this compute pipeline
    ) const
{
    auto hash = GenerateHashForComputePipeline(pPipelineInfo);
    auto pPipelineDumpFile = CreatePipelineDumpFile(cl::PipelineDumpDir.c_str(), pPipelineInfo, nullptr, &hash);
    if (pPipelineDumpFile != nullptr)
    {
        DumpComputePipelineInfo(pPipelineDumpFile, pPipelineInfo);
        DestroyPipelineDumpFile(pPipelineDumpFile);
    }
}

} // Llpc
