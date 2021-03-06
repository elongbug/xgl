##
 ###############################################################################
 #
 # Copyright (c) 2017 Advanced Micro Devices, Inc. All rights reserved.
 #
 # Permission is hereby granted, free of charge, to any person obtaining a copy
 # of this software and associated documentation files (the "Software"), to deal
 # in the Software without restriction, including without limitation the rights
 # to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 # copies of the Software, and to permit persons to whom the Software is
 # furnished to do so, subject to the following conditions:
 #
 # The above copyright notice and this permission notice shall be included in
 # all copies or substantial portions of the Software.
 #
 # THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 # IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 # FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
 # AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 # LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 # OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 # THE SOFTWARE.
 ##############################################################################/


#**********************************************************************************************************************
# @file  genGlslEmuLib.py
# @brief LLPC python script file: generates LLVM-IR library to emulate GLSL operations.
#**********************************************************************************************************************

import binascii
import os
import subprocess
import sys

import genGlslArithOpEmuCode
import genGlslImageOpEmuCode

# Change working directory (/llpc/patch/generate/)
os.chdir(os.path.split(sys.argv[0])[0] + "/..")

LLVM_AS_DIR = sys.argv[1]
LLVM_LINK_DIR = sys.argv[2]
OS_TYPE = sys.argv[3]

# LLVM utility binaries
LLVM_AS = LLVM_AS_DIR + "llvm-as"
LLVM_LINK = LLVM_LINK_DIR + "llvm-link"

# Cleanup, remove those auto-generated files
print("*******************************************************************************")
print("                              Pre-Compile Cleanup                              ")
print("*******************************************************************************")

for f in os.listdir("./"):
    if f.startswith("g_") or  f.endswith(".bc") or f.endswith(".lib"): # Common library
        print(">>>  (LL-clean) remove " + f)
        os.remove(f)
    elif os.path.isdir(f) and f.startswith("gfx"): # GFX library
        gfx = f
        os.chdir("./" + gfx)

        for f in os.listdir("./"):
            if f.startswith("g_") or  f.endswith(".bc") or f.endswith(".lib"):
                print(">>>  (LL-clean) remove " + gfx + "/" + f)
                os.remove(f)

        os.chdir("./..")

print("")

# =====================================================================================================================
# Generate GFX-independent LLVM emulation library
# =====================================================================================================================

# Generate .ll files
print("*******************************************************************************")
print("                 Generate LLVM Emulation IR (GLSL Arithmetic)                  ")
print("*******************************************************************************")
genGlslArithOpEmuCode.main("./script/genGlslArithOpEmuCode.txt", "g_glslArithOpEmu.ll", "std32")
genGlslArithOpEmuCode.main("./script/genGlslArithOpEmuCodeF64.txt", "g_glslArithOpEmuF64.ll", "float64")

# Generate .lib file
print("*******************************************************************************")
print("                   Generate LLVM Emulation Library (Common)                    ")
print("*******************************************************************************")

# Assemble .ll files to .bc files
for f in os.listdir("./"):
    if f.endswith(".ll"):
        cmd = LLVM_AS + " " + f
        print(">>>  (LL-as) " + cmd)
        if OS_TYPE == "win" :
            subprocess.call(cmd)
        else :
            subprocess.call(cmd, shell = True)

# Link special emulation .bc files to libraries (null fragment shader, copy shader)
SPECIAL_EMUS = ["NullFs", "CopyShader"]
for feature in SPECIAL_EMUS:
    # Search for the .bc file
    bcFile = ""
    for f in os.listdir("./"):
        if f.startswith("glsl" + feature) and f.endswith("Emu.bc"):
            bcFile = f

    if bcFile == "": # Not found
        continue

    # Link .bc files to .lib file
    libFile = "glsl" + feature + "Emu.lib"
    cmd = LLVM_LINK + " -o=" + libFile + " " + bcFile
    print(">>>  (LL-link) " + cmd)
    if OS_TYPE == "win" :
        subprocess.call(cmd)
    else :
        subprocess.call(cmd, shell = True)

    # Convert .lib file to a hex file
    hFile = "g_llpcGlsl" + feature + "EmuLib.h"
    print(">>>  (LL-bin2hex) " + libFile + "  ==>  " + hFile)
    fBin = open(libFile, "rb")
    binData = fBin.read()
    fBin.close()

    hexData = binascii.hexlify(binData).decode()
    fHex = open(hFile, "w")
    hexText = ""
    i = 0
    while i < len(hexData):
        hexText += "0x"
        hexText += hexData[i]
        hexText += hexData[i + 1]
        i += 2
        if (i != len(hexData)):
            hexText += ", "
        if (i % 32 == 0):
            hexText += "\n"
    fHex.write(hexText)
    fHex.close()

    # Cleanup, remove those temporary files
    print(">>>  (LL-clean) remove " + bcFile)
    os.remove(bcFile)
    print(">>>  (LL-clean) remove " + libFile)
    os.remove(libFile)

# Link general emulation .bc files to libraries (GLSL operations and built-ins)
# Collect .bc files
bcFiles = ""
for f in os.listdir("./"):
    if f.endswith(".bc"):
        bcFiles += f + " "

# Link .bc files to .lib file
libFile = "glslEmu.lib"
cmd = LLVM_LINK + " -o=" + libFile + " " + bcFiles
print(">>>  (LL-link) " + cmd)
if OS_TYPE == "win" :
    subprocess.call(cmd)
else :
    subprocess.call(cmd, shell = True)

# Convert .lib file to a hex file
hFile = "g_llpcGlslEmuLib.h"
print(">>>  (LL-bin2hex) " + libFile + "  ==>  " + hFile)
fBin = open(libFile, "rb")
binData = fBin.read()
fBin.close()

hexData = binascii.hexlify(binData).decode()
fHex = open(hFile, "w")
hexText = ""
i = 0
while i < len(hexData):
    hexText += "0x"
    hexText += hexData[i]
    hexText += hexData[i + 1]
    i += 2
    if (i != len(hexData)):
        hexText += ", "
    if (i % 32 == 0):
        hexText += "\n"
fHex.write(hexText)
fHex.close()

# Cleanup, remove those temporary files
for f in bcFiles.split():
    print(">>>  (LL-clean) remove " + f);
    os.remove(f)
print(">>>  (LL-clean) remove " + libFile);
os.remove(libFile)

print("")

# =====================================================================================================================
# Generate GFX-dependent LLVM emulation library
# =====================================================================================================================

# Assemble .ll files to .bc files and link emulation .bc files to libraries
GFX_EMUS = ["gfx6", "gfx9"]
for gfx in GFX_EMUS:
    print("*******************************************************************************")
    print("                   Generate LLVM Emulation IR (GLSL Image) for %s             "%(gfx.upper()))
    print("*******************************************************************************")
    genGlslImageOpEmuCode.main("./script/genGlslImageOpEmuCode.txt", "%s/g_glslImageOpEmu.ll"%(gfx), gfx)

    print("*******************************************************************************")
    print("                    Generate LLVM Emulation Library (%s)                     "%(gfx.upper()))
    print("*******************************************************************************")

    # Assemble .ll files to .bc files
    for f in os.listdir(gfx):
        if f.endswith(".ll"):
            cmd = LLVM_AS + " " + gfx + "/" + f
            print(">>>  (LL-as) " + cmd)
            if OS_TYPE == "win" :
                subprocess.call(cmd)
            else :
                subprocess.call(cmd, shell = True)

    # Search for the .bc file
    bcFiles = ""
    for f in os.listdir(gfx):
        if f.endswith(".bc"):
            bcFiles += gfx + "/" + f + " "

    # Link .bc files to .lib file
    libFile = gfx + "/glslEmu" + gfx.capitalize() + ".lib"
    cmd = LLVM_LINK + " -o=" + libFile + " " + bcFiles
    print(">>>  (LL-link) " + cmd)
    if OS_TYPE == "win" :
        subprocess.call(cmd)
    else :
        subprocess.call(cmd, shell = True)

    # Convert .lib file to a hex file
    hFile = gfx + "/g_llpcGlslEmuLib" + gfx.capitalize() + ".h"
    print(">>>  (LL-bin2hex) " + libFile + "  ==>  " + hFile)
    fBin = open(libFile, "rb")
    binData = fBin.read()
    fBin.close()

    hexData = binascii.hexlify(binData).decode()
    fHex = open(hFile, "w")
    hexText = ""
    i = 0
    while i < len(hexData):
        hexText += "0x"
        hexText += hexData[i]
        hexText += hexData[i + 1]
        i += 2
        if (i != len(hexData)):
            hexText += ", "
        if (i % 32 == 0):
            hexText += "\n"
    fHex.write(hexText)
    fHex.close()

    # Cleanup, remove those temporary files
    for f in bcFiles.split():
        print(">>>  (LL-clean) remove " + f);
        os.remove(f)
    print(">>>  (LL-clean) remove " + libFile);
    os.remove(libFile)

    print("")

