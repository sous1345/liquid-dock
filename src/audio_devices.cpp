#include "audio_devices.h"
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propvarutil.h>

namespace audio {
namespace {

// Windows has no public call to change the default playback device; the Sound control panel and
// Settings use this interface (stable since Windows 7). If it ever fails, the caller falls back to
// opening the sound settings.
struct DECLSPEC_UUID("f8679f50-850a-41cf-9c72-430f290290c8") IPolicyConfig : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, INT, void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, void*, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, INT, PINT64, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR, ERole) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(PCWSTR, INT) = 0;
};
const CLSID kPolicyConfigClient = { 0x870af99c, 0x171d, 0x4f9e, { 0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9 } };

}   // namespace

std::vector<Device> Outputs() {
    std::vector<Device> out;
    ComPtr<IMMDeviceEnumerator> en;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))) return out;
    std::wstring defaultId;
    ComPtr<IMMDevice> def;
    if (SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eMultimedia, &def))) {
        LPWSTR id = nullptr;
        if (SUCCEEDED(def->GetId(&id))) { defaultId = id; CoTaskMemFree(id); }
    }
    ComPtr<IMMDeviceCollection> all;
    if (FAILED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &all))) return out;
    UINT n = 0; all->GetCount(&n);
    for (UINT i = 0; i < n; ++i) {
        ComPtr<IMMDevice> d;
        if (FAILED(all->Item(i, &d))) continue;
        Device dev;
        LPWSTR id = nullptr;
        if (FAILED(d->GetId(&id))) continue;
        dev.id = id; CoTaskMemFree(id);
        ComPtr<IPropertyStore> props;
        if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &props))) {
            PROPVARIANT v; PropVariantInit(&v);
            if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR) dev.name = v.pwszVal;
            PropVariantClear(&v);
        }
        if (dev.name.empty()) dev.name = L"출력 장치";
        dev.isDefault = dev.id == defaultId;
        out.push_back(std::move(dev));
    }
    return out;
}

bool SetDefault(const std::wstring& id) {
    ComPtr<IPolicyConfig> pc;
    HRESULT hr = CoCreateInstance(kPolicyConfigClient, nullptr, CLSCTX_ALL, __uuidof(IPolicyConfig), (void**)pc.GetAddressOf());
    if (FAILED(hr)) { Log(L"audio: policy config unavailable %08x", hr); return false; }
    bool ok = true;
    for (ERole role : { eConsole, eMultimedia, eCommunications })
        if (FAILED(hr = pc->SetDefaultEndpoint(id.c_str(), role))) { ok = false; Log(L"audio: set default failed %08x", hr); }
    return ok;
}

}   // namespace audio
