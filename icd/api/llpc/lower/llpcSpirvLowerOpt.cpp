/*
 *******************************************************************************
 *
 * Copyright (c) 2017 Advanced Micro Devices, Inc. All rights reserved.
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
 * @file  llpcSpirvLowerOpt.cpp
 * @brief LLPC source file: contains implementation of class Llpc::SpirvLowerOpt.
 ***********************************************************************************************************************
 */
#define DEBUG_TYPE "llpc-spirv-lower-opt"

#include "llvm/IR/Instructions.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include "SPIRVInternal.h"
#include "llpcSpirvLowerOpt.h"

using namespace llvm;
using namespace SPIRV;
using namespace Llpc;

namespace Llpc
{

extern TimeProfileResult g_timeProfileResult;

// =====================================================================================================================
// Initializes static members.
char SpirvLowerOpt::ID = 0;

// =====================================================================================================================
SpirvLowerOpt::SpirvLowerOpt()
    :
    SpirvLower(ID)
{
    initializeSpirvLowerOptPass(*PassRegistry::getPassRegistry());
}

// =====================================================================================================================
// Executes this SPIR-V lowering pass on the specified LLVM module.
bool SpirvLowerOpt::runOnModule(
    Module& module)  // [in,out] LLVM module to be run on
{
    TimeProfiler timeProfiler(&g_timeProfileResult.lowerOptTime);

    bool changed = false;

    DEBUG(dbgs() << "Run the pass Spirv-Lower-Opt\n");

    SpirvLower::Init(&module);

    // Invoke optimization
    changed = OptimizeModule(&module);

    DEBUG(dbgs() << "After the pass Spirv-Lower-Opt: " << module);

    std::string errMsg;
    raw_string_ostream errStream(errMsg);
    if (verifyModule(module, &errStream))
    {
        LLPC_ERRS("Fails to verify module (" DEBUG_TYPE "): " << errStream.str() << "\n");
    }

    return changed;
}

} // Llpc

// =====================================================================================================================
// Initializes the pass of general optimizations for SPIR-V lowering.
INITIALIZE_PASS(SpirvLowerOpt, "spirv-lower-opt",
                "Lower SPIR-V with general optimizations", false, false)
