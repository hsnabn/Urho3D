//
// Copyright (c) 2008-2014 the Urho3D project.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//

#include "Precompiled.h"
#include "Context.h"
#include "Deserializer.h"
#include "FileSystem.h"
#include "Graphics.h"
#include "Log.h"
#include "Profiler.h"
#include "ResourceCache.h"
#include "Shader.h"
#include "ShaderVariation.h"
#include "XMLFile.h"

#include "DebugNew.h"

namespace Urho3D
{

Shader::Shader(Context* context) :
    Resource(context)
{
}

Shader::~Shader()
{
    ResourceCache* cache = GetSubsystem<ResourceCache>();
    if (cache)
        cache->ResetDependencies(this);
}

void Shader::RegisterObject(Context* context)
{
    context->RegisterFactory<Shader>();
}

bool Shader::Load(Deserializer& source)
{
    PROFILE(LoadShader);
    
    Graphics* graphics = GetSubsystem<Graphics>();
    if (!graphics)
        return false;
    
    // Load the shader source code and resolve any includes
    String shaderCode;
    if (!ProcessSource(shaderCode, source))
        return false;
    
    // Customize the vertex & pixel shader source code to not include the unnecessary shader,
    // and on OpenGL, rename either VS() or PS() to main()
    #ifdef USE_OPENGL
    vsSourceCode_ = shaderCode;
    vsSourceCode_.Replace("void VS(", "void main(");
    vsSourceCode_.Replace("void PS(", "/* void PS(");
    vsSourceCode_ += "*/\n";
    
    psSourceCode_ = shaderCode;
    psSourceCode_.Replace("attribute ", "// attribute ");
    psSourceCode_.Replace("void VS(", "/* void VS(");
    psSourceCode_.Replace("void PS(", "*/\nvoid main(");
    #else
    vsSourceCode_ = shaderCode;
    vsSourceCode_.Replace("void PS(", "/* void PS(");
    vsSourceCode_ += "*/\n";
    
    psSourceCode_ = shaderCode;
    psSourceCode_.Replace("void VS(", "/* void VS(");
    psSourceCode_.Replace("void PS(", "*/\nvoid PS(");
    #endif
    
    // If variations had already been created, release them and require recompile
    for (HashMap<StringHash, SharedPtr<ShaderVariation> >::Iterator i = vsVariations_.Begin(); i != vsVariations_.End(); ++i)
        i->second_->Release();
    for (HashMap<StringHash, SharedPtr<ShaderVariation> >::Iterator i = psVariations_.Begin(); i != psVariations_.End(); ++i)
        i->second_->Release();
    
    SetMemoryUse(sizeof(Shader) + vsSourceCode_.Length() + psSourceCode_.Length() + (vsVariations_.Size() + psVariations_.Size()) *
        sizeof(ShaderVariation));
    
    return true;
}

ShaderVariation* Shader::GetVariation(ShaderType type, const String& definesIn)
{
    String defines = SanitateDefines(definesIn);
    StringHash definesHash(defines);
    
    if (type == VS)
    {
        HashMap<StringHash, SharedPtr<ShaderVariation> >::Iterator i = vsVariations_.Find(definesHash);
        // Create the shader variation now if not created yet
        if (i == vsVariations_.End())
        {
            i = vsVariations_.Insert(MakePair(definesHash, SharedPtr<ShaderVariation>(new ShaderVariation(this, VS))));
            String path, fileName, extension;
            SplitPath(GetName(), path, fileName, extension);
            String fullName = path + fileName + "_" + defines.Replaced(' ', '_');
            if (fullName.EndsWith("_"))
                fullName.Resize(fullName.Length() - 1);
            i->second_->SetName(fullName);
            i->second_->SetDefines(defines);
            
            SetMemoryUse(GetMemoryUse() + sizeof(ShaderVariation));
        }
        
        return i->second_;
    }
    else
    {
        HashMap<StringHash, SharedPtr<ShaderVariation> >::Iterator i = psVariations_.Find(definesHash);
        // Create the shader variation now if not created yet
        if (i == psVariations_.End())
        {
            i = psVariations_.Insert(MakePair(definesHash, SharedPtr<ShaderVariation>(new ShaderVariation(this, PS))));
            String path, fileName, extension;
            SplitPath(GetName(), path, fileName, extension);
            String fullName = path + fileName + "_" + defines.Replaced(' ', '_');
            if (fullName.EndsWith("_"))
                fullName.Resize(fullName.Length() - 1);
            i->second_->SetName(fullName);
            i->second_->SetDefines(defines);
            
            SetMemoryUse(GetMemoryUse() + sizeof(ShaderVariation));
        }
        
        return i->second_;
    }
}

bool Shader::ProcessSource(String& code, Deserializer& source)
{
    ResourceCache* cache = GetSubsystem<ResourceCache>();
    if (!cache)
        return false;
    
    // Store resource dependencies for includes so that we know to reload if any of them changes
    if (source.GetName() != GetName())
        cache->StoreResourceDependency(this, source.GetName());
    
    while (!source.IsEof())
    {
        String line = source.ReadLine();
        
        if (line.StartsWith("#include"))
        {
            String includeFileName = GetPath(source.GetName()) + line.Substring(9).Replaced("\"", "").Trimmed();
            
            SharedPtr<File> includeFile = cache->GetFile(includeFileName);
            if (!includeFile)
                return false;
            
            // Add the include file into the current code recursively
            if (!ProcessSource(code, *includeFile))
                return false;
        }
        else
        {
            code += line;
            code += "\n";
        }
    }

    // Finally insert an empty line to mark the space between files
    code += "\n";
    
    return true;
}

String Shader::SanitateDefines(const String& definesIn)
{
    String ret;
    ret.Reserve(definesIn.Length());
    
    unsigned numSpaces = 0;
    unsigned start = 0, end = definesIn.Length();
    
    // Trim spaces from start & begin. Do not use String::Trimmed() as we also need to trim spaces from the middle
    for (unsigned i = 0; i < definesIn.Length(); ++i)
    {
        if (definesIn[i] != ' ')
        {
            start = i;
            break;
        }
    }
    for (unsigned i = definesIn.Length() - 1; i < definesIn.Length(); --i)
    {
        if (definesIn[i] != ' ')
        {
            end = i + 1;
            break;
        }
    }
    for (unsigned i = start; i < end; ++i)
    {
        // Ensure only one space in a row
        if (definesIn[i] == ' ')
            ++numSpaces;
        else
            numSpaces = 0;
        
        if (numSpaces <= 1)
            ret += definesIn[i];
    }
    
    return ret;
}

}
