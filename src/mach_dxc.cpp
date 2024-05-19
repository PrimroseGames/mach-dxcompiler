// TODO: investigate if we can eliminate this for Windows builds
#ifdef _WIN32
    #ifdef _MSC_VER
        #define __C89_NAMELESS
        #define __C89_NAMELESSUNIONNAME
        #define WIN32_LEAN_AND_MEAN
        #include <windows.h>
        #include <wrl/client.h>
        #define CComPtr Microsoft::WRL::ComPtr
    #else // _MSC_VER
        #include <windows.h>
        #include <wrl/client.h>
    #endif // _MSC_VER
#endif // _WIN32

// Avoid __declspec(dllimport) since dxcompiler is static.
#define DXC_API_IMPORT
#include <dxcapi.h>
#include <cassert>
#include <stddef.h>

#include "mach_dxc.h"

#ifdef __cplusplus
extern "C" {
#endif


// Mach change start: static dxcompiler/dxil
BOOL MachDxcompilerInvokeDllMain();
void MachDxcompilerInvokeDllShutdown();

static CComPtr<IDxcUtils> pUtils;

class CustomIncludeHandler : public IDxcIncludeHandler
{
    MachDxcIncludeHandlerCallback callback;

public:
    HRESULT STDMETHODCALLTYPE LoadSource(_In_ LPCWSTR pFilename, _COM_Outptr_result_maybenull_ IDxcBlob** ppIncludeSource) override
    {
        // LPCWSTR to char16_t*
        auto pFileNameWideStr = std::wstring(pFilename);

        auto pFilenameStr = std::basic_string<char16_t>(pFileNameWideStr.begin(), pFileNameWideStr.end());

        char16_t const* callbackResultStr;
        auto hr = callback(pFilenameStr.c_str(), &callbackResultStr);

        if (hr != 0) {
            printf("callback failed");
            return E_FAIL;
        }

        auto str16 = std::basic_string<char16_t>(callbackResultStr);

        if (str16.size() == 0) {
            printf("Include file not found: %ls\n", pFileNameWideStr.c_str());
        }

        CComPtr<IDxcBlobEncoding> pEncoding;
    
        pUtils->CreateBlobFromPinned(str16.c_str(), str16.size() * sizeof(wchar_t), DXC_CP_UTF16, &pEncoding.p);
        *ppIncludeSource = pEncoding.Detach();

        return S_OK;

        /*
        ComPtr<IDxcBlobEncoding> pEncoding;
        std::string path = Paths::Normalize(UNICODE_TO_MULTIBYTE(pFilename));
        if (IncludedFiles.find(path) != IncludedFiles.end())
        {
            // Return empty string blob if this file has been included before
            static const char nullStr[] = " ";
            pUtils->CreateBlobFromPinned(nullStr, ARRAYSIZE(nullStr), DXC_CP_ACP, pEncoding.GetAddressOf());
            *ppIncludeSource = pEncoding.Detach();
            return S_OK;
        }

        HRESULT hr = pUtils->LoadFile(pFilename, nullptr, pEncoding.GetAddressOf());
        if (SUCCEEDED(hr))
        {
            IncludedFiles.insert(path);
            *ppIncludeSource = pEncoding.Detach();
        }
        return hr;*/
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,
                                           void **ppvObject) override {
        return E_NOINTERFACE;
    }
    
    ULONG STDMETHODCALLTYPE AddRef(void) override {	return 0; }
    ULONG STDMETHODCALLTYPE Release(void) override { return 0; }

    void SetCallback(MachDxcIncludeHandlerCallback callback) {
        this->callback = callback;
    }
};

CComPtr<IDxcIncludeHandler> includeHandler = nullptr;

//----------------
// MachDxcCompiler
//----------------

MACH_EXPORT MachDxcCompiler machDxcInit() {
    MachDxcompilerInvokeDllMain();
    CComPtr<IDxcCompiler3> dxcInstance;
    HRESULT hr = DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&dxcInstance));
    assert(SUCCEEDED(hr));


    hr = DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&pUtils));
    assert(SUCCEEDED(hr));

    includeHandler = new CustomIncludeHandler();

    return reinterpret_cast<MachDxcCompiler>(dxcInstance.Detach());
}

MACH_EXPORT void machDxcDeinit(MachDxcCompiler compiler) {
    CComPtr<IDxcCompiler3> dxcInstance = CComPtr(reinterpret_cast<IDxcCompiler3*>(compiler));
    dxcInstance.Release();
    MachDxcompilerInvokeDllShutdown();
}

//---------------------
// MachDxcCompileResult
//---------------------
MACH_EXPORT MachDxcCompileResult machDxcCompile(
    MachDxcCompiler compiler,
    char const* code,
    size_t code_len,
    char const* const* args,
    size_t args_len,
    MachDxcIncludeHandlerCallback includeHandlerCallback
) {
    CComPtr<IDxcCompiler3> dxcInstance = CComPtr(reinterpret_cast<IDxcCompiler3*>(compiler));
    CComPtr<CustomIncludeHandler> includeHandler = CComPtr(reinterpret_cast<CustomIncludeHandler*>(::includeHandler.p));
    
    includeHandler->SetCallback(includeHandlerCallback);

    CComPtr<IDxcUtils> pUtils;
    DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&pUtils));
    CComPtr<IDxcBlobEncoding> pSource;
    pUtils->CreateBlob(code, code_len, CP_UTF8, &pSource);

    DxcBuffer sourceBuffer;
    sourceBuffer.Ptr = pSource->GetBufferPointer();
    sourceBuffer.Size = pSource->GetBufferSize();
    sourceBuffer.Encoding = 0;

    // We have args in char form, but dxcInstance->Compile expects wchar_t form.
    LPCWSTR* arguments = (LPCWSTR*)malloc(sizeof(LPCWSTR) * args_len);
    wchar_t* wtext_buf = (wchar_t*)malloc(4096);
    wchar_t* wtext_cursor = wtext_buf;
    assert(arguments);
    assert(wtext_buf);

    for (int i=0; i < args_len; i++) {
        size_t available = 4096 / sizeof(wchar_t) - (wtext_cursor - wtext_buf);
        size_t written = std::mbstowcs(wtext_cursor, args[i], available);
        arguments[i] = wtext_cursor;
        wtext_cursor += written + 1;
    }

    CComPtr<IDxcResult> pCompileResult;
    HRESULT hr = dxcInstance->Compile(
        &sourceBuffer,
        arguments,
        (uint32_t)args_len,
        includeHandlerCallback != nullptr ? includeHandler : nullptr,
        IID_PPV_ARGS(&pCompileResult)
    );
    assert(SUCCEEDED(hr));
    free(arguments);
    free(wtext_buf);
    return reinterpret_cast<MachDxcCompileResult>(pCompileResult.Detach());
}

MACH_EXPORT MachDxcCompileError machDxcCompileResultGetError(MachDxcCompileResult err) {
    CComPtr<IDxcResult> pCompileResult = CComPtr(reinterpret_cast<IDxcResult*>(err));
    CComPtr<IDxcBlobUtf8> pErrors;
    pCompileResult->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&pErrors), nullptr);
    if (pErrors && pErrors->GetStringLength() > 0) {
        return reinterpret_cast<MachDxcCompileError>(pErrors.Detach());
    }
    return nullptr;
}

MACH_EXPORT MachDxcCompileObject machDxcCompileResultGetObject(MachDxcCompileResult err) {
    CComPtr<IDxcResult> pCompileResult = CComPtr(reinterpret_cast<IDxcResult*>(err));
    CComPtr<IDxcBlob> pObject;
    pCompileResult->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&pObject), nullptr);
    if (pObject && pObject->GetBufferSize() > 0) {
        return reinterpret_cast<MachDxcCompileObject>(pObject.Detach());
    }
    return nullptr;
}

MACH_EXPORT void machDxcCompileResultDeinit(MachDxcCompileResult err) {
    CComPtr<IDxcResult> pCompileResult = CComPtr(reinterpret_cast<IDxcResult*>(err));
    pCompileResult.Release();
}

//---------------------
// MachDxcCompileObject
//---------------------
MACH_EXPORT char const* machDxcCompileObjectGetBytes(MachDxcCompileObject err) {
    CComPtr<IDxcBlob> pObject = CComPtr(reinterpret_cast<IDxcBlob*>(err));
    return (char const*)(pObject->GetBufferPointer());
}

MACH_EXPORT size_t machDxcCompileObjectGetBytesLength(MachDxcCompileObject err) {
    CComPtr<IDxcBlob> pObject = CComPtr(reinterpret_cast<IDxcBlob*>(err));
    return pObject->GetBufferSize();
}

MACH_EXPORT void machDxcCompileObjectDeinit(MachDxcCompileObject err) {
    CComPtr<IDxcBlob> pObject = CComPtr(reinterpret_cast<IDxcBlob*>(err));
    pObject.Release();
}

//--------------------
// MachDxcCompileError
//--------------------
MACH_EXPORT char const* machDxcCompileErrorGetString(MachDxcCompileError err) {
    CComPtr<IDxcBlobUtf8> pErrors = CComPtr(reinterpret_cast<IDxcBlobUtf8*>(err));
    return (char const*)(pErrors->GetBufferPointer());
}

MACH_EXPORT size_t machDxcCompileErrorGetStringLength(MachDxcCompileError err) {
    CComPtr<IDxcBlobUtf8> pErrors = CComPtr(reinterpret_cast<IDxcBlobUtf8*>(err));
    return pErrors->GetStringLength();
}

MACH_EXPORT void machDxcCompileErrorDeinit(MachDxcCompileError err) {
    CComPtr<IDxcBlobUtf8> pErrors = CComPtr(reinterpret_cast<IDxcBlobUtf8*>(err));
    pErrors.Release();
}

#ifdef __cplusplus
} // extern "C"
#endif
