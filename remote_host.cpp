// remote_host.cpp: remote desktop kiểu Moonlight/Sunshine cho Windows, C++ thuần Win32, không thư viện ngoài.
//   Chụp: DXGI Desktop Duplication (màn hình chính) -> NV12 -> Media Foundation H.264 (NVENC/QSV, thiếu thì CPU)
//   Mạng: HTTPS + WebSocket qua Schannel, quảng bá mDNS _remotedesk._tcp. Điều khiển: SendInput. Cửa sổ kiểu Apple, sáng/tối theo Windows. Đăng nhập bằng tài khoản do PC quản lý.
//   Client: app iPhone (ios/) hoặc Safari mở https://<IP PC>:8443 (remote_desktop.html). Keymap lưu ở remote_keymap.json.
//
// Build: bash build_host.sh       Chạy: remote_host.exe [--port 8443] [--theme light|dark] | --self-test
#define _WIN32_WINNT 0x0A00
#define WIN32_LEAN_AND_MEAN
#define SECURITY_WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <codecapi.h>
#include <strmif.h>
#include <mmsystem.h>
#include <wincrypt.h>
#include <ncrypt.h>
#include <bcrypt.h>
#include <schannel.h>
#include <security.h>
#include <windns.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <gdiplus.h>
#include <winhttp.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <deque>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// mingw thiếu uuid này cho ComPtr::As
__CRT_UUID_DECL(ICodecAPI, 0x901db4c7, 0x31ce, 0x41a2, 0x85, 0xdc, 0x8f, 0xa0, 0xbf, 0x41, 0xb8, 0xda)
using Microsoft::WRL::ComPtr;
using std::string;
using Clock = std::chrono::steady_clock;

static std::atomic<int> g_fps{60}, g_mbps{20}, g_width{1920};
static int g_port = 8443;
static std::atomic<bool> g_hevc{true}, g_remote{false};  // mã hoá HEVC; cho vào từ Internet (mở cổng router)
static std::wstring g_dir;  // thư mục chứa exe
static CredHandle g_cred;
static PCCERT_CONTEXT g_cert;
static HWND g_hwnd;  // cửa sổ chính (null khi self-test)
static std::mutex g_logmu;
static std::vector<string> g_logq;
#define WM_APP_LOG (WM_APP + 1)

static void logf(const char* fmt, ...) {
    char b[512];
    va_list a;
    va_start(a, fmt);
    vsnprintf(b, sizeof b, fmt, a);
    va_end(a);
    printf("%s\n", b);
    fflush(stdout);
    std::lock_guard g(g_logmu);
    g_logq.push_back(b);
    if (g_hwnd) PostMessageW(g_hwnd, WM_APP_LOG, 0, 0);
}

static string read_file(const std::wstring& p) {
    std::ifstream f(std::filesystem::path(p), std::ios::binary);
    return f ? string(std::istreambuf_iterator<char>(f), {}) : string();
}
static string utf8(const std::wstring& w) {
    string s(WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr), 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), (int)s.size(), nullptr, nullptr);
    s.pop_back();
    return s;
}

// ======================= H.264 / HEVC Annex B =======================
static std::vector<std::pair<size_t, size_t>> nal_spans(const string& d) {  // (đầu payload, cuối)
    std::vector<size_t> st;
    for (size_t i = 0; i + 2 < d.size(); i++)
        if (!d[i] && !d[i + 1] && d[i + 2] == 1) st.push_back(i + 3), i += 2;
    std::vector<std::pair<size_t, size_t>> out;
    for (size_t k = 0; k < st.size(); k++) {
        size_t e = k + 1 < st.size() ? st[k + 1] - 3 : d.size();
        while (e > st[k] && !d[e - 1]) e--;
        if (e > st[k]) out.push_back({st[k], e});
    }
    return out;
}
static int nal_type(const string& d, size_t s, bool hevc) { return hevc ? ((uint8_t)d[s] >> 1) & 63 : (uint8_t)d[s] & 31; }
static bool has_nal(const string& d, int t, bool hevc = false) {
    for (auto [s, e] : nal_spans(d)) if (nal_type(d, s, hevc) == t) return true;
    return false;
}
// Khung khoá: IDR (5) của H.264; IRAP (16–21: BLA, IDR, CRA) của HEVC.
static bool is_key(const string& d, bool hevc = false) {
    for (auto [s, e] : nal_spans(d)) {
        int t = nal_type(d, s, hevc);
        if (hevc ? (t >= 16 && t <= 21) : t == 5) return true;
    }
    return false;
}
// Ghi nhớ bộ tham số (H.264: SPS 7 + PPS 8; HEVC: VPS 32 + SPS 33 + PPS 34), gắn lại vào khung khoá nếu encoder không lặp.
static bool fix_headers(string& d, string& hdrs, bool hevc = false) {
    bool key = is_key(d, hevc);
    if (has_nal(d, hevc ? 33 : 7, hevc)) {
        hdrs.clear();
        for (auto [s, e] : nal_spans(d)) {
            int t = nal_type(d, s, hevc);
            if (hevc ? (t >= 32 && t <= 34) : (t == 7 || t == 8)) hdrs += string("\0\0\0\1", 4) + d.substr(s, e - s);
        }
    } else if (key && !hdrs.empty()) d = hdrs + d;
    return key;
}

// ======================= Chụp màn hình (DXGI) =======================
struct Capture {
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<IDXGIOutputDuplication> dup;
    ComPtr<ID3D11Texture2D> staging;
    int w = 0, h = 0;
    std::vector<uint8_t> bgra;  // khung gần nhất, stride w*4
    std::vector<uint8_t> shape, saved;
    DXGI_OUTDUPL_POINTER_SHAPE_INFO si{};
    POINT pos{};
    bool visible = false;
    int px0 = 0, py0 = 0, px1 = 0, py1 = 0;  // vùng đã vẽ con trỏ

    // shortcut: chỉ màn hình chính, không xoay; cần đa màn hình thì cho chọn output.
    bool init() {
        dup.Reset(); staging.Reset(); ctx.Reset(); dev.Reset();
        ComPtr<IDXGIFactory1> f;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f)))) return false;
        ComPtr<IDXGIAdapter1> a;
        for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
            ComPtr<IDXGIOutput> o;
            for (UINT j = 0; a->EnumOutputs(j, &o) != DXGI_ERROR_NOT_FOUND; j++) {
                DXGI_OUTPUT_DESC od;
                o->GetDesc(&od);
                if (od.DesktopCoordinates.left || od.DesktopCoordinates.top) continue;
                ComPtr<IDXGIOutput1> o1;
                if (FAILED(o.As(&o1)) ||
                    FAILED(D3D11CreateDevice(a.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                             &dev, nullptr, &ctx)) ||
                    FAILED(o1->DuplicateOutput(dev.Get(), &dup)))
                    return false;
                w = od.DesktopCoordinates.right, h = od.DesktopCoordinates.bottom;
                return true;
            }
        }
        return false;
    }
    // 1 = có thay đổi (hình hoặc chuột), 0 = hết giờ, -1 = mất quyền (cần init lại)
    int grab(UINT timeout_ms) {
        DXGI_OUTDUPL_FRAME_INFO fi;
        ComPtr<IDXGIResource> res;
        HRESULT hr = dup->AcquireNextFrame(timeout_ms, &fi, &res);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) return 0;
        if (FAILED(hr)) return -1;
        if (fi.LastMouseUpdateTime.QuadPart) visible = fi.PointerPosition.Visible, pos = fi.PointerPosition.Position;
        if (fi.PointerShapeBufferSize) {
            shape.resize(fi.PointerShapeBufferSize);
            UINT req;
            if (FAILED(dup->GetFramePointerShape((UINT)shape.size(), shape.data(), &req, &si))) shape.clear();
        }
        if (fi.LastPresentTime.QuadPart) {
            ComPtr<ID3D11Texture2D> tex;
            res.As(&tex);
            D3D11_TEXTURE2D_DESC td;
            tex->GetDesc(&td);
            if (!staging || (int)td.Width != w || (int)td.Height != h) {
                w = td.Width, h = td.Height;
                td.Usage = D3D11_USAGE_STAGING, td.BindFlags = 0, td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                td.MiscFlags = 0, td.MipLevels = td.ArraySize = 1;
                staging.Reset();
                dev->CreateTexture2D(&td, nullptr, &staging);
            }
            ctx->CopyResource(staging.Get(), tex.Get());
            D3D11_MAPPED_SUBRESOURCE m;
            if (SUCCEEDED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) {
                bgra.resize((size_t)w * h * 4);
                for (int y = 0; y < h; y++) memcpy(&bgra[(size_t)y * w * 4], (uint8_t*)m.pData + (size_t)y * m.RowPitch, w * 4);
                ctx->Unmap(staging.Get(), 0);
            }
        }
        dup->ReleaseFrame();
        return 1;
    }
    // Desktop Duplication không có con trỏ trong hình: vẽ vào, đổi màu xong thì khôi phục.
    void cursor(bool draw) {
        if (!draw) {
            for (int y = py0, i = 0; y < py1; y++, i++)
                memcpy(&bgra[((size_t)y * w + px0) * 4], &saved[(size_t)i * (px1 - px0) * 4], (px1 - px0) * 4);
            py1 = py0;
            return;
        }
        if (!visible || shape.empty() || bgra.empty()) return;
        bool mono = si.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME;
        int cw = si.Width, ch = mono ? si.Height / 2 : si.Height;
        px0 = std::max(0, (int)pos.x), py0 = std::max(0, (int)pos.y);
        px1 = std::min(w, (int)pos.x + cw), py1 = std::min(h, (int)pos.y + ch);
        if (px0 >= px1 || py0 >= py1) { py1 = py0; return; }
        saved.resize((size_t)(px1 - px0) * (py1 - py0) * 4);
        for (int y = py0, i = 0; y < py1; y++, i++)
            memcpy(&saved[(size_t)i * (px1 - px0) * 4], &bgra[((size_t)y * w + px0) * 4], (px1 - px0) * 4);
        for (int y = py0; y < py1; y++)
            for (int x = px0; x < px1; x++) {
                int sx = x - pos.x, sy = y - pos.y;
                uint8_t* d = &bgra[((size_t)y * w + x) * 4];
                if (mono) {
                    int bit = 0x80 >> (sx & 7);
                    bool a = shape[sy * si.Pitch + sx / 8] & bit, xr = shape[(sy + ch) * si.Pitch + sx / 8] & bit;
                    for (int c = 0; c < 3; c++) d[c] = a ? (xr ? 255 - d[c] : d[c]) : (xr ? 255 : 0);
                } else {
                    const uint8_t* s = &shape[sy * si.Pitch + sx * 4];
                    for (int c = 0; c < 3; c++)
                        d[c] = si.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR ? (s[c] * s[3] + d[c] * (255 - s[3])) / 255
                               : s[3] ? d[c] ^ s[c] : s[c];
                }
            }
    }
};

// BGRA (sw×sh) -> NV12 (dw×dh), co giãn lấy mẫu gần nhất, hệ số BT.709 giới hạn (trình duyệt mặc định 709 cho HD).
// shortcut: đổi màu trên CPU (~3 ms ở 1080p); muốn 4K/120 thì chuyển sang Video Processor trên GPU.
static void bgra_to_nv12(const uint8_t* src, int sw, int sh, uint8_t* dst, int dw, int dh) {
    std::vector<int> sxs(dw);
    for (int x = 0; x < dw; x++) sxs[x] = (int)((int64_t)x * sw / dw) * 4;
    uint8_t* uv = dst + (size_t)dw * dh;
    for (int y = 0; y < dh; y++) {
        const uint8_t* row = src + (size_t)((int64_t)y * sh / dh) * sw * 4;
        uint8_t* yd = dst + (size_t)y * dw;
        for (int x = 0; x < dw; x++) {
            const uint8_t* p = row + sxs[x];
            yd[x] = (uint8_t)(((47 * p[2] + 157 * p[1] + 16 * p[0] + 128) >> 8) + 16);
        }
        if (y & 1) continue;
        uint8_t* ud = uv + (size_t)(y / 2) * dw;
        for (int x = 0; x < dw; x += 2) {
            const uint8_t* p = row + sxs[x];
            ud[x] = (uint8_t)(((-26 * p[2] - 86 * p[1] + 112 * p[0] + 128) >> 8) + 128);
            ud[x + 1] = (uint8_t)(((112 * p[2] - 102 * p[1] - 10 * p[0] + 128) >> 8) + 128);
        }
    }
}

// ======================= Mã hoá HEVC / H.264 (Media Foundation) =======================
struct Encoder {
    ComPtr<IMFTransform> mft;
    ComPtr<IMFMediaEventGenerator> gen;
    ComPtr<ICodecAPI> api;
    DWORD in_id = 0, out_id = 0, out_cb = 0;
    bool async = false, provides = false, hevc = false;  // HEVC: cùng chất lượng tốn khoảng nửa bitrate so với H.264
    int w = 0, h = 0, fps = 0, need_input = 0;
    string name, hdrs;

    bool setv(const GUID& g, UINT v, VARTYPE vt = VT_UI4) {
        VARIANT var;
        VariantInit(&var);
        var.vt = vt;
        if (vt == VT_BOOL) var.boolVal = v ? VARIANT_TRUE : VARIANT_FALSE; else var.ulVal = v;
        return api && SUCCEEDED(api->SetValue(&g, &var));
    }
    bool setup(IMFActivate* act, int mbps) {
        if (FAILED(act->ActivateObject(IID_PPV_ARGS(&mft)))) return false;
        WCHAR* fn = nullptr;
        UINT32 len;
        if (SUCCEEDED(act->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &fn, &len))) name = utf8(fn), CoTaskMemFree(fn);
        ComPtr<IMFAttributes> at;
        UINT32 as = 0;
        if (SUCCEEDED(mft->GetAttributes(&at))) {
            at->GetUINT32(MF_TRANSFORM_ASYNC, &as);
            if (as) at->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
            at->SetUINT32(MF_LOW_LATENCY, TRUE);
        }
        async = as;
        if (async && FAILED(mft.As(&gen))) return false;
        if (mft->GetStreamIDs(1, &in_id, 1, &out_id) == E_NOTIMPL) in_id = out_id = 0;
        mft.As(&api);
        setv(CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR);
        setv(CODECAPI_AVEncCommonMeanBitRate, mbps * 1000000);
        setv(CODECAPI_AVLowLatencyMode, 1, VT_BOOL);
        setv(CODECAPI_AVEncMPVGOPSize, fps * 10);
        setv(CODECAPI_AVEncMPVDefaultBPictureCount, 0);
        ComPtr<IMFMediaType> ot, it;
        MFCreateMediaType(&ot);
        ot->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        ot->SetGUID(MF_MT_SUBTYPE, hevc ? MFVideoFormat_HEVC : MFVideoFormat_H264);
        ot->SetUINT32(MF_MT_AVG_BITRATE, mbps * 1000000);
        ot->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        ot->SetUINT32(MF_MT_MPEG2_PROFILE, hevc ? (UINT32)eAVEncH265VProfile_Main_420_8 : (UINT32)eAVEncH264VProfile_Main);
        ot->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
        MFSetAttributeSize(ot.Get(), MF_MT_FRAME_SIZE, w, h);
        MFSetAttributeRatio(ot.Get(), MF_MT_FRAME_RATE, fps, 1);
        MFSetAttributeRatio(ot.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        if (FAILED(mft->SetOutputType(out_id, ot.Get(), 0))) return false;
        MFCreateMediaType(&it);
        it->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        it->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        it->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(it.Get(), MF_MT_FRAME_SIZE, w, h);
        MFSetAttributeRatio(it.Get(), MF_MT_FRAME_RATE, fps, 1);
        if (FAILED(mft->SetInputType(in_id, it.Get(), 0))) return false;
        stream_info();
        ComPtr<IMFMediaType> cur;  // một số encoder chỉ để SPS/PPS ở đây
        UINT32 n = 0;
        if (SUCCEEDED(mft->GetOutputCurrentType(out_id, &cur)) && SUCCEEDED(cur->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &n)) && n) {
            hdrs.resize(n);
            cur->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, (UINT8*)hdrs.data(), n, nullptr);
        }
        mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        return true;
    }
    void stream_info() {
        MFT_OUTPUT_STREAM_INFO osi{};
        mft->GetOutputStreamInfo(out_id, &osi);
        provides = osi.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES);
        out_cb = std::max<DWORD>(osi.cbSize, w * h * 3 / 2);
    }
    // only: 1 = chỉ phần cứng, 2 = chỉ phần mềm, 0 = phần cứng rồi phần mềm
    bool init(int w_, int h_, int fps_, int mbps, int only = 0, bool hevc_ = false) {
        w = w_, h = h_, fps = fps_, hevc = hevc_;
        MFT_REGISTER_TYPE_INFO in{MFMediaType_Video, MFVideoFormat_NV12}, out{MFMediaType_Video, hevc ? MFVideoFormat_HEVC : MFVideoFormat_H264};
        for (UINT32 flags : {MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER}) {
            if ((only == 1 && !(flags & MFT_ENUM_FLAG_HARDWARE)) || (only == 2 && (flags & MFT_ENUM_FLAG_HARDWARE))) continue;
            IMFActivate** acts = nullptr;
            UINT32 n = 0;
            if (FAILED(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, flags, &in, &out, &acts, &n))) continue;
            bool ok = false;
            for (UINT32 i = 0; i < n; i++) {
                if (!ok && setup(acts[i], mbps)) {
                    // thử một khung để loại encoder nhận kiểu nhưng không chạy (vd. thiếu D3D)
                    std::vector<uint8_t> blank((size_t)w * h * 3 / 2, 128);
                    std::vector<string> o;
                    ok = encode(blank.data(), 0, true, o);
                }
                if (!ok) { if (mft) acts[i]->ShutdownObject(); mft.Reset(), gen.Reset(), api.Reset(), need_input = 0, hdrs.clear(); }
                acts[i]->Release();
            }
            CoTaskMemFree(acts);
            if (ok) return true;
        }
        return false;
    }
    bool pull(std::vector<string>& out) {
        MFT_OUTPUT_DATA_BUFFER ob{};
        ob.dwStreamID = out_id;
        ComPtr<IMFSample> own;
        if (!provides) {
            ComPtr<IMFMediaBuffer> b;
            MFCreateSample(&own);
            MFCreateMemoryBuffer(out_cb, &b);
            own->AddBuffer(b.Get());
            ob.pSample = own.Get();
        }
        DWORD st = 0;
        HRESULT hr = mft->ProcessOutput(0, 1, &ob, &st);
        if (ob.pEvents) ob.pEvents->Release();
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            ComPtr<IMFMediaType> t;
            if (SUCCEEDED(mft->GetOutputAvailableType(out_id, 0, &t))) mft->SetOutputType(out_id, t.Get(), 0);
            stream_info();
            return true;
        }
        if (FAILED(hr) || !ob.pSample) return false;
        ComPtr<IMFMediaBuffer> cb;
        BYTE* p;
        DWORD n;
        if (SUCCEEDED(ob.pSample->ConvertToContiguousBuffer(&cb)) && SUCCEEDED(cb->Lock(&p, nullptr, &n))) {
            out.emplace_back((char*)p, n);
            fix_headers(out.back(), hdrs, hevc);
            cb->Unlock();
        }
        if (provides) ob.pSample->Release();
        return true;
    }
    bool event(MediaEventType& t, bool wait) {
        ComPtr<IMFMediaEvent> ev;
        if (FAILED(gen->GetEvent(wait ? 0 : MF_EVENT_FLAG_NO_WAIT, &ev))) return false;
        return SUCCEEDED(ev->GetType(&t));
    }
    bool encode(const uint8_t* nv12, int64_t idx, bool key, std::vector<string>& out) {
        if (key) setv(CODECAPI_AVEncVideoForceKeyFrame, 1);
        ComPtr<IMFMediaBuffer> b;
        ComPtr<IMFSample> s;
        DWORD sz = w * h * 3 / 2;
        BYTE* p;
        if (FAILED(MFCreateMemoryBuffer(sz, &b)) || FAILED(b->Lock(&p, nullptr, nullptr))) return false;
        memcpy(p, nv12, sz);
        b->Unlock();
        b->SetCurrentLength(sz);
        MFCreateSample(&s);
        s->AddBuffer(b.Get());
        s->SetSampleTime(idx * 10000000LL / fps);
        s->SetSampleDuration(10000000LL / fps);
        if (!async) {
            if (FAILED(mft->ProcessInput(in_id, s.Get(), 0))) return false;
            while (pull(out)) {}
            return true;
        }
        MediaEventType t;
        while (!need_input) {
            if (!event(t, true)) return false;
            if (t == METransformNeedInput) need_input++;
            else if (t == METransformHaveOutput) pull(out);
        }
        if (FAILED(mft->ProcessInput(in_id, s.Get(), 0))) return false;
        need_input--;
        // chờ hình của chính khung này (tối đa 50 ms) thay vì để nó trễ sang lượt sau
        for (auto t0 = Clock::now(); Clock::now() - t0 < std::chrono::milliseconds(50);) {
            if (!event(t, false)) { Sleep(1); continue; }
            if (t == METransformNeedInput) need_input++;
            else if (t == METransformHaveOutput) { pull(out); break; }
        }
        return true;
    }
};

// ======================= JSON tối thiểu (chỉ cho tin nhắn của trang) =======================
static size_t jfind(const string& m, const char* key) {
    string pat = string("\"") + key + "\":";
    for (size_t p = m.find(pat); p != string::npos; p = m.find(pat, p + 1)) {
        size_t bs = 0;
        while (p > bs && m[p - 1 - bs] == '\\') bs++;
        if (bs % 2 == 0) return std::min(m.find_first_not_of(" \t\r\n", p + pat.size()), m.size());  // " không bị escape: đúng là khoá
    }
    return string::npos;
}
static void put_utf8(string& o, uint32_t c) {
    if (c < 0x80) o += (char)c;
    else if (c < 0x800) o += (char)(0xC0 | c >> 6), o += (char)(0x80 | (c & 63));
    else if (c < 0x10000) o += (char)(0xE0 | c >> 12), o += (char)(0x80 | (c >> 6 & 63)), o += (char)(0x80 | (c & 63));
    else o += (char)(0xF0 | c >> 18), o += (char)(0x80 | (c >> 12 & 63)), o += (char)(0x80 | (c >> 6 & 63)), o += (char)(0x80 | (c & 63));
}
static bool jstr(const string& m, const char* key, string& o) {
    size_t p = jfind(m, key);
    if (p == string::npos || p >= m.size() || m[p] != '"') return false;
    o.clear();
    for (p++; p < m.size();) {
        char c = m[p++];
        if (c == '"') return true;
        if (c != '\\') { o += c; continue; }
        if (p >= m.size()) return false;
        c = m[p++];
        if (c == 'u') {
            auto hex = [&](uint32_t& v) { if (p + 4 > m.size()) return false; v = std::stoul(m.substr(p, 4), nullptr, 16); p += 4; return true; };
            uint32_t cp, lo;
            if (!hex(cp)) return false;
            if (cp >= 0xD800 && cp < 0xDC00 && m.compare(p, 2, "\\u") == 0 && (p += 2, hex(lo))) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            put_utf8(o, cp);
        } else o += c == 'n' ? '\n' : c == 't' ? '\t' : c == 'r' ? '\r' : c == 'b' ? '\b' : c == 'f' ? '\f' : c;
    }
    return false;
}
static bool jnum(const string& m, const char* key, double& v) {
    size_t p = jfind(m, key);
    if (p == string::npos) return false;
    char* e;
    v = strtod(m.c_str() + p, &e);
    return e != m.c_str() + p && std::isfinite(v);
}
static string jesc(const string& s) {
    string o;
    for (char c : s) { if (c == '"' || c == '\\') o += '\\'; if ((unsigned char)c >= 0x20) o += c; }
    return o;
}

// ======================= Điều khiển (SendInput) =======================
static const std::map<string, WORD> VK = {
    {"BACK", 8}, {"TAB", 9}, {"ENTER", 13}, {"SHIFT", 16}, {"CTRL", 17}, {"ALT", 18}, {"PAUSE", 19}, {"CAPS", 20},
    {"ESC", 27}, {"SPACE", 32}, {"PGUP", 33}, {"PGDN", 34}, {"END", 35}, {"HOME", 36}, {"LEFT", 37}, {"UP", 38},
    {"RIGHT", 39}, {"DOWN", 40}, {"PRTSC", 44}, {"INS", 45}, {"DEL", 46}, {"WIN", 91}, {"MENU", 93},
    {"LSHIFT", 160}, {"RSHIFT", 161}, {"LCTRL", 162}, {"RCTRL", 163}, {"LALT", 164}, {"RALT", 165},
    {";", 186}, {"=", 187}, {",", 188}, {"-", 189}, {".", 190}, {"/", 191}, {"`", 192}, {"[", 219}, {"\\", 220},
    {"]", 221}, {"'", 222}};
static bool extended(WORD vk) { return (vk >= 33 && vk <= 46 && vk != 44) || vk == 91 || vk == 93 || vk == 163 || vk == 165; }
static INPUT mouse_in(DWORD f, LONG dx = 0, LONG dy = 0, LONG data = 0) {
    INPUT i{};
    i.type = INPUT_MOUSE, i.mi.dx = dx, i.mi.dy = dy, i.mi.mouseData = (DWORD)data, i.mi.dwFlags = f;
    return i;
}
static INPUT key_in(WORD vk, WORD scan, DWORD f) {
    INPUT i{};
    i.type = INPUT_KEYBOARD, i.ki.wVk = vk, i.ki.wScan = scan, i.ki.dwFlags = f;
    return i;
}
// "CTRL+SHIFT+ESC" / "W" / "LMB" / "WHEELUP"; nhả theo thứ tự ngược
static bool combo(string keys, bool down, std::vector<INPUT>& out) {
    std::transform(keys.begin(), keys.end(), keys.begin(), ::toupper);
    std::vector<INPUT> v;
    std::stringstream ss(keys);
    for (string k; std::getline(ss, k, '+');) {
        k.erase(0, k.find_first_not_of(' '));
        k.erase(k.find_last_not_of(' ') + 1);
        if (k == "LMB") v.push_back(mouse_in(down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP));
        else if (k == "RMB") v.push_back(mouse_in(down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP));
        else if (k == "MMB") v.push_back(mouse_in(down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP));
        else if (k == "WHEELUP" || k == "WHEELDOWN") { if (down) v.push_back(mouse_in(MOUSEEVENTF_WHEEL, 0, 0, k == "WHEELUP" ? 120 : -120)); }
        else {
            WORD vk = VK.count(k) ? VK.at(k) : k.size() == 1 && isalnum((unsigned char)k[0]) ? (WORD)k[0] : 0;
            if (!vk) return false;
            // Gửi scan code như bàn phím thật: game DirectInput/raw input bỏ qua phím chỉ có mã VK
            UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC_EX);
            DWORD f = down ? 0 : KEYEVENTF_KEYUP;
            // mũi tên, Home, Del... trùng scan code với bàn phím số: thiếu cờ extended thì game nhận thành phím số
            if (sc) f |= KEYEVENTF_SCANCODE;
            if ((sc & 0xFF00) == 0xE000 || extended(vk)) f |= KEYEVENTF_EXTENDEDKEY;
            v.push_back(key_in(vk, (WORD)(sc & 0xFF), f));
        }
    }
    if (!down) std::reverse(v.begin(), v.end());
    out.insert(out.end(), v.begin(), v.end());
    return true;
}
static bool translate(const string& m, std::vector<INPUT>& out) {
    string t, s;
    double x, y, d;
    if (!jstr(m, "t", t)) return false;
    if (t == "k") return jstr(m, "keys", s) && jnum(m, "d", d) && combo(s, d != 0, out);
    if (t == "m" && jnum(m, "x", x) && jnum(m, "y", y)) {  // tuyệt đối 0..1 trên màn hình chính (đúng vùng đang phát)
        out.push_back(mouse_in(MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE, lround(std::clamp(x, 0.0, 1.0) * 65535),
                               lround(std::clamp(y, 0.0, 1.0) * 65535)));
        return true;
    }
    if (t == "r" && jnum(m, "dx", x) && jnum(m, "dy", y)) return out.push_back(mouse_in(MOUSEEVENTF_MOVE, lround(x), lround(y))), true;
    if (t == "w" && jnum(m, "dy", y)) {
        out.push_back(mouse_in(MOUSEEVENTF_WHEEL, 0, 0, lround(y)));
        if (jnum(m, "dx", x) && x) out.push_back(mouse_in(MOUSEEVENTF_HWHEEL, 0, 0, lround(x)));
        return true;
    }
    if (t == "text" && jstr(m, "s", s)) {
        std::wstring w(MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0), 0);
        MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), (int)w.size());
        for (wchar_t u : w)  // KEYEVENTF_UNICODE nhận từng đơn vị UTF-16 (emoji = 2)
            out.push_back(key_in(0, u, KEYEVENTF_UNICODE)), out.push_back(key_in(0, u, KEYEVENTF_UNICODE | KEYEVENTF_KEYUP));
        return true;
    }
    return false;
}

// ======================= Chứng chỉ tự ký (trong kho CurrentUser\My) =======================
static std::vector<string> local_ips() {
    std::vector<string> ips{"127.0.0.1"};
    char host[256];
    addrinfo hints{}, *r = nullptr;
    hints.ai_family = AF_INET;
    if (!gethostname(host, sizeof host) && !getaddrinfo(host, nullptr, &hints, &r)) {
        for (auto* p = r; p; p = p->ai_next) {
            char b[32];
            inet_ntop(AF_INET, &((sockaddr_in*)p->ai_addr)->sin_addr, b, sizeof b);
            if (std::find(ips.begin(), ips.end(), b) == ips.end()) ips.push_back(b);
        }
        freeaddrinfo(r);
    }
    return ips;
}
// Tailscale cấp IP trong dải CGNAT 100.64.0.0/10.
static bool is_tailscale(const string& ip) {
    int a = 0, b = 0;
    return sscanf(ip.c_str(), "%d.%d", &a, &b) == 2 && a == 100 && b >= 64 && b < 128;
}
// Địa chỉ iPhone có thể dùng: LAN trước (nhanh nhất ở nhà), Tailscale sau (khi ở ngoài).
static std::vector<string> reachable_ips() {
    std::vector<string> v;
    for (auto& ip : local_ips()) if (ip != "127.0.0.1") v.push_back(ip);
    std::stable_partition(v.begin(), v.end(), [](const string& ip) { return !is_tailscale(ip); });
    return v;
}
// Bitrate thích ứng kiểu TCP (giảm nhân, tăng cộng). busy = tỉ lệ thời gian trong một giây bị kẹt khi gửi hình.
// Kẹt quá nửa giây thì hạ 30%; thông thoáng 2 giây liền thì nâng 20%, không vượt mức người dùng chọn.
static int adapt_bitrate(int cur, int max_mbps, double busy, int& calm) {
    if (busy > 0.5) { calm = 0; return std::max(2, cur * 7 / 10); }
    if (busy >= 0.15 || cur >= max_mbps) { calm = 0; return cur; }
    if (++calm < 2) return cur;
    calm = 0;
    return std::min(max_mbps, cur + std::max(1, cur / 5));
}

static bool cert_ok(PCCERT_CONTEXT c, const std::vector<string>& ips) {
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    if (CompareFileTime(&c->pCertInfo->NotAfter, &now) <= 0) return false;
    PCERT_EXTENSION e = CertFindExtension(szOID_SUBJECT_ALT_NAME2, c->pCertInfo->cExtension, c->pCertInfo->rgExtension);
    CERT_ALT_NAME_INFO* info;
    DWORD n;
    if (!e || !CryptDecodeObjectEx(X509_ASN_ENCODING, X509_ALTERNATE_NAME, e->Value.pbData, e->Value.cbData,
                                   CRYPT_DECODE_ALLOC_FLAG, nullptr, &info, &n))
        return false;
    bool all = true;
    for (auto& ip : ips) {
        in_addr a;
        inet_pton(AF_INET, ip.c_str(), &a);
        bool f = false;
        for (DWORD i = 0; i < info->cAltEntry; i++)
            f |= info->rgAltEntry[i].dwAltNameChoice == CERT_ALT_NAME_IP_ADDRESS && info->rgAltEntry[i].IPAddress.cbData == 4 &&
                 !memcmp(info->rgAltEntry[i].IPAddress.pbData, &a, 4);
        all &= f;
    }
    LocalFree(info);
    return all;
}
static PCCERT_CONTEXT ensure_cert(const std::wstring& cn, const std::vector<string>& ips, bool& created) {
    created = false;
    HCERTSTORE st = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0, CERT_SYSTEM_STORE_CURRENT_USER, L"MY");
    if (!st) return nullptr;
    PCCERT_CONTEXT c = nullptr, good = nullptr;
    std::vector<PCCERT_CONTEXT> stale;
    while ((c = CertFindCertificateInStore(st, X509_ASN_ENCODING, 0, CERT_FIND_SUBJECT_STR_W, cn.c_str(), c)))
        (!good && cert_ok(c, ips) ? good : stale.emplace_back()) = CertDuplicateCertificateContext(c);
    for (auto s : stale) CertDeleteCertificateFromStore(s);
    if (good) return CertCloseStore(st, 0), good;

    std::wstring keyname = L"RemoteDesk-" + cn;
    NCRYPT_PROV_HANDLE prov = 0;
    NCRYPT_KEY_HANDLE key = 0;
    DWORD bits = 2048;
    if (NCryptOpenStorageProvider(&prov, MS_KEY_STORAGE_PROVIDER, 0) ||
        NCryptCreatePersistedKey(prov, &key, BCRYPT_RSA_ALGORITHM, keyname.c_str(), 0, NCRYPT_OVERWRITE_KEY_FLAG) ||
        NCryptSetProperty(key, NCRYPT_LENGTH_PROPERTY, (PBYTE)&bits, sizeof bits, 0) || NCryptFinalizeKey(key, 0))
        return CertCloseStore(st, 0), nullptr;

    std::wstring dn = L"CN=" + cn;
    DWORD nlen = 0;
    CertStrToNameW(X509_ASN_ENCODING, dn.c_str(), CERT_X500_NAME_STR, nullptr, nullptr, &nlen, nullptr);
    std::vector<BYTE> nb(nlen);
    CertStrToNameW(X509_ASN_ENCODING, dn.c_str(), CERT_X500_NAME_STR, nullptr, nb.data(), &nlen, nullptr);
    CERT_NAME_BLOB name{nlen, nb.data()};

    std::vector<in_addr> addrs(ips.size());
    std::vector<CERT_ALT_NAME_ENTRY> alt(1 + ips.size());
    alt[0].dwAltNameChoice = CERT_ALT_NAME_DNS_NAME, alt[0].pwszDNSName = (LPWSTR)L"localhost";
    for (size_t i = 0; i < ips.size(); i++) {
        inet_pton(AF_INET, ips[i].c_str(), &addrs[i]);
        alt[i + 1].dwAltNameChoice = CERT_ALT_NAME_IP_ADDRESS, alt[i + 1].IPAddress = {4, (BYTE*)&addrs[i]};
    }
    CERT_ALT_NAME_INFO san{(DWORD)alt.size(), alt.data()};
    LPSTR eku_oid = (LPSTR)szOID_PKIX_KP_SERVER_AUTH;
    CERT_ENHKEY_USAGE eku{1, &eku_oid};
    CERT_BASIC_CONSTRAINTS2_INFO bc{TRUE, FALSE, 0};  // CA để iOS cho bật "tin cậy hoàn toàn"
    BYTE* enc[3];
    DWORD el[3];
    CryptEncodeObjectEx(X509_ASN_ENCODING, X509_ALTERNATE_NAME, &san, CRYPT_ENCODE_ALLOC_FLAG, nullptr, &enc[0], &el[0]);
    CryptEncodeObjectEx(X509_ASN_ENCODING, X509_ENHANCED_KEY_USAGE, &eku, CRYPT_ENCODE_ALLOC_FLAG, nullptr, &enc[1], &el[1]);
    CryptEncodeObjectEx(X509_ASN_ENCODING, X509_BASIC_CONSTRAINTS2, &bc, CRYPT_ENCODE_ALLOC_FLAG, nullptr, &enc[2], &el[2]);
    CERT_EXTENSION ext[3] = {{(LPSTR)szOID_SUBJECT_ALT_NAME2, FALSE, {el[0], enc[0]}},
                             {(LPSTR)szOID_ENHANCED_KEY_USAGE, FALSE, {el[1], enc[1]}},
                             {(LPSTR)szOID_BASIC_CONSTRAINTS2, TRUE, {el[2], enc[2]}}};
    CERT_EXTENSIONS exts{3, ext};
    CRYPT_KEY_PROV_INFO kpi{(LPWSTR)keyname.c_str(), (LPWSTR)MS_KEY_STORAGE_PROVIDER, 0, 0, 0, nullptr, 0};
    CRYPT_ALGORITHM_IDENTIFIER alg{(LPSTR)szOID_RSA_SHA256RSA, {}};
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    auto shift = [&](int64_t days) {
        ULARGE_INTEGER u{{ft.dwLowDateTime, ft.dwHighDateTime}};
        u.QuadPart += days * 864000000000LL;
        FILETIME f{u.LowPart, u.HighPart};
        SYSTEMTIME s;
        FileTimeToSystemTime(&f, &s);
        return s;
    };
    SYSTEMTIME t0 = shift(-1), t1 = shift(800);  // iOS từ chối chứng chỉ > 825 ngày
    PCCERT_CONTEXT nc = CertCreateSelfSignCertificate(key, &name, 0, &kpi, &alg, &t0, &t1, &exts);
    for (auto p : enc) LocalFree(p);
    NCryptFreeObject(key);
    NCryptFreeObject(prov);
    if (nc) {
        CertAddCertificateContextToStore(st, nc, CERT_STORE_ADD_REPLACE_EXISTING, &good);
        CertFreeCertificateContext(nc);
        created = true;
    }
    CertCloseStore(st, 0);
    return good;
}
static void delete_cert(PCCERT_CONTEXT c, const std::wstring& cn) {
    HCERTSTORE st = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0, CERT_SYSTEM_STORE_CURRENT_USER, L"MY");
    PCCERT_CONTEXT f = st ? CertFindCertificateInStore(st, X509_ASN_ENCODING, 0, CERT_FIND_EXISTING, c, nullptr) : nullptr;
    if (f) CertDeleteCertificateFromStore(f);
    if (st) CertCloseStore(st, 0);
    NCRYPT_PROV_HANDLE prov;
    NCRYPT_KEY_HANDLE key;
    if (!NCryptOpenStorageProvider(&prov, MS_KEY_STORAGE_PROVIDER, 0)) {
        if (!NCryptOpenKey(prov, &key, (L"RemoteDesk-" + cn).c_str(), 0, 0)) NCryptDeleteKey(key, 0);
        NCryptFreeObject(prov);
    }
}

// ======================= TLS (Schannel) + WebSocket =======================
static bool send_all(SOCKET s, const char* p, size_t n) {
    while (n) {
        int k = ::send(s, p, (int)std::min<size_t>(n, 1 << 20), 0);
        if (k <= 0) return false;
        p += k, n -= k;
    }
    return true;
}
struct Tls {
    SOCKET s;
    CtxtHandle ctx{};
    bool has_ctx = false;
    SecPkgContext_StreamSizes sz{};
    string in, plain;  // in: bản mã chưa giải, plain: dữ liệu đã giải chưa đọc
    std::mutex wmu;
    explicit Tls(SOCKET s_) : s(s_) {}
    ~Tls() { if (has_ctx) DeleteSecurityContext(&ctx); }
    bool recv_more() {
        char b[16384];
        int n = ::recv(s, b, sizeof b, 0);
        if (n <= 0) return false;
        in.append(b, n);
        return true;
    }
    bool handshake() {
        bool more = true;
        for (;;) {
            if (more && !recv_more()) return false;
            SecBuffer ib[2] = {{(ULONG)in.size(), SECBUFFER_TOKEN, in.data()}, {0, SECBUFFER_EMPTY, nullptr}};
            SecBuffer ob[1] = {{0, SECBUFFER_TOKEN, nullptr}};
            SecBufferDesc id{SECBUFFER_VERSION, 2, ib}, od{SECBUFFER_VERSION, 1, ob};
            ULONG attr;
            SECURITY_STATUS r = AcceptSecurityContext(&g_cred, has_ctx ? &ctx : nullptr, &id,
                ASC_REQ_SEQUENCE_DETECT | ASC_REQ_REPLAY_DETECT | ASC_REQ_CONFIDENTIALITY | ASC_REQ_EXTENDED_ERROR |
                ASC_REQ_ALLOCATE_MEMORY | ASC_REQ_STREAM, 0, has_ctx ? nullptr : &ctx, &od, &attr, nullptr);
            if (r == SEC_E_INCOMPLETE_MESSAGE) { more = true; continue; }
            if (ob[0].pvBuffer) {
                bool ok = send_all(s, (char*)ob[0].pvBuffer, ob[0].cbBuffer);
                FreeContextBuffer(ob[0].pvBuffer);
                if (!ok) return false;
            }
            if (r != SEC_E_OK && r != SEC_I_CONTINUE_NEEDED) return false;
            has_ctx = true;
            in = ib[1].BufferType == SECBUFFER_EXTRA ? in.substr(in.size() - ib[1].cbBuffer) : string();
            if (r == SEC_E_OK) return QueryContextAttributes(&ctx, SECPKG_ATTR_STREAM_SIZES, &sz) == SEC_E_OK;
            more = in.empty();
        }
    }
    bool fill() {  // giải thêm một bản ghi vào plain
        for (;;) {
            if (!in.empty()) {
                SecBuffer b[4] = {{(ULONG)in.size(), SECBUFFER_DATA, in.data()}, {}, {}, {}};
                SecBufferDesc d{SECBUFFER_VERSION, 4, b};
                SECURITY_STATUS r = DecryptMessage(&ctx, &d, 0, nullptr);
                if (r == SEC_E_OK) {
                    string extra;
                    for (auto& x : b) {
                        if (x.BufferType == SECBUFFER_DATA) plain.append((char*)x.pvBuffer, x.cbBuffer);
                        if (x.BufferType == SECBUFFER_EXTRA) extra.assign((char*)x.pvBuffer, x.cbBuffer);
                    }
                    in = extra;
                    return true;
                }
                if (r != SEC_E_INCOMPLETE_MESSAGE) return false;  // hết phiên hoặc lỗi
            }
            if (!recv_more()) return false;
        }
    }
    bool need(size_t n) {
        while (plain.size() < n) if (!fill()) return false;
        return true;
    }
    bool send(const char* p, size_t n) {
        std::lock_guard g(wmu);
        std::vector<char> buf(sz.cbHeader + sz.cbMaximumMessage + sz.cbTrailer);
        while (n) {
            ULONG k = (ULONG)std::min<size_t>(n, sz.cbMaximumMessage);
            memcpy(buf.data() + sz.cbHeader, p, k);
            SecBuffer b[4] = {{sz.cbHeader, SECBUFFER_STREAM_HEADER, buf.data()}, {k, SECBUFFER_DATA, buf.data() + sz.cbHeader},
                              {sz.cbTrailer, SECBUFFER_STREAM_TRAILER, buf.data() + sz.cbHeader + k}, {0, SECBUFFER_EMPTY, nullptr}};
            SecBufferDesc d{SECBUFFER_VERSION, 4, b};
            if (EncryptMessage(&ctx, 0, &d, 0) != SEC_E_OK || !send_all(s, buf.data(), b[0].cbBuffer + b[1].cbBuffer + b[2].cbBuffer))
                return false;
            p += k, n -= k;
        }
        return true;
    }
};

static string sha1_b64(const string& s) {
    BCRYPT_ALG_HANDLE a;
    BCRYPT_HASH_HANDLE h;
    UCHAR d[20];
    BCryptOpenAlgorithmProvider(&a, BCRYPT_SHA1_ALGORITHM, nullptr, 0);
    BCryptCreateHash(a, &h, nullptr, 0, nullptr, 0, 0);
    BCryptHashData(h, (PUCHAR)s.data(), (ULONG)s.size(), 0);
    BCryptFinishHash(h, d, 20, 0);
    BCryptDestroyHash(h);
    BCryptCloseAlgorithmProvider(a, 0);
    char out[64];
    DWORD n = sizeof out;
    CryptBinaryToStringA(d, 20, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, out, &n);
    return string(out, n);
}
static string ws_accept(const string& key) { return sha1_b64(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"); }
static string ws_frame(int op, const string& p) {
    string h(1, (char)(0x80 | op));
    if (p.size() < 126) h += (char)p.size();
    else if (p.size() < 65536) h += (char)126, h += (char)(p.size() >> 8), h += (char)p.size();
    else { h += (char)127; for (int i = 7; i >= 0; i--) h += (char)(p.size() >> (8 * i)); }
    return h + p;
}
static bool ws_send(Tls& t, int op, const string& p) { string f = ws_frame(op, p); return t.send(f.data(), f.size()); }
// shortcut: không ghép tin nhắn phân mảnh (FIN=0); trình duyệt không phân mảnh tin nhắn nhỏ.
static bool ws_read(Tls& t, int& op, string& msg) {
    for (;;) {
        if (!t.need(2)) return false;
        auto u = [&](size_t i) { return (uint8_t)t.plain[i]; };
        op = u(0) & 15;
        uint64_t len = u(1) & 127;
        size_t hl = 2;
        if (len == 126) { if (!t.need(4)) return false; len = u(2) << 8 | u(3); hl = 4; }
        else if (len == 127) { if (!t.need(10)) return false; len = 0; for (int i = 0; i < 8; i++) len = len << 8 | u(2 + i); hl = 10; }
        bool masked = u(1) & 128;
        if (len > (1 << 20) || !masked) return false;  // client bắt buộc che mặt nạ
        if (!t.need(hl + 4 + len)) return false;
        const char* mk = &t.plain[hl];
        msg.assign(t.plain, hl + 4, len);
        for (size_t i = 0; i < len; i++) msg[i] ^= mk[i & 3];
        t.plain.erase(0, hl + 4 + len);
        if (op == 9) { if (!ws_send(t, 10, msg)) return false; continue; }
        if (op == 10) continue;
        return op != 8;
    }
}

// ======================= Khoá thiết bị và chống dò =======================
// Đăng nhập đúng tài khoản thì PC cấp cho thiết bị đó một khoá 256-bit (iPhone cất trong Keychain).
// Các lần sau thiết bị vào thẳng bằng khoá, không gửi lại mật khẩu. Đổi tài khoản hoặc "Đăng xuất hết" thì mọi khoá mất hiệu lực.
static std::mutex g_devmu;
static std::vector<string> g_devices;  // SHA-256 (hex) của khoá đã cấp, cũ trước mới sau
static std::wstring g_devfile;         // remote_devices.txt cạnh exe

static string hexs(const uint8_t* p, size_t n) {
    static const char* d = "0123456789abcdef";
    string s;
    for (size_t i = 0; i < n; i++) s += d[p[i] >> 4], s += d[p[i] & 15];
    return s;
}
static string sha256_hex(const string& s) {
    BCRYPT_ALG_HANDLE a;
    BCRYPT_HASH_HANDLE h;
    UCHAR d[32];
    BCryptOpenAlgorithmProvider(&a, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    BCryptCreateHash(a, &h, nullptr, 0, nullptr, 0, 0);
    BCryptHashData(h, (PUCHAR)s.data(), (ULONG)s.size(), 0);
    BCryptFinishHash(h, d, 32, 0);
    BCryptDestroyHash(h);
    BCryptCloseAlgorithmProvider(a, 0);
    return hexs(d, 32);
}
static void save_devices() {  // gọi khi đang giữ g_devmu
    std::ofstream f(std::filesystem::path(g_devfile), std::ios::binary);
    for (auto& d : g_devices) f << d << "\n";
}
static void load_devices() {
    std::lock_guard g(g_devmu);
    g_devices.clear();
    std::stringstream ss(read_file(g_devfile));
    for (string l; std::getline(ss, l);) if (l.size() >= 64) g_devices.push_back(l.substr(0, 64));
}
static string issue_token() {
    uint8_t r[32];
    BCryptGenRandom(nullptr, r, sizeof r, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    string tok = hexs(r, sizeof r);
    std::lock_guard g(g_devmu);
    g_devices.push_back(sha256_hex(tok));
    if (g_devices.size() > 32) g_devices.erase(g_devices.begin());  // giữ 32 lần ghép gần nhất
    save_devices();
    return tok;
}
static bool token_ok(const string& tok) {
    if (tok.size() != 64) return false;
    string h = sha256_hex(tok);
    std::lock_guard g(g_devmu);
    return std::find(g_devices.begin(), g_devices.end(), h) != g_devices.end();
}
static void forget_devices() {
    std::lock_guard g(g_devmu);
    g_devices.clear();
    save_devices();
}
static size_t device_count() {
    std::lock_guard g(g_devmu);
    return g_devices.size();
}

// Địa chỉ riêng: LAN, hotspot iPhone, Tailscale/CGNAT, máy này.
static bool is_private(const string& ip) {
    int a = 0, b = 0;
    if (sscanf(ip.c_str(), "%d.%d", &a, &b) != 2) return false;
    return a == 10 || a == 127 || (a == 192 && b == 168) || (a == 172 && b >= 16 && b < 32) || (a == 169 && b == 254) ||
           (a == 100 && b >= 64 && b < 128);
}

// Sai 5 lần thì khoá địa chỉ đó 5 phút, mỗi lần sai thêm khoá gấp đôi (tối đa một ngày).
static std::mutex g_failmu;
static std::map<string, std::pair<int, Clock::time_point>> g_fail;
static bool auth_locked(const string& ip) {
    std::lock_guard g(g_failmu);
    auto it = g_fail.find(ip);
    return it != g_fail.end() && Clock::now() < it->second.second;
}
static void auth_failed(const string& ip) {
    std::lock_guard g(g_failmu);
    if (g_fail.size() > 4096)  // máy quét Internet: bỏ các địa chỉ đã hết khoá
        for (auto it = g_fail.begin(); it != g_fail.end();) it = Clock::now() >= it->second.second ? g_fail.erase(it) : std::next(it);
    auto& [n, until] = g_fail[ip];
    if (++n >= 5) {
        int mins = std::min(5 << std::min(n - 5, 8), 24 * 60);
        until = Clock::now() + std::chrono::minutes(mins);
        logf("Khoá %s trong %d phút vì sai %d lần", ip.c_str(), mins, n);
    }
}
static void auth_ok(const string& ip) {
    std::lock_guard g(g_failmu);
    g_fail.erase(ip);
}

// Dò mật khẩu từ nhiều địa chỉ Internet cùng lúc: quá 20 lần sai trong 10 phút thì 10 phút không nhận mật khẩu
// từ Internet nữa (thiết bị đã có khoá và máy trong nhà vẫn vào được).
static std::deque<Clock::time_point> g_netfail;
static Clock::time_point g_netblock;
static bool net_login_blocked() {
    std::lock_guard g(g_failmu);
    return Clock::now() < g_netblock;
}
static void net_login_failed() {
    std::lock_guard g(g_failmu);
    auto now = Clock::now();
    g_netfail.push_back(now);
    while (now - g_netfail.front() > std::chrono::minutes(10)) g_netfail.pop_front();
    if (g_netfail.size() >= 20) {
        g_netblock = now + std::chrono::minutes(10);
        g_netfail.clear();
        logf("Có nơi đang dò mật khẩu từ Internet: tạm 10 phút không nhận mật khẩu từ Internet");
    }
}

// ======================= Tài khoản đăng nhập (PC là máy chủ) =======================
// Chủ PC tạo một tài khoản; iPhone/Safari đăng nhập bằng nó từ trong nhà hay từ Internet.
// Mật khẩu không lưu, chỉ lưu PBKDF2-HMAC-SHA256 600 000 vòng (mức OWASP khuyên) với muối ngẫu nhiên.
// remote_account.txt: tên, muối (chuỗi hex 32 ký tự, dùng nguyên chuỗi làm muối), số vòng, băm hex.
constexpr int ACC_ITER = 600000;
static std::mutex g_accmu;
static std::wstring g_accfile;  // remote_account.txt cạnh exe
static string g_acc_user, g_acc_salt, g_acc_hash;
static int g_acc_iter = 0;

static string pbkdf2_hex(const string& pass, const string& salt, int iter) {
    BCRYPT_ALG_HANDLE a;
    UCHAR out[32];
    if (BCryptOpenAlgorithmProvider(&a, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG)) return "";
    NTSTATUS st = BCryptDeriveKeyPBKDF2(a, (PUCHAR)pass.data(), (ULONG)pass.size(), (PUCHAR)salt.data(), (ULONG)salt.size(),
                                        (ULONGLONG)iter, out, sizeof out, 0);
    BCryptCloseAlgorithmProvider(a, 0);
    return st ? "" : hexs(out, sizeof out);
}
static string trim(string u) {
    u.erase(0, u.find_first_not_of(" \t\r\n"));
    u.erase(u.find_last_not_of(" \t\r\n") + 1);
    return u;
}
static string acc_key(string u) {  // so tên không phân biệt hoa thường (chữ Latin)
    u = trim(u);
    for (auto& c : u) if (c >= 'A' && c <= 'Z') c += 32;
    return u;
}
static void load_account() {
    std::stringstream ss(read_file(g_accfile));
    string u, salt, it, h;
    std::getline(ss, u), std::getline(ss, salt), std::getline(ss, it), std::getline(ss, h);
    u = trim(u), salt = trim(salt), h = trim(h);
    bool ok = !u.empty() && salt.size() >= 16 && atoi(it.c_str()) > 0 && h.size() == 64;
    std::lock_guard g(g_accmu);
    g_acc_user = ok ? u : "", g_acc_salt = ok ? salt : "", g_acc_hash = ok ? h : "", g_acc_iter = ok ? atoi(it.c_str()) : 0;
}
static bool has_account() {
    std::lock_guard g(g_accmu);
    return !g_acc_hash.empty();
}
static string account_user() {
    std::lock_guard g(g_accmu);
    return g_acc_user;
}
// Tạo hoặc đổi tài khoản. Trả về lời báo lỗi, rỗng là được.
static string set_account(const string& user, const string& pass) {
    string u = trim(user);
    size_t chars = 0;
    for (unsigned char c : pass) chars += (c & 0xC0) != 0x80;  // đếm ký tự UTF-8, không đếm byte
    if (u.empty() || u.size() > 64) return "Tên đăng nhập cần từ 1 đến 64 ký tự.";
    if (chars < 8) return "Mật khẩu cần ít nhất 8 ký tự.";
    if (pass.size() > 1024) return "Mật khẩu quá dài.";
    uint8_t r[16];
    BCryptGenRandom(nullptr, r, sizeof r, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    string salt = hexs(r, sizeof r), h = pbkdf2_hex(pass, salt, ACC_ITER);
    if (h.empty()) return "Không băm được mật khẩu (BCrypt).";
    {
        std::ofstream f(std::filesystem::path(g_accfile), std::ios::binary | std::ios::trunc);
        f << u << "\n" << salt << "\n" << ACC_ITER << "\n" << h << "\n";
        if (!f) return "Không ghi được remote_account.txt.";
    }
    std::lock_guard g(g_accmu);
    g_acc_user = u, g_acc_salt = salt, g_acc_hash = h, g_acc_iter = ACC_ITER;
    return "";
}
static bool check_login(const string& user, const string& pass) {
    string u, salt, h;
    int it;
    {
        std::lock_guard g(g_accmu);
        u = g_acc_user, salt = g_acc_salt, h = g_acc_hash, it = g_acc_iter;
    }
    if (h.empty() || pass.size() > 1024) return false;
    string got = pbkdf2_hex(pass, salt, it);  // luôn băm, kể cả khi sai tên: thời gian trả lời không lộ tên đúng
    unsigned char d = acc_key(user) != acc_key(u) || got.size() != h.size();
    for (size_t i = 0; i < got.size() && i < h.size(); i++) d |= got[i] ^ h[i];
    return d == 0;
}

// ======================= Mở cổng trên router (UPnP IGD) =======================
// iPhone ở ngoài vào thẳng PC qua IP công khai của nhà, không qua máy chủ hay dịch vụ nào (giống Moonlight/Sunshine).
// Kênh đã mã hoá TLS (chứng chỉ được app ghim) và chỉ nhận khoá thiết bị đã ghép.
struct Igd {
    string host, control, service, local, ext_ip;
    int port = 0, ext_port = 0;
};
static std::mutex g_igdmu;
static Igd g_igd;  // cổng đang mở (ext_port = 0: chưa mở)
static string g_netstatus = "Tắt: chỉ vào được trong mạng nhà.";

static string lower(string s) {
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    return s;
}
static bool split_url(const string& url, string& host, int& port, string& path) {
    if (url.compare(0, 7, "http://")) return false;
    size_t slash = url.find('/', 7);
    string hp = url.substr(7, slash == string::npos ? string::npos : slash - 7);
    path = slash == string::npos ? "/" : url.substr(slash);
    size_t c = hp.find(':');
    host = hp.substr(0, c);
    port = c == string::npos ? 80 : atoi(hp.c_str() + c + 1);
    return !host.empty() && port > 0;
}
static string xml_tag(const string& x, const string& tag, size_t from = 0) {
    size_t a = x.find("<" + tag + ">", from);
    if (a == string::npos) return "";
    a += tag.size() + 2;
    size_t b = x.find("</" + tag + ">", a);
    return b == string::npos ? "" : x.substr(a, b - a);
}
static bool http_req(const string& host, int port, const string& req, string& resp, string* local = nullptr) {
    sockaddr_in a{};
    a.sin_family = AF_INET, a.sin_port = htons((u_short)port);
    if (inet_pton(AF_INET, host.c_str(), &a.sin_addr) != 1) return false;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    DWORD to = 3000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char*)&to, sizeof to);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (char*)&to, sizeof to);
    bool ok = !connect(s, (sockaddr*)&a, sizeof a) && send_all(s, req.data(), req.size());
    if (ok && local) {  // IP của PC phía router: dùng làm NewInternalClient
        sockaddr_in me{};
        int n = sizeof me;
        char b[32];
        if (!getsockname(s, (sockaddr*)&me, &n) && inet_ntop(AF_INET, &me.sin_addr, b, sizeof b)) *local = b;
    }
    resp.clear();
    char b[4096];
    for (int n; ok && (n = recv(s, b, sizeof b, 0)) > 0;) resp.append(b, n);
    closesocket(s);
    return ok && !resp.empty();
}
static bool igd_discover(Igd& u) {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    DWORD to = 2500;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char*)&to, sizeof to);
    for (auto& ip : reachable_ips())
        if (!is_tailscale(ip)) {  // hỏi qua card mạng nhà, không qua VPN
            in_addr ifa;
            inet_pton(AF_INET, ip.c_str(), &ifa);
            setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF, (char*)&ifa, sizeof ifa);
            break;
        }
    sockaddr_in a{};
    a.sin_family = AF_INET, a.sin_port = htons(1900);
    inet_pton(AF_INET, "239.255.255.250", &a.sin_addr);
    string q = "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 2\r\n"
               "ST: urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\n\r\n";
    sendto(s, q.data(), (int)q.size(), 0, (sockaddr*)&a, sizeof a);
    string loc;
    char b[2048];
    for (int i = 0; i < 10 && loc.empty(); i++) {
        int n = recvfrom(s, b, sizeof b, 0, nullptr, nullptr);
        if (n <= 0) break;
        string r(b, n), l = lower(r);
        size_t p = l.find("\r\nlocation:");
        if (p == string::npos) continue;
        p += 11;
        loc = r.substr(p, r.find("\r\n", p) - p);
        loc.erase(0, loc.find_first_not_of(' '));
    }
    closesocket(s);
    string path, desc;
    if (loc.empty() || !split_url(loc, u.host, u.port, path) ||
        !http_req(u.host, u.port, "GET " + path + " HTTP/1.1\r\nHost: " + u.host + ":" + std::to_string(u.port) +
                                      "\r\nConnection: close\r\n\r\n", desc, &u.local))
        return false;
    for (const char* svc : {"urn:schemas-upnp-org:service:WANIPConnection:2", "urn:schemas-upnp-org:service:WANIPConnection:1",
                            "urn:schemas-upnp-org:service:WANPPPConnection:1"}) {
        size_t p = desc.find(svc);
        string ctl = p == string::npos ? "" : xml_tag(desc, "controlURL", p), h, cp;
        if (ctl.empty()) continue;
        int pt;
        if (split_url(ctl, h, pt, cp)) ctl = cp;
        if (ctl[0] != '/') ctl = "/" + ctl;
        u.control = ctl, u.service = svc;
        return true;
    }
    return false;
}
static bool igd_call(const Igd& u, const string& action, const string& args, string& resp) {
    string body = "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
                  "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:" + action + " xmlns:u=\"" +
                  u.service + "\">" + args + "</u:" + action + "></s:Body></s:Envelope>";
    string req = "POST " + u.control + " HTTP/1.1\r\nHost: " + u.host + ":" + std::to_string(u.port) +
                 "\r\nContent-Type: text/xml; charset=\"utf-8\"\r\nSOAPAction: \"" + u.service + "#" + action +
                 "\"\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
    return http_req(u.host, u.port, req, resp) && resp.compare(0, 12, "HTTP/1.1 200") == 0;
}
static bool igd_map(Igd& u) {
    string r;
    if (igd_call(u, "GetExternalIPAddress", "", r)) u.ext_ip = xml_tag(r, "NewExternalIPAddress");
    for (int port = g_port; port < g_port + 6; port++)
        for (int lease : {3600, 0}) {  // vài router chỉ nhận cổng vĩnh viễn (lỗi 725)
            string args = "<NewRemoteHost></NewRemoteHost><NewExternalPort>" + std::to_string(port) +
                          "</NewExternalPort><NewProtocol>TCP</NewProtocol><NewInternalPort>" + std::to_string(g_port) +
                          "</NewInternalPort><NewInternalClient>" + u.local + "</NewInternalClient><NewEnabled>1</NewEnabled>"
                          "<NewPortMappingDescription>Remote Desk</NewPortMappingDescription><NewLeaseDuration>" +
                          std::to_string(lease) + "</NewLeaseDuration>";
            if (igd_call(u, "AddPortMapping", args, r)) { u.ext_port = port; return true; }
            if (r.find("725") == string::npos) break;  // lỗi khác (vd. 718: cổng đã có máy khác dùng): thử cổng kế
        }
    return false;
}
static void igd_unmap(const Igd& u) {
    if (!u.ext_port) return;
    string r;
    igd_call(u, "DeletePortMapping", "<NewRemoteHost></NewRemoteHost><NewExternalPort>" + std::to_string(u.ext_port) +
                                         "</NewExternalPort><NewProtocol>TCP</NewProtocol>", r);
}
static void set_netstatus(const string& m) {
    std::lock_guard g(g_igdmu);
    g_netstatus = m;
}
// IP công khai thật khi nhà có hai lớp router (UPnP chỉ thấy IP phía modem). Chỉ hỏi địa chỉ qua Cloudflare,
// không cần tài khoản; hình và điều khiển không đi qua đây.
static string public_ip() {
    string ip, body;
    HINTERNET ses = WinHttpOpen(L"RemoteDesk", WINHTTP_ACCESS_TYPE_NO_PROXY, nullptr, nullptr, 0);
    HINTERNET con = ses ? WinHttpConnect(ses, L"1.1.1.1", INTERNET_DEFAULT_HTTPS_PORT, 0) : nullptr;
    HINTERNET req = con ? WinHttpOpenRequest(con, L"GET", L"/cdn-cgi/trace", nullptr, nullptr, nullptr, WINHTTP_FLAG_SECURE) : nullptr;
    if (req) WinHttpSetTimeouts(req, 3000, 3000, 3000, 3000);
    if (req && WinHttpSendRequest(req, nullptr, 0, nullptr, 0, 0, 0) && WinHttpReceiveResponse(req, nullptr)) {
        char b[1024];
        DWORD n;
        while (WinHttpReadData(req, b, sizeof b, &n) && n) body.append(b, n);
    }
    for (HINTERNET h : {req, con, ses}) if (h) WinHttpCloseHandle(h);
    size_t p = body.find("\nip=");
    if (p != string::npos) ip = body.substr(p + 4, body.find('\n', p + 4) - p - 4);
    return ip.find_first_not_of("0123456789.") == string::npos ? ip : "";  // chỉ nhận IPv4
}
// Thử nối TCP (chờ tối đa 3 giây). Từ PC nối vào IP công khai của chính nhà mình chỉ thông khi modem đã chuyển cổng
// và hỗ trợ NAT vòng (hairpin): thông là chắc chắn mở, không thông thì chưa chắc.
static bool port_open(const string& ip, int port) {
    sockaddr_in a{};
    a.sin_family = AF_INET, a.sin_port = htons((u_short)port);
    if (inet_pton(AF_INET, ip.c_str(), &a.sin_addr) != 1) return false;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    connect(s, (sockaddr*)&a, sizeof a);
    fd_set w, e;
    FD_ZERO(&w);  // FD_* là macro do{...}while(0): không nối bằng dấu phẩy được
    FD_ZERO(&e);
    FD_SET(s, &w);
    FD_SET(s, &e);
    timeval tv{3, 0};
    bool ok = select(0, nullptr, &w, &e, &tv) == 1 && FD_ISSET(s, &w);
    closesocket(s);
    return ok;
}
// Luồng nền: bật "Từ Internet" thì mở cổng trên router và gia hạn mỗi 20 phút; tắt thì đóng cổng.
static void remote_loop() {
    auto next = Clock::now();
    bool was = false, told = false;
    string lan;
    for (auto& ip : reachable_ips()) if (lan.empty() && !is_tailscale(ip)) lan = ip;
    const string port = std::to_string(g_port);
    for (;; Sleep(1000)) {
        bool on = g_remote;
        if (!on && was) {
            Igd u;
            {
                std::lock_guard g(g_igdmu);
                u = g_igd;
                g_igd = Igd{};
                g_netstatus = "Tắt: chỉ vào được trong mạng nhà.";
            }
            igd_unmap(u);
            logf("Đã đóng cổng trên router");
            was = false;
        }
        if (!on || (was && Clock::now() < next)) continue;
        was = true;
        next = Clock::now() + std::chrono::minutes(1);  // lỗi thì một phút sau thử lại
        Igd u;
        if (!igd_discover(u)) { set_netstatus("Router không bật UPnP: tự chuyển cổng TCP " + port + " về " + lan + " trên router."); continue; }
        if (!igd_map(u)) { set_netstatus("Router từ chối mở cổng " + port + " (UPnP)."); continue; }
        string wan = u.ext_ip;  // IP phía Internet của router này
        bool two = wan.empty() || is_private(wan);
        string pub = two ? public_ip() : wan;
        if (pub.empty()) { set_netstatus("Đã mở cổng trên router nhưng không biết IP công khai của nhà."); continue; }
        bool reach = port_open(pub, u.ext_port);
        if (two && !told) {
            string modem = wan.substr(0, wan.rfind('.') + 1) + "1";
            logf("Nhà có 2 lớp router: modem nhà mạng (thường là %s) → router %s → PC. Đã tự mở cổng trên router trong. "
                 "Còn một việc làm tay một lần: vào trang quản trị modem, chuyển cổng TCP %d về %s:%d (hoặc đặt DMZ = %s).",
                 modem.c_str(), wan.c_str(), u.ext_port, wan.c_str(), u.ext_port, wan.c_str());
            told = true;
        }
        bool fresh;
        {
            std::lock_guard g(g_igdmu);
            fresh = g_igd.ext_ip != pub || g_igd.ext_port != u.ext_port;
            g_igd = u;
            g_igd.ext_ip = pub;  // địa chỉ iPhone dùng từ ngoài
            g_netstatus = reach ? "Đang mở: " + pub + ":" + std::to_string(u.ext_port) + " (đã thử thông)"
                          : two ? "Cần chuyển cổng " + std::to_string(u.ext_port) + " trên modem nhà mạng (xem nhật ký)"
                                : "Đang mở: " + pub + ":" + std::to_string(u.ext_port) + " (chưa thử được từ PC, hãy thử bằng 4G)";
        }
        if (fresh) logf("Cổng từ Internet: %s:%d%s", pub.c_str(), u.ext_port, reach ? " (đã thử thông)" : "");
        next = Clock::now() + std::chrono::minutes(reach ? 20 : 2);  // chưa thông thì 2 phút kiểm lại (chờ bạn sửa modem)
    }
}
// Địa chỉ gửi cho app: LAN, Tailscale, rồi IP công khai nếu đã mở cổng.
static std::vector<string> client_addrs() {
    std::vector<string> v;
    for (auto& ip : reachable_ips()) v.push_back(ip + ":" + std::to_string(g_port));
    std::lock_guard g(g_igdmu);
    if (g_igd.ext_port) v.push_back(g_igd.ext_ip + ":" + std::to_string(g_igd.ext_port));
    return v;
}

// ======================= Phiên điều khiển =======================
struct Session;
static std::mutex g_cmu;
static Session* g_current = nullptr;

struct Session {
    Tls t;
    std::atomic<bool> stop{false}, want_key{true}, restart{false};
    std::atomic<uint64_t> frames{0}, bytes{0};
    std::atomic<int> live_mbps{0};  // bitrate đang dùng sau khi tự điều chỉnh theo mạng
    std::mutex info_mu;
    string enc_name, peer = "?";
    int vw = 0, vh = 0;
    bool client_hevc = false;  // client báo giải mã được HEVC
    explicit Session(SOCKET s) : t(s) {
        sockaddr_in a{};
        int n = sizeof a;
        char b[32];
        if (!getpeername(s, (sockaddr*)&a, &n) && inet_ntop(AF_INET, &a.sin_addr, b, sizeof b)) peer = b;
    }

    // Một lượt phát với cài đặt hiện tại; trả về khi dừng, khi đổi cài đặt (restart) hoặc lỗi.
    void stream_once() {
        Capture cap;
        Encoder enc;
        int fps = g_fps, mbps = g_mbps, width = g_width;
        if (!cap.init()) { logf("Không chụp được màn hình (DXGI)"); stop = true; return; }
        int dw = std::min(width ? width : cap.w, cap.w) & ~1, dh = (int)((int64_t)cap.h * dw / cap.w) & ~1;
        bool want_hevc = g_hevc && client_hevc;
        if (!enc.init(dw, dh, fps, mbps, 0, want_hevc) && !(want_hevc && enc.init(dw, dh, fps, mbps))) {
            logf("Không mở được encoder");
            stop = true;
            return;
        }
        if (want_hevc && !enc.hevc) logf("Máy này không mã hoá được HEVC, dùng H.264");
        {
            std::lock_guard g(info_mu);
            enc_name = enc.name, vw = dw, vh = dh;
        }
        logf("Phát tới %s: %dx%d, %d fps, %d Mbps, %s", peer.c_str(), dw, dh, fps, mbps, enc.name.c_str());
        std::vector<uint8_t> nv12((size_t)dw * dh * 3 / 2);
        std::vector<string> out;
        auto period = std::chrono::microseconds(1000000 / fps);
        auto last = Clock::now() - period, win = Clock::now();
        Clock::duration busy{};  // thời gian bị kẹt khi gửi trong giây hiện tại
        int cur = mbps, calm = 0;
        bool dirty = false, abr_warned = false;
        live_mbps = mbps;
        for (int64_t n = 1; !stop && !restart;) {
            int r = cap.grab((UINT)(period.count() / 2000 + 1));
            if (r < 0) { Sleep(100); if (!cap.init()) Sleep(500); dirty = true; continue; }  // UAC, đổi độ phân giải...
            dirty |= r > 0;
            if (auto now = Clock::now(); now - win >= std::chrono::seconds(1)) {  // mỗi giây: mạng có theo kịp bitrate không
                double frac = std::chrono::duration<double>(busy).count() / std::chrono::duration<double>(now - win).count();
                int want = adapt_bitrate(cur, mbps, frac, calm);
                if (want != cur && enc.setv(CODECAPI_AVEncCommonMeanBitRate, want * 1000000)) {
                    logf(want < cur ? "Mạng chậm: hạ bitrate %d → %d Mbps" : "Mạng ổn lại: nâng bitrate %d → %d Mbps", cur, want);
                    cur = want, live_mbps = want;
                } else if (want != cur && !abr_warned) {
                    logf("Encoder không cho đổi bitrate khi đang phát; hãy hạ bitrate bằng tay nếu hình bị trễ");
                    abr_warned = true;
                }
                win = now, busy = {};
            }
            bool key = want_key.load();
            if ((!dirty && !key) || cap.bgra.empty() || (Clock::now() - last < period && !key)) continue;
            last = Clock::now(), dirty = false, want_key = false;
            cap.cursor(true);
            bgra_to_nv12(cap.bgra.data(), cap.w, cap.h, nv12.data(), dw, dh);
            cap.cursor(false);
            out.clear();
            if (!enc.encode(nv12.data(), n++, key, out)) { logf("Encoder lỗi"); stop = true; break; }
            for (auto& o : out) {
                auto t0 = Clock::now();
                // byte đầu: bit 0 = khung khoá, bit 1 = HEVC
                if (!ws_send(t, 2, string(1, (char)(is_key(o, enc.hevc) | (enc.hevc ? 2 : 0))) + o)) { stop = true; break; }
                busy += Clock::now() - t0;
                frames++, bytes += o.size();
            }
        }
        enc.mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
    }

    void video() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        while (!stop) restart = false, want_key = true, stream_once();
        CoUninitialize();
        shutdown(t.s, SD_BOTH);  // để vòng đọc thoát
    }

    // 4000 = thiết bị khác đã kết nối, 4001 = PC ngắt (client không tự nối lại)
    void kick(int code, const char* reason) {
        ws_send(t, 8, string{(char)(code >> 8), (char)(code & 255)} + reason);
        shutdown(t.s, SD_BOTH);
    }

    void run() {
        if (!t.handshake()) return;
        size_t e;
        while ((e = t.plain.find("\r\n\r\n")) == string::npos) if (t.plain.size() > 16384 || !t.fill()) return;
        string req = t.plain.substr(0, e + 2), low = req;
        t.plain.erase(0, e + 4);
        std::transform(low.begin(), low.end(), low.begin(), ::tolower);
        size_t sp1 = req.find(' '), sp2 = req.find(' ', sp1 + 1);
        string path = sp1 == string::npos || sp2 == string::npos ? "" : req.substr(sp1 + 1, sp2 - sp1 - 1);
        path = path.substr(0, path.find('?'));
        size_t kp = low.find("\r\nsec-websocket-key:");
        if (kp == string::npos || path != "/ws") {
            string body, ct = "text/html; charset=utf-8", status = "200 OK";
            if (path == "/") body = read_file(g_dir + L"\\remote_desktop.html");
            else if (path == "/remote.cer") body.assign((char*)g_cert->pbCertEncoded, g_cert->cbCertEncoded), ct = "application/x-x509-ca-cert";
            else status = "404 Not Found", ct = "text/plain", body = "not found";
            string r = "HTTP/1.1 " + status + "\r\nContent-Type: " + ct + "\r\nContent-Length: " + std::to_string(body.size()) +
                       "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n" + body;
            t.send(r.data(), r.size());
            return;
        }
        kp += 20;
        string key = req.substr(kp, req.find("\r\n", kp) - kp);
        key.erase(0, key.find_first_not_of(' '));
        key.erase(key.find_last_not_of(' ') + 1);
        string r = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " +
                   ws_accept(key) + "\r\n\r\n";
        if (!t.send(r.data(), r.size())) return;

        int op;
        string m, user, pass, tok, issued;
        if (!ws_read(t, op, m)) return;
        jstr(m, "user", user);
        jstr(m, "pass", pass);
        jstr(m, "token", tok);
        size_t hv = jfind(m, "hevc");
        client_hevc = hv != string::npos && m.compare(hv, 4, "true") == 0;
        bool local = is_private(peer);
        if (auth_locked(peer)) {
            ws_send(t, 1, R"({"t":"locked"})");
            return;
        }
        bool ok = token_ok(tok);
        if (!ok && !has_account()) {
            ws_send(t, 1, R"({"t":"noaccount"})");
            return;
        }
        if (!ok && !pass.empty()) {
            if (!local && net_login_blocked()) {
                ws_send(t, 1, R"({"t":"locked"})");
                return;
            }
            ok = check_login(user, pass);
            SecureZeroMemory(pass.data(), pass.size());
            if (ok) {
                issued = issue_token();
                logf("Đăng nhập tài khoản từ %s: đã cấp khoá cho thiết bị mới", peer.c_str());
            }
        }
        if (!ok) {
            auth_failed(peer);
            if (!local) net_login_failed();
            Sleep(1000);  // chậm lại để khỏi dò
            ws_send(t, 1, R"({"t":"badlogin"})");
            return;
        }
        auth_ok(peer);
        if (!local && issued.empty()) logf("Thiết bị đã đăng nhập vào từ Internet (%s)", peer.c_str());
        {
            std::lock_guard g(g_cmu);
            if (g_current) g_current->kick(4000, "client khác đã kết nối");
            g_current = this;
        }
        string km = read_file(g_dir + L"\\remote_keymap.json");
        string addrs;  // mọi địa chỉ của PC (LAN, Tailscale, IP công khai) để app tự chọn khi ở nhà hay ở ngoài
        for (auto& a : client_addrs()) addrs += (addrs.empty() ? "\"" : ",\"") + a + "\"";
        ws_send(t, 1, R"({"t":"hello","addrs":[)" + addrs + "]" + (issued.empty() ? "" : R"(,"token":")" + issued + "\"") +
                          R"(,"keymap":)" + (km.empty() ? string("null") : km) + "}");
        std::thread vt(&Session::video, this);
        while (ws_read(t, op, m)) {
            string ty;
            if (op != 1 || !jstr(m, "t", ty)) continue;
            if (ty == "key") want_key = true;
            else if (ty == "ping") {
                size_t p = jfind(m, "c"), q = p == string::npos ? p : m.find_first_not_of("-0123456789.eE+", p);
                std::lock_guard g(info_mu);
                ws_send(t, 1, R"({"t":"pong","c":)" + (p == string::npos ? string("0") : m.substr(p, q - p)) + R"(,"enc":")" +
                                  jesc(enc_name) + R"(","size":[)" + std::to_string(vw) + "," + std::to_string(vh) + "]}");
            } else if (ty == "keymap") {
                size_t p = jfind(m, "data");
                string d = p == string::npos ? "" : m.substr(p, m.size() - p - 1);
                if (d.size() > 1 && d.size() < 65536 && d.front() == '[' && d.back() == ']')
                    std::ofstream(std::filesystem::path(g_dir + L"\\remote_keymap.json"), std::ios::binary) << d;
            } else {
                std::vector<INPUT> in;
                if (translate(m, in)) { if (!in.empty()) SendInput((UINT)in.size(), in.data(), sizeof(INPUT)); }
                else logf("Bỏ tin nhắn lạ: %.200s", m.c_str());
            }
        }
        stop = true;
        vt.join();
        std::lock_guard g(g_cmu);
        if (g_current == this) g_current = nullptr;
    }
};

static void client_thread(SOCKET s) {
    DWORD to = 10000;
    BOOL nd = TRUE;
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (char*)&to, sizeof to);  // client treo thì bỏ, không kẹt luồng hình
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (char*)&nd, sizeof nd);
    // Bộ đệm gửi nhỏ cố định (Windows tự nới tới vài MB): mạng chậm thì hình không dồn hàng giây trong hàng đợi,
    // việc gửi bị chặn sớm và bitrate tự hạ. 256 KB vẫn đủ ~40 Mbps ở ping 50 ms.
    int sndbuf = 256 * 1024;
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, (char*)&sndbuf, sizeof sndbuf);
    {
        Session ss(s);
        ss.run();
    }
    // đóng êm: closesocket ngay khi còn dữ liệu chưa đọc sẽ gửi RST và client mất tin nhắn cuối
    shutdown(s, SD_SEND);
    to = 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char*)&to, sizeof to);
    char b[4096];
    while (recv(s, b, sizeof b, 0) > 0) {}
    closesocket(s);
}

static SOCKET listen_on(int port) {
    SOCKET l = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{};
    a.sin_family = AF_INET, a.sin_port = htons((u_short)port);
    if (bind(l, (sockaddr*)&a, sizeof a) || listen(l, 16)) { closesocket(l); return INVALID_SOCKET; }
    return l;
}

static bool setup_tls(const std::wstring& cn, bool& created) {
    g_cert = ensure_cert(cn, local_ips(), created);
    if (!g_cert) return false;
    SCHANNEL_CRED sc{};
    sc.dwVersion = SCHANNEL_CRED_VERSION;
    sc.cCreds = 1;
    sc.paCred = &g_cert;
    sc.grbitEnabledProtocols = SP_PROT_TLS1_2_SERVER;
    return AcquireCredentialsHandleW(nullptr, (LPWSTR)UNISP_NAME_W, SECPKG_CRED_INBOUND, nullptr, &sc, nullptr, nullptr,
                                     &g_cred, nullptr) == SEC_E_OK;
}

// ======================= Giao diện Windows: kiểu "white gallery" của Apple, có bản tối =======================
// Nền trắng/#f5f5f7, chữ Ink, thẻ bo 28 px không bóng, nút pill viền mảnh, phân đoạn kiểu Apple.
// Theo chế độ sáng/tối của Windows (App mode) và tự đổi khi người dùng đổi.
enum { ID_ACCOUNT = 101, ID_KICK, ID_FORGET, ID_ACC_SAVE, ID_ACC_CANCEL, ID_ACC_EDIT = 120, ID_SEG = 200 };  // phân đoạn: ID_SEG + nhóm*10 + vị trí
static const int RES_W[] = {1280, 1920, 2560, 0}, FPS_V[] = {30, 60, 90, 120}, RATE_V[] = {5, 10, 20, 30, 50};
static DNS_SERVICE_REGISTER_REQUEST g_reg{};

static std::wstring wide(const string& s) {
    std::wstring w(MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0), 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), (int)w.size());
    return w;
}
static std::wstring ini_path() { return g_dir + L"\\remote_settings.ini"; }
static int pick(const int* v, int n, int x, int def) {
    for (int i = 0; i < n; i++) if (v[i] == x) return i;
    return def;
}
static void load_settings() {
    auto ini = ini_path();
    g_width = RES_W[pick(RES_W, 4, GetPrivateProfileIntW(L"stream", L"width", 1920, ini.c_str()), 1)];
    g_fps = FPS_V[pick(FPS_V, 4, GetPrivateProfileIntW(L"stream", L"fps", 60, ini.c_str()), 1)];
    g_mbps = RATE_V[pick(RATE_V, 5, GetPrivateProfileIntW(L"stream", L"mbps", 20, ini.c_str()), 2)];
    g_hevc = GetPrivateProfileIntW(L"stream", L"hevc", 1, ini.c_str()) != 0;
    g_remote = GetPrivateProfileIntW(L"net", L"remote", 0, ini.c_str()) != 0;
}
static void save_settings() {
    auto ini = ini_path();
    WritePrivateProfileStringW(L"stream", L"width", std::to_wstring(g_width).c_str(), ini.c_str());
    WritePrivateProfileStringW(L"stream", L"fps", std::to_wstring(g_fps).c_str(), ini.c_str());
    WritePrivateProfileStringW(L"stream", L"mbps", std::to_wstring(g_mbps).c_str(), ini.c_str());
    WritePrivateProfileStringW(L"stream", L"hevc", g_hevc ? L"1" : L"0", ini.c_str());
    WritePrivateProfileStringW(L"net", L"remote", g_remote ? L"1" : L"0", ini.c_str());
}

// Quảng bá _remotedesk._tcp qua mDNS (API có sẵn từ Windows 10 1809) để app iPhone tự thấy PC.
static VOID WINAPI mdns_done(DWORD status, PVOID, PDNS_SERVICE_INSTANCE inst) {
    if (status) logf("Không quảng bá được trên mạng LAN (mã %lu); trên iPhone hãy nhập IP bằng tay", status);
    else logf("Đã quảng bá PC trên mạng LAN (mDNS)");
    if (inst) DnsServiceFreeInstance(inst);
}
static void mdns_register() {
    wchar_t comp[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD n = ARRAYSIZE(comp);
    GetComputerNameW(comp, &n);
    std::wstring inst = std::wstring(comp) + L"._remotedesk._tcp.local", host = std::wstring(comp) + L".local";
    IP4_ADDRESS ip = 0;
    for (auto& s : reachable_ips()) if (!ip) inet_pton(AF_INET, s.c_str(), &ip);  // mDNS chỉ có trong LAN: ưu tiên IP LAN
    PCWSTR keys[] = {L"v"}, vals[] = {L"1"};
    g_reg.Version = DNS_QUERY_REQUEST_VERSION1;
    g_reg.pServiceInstance = DnsServiceConstructInstance(inst.c_str(), host.c_str(), ip ? &ip : nullptr, nullptr,
                                                         (WORD)g_port, 0, 0, 1, keys, vals);
    g_reg.pRegisterCompletionCallback = mdns_done;
    if (!g_reg.pServiceInstance || DnsServiceRegister(&g_reg, nullptr) != DNS_REQUEST_PENDING)
        logf("Không quảng bá được trên mạng LAN; trên iPhone hãy nhập IP bằng tay");
}

// ---- Bảng màu (token của bản hướng dẫn; bản tối theo trang tối của Apple) ----
enum Role { R_CANVAS, R_BAND, R_CARD, R_HAIR, R_TRACK, R_SEGON, R_INK, R_SLATE, R_STEEL, R_BLUE, R_ORANGE, R_COUNT };
static const COLORREF LIGHT[R_COUNT] = {RGB(255, 255, 255), RGB(245, 245, 247), RGB(255, 255, 255), RGB(214, 214, 214),
                                        RGB(230, 230, 232), RGB(255, 255, 255), RGB(29, 29, 31), RGB(112, 112, 112),
                                        RGB(134, 134, 139), RGB(0, 113, 227), RGB(182, 68, 0)};
static const COLORREF DARK[R_COUNT] = {RGB(0, 0, 0), RGB(22, 22, 23), RGB(29, 29, 31), RGB(66, 66, 69),
                                       RGB(51, 51, 54), RGB(99, 99, 102), RGB(245, 245, 247), RGB(161, 161, 166),
                                       RGB(134, 134, 139), RGB(0, 113, 227), RGB(245, 99, 0)};
static const COLORREF* P = LIGHT;
static HBRUSH g_brush[R_COUNT];
static bool g_dark;

enum Font { F_KICKER, F_LIVE, F_HEAD, F_BODY, F_SMALL, F_BIG, F_TITLE, F_COUNT };
static HFONT g_font[F_COUNT];

// Bố cục theo 96 DPI. Dải #f5f5f7 từ BAND_Y; hai thẻ trắng bo 28 px.
constexpr int WIN_W = 520, WIN_H = 856, BAND_Y = 300, TRACK_X = 196, TRACK_W = 280, SEG_H = 30;
static const RECT CARD_Q = {20, 320, 500, 540}, CARD_NET = {20, 556, 500, 686}, CARD_LOG = {20, 702, 500, 840};
static const int ROW_Y[4] = {384, 422, 460, 498}, NET_Y = 618;
struct Ctl { HWND h; int x, y, w, h_; Font font; Role fg, bg; HWND parent; };  // fg = R_BLUE: nút chính tô xanh
struct Seg { HWND h; int grp, idx, n, x, w, y; };  // rãnh phân đoạn của nhóm: x, w, y theo 96 DPI
static std::vector<Ctl> g_ctls;
static std::vector<Seg> g_segs;
static HWND h_kicker, h_live, h_status, h_detail, h_acct, h_accbtn, h_kick, h_log, h_net;
static HWND g_dlg, h_dlgerr;  // hộp tạo/đổi tài khoản
static std::vector<HWND> g_dlg_edits;
static int g_sel[5];
static bool g_streaming;
static ULONG_PTR g_gdip;

static int S(int v) { return MulDiv(v, GetDpiForWindow(g_hwnd), 96); }
static Gdiplus::Color gc(COLORREF c) { return Gdiplus::Color(255, GetRValue(c), GetGValue(c), GetBValue(c)); }

static void rrect(Gdiplus::Graphics& g, float x, float y, float w, float h, float r, COLORREF fill, COLORREF line = CLR_INVALID,
                  float lw = 1) {
    Gdiplus::GraphicsPath p;
    float d = std::min({r * 2, w, h});
    p.AddArc(x, y, d, d, 180, 90);
    p.AddArc(x + w - d, y, d, d, 270, 90);
    p.AddArc(x + w - d, y + h - d, d, d, 0, 90);
    p.AddArc(x, y + h - d, d, d, 90, 90);
    p.CloseFigure();
    if (fill != CLR_INVALID) {
        Gdiplus::SolidBrush b(gc(fill));
        g.FillPath(&b, &p);
    }
    if (line != CLR_INVALID) {
        Gdiplus::Pen pen(gc(line), lw);
        g.DrawPath(&pen, &p);
    }
}

static int g_force_theme = -1;  // --theme light|dark; mặc định theo Windows
static bool system_dark() {
    if (g_force_theme >= 0) return g_force_theme;
    DWORD v = 1, n = sizeof v;
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme",
                 RRF_RT_REG_DWORD, nullptr, &v, &n);
    return v == 0;
}
static void apply_theme() {
    g_dark = system_dark();
    P = g_dark ? DARK : LIGHT;
    for (int i = 0; i < R_COUNT; i++) {
        if (g_brush[i]) DeleteObject(g_brush[i]);
        g_brush[i] = CreateSolidBrush(P[i]);
    }
    BOOL dark = g_dark;
    COLORREF cap = P[R_CANVAS], txt = P[R_INK], border = P[R_HAIR];
    for (HWND w : {g_hwnd, g_dlg}) {
        if (!w) continue;
        DwmSetWindowAttribute(w, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof dark);
        DwmSetWindowAttribute(w, DWMWA_CAPTION_COLOR, &cap, sizeof cap);  // thanh tiêu đề cùng màu nền (Windows 11)
        DwmSetWindowAttribute(w, DWMWA_TEXT_COLOR, &txt, sizeof txt);
        DwmSetWindowAttribute(w, DWMWA_BORDER_COLOR, &border, sizeof border);
        RedrawWindow(w, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
    }
}

static void layout() {
    auto mk = [](const wchar_t* face, int px, int weight) {
        return CreateFontW(-S(px), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, face);
    };
    for (HFONT& f : g_font) if (f) DeleteObject(f), f = nullptr;
    const wchar_t *disp = L"Segoe UI Variable Display", *text = L"Segoe UI Variable Text";  // thay cho SF Pro
    g_font[F_KICKER] = mk(disp, 19, FW_SEMIBOLD), g_font[F_LIVE] = mk(text, 12, FW_SEMIBOLD);
    g_font[F_HEAD] = mk(disp, 34, FW_SEMIBOLD), g_font[F_BODY] = mk(text, 15, FW_NORMAL);
    g_font[F_SMALL] = mk(text, 13, FW_NORMAL), g_font[F_BIG] = mk(disp, 44, FW_SEMIBOLD);
    g_font[F_TITLE] = mk(disp, 19, FW_SEMIBOLD);
    for (auto& c : g_ctls) {
        MoveWindow(c.h, S(c.x), S(c.y), S(c.w), S(c.h_), TRUE);
        SendMessageW(c.h, WM_SETFONT, (WPARAM)g_font[c.font], TRUE);
    }
    for (auto& s : g_segs) {  // các phân đoạn khít nhau theo pixel, không hở
        int x0 = S(s.x), w = S(s.w), y = S(s.y);
        int a = x0 + w * s.idx / s.n, b = x0 + w * (s.idx + 1) / s.n;
        MoveWindow(s.h, a, y, b - a, S(SEG_H), TRUE);
        SendMessageW(s.h, WM_SETFONT, (WPARAM)g_font[F_SMALL], TRUE);
    }
}

static HWND ctl(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id, Font f, Role fg, Role bg,
               HWND parent = nullptr) {
    if (!parent) parent = g_hwnd;
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, parent, (HMENU)(INT_PTR)id,
                             GetModuleHandleW(nullptr), nullptr);
    g_ctls.push_back({c, x, y, w, h, f, fg, bg, parent});
    return c;
}
static HWND label(const wchar_t* text, int x, int y, int w, int h, Font f, Role fg, Role bg, DWORD style = SS_LEFT,
                  HWND parent = nullptr) {
    return ctl(L"STATIC", text, style | SS_NOPREFIX, x, y, w, h, 0, f, fg, bg, parent);
}
static LRESULT ctl_color(WPARAM wp, LPARAM lp) {  // chữ tĩnh, ô nhập: màu theo vai trò, nền trùng nền phía sau
    for (auto& c : g_ctls)
        if (c.h == (HWND)lp) {
            SetTextColor((HDC)wp, P[c.fg == R_BLUE ? R_INK : c.fg]);
            SetBkColor((HDC)wp, P[c.bg]);
            return (LRESULT)g_brush[c.bg];
        }
    return 0;
}
static void segmented(int grp, const wchar_t* title, std::initializer_list<const wchar_t*> items, int y = 0,
                      int x = TRACK_X, int w = TRACK_W) {
    if (!y) y = ROW_Y[grp];
    label(title, 48, y + 6, 140, 20, F_SMALL, R_SLATE, R_CARD);
    int i = 0;
    for (auto s : items) {
        HWND h = CreateWindowExW(0, L"BUTTON", s, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 0, 0, g_hwnd,
                                 (HMENU)(INT_PTR)(ID_SEG + grp * 10 + i), GetModuleHandleW(nullptr), nullptr);
        g_segs.push_back({h, grp, i++, (int)items.size(), x, w, y});
    }
}

// Nút tự vẽ: pill viền mảnh, pill xanh cho việc chính (Tạo tài khoản, Lưu), hoặc một phân đoạn trên rãnh xám.
static void draw_item(const DRAWITEMSTRUCT* d) {
    int w = d->rcItem.right, h = d->rcItem.bottom;
    bool down = d->itemState & ODS_SELECTED, focus = d->itemState & ODS_FOCUS, off = d->itemState & ODS_DISABLED;
    HDC m = CreateCompatibleDC(d->hDC);
    HBITMAP bmp = CreateCompatibleBitmap(d->hDC, w, h), old = (HBITMAP)SelectObject(m, bmp);
    Role bg = R_CARD, role = R_INK;  // nền phía sau nút: phân đoạn luôn trên thẻ; nút pill theo chỗ đặt
    for (auto& c : g_ctls) if (c.h == d->hwndItem) bg = c.bg, role = c.fg;
    bool primary = role == R_BLUE && (int)d->CtlID < ID_SEG;
    COLORREF fg = primary ? RGB(255, 255, 255) : P[off ? R_STEEL : R_INK];
    RECT all{0, 0, w, h};
    FillRect(m, &all, g_brush[bg]);  // tô nền bằng GDI: GDI+ khử răng cưa sẽ pha mép với nền đen
    {
        Gdiplus::Graphics g(m);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        if ((int)d->CtlID >= ID_SEG) {
            const Seg* s = nullptr;
            for (auto& x : g_segs) if (x.h == d->hwndItem) s = &x;
            RECT r;
            GetWindowRect(d->hwndItem, &r);
            MapWindowPoints(nullptr, g_hwnd, (POINT*)&r, 2);
            float tx = s ? (float)(S(s->x) - r.left) : 0;  // rãnh của cả nhóm, toạ độ theo nút này
            rrect(g, tx, 0, (float)S(s ? s->w : TRACK_W), (float)h, h / 2.f, P[R_TRACK]);
            if (s && g_sel[s->grp] == s->idx) rrect(g, 2.5f, 2.5f, w - 5.f, h - 5.f, (h - 5) / 2.f, P[R_SEGON], P[R_HAIR]);
            if (focus) rrect(g, 1, 1, w - 2.f, h - 2.f, (h - 2) / 2.f, CLR_INVALID, P[R_BLUE], 2);
        } else if (primary) {
            rrect(g, 0, 0, (float)w, (float)h, h / 2.f, down ? RGB(0, 92, 184) : P[R_BLUE]);
            if (focus) rrect(g, 2, 2, w - 4.f, h - 4.f, (h - 4) / 2.f, CLR_INVALID, RGB(255, 255, 255), 1.5f);
        } else {
            rrect(g, 0.5f, 0.5f, w - 1.f, h - 1.f, h / 2.f, down ? P[R_BAND] : P[bg], P[off ? R_HAIR : R_STEEL]);
            if (focus) rrect(g, 1, 1, w - 2.f, h - 2.f, (h - 2) / 2.f, CLR_INVALID, P[R_BLUE], 2);
        }
    }
    wchar_t t[64];
    GetWindowTextW(d->hwndItem, t, 64);
    SetBkMode(m, TRANSPARENT);
    SetTextColor(m, fg);
    HFONT of = (HFONT)SelectObject(m, g_font[F_SMALL]);
    RECT rc{0, 0, w, h};
    DrawTextW(m, t, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(m, of);
    BitBlt(d->hDC, 0, 0, w, h, m, 0, 0, SRCCOPY);
    SelectObject(m, old);
    DeleteObject(bmp);
    DeleteDC(m);
}

// Vẽ cả cửa sổ vào ảnh đệm rồi chép một lần: không nhấp nháy.
template <class F> static void buffered_paint(HWND h, F draw) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps);
    RECT cr;
    GetClientRect(h, &cr);
    HDC m = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, cr.right, cr.bottom), old = (HBITMAP)SelectObject(m, bmp);
    FillRect(m, &cr, g_brush[R_CANVAS]);
    draw(m, cr);
    BitBlt(dc, 0, 0, cr.right, cr.bottom, m, 0, 0, SRCCOPY);
    SelectObject(m, old);
    DeleteObject(bmp);
    DeleteDC(m);
    EndPaint(h, &ps);
}
static void paint(HWND h) {
    buffered_paint(h, [](HDC m, RECT cr) {
        RECT band{0, S(BAND_Y), cr.right, cr.bottom};
        FillRect(m, &band, g_brush[R_BAND]);
        Gdiplus::Graphics g(m);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        for (const RECT& c : {CARD_Q, CARD_NET, CARD_LOG})
            rrect(g, (float)S(c.left), (float)S(c.top), (float)S(c.right - c.left), (float)S(c.bottom - c.top), (float)S(28), P[R_CARD]);
    });
}

static void set_text(HWND h, const std::wstring& s) {  // chỉ vẽ lại khi đổi, tránh nhấp nháy
    wchar_t cur[512];
    GetWindowTextW(h, cur, 512);
    if (s != cur) SetWindowTextW(h, s.c_str()), InvalidateRect(h, nullptr, TRUE);
}
static void refresh_status() {
    static uint64_t pf, pb;
    static Session* ps;
    bool acc = has_account();
    std::wstring detail = acc ? L"Mở app Remote Desk trên iPhone rồi đăng nhập."
                              : L"Tạo tài khoản để iPhone đăng nhập vào PC này.";
    bool live;
    {
        std::lock_guard g(g_cmu);
        live = g_current;
        if (live) {
            uint64_t f = g_current->frames, b = g_current->bytes;
            if (ps != g_current) pf = f, pb = b, ps = g_current;
            std::lock_guard g2(g_current->info_mu);
            string enc = g_current->enc_name.substr(0, g_current->enc_name.find(" Encoder"));  // "NVIDIA HEVC Encoder MFT" -> "NVIDIA HEVC"
            wchar_t d[256];
            int lm = g_current->live_mbps;
            swprintf(d, 256, L"%ls%ls  ·  %d×%d  ·  %llu khung/s  ·  %.1f Mbps%ls  ·  %ls", wide(g_current->peer).c_str(),
                     is_tailscale(g_current->peer) ? L" (Tailscale)" : L"", g_current->vw, g_current->vh,
                     (unsigned long long)(f - pf), (b - pb) * 8 / 1e6,
                     lm && lm < g_mbps ? (L" (tự hạ còn " + std::to_wstring(lm) + L")").c_str() : L"", wide(enc).c_str());
            detail = d;
            pf = f, pb = b;
        } else ps = nullptr;
    }
    if (live != g_streaming) {
        g_streaming = live;
        ShowWindow(h_kicker, live ? SW_HIDE : SW_SHOW);
        ShowWindow(h_live, live ? SW_SHOW : SW_HIDE);
        EnableWindow(h_kick, live);
    }
    set_text(h_status, live ? L"Đang phát tới iPhone." : L"Sẵn sàng kết nối.");
    set_text(h_detail, detail);
    string net;
    {
        std::lock_guard g(g_igdmu);
        net = g_netstatus;
    }
    set_text(h_net, wide(net) + L"  ·  " + std::to_wstring(device_count()) + L" thiết bị đã đăng nhập");
    set_text(h_acct, acc ? wide(account_user()) : L"Chưa tạo");
    set_text(h_accbtn, acc ? L"Đổi tài khoản" : L"Tạo tài khoản");
    for (auto& c : g_ctls)  // chưa có tài khoản: nút tạo tô xanh, là việc cần làm trước
        if (c.h == h_accbtn && (c.fg == R_BLUE) != !acc) c.fg = acc ? R_INK : R_BLUE, InvalidateRect(h_accbtn, nullptr, FALSE);
    for (auto& c : g_ctls)
        if (c.h == h_acct && (c.fg == R_SLATE) != !acc) c.fg = acc ? R_INK : R_SLATE, InvalidateRect(h_acct, nullptr, TRUE);
}
static void append_logs() {
    std::vector<string> q;
    {
        std::lock_guard g(g_logmu);
        q.swap(g_logq);
    }
    for (auto& s : q) {
        SYSTEMTIME t;
        GetLocalTime(&t);
        wchar_t ts[16];
        swprintf(ts, 16, L"%02d:%02d:%02d   ", t.wHour, t.wMinute, t.wSecond);
        if (GetWindowTextLengthW(h_log) > 20000) SetWindowTextW(h_log, L"");
        int n = GetWindowTextLengthW(h_log);
        SendMessageW(h_log, EM_SETSEL, n, n);
        SendMessageW(h_log, EM_REPLACESEL, FALSE, (LPARAM)((n ? L"\r\n" : L"") + (ts + wide(s))).c_str());
    }
}
static void apply_setting(int grp, int idx) {
    g_sel[grp] = idx;
    if (grp == 0) g_width = RES_W[idx];
    if (grp == 1) g_fps = FPS_V[idx];
    if (grp == 2) g_mbps = RATE_V[idx];
    if (grp == 3) g_hevc = idx == 1;
    if (grp == 4) g_remote = idx == 1;  // luồng nền tự mở/đóng cổng router
    save_settings();
    for (auto& s : g_segs) if (s.grp == grp) InvalidateRect(s.h, nullptr, FALSE);
    if (grp == 4) return;
    std::lock_guard g(g_cmu);
    if (g_current) g_current->restart = true;  // phát lại ngay với cài đặt mới, iPhone không phải nối lại
}

// ---- Hộp tạo/đổi tài khoản ----
constexpr int DLG_W = 360, DLG_H = 404;
static string edit_text(HWND e) {
    int n = GetWindowTextLengthW(e);
    std::wstring w(n + 1, 0);
    GetWindowTextW(e, w.data(), n + 1);
    w.resize(n);
    string s = n ? utf8(w) : "";
    SecureZeroMemory(w.data(), w.size() * sizeof(wchar_t));
    return s;
}
static void account_save(HWND dlg) {
    bool change = has_account();
    string u = edit_text(g_dlg_edits[0]), p1 = edit_text(g_dlg_edits[1]), p2 = edit_text(g_dlg_edits[2]);
    HCURSOR oc = SetCursor(LoadCursorW(nullptr, (LPCWSTR)IDC_WAIT));  // băm 600 000 vòng mất chừng nửa giây
    string err = p1 != p2 ? "Hai lần nhập mật khẩu không khớp." : set_account(u, p1);
    SetCursor(oc);
    SecureZeroMemory(p1.data(), p1.size());
    SecureZeroMemory(p2.data(), p2.size());
    if (!err.empty()) {
        SetWindowTextW(h_dlgerr, wide(err).c_str());
        return;
    }
    if (change) {  // đổi tài khoản: mọi thiết bị phải đăng nhập lại bằng mật khẩu mới
        forget_devices();
        std::lock_guard g(g_cmu);
        if (g_current) g_current->kick(4001, "Tài khoản đã đổi");
    }
    logf(change ? "Đã đổi tài khoản \"%s\": mọi thiết bị phải đăng nhập lại" : "Đã tạo tài khoản \"%s\"", trim(u).c_str());
    DestroyWindow(dlg);
}
static LRESULT CALLBACK dlg_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT:  // ô nhập kiểu Apple: viên bo tròn viền xám mảnh, ô đang gõ viền xanh
        buffered_paint(h, [h](HDC m, RECT) {
            Gdiplus::Graphics g(m);
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            HWND f = GetFocus();
            for (HWND e : g_dlg_edits) {
                RECT r;
                GetWindowRect(e, &r);
                MapWindowPoints(nullptr, h, (POINT*)&r, 2);
                float x = r.left - S(16) + 0.5f, y = r.top - S(9) + 0.5f, w = r.right - r.left + S(32), hh = r.bottom - r.top + S(18);
                rrect(g, x, y, w, hh, hh / 2, CLR_INVALID, P[f == e ? R_BLUE : R_STEEL], f == e ? 2.f : 1.f);
            }
        });
        return 0;
    case WM_DRAWITEM: draw_item((DRAWITEMSTRUCT*)lp); return TRUE;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
        if (LRESULT r = ctl_color(wp, lp)) return r;
        break;
    case WM_COMMAND:
        if (HIWORD(wp) == EN_SETFOCUS || HIWORD(wp) == EN_KILLFOCUS) InvalidateRect(h, nullptr, FALSE);
        else if (LOWORD(wp) == ID_ACC_SAVE || LOWORD(wp) == IDOK) account_save(h);  // IDOK: phím Enter
        else if (LOWORD(wp) == ID_ACC_CANCEL || LOWORD(wp) == IDCANCEL) DestroyWindow(h);  // IDCANCEL: phím Esc
        return 0;
    case WM_DESTROY:
        std::erase_if(g_ctls, [h](const Ctl& c) { return c.parent == h; });
        g_dlg_edits.clear();
        g_dlg = nullptr;
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}
// Hộp riêng, khoá cửa sổ chính tới khi đóng (như hộp thoại hệ thống).
static void account_dialog() {
    if (g_dlg) return;
    bool change = has_account();
    HINSTANCE hi = GetModuleHandleW(nullptr);
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{sizeof wc};
        wc.lpfnWndProc = dlg_proc, wc.hInstance = hi, wc.lpszClassName = L"RemoteDeskAccount";
        wc.hIcon = LoadIconW(hi, MAKEINTRESOURCEW(1)), wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
        RegisterClassExW(&wc);
        registered = true;
    }
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
    g_dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"RemoteDeskAccount", change ? L"Đổi tài khoản" : L"Tạo tài khoản", style,
                            0, 0, 100, 100, g_hwnd, nullptr, hi, nullptr);
    RECT rc{0, 0, S(DLG_W), S(DLG_H)}, o;
    AdjustWindowRectExForDpi(&rc, style, FALSE, WS_EX_DLGMODALFRAME, GetDpiForWindow(g_hwnd));
    GetWindowRect(g_hwnd, &o);
    int w = rc.right - rc.left, hh = rc.bottom - rc.top;
    SetWindowPos(g_dlg, nullptr, o.left + (o.right - o.left - w) / 2, o.top + S(90), w, hh, SWP_NOZORDER);

    label(change ? L"Đổi tài khoản" : L"Tạo tài khoản", 28, 22, 304, 44, F_HEAD, R_INK, R_CANVAS, SS_LEFT, g_dlg);
    label(change ? L"Mọi thiết bị đang đăng nhập sẽ phải đăng nhập lại bằng mật khẩu mới."
                 : L"iPhone đăng nhập bằng tài khoản này để điều khiển PC, ở nhà hay ở ngoài.",
          28, 70, 304, 38, F_SMALL, R_SLATE, R_CANVAS, SS_LEFT, g_dlg);
    const wchar_t* names[] = {L"Tên đăng nhập", L"Mật khẩu (ít nhất 8 ký tự)", L"Nhập lại mật khẩu"};
    for (int i = 0; i < 3; i++) {
        int y = 118 + i * 70;
        label(names[i], 28, y, 304, 18, F_SMALL, R_SLATE, R_CANVAS, SS_LEFT, g_dlg);
        g_dlg_edits.push_back(ctl(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL | (i ? ES_PASSWORD : 0), 44, y + 30, 272, 22,
                                  ID_ACC_EDIT + i, F_BODY, R_INK, R_CANVAS, g_dlg));
        SendMessageW(g_dlg_edits.back(), EM_LIMITTEXT, i ? 256 : 64, 0);
    }
    SetWindowTextW(g_dlg_edits[0], wide(account_user()).c_str());
    h_dlgerr = label(L"", 28, 322, 304, 20, F_SMALL, R_ORANGE, R_CANVAS, SS_LEFT, g_dlg);
    ctl(L"BUTTON", L"Huỷ", WS_TABSTOP | BS_OWNERDRAW, 152, 354, 84, 34, ID_ACC_CANCEL, F_SMALL, R_INK, R_CANVAS, g_dlg);
    ctl(L"BUTTON", change ? L"Lưu" : L"Tạo", WS_TABSTOP | BS_OWNERDRAW, 246, 354, 86, 34, ID_ACC_SAVE, F_SMALL, R_BLUE, R_CANVAS, g_dlg);
    layout();
    apply_theme();
    ShowWindow(g_dlg, SW_SHOW);
    SetFocus(g_dlg_edits[change ? 1 : 0]);
    EnableWindow(g_hwnd, FALSE);
    MSG m{};
    while (g_dlg && GetMessageW(&m, nullptr, 0, 0) > 0)
        if (!IsDialogMessageW(g_dlg, &m)) TranslateMessage(&m), DispatchMessageW(&m);
    if (m.message == WM_QUIT) PostQuitMessage((int)m.wParam);
    EnableWindow(g_hwnd, TRUE);
    SetForegroundWindow(g_hwnd);
    refresh_status();
}

static LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_APP_LOG: append_logs(); return 0;
    case WM_TIMER: refresh_status(); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: paint(h); return 0;
    case WM_DRAWITEM: draw_item((DRAWITEMSTRUCT*)lp); return TRUE;
    case WM_CTLCOLORSTATIC:  // chữ tĩnh và ô nhật ký
        if (LRESULT r = ctl_color(wp, lp)) return r;
        break;
    case WM_COMMAND:
        if (LOWORD(wp) == ID_ACCOUNT) {
            account_dialog();
        } else if (LOWORD(wp) == ID_FORGET) {
            forget_devices();
            logf("Đã đăng xuất mọi thiết bị: lần sau phải nhập mật khẩu");
        } else if (LOWORD(wp) == ID_KICK) {
            std::lock_guard g(g_cmu);
            if (g_current) g_current->kick(4001, "PC đã ngắt kết nối");
        } else if (LOWORD(wp) >= ID_SEG && HIWORD(wp) == BN_CLICKED) apply_setting((LOWORD(wp) - ID_SEG) / 10, (LOWORD(wp) - ID_SEG) % 10);
        return 0;
    case WM_SETTINGCHANGE:
        if (lp && !wcscmp((const wchar_t*)lp, L"ImmersiveColorSet")) apply_theme();  // người dùng đổi sáng/tối
        break;
    case WM_DPICHANGED: {
        RECT* r = (RECT*)lp;
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        layout();
        InvalidateRect(h, nullptr, TRUE);
        return 0;
    }
    case WM_DESTROY: {
        if (g_reg.pServiceInstance) DnsServiceDeRegister(&g_reg, nullptr);
        Igd u;  // app tắt thì đóng cổng router, không để cổng mở khi không có gì nghe
        {
            std::lock_guard g(g_igdmu);
            u = g_igd;
        }
        igd_unmap(u);
        PostQuitMessage(0);
        return 0;
    }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void run_ui() {
    Gdiplus::GdiplusStartupInput gsi;
    Gdiplus::GdiplusStartup(&g_gdip, &gsi, nullptr);
    INITCOMMONCONTROLSEX icc{sizeof icc, ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    HINSTANCE hi = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = wnd_proc, wc.hInstance = hi, wc.lpszClassName = L"RemoteDeskHost";
    wc.hIcon = LoadIconW(hi, MAKEINTRESOURCEW(1)), wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
    RegisterClassExW(&wc);
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"Remote Desk", style, CW_USEDEFAULT, CW_USEDEFAULT, 100, 100,
                             nullptr, nullptr, hi, nullptr);
    RECT rc{0, 0, S(WIN_W), S(WIN_H)};
    AdjustWindowRectExForDpi(&rc, style, FALSE, 0, GetDpiForWindow(g_hwnd));
    SetWindowPos(g_hwnd, nullptr, 0, 0, rc.right - rc.left, rc.bottom - rc.top, SWP_NOMOVE | SWP_NOZORDER);
    g_sel[0] = pick(RES_W, 4, g_width, 1), g_sel[1] = pick(FPS_V, 4, g_fps, 1), g_sel[2] = pick(RATE_V, 5, g_mbps, 2);
    g_sel[3] = g_hevc, g_sel[4] = g_remote;

    // Phần trên, nền trắng: nhãn, tiêu đề lớn, một dòng phụ, tên tài khoản là "sản phẩm" ở giữa sân khấu
    h_kicker = label(L"Remote Desk", 40, 30, 440, 26, F_KICKER, R_INK, R_CANVAS);
    h_live = label(L"TRỰC TIẾP", 40, 36, 440, 18, F_LIVE, R_ORANGE, R_CANVAS);
    ShowWindow(h_live, SW_HIDE);
    h_status = label(L"", 40, 60, 440, 46, F_HEAD, R_INK, R_CANVAS);
    h_detail = label(L"", 40, 108, 440, 22, F_BODY, R_SLATE, R_CANVAS, SS_LEFT | SS_ENDELLIPSIS);
    label(L"TÀI KHOẢN", 40, 150, 200, 18, F_LIVE, R_SLATE, R_CANVAS);
    h_acct = label(L"", 38, 170, 440, 60, F_BIG, R_INK, R_CANVAS, SS_LEFT | SS_ENDELLIPSIS);
    h_accbtn = ctl(L"BUTTON", L"", WS_TABSTOP | BS_OWNERDRAW, 40, 246, 128, 32, ID_ACCOUNT, F_SMALL, R_INK, R_CANVAS);
    h_kick = ctl(L"BUTTON", L"Ngắt kết nối", WS_TABSTOP | BS_OWNERDRAW, 178, 246, 116, 32, ID_KICK, F_SMALL, R_INK, R_CANVAS);
    EnableWindow(h_kick, FALSE);

    // Dải #f5f5f7: thẻ chất lượng hình với ba nhóm phân đoạn, thẻ nhật ký
    label(L"Chất lượng hình", 48, 342, 400, 26, F_TITLE, R_INK, R_CARD);
    segmented(0, L"Độ phân giải", {L"720p", L"1080p", L"1440p", L"Gốc"});
    segmented(1, L"Khung hình", {L"30", L"60", L"90", L"120"});
    segmented(2, L"Bitrate (Mbps)", {L"5", L"10", L"20", L"30", L"50"});
    segmented(3, L"Mã hoá", {L"H.264", L"HEVC"});

    // Thẻ truy cập từ xa: tự mở cổng router (UPnP) để iPhone ở ngoài vào thẳng, kèm số thiết bị đã đăng nhập
    label(L"Truy cập từ xa", 48, 576, 400, 26, F_TITLE, R_INK, R_CARD);
    segmented(4, L"Từ Internet", {L"Tắt", L"Bật"}, NET_Y, TRACK_X, 140);
    ctl(L"BUTTON", L"Đăng xuất hết", WS_TABSTOP | BS_OWNERDRAW, 352, NET_Y, 124, 30, ID_FORGET, F_SMALL, R_INK, R_CARD);
    h_net = label(L"", 48, 656, 424, 20, F_SMALL, R_SLATE, R_CARD, SS_LEFT | SS_ENDELLIPSIS);

    label(L"Nhật ký", 48, 722, 400, 26, F_TITLE, R_INK, R_CARD);
    h_log = ctl(L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, 48, 754, 424, 74, 0, F_SMALL, R_SLATE, R_CARD);

    layout();
    apply_theme();
    append_logs();
    refresh_status();
    SetTimer(g_hwnd, 1, 1000, nullptr);
    ShowWindow(g_hwnd, SW_SHOW);
    if (!has_account()) account_dialog();  // lần đầu chạy: tạo tài khoản ngay
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0)
        if (!IsDialogMessageW(g_hwnd, &m)) TranslateMessage(&m), DispatchMessageW(&m);
}

// ======================= Self-test =======================
#define CHECK(x) do { if (!(x)) { printf("SELF-TEST LỖI dòng %d: %s\n", __LINE__, #x); return 1; } } while (0)
static int self_test() {
    // WebSocket accept (ví dụ trong RFC 6455)
    CHECK(ws_accept("dGhlIHNhbXBsZSBub25jZQ==") == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
    CHECK(ws_frame(2, string(300, 'x')).substr(0, 4) == string("\x82\x7e\x01\x2c", 4));
    // JSON
    string s;
    double d;
    CHECK(jstr(R"({"t":"text","s":"a\"t\":\u1ec7\ud83d\ude00\\"})", "s", s) && s == "a\"t\":\xe1\xbb\x87\xf0\x9f\x98\x80\\");
    CHECK(jstr(R"({"s":"\"t\":\"x","t":"k"})", "t", s) && s == "k");
    CHECK(jnum(R"({"t":"m","x":0.25})", "x", d) && d == 0.25 && !jnum(R"({"x":"a"})", "x", d));
    // Điều khiển: chỉ dựng INPUT, không gửi
    std::vector<INPUT> in;
    CHECK(combo("ctrl+shift+esc", true, in) && in.size() == 3 && in[0].ki.wVk == 17 && in[2].ki.wVk == 27 && !(in[0].ki.dwFlags & KEYEVENTF_KEYUP));
    in.clear();
    CHECK(combo("CTRL+SHIFT+ESC", false, in) && in[0].ki.wVk == 27 && in[2].ki.wVk == 17 && (in[0].ki.dwFlags & KEYEVENTF_KEYUP));
    in.clear();
    CHECK(combo("UP", true, in) && (in[0].ki.dwFlags & KEYEVENTF_EXTENDEDKEY) && !combo("FOO", true, in));
    in.clear();
    CHECK(combo("W", true, in) && (in[0].ki.dwFlags & KEYEVENTF_SCANCODE) && in[0].ki.wScan == 0x11);  // scan code cho game
    in.clear();
    CHECK(translate(R"({"t":"m","x":1,"y":0.5})", in) && in[0].mi.dx == 65535 && in[0].mi.dy == 32768);
    in.clear();
    CHECK(translate(R"({"t":"text","s":"a\ud83d\ude00"})", in) && in.size() == 6 && in[2].ki.wScan == 0xD83D && in[4].ki.wScan == 0xDE00);
    in.clear();
    CHECK(translate(R"({"t":"w","dy":-40})", in) && (LONG)in[0].mi.mouseData == -40 && !translate(R"({"t":"zz"})", in));
    // HEVC: nhận diện NAL, gắn lại VPS/SPS/PPS vào khung khoá
    string hh, h1 = string("\0\0\0\1\x40\x01\x0c", 7) + string("\0\0\0\1\x42\x01\x01", 7) + string("\0\0\0\1\x44\x01\xc0", 7) +
                    string("\0\0\0\1\x26\x01\xaf", 7);
    string h2 = string("\0\0\0\1\x26\x01\x9a", 7), h3 = string("\0\0\0\1\x02\x01\xd0", 7);
    CHECK(fix_headers(h1, hh, true) && hh.size() == 21 && fix_headers(h2, hh, true) && has_nal(h2, 32, true) && has_nal(h2, 34, true));
    CHECK(!fix_headers(h3, hh, true) && !has_nal(h3, 32, true) && !is_key(h1, false));
    // Tài khoản: PBKDF2 đúng chuẩn (vector thử đã biết), tạo/đọc lại/kiểm mật khẩu, chặn dò từ Internet
    CHECK(pbkdf2_hex("password", "salt", 1) == "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b");
    {
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        g_accfile = std::wstring(tmp) + L"rd_selftest_account.txt";
        DeleteFileW(g_accfile.c_str());
        load_account();
        CHECK(!has_account() && !check_login("", ""));
        CHECK(!set_account("  ", "matkhau123").empty() && !set_account("vdt", "ngan").empty() && !has_account());
        CHECK(set_account(" VDT ", "mật khẩu dài").empty() && account_user() == "VDT");
        load_account();  // đọc lại từ file
        CHECK(check_login("vdt", "mật khẩu dài") && check_login(" Vdt", "mật khẩu dài"));
        CHECK(!check_login("vdt", "mật khẩu dàI") && !check_login("khac", "mật khẩu dài") && !check_login("vdt", ""));
        DeleteFileW(g_accfile.c_str());
    }
    for (int i = 0; i < 19; i++) net_login_failed();
    CHECK(!net_login_blocked());
    net_login_failed();
    CHECK(net_login_blocked());
    {
        std::lock_guard g(g_failmu);
        g_netblock = {};
    }
    // Khoá thiết bị, chống dò, nhận diện địa chỉ riêng, đọc trả lời của router (không đụng router thật)
    CHECK(sha256_hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    {
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        g_devfile = std::wstring(tmp) + L"rd_selftest_devices.txt";
        string tk = issue_token();
        load_devices();  // đọc lại từ file
        CHECK(tk.size() == 64 && token_ok(tk) && !token_ok(string(64, '0')) && !token_ok("abc"));
        forget_devices();
        CHECK(!token_ok(tk) && device_count() == 0);
        DeleteFileW(g_devfile.c_str());
    }
    CHECK(is_private("192.168.0.103") && is_private("100.99.1.1") && is_private("172.20.10.2") && is_private("127.0.0.1"));
    CHECK(!is_private("14.162.162.13") && !is_private("172.32.0.1") && !is_private("100.128.0.1"));
    for (int i = 0; i < 4; i++) auth_failed("203.0.113.9");
    CHECK(!auth_locked("203.0.113.9"));
    auth_failed("203.0.113.9");
    CHECK(auth_locked("203.0.113.9") && !auth_locked("203.0.113.10"));
    auth_ok("203.0.113.9");
    CHECK(!auth_locked("203.0.113.9"));
    {
        string h, pa;
        int pt;
        CHECK(split_url("http://192.168.0.1:1900/gatedesc.xml", h, pt, pa) && h == "192.168.0.1" && pt == 1900 && pa == "/gatedesc.xml");
        CHECK(split_url("http://10.0.0.1/ctl", h, pt, pa) && pt == 80 && pa == "/ctl" && !split_url("https://x/", h, pt, pa));
        CHECK(xml_tag("<a><NewExternalIPAddress>1.2.3.4</NewExternalIPAddress></a>", "NewExternalIPAddress") == "1.2.3.4");
    }
    // Bitrate thích ứng và địa chỉ Tailscale
    int calm = 0;
    CHECK(adapt_bitrate(20, 20, 0.8, calm) == 14 && adapt_bitrate(2, 20, 0.9, calm) == 2);
    CHECK(adapt_bitrate(14, 20, 0.05, calm) == 14 && adapt_bitrate(14, 20, 0.05, calm) == 16);
    CHECK(adapt_bitrate(20, 20, 0.05, calm) == 20 && adapt_bitrate(14, 20, 0.3, calm) == 14 && calm == 0);
    CHECK(is_tailscale("100.99.115.103") && !is_tailscale("100.20.1.1") && !is_tailscale("192.168.0.103"));
    // Màu: trắng -> Y 235, đen -> Y 16, xám -> U/V 128
    std::vector<uint8_t> px = {255, 255, 255, 0, 0, 0, 0, 0, 128, 128, 128, 0, 128, 128, 128, 0}, nv(6);
    bgra_to_nv12(px.data(), 2, 2, nv.data(), 2, 2);
    CHECK(nv[0] == 235 && nv[1] == 16 && nv[4] == 128 && nv[5] == 128);
    // SPS/PPS gắn lại vào IDR
    string hdr, k1 = string("\0\0\0\1\x67\x42\0\0\1\x68\xce\0\0\1\x65\x88", 16), k2 = string("\0\0\0\1\x65\x99", 6);
    CHECK(fix_headers(k1, hdr) && hdr.size() == 12 && fix_headers(k2, hdr) && has_nal(k2, 7) && has_nal(k2, 8));
    // Encoder: phần cứng và phần mềm, khung đầu có SPS+IDR, ép khung khoá được
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    int ok_enc = 0;
    for (int only : {1, 2}) {
        Encoder e;
        if (!e.init(640, 360, 30, 4, only)) { printf("encoder %s: không có\n", only == 1 ? "phần cứng" : "phần mềm"); continue; }
        std::vector<uint8_t> f(640 * 360 * 3 / 2);
        std::vector<string> o;
        std::vector<bool> keys;
        for (int i = 1; i <= 8; i++) {
            for (size_t j = 0; j < f.size(); j++) f[j] = (uint8_t)(j * 7 + i * 13);
            size_t before = o.size();
            CHECK(e.encode(f.data(), i, i == 1 || i == 5, o));  // init đã dùng IDR đầu cho khung thử
            for (size_t j = before; j < o.size(); j++) keys.push_back(has_nal(o[j], 5));
        }
        CHECK(!o.empty() && has_nal(o[0], 7) && has_nal(o[0], 5));
        CHECK(std::count(keys.begin(), keys.end(), true) >= 1 && std::count(keys.begin(), keys.end(), false) >= 1);
        printf("encoder %s: %s, %zu gói, khoá ở %s\n", only == 1 ? "phần cứng" : "phần mềm", e.name.c_str(), o.size(),
               std::count(keys.begin() + 1, keys.end(), true) ? "khung ép" : "khung đầu");
        ok_enc++;
    }
    CHECK(ok_enc > 0);
    {
        Encoder e;  // HEVC phần cứng: gói đầu phải có VPS + SPS + PPS + khung khoá
        if (e.init(640, 360, 30, 4, 1, true)) {
            std::vector<uint8_t> f(640 * 360 * 3 / 2, 100);
            std::vector<string> o;
            for (int i = 1; i <= 4; i++) CHECK(e.encode(f.data(), i, i == 1, o));
            CHECK(!o.empty() && has_nal(o[0], 32, true) && has_nal(o[0], 33, true) && has_nal(o[0], 34, true) && is_key(o[0], true));
            printf("encoder HEVC: %s, %zu gói\n", e.name.c_str(), o.size());
        } else {
            printf("encoder HEVC: không có (sẽ dùng H.264)\n");
        }
    }
    // Chụp màn hình (chỉ đọc)
    Capture cap;
    CHECK(cap.init());
    int got = 0;
    for (int i = 0; i < 20 && cap.bgra.empty(); i++) got = cap.grab(100);
    CHECK(got >= 0 && !cap.bgra.empty());
    cap.cursor(true);
    cap.cursor(false);
    printf("chụp: %dx%d\n", cap.w, cap.h);
    CoUninitialize();
    // Chứng chỉ: tạo, dùng lại, xoá (tên riêng cho test, không đụng chứng chỉ thật)
    bool created;
    PCCERT_CONTEXT c = ensure_cert(L"RemoteDesk Selftest", {"127.0.0.1", "10.1.2.3"}, created);
    CHECK(c && created && cert_ok(c, {"10.1.2.3"}) && !cert_ok(c, {"10.9.9.9"}));
    PCCERT_CONTEXT c2 = ensure_cert(L"RemoteDesk Selftest", {"127.0.0.1"}, created);
    CHECK(c2 && !created);
    delete_cert(c2, L"RemoteDesk Selftest");
    CertFreeCertificateContext(c);
    printf("self-test OK\n");
    return 0;
}

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);  // chụp và bấm theo pixel thật
    timeBeginPeriod(1);
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    MFStartup(MF_VERSION);
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    g_dir = exe;
    g_dir.resize(g_dir.find_last_of(L"\\/"));
    for (int i = 1; i < argc; i++) {
        string a = argv[i];
        if (a == "--self-test") {
            // exe dạng cửa sổ: chạy từ cmd thì gắn vào console cha để in kết quả
            HANDLE o = GetStdHandle(STD_OUTPUT_HANDLE);
            if ((!o || GetFileType(o) == FILE_TYPE_UNKNOWN) && AttachConsole(ATTACH_PARENT_PROCESS)) freopen("CONOUT$", "w", stdout);
            return self_test();
        }
        if (a == "--port" && i + 1 < argc) g_port = atoi(argv[++i]);
        if (a == "--theme" && i + 1 < argc) g_force_theme = string(argv[++i]) == "dark";
        if (a == "--upnp-check") {  // chỉ hỏi router (không mở cổng): có UPnP không, IP công khai là gì
            HANDLE o = GetStdHandle(STD_OUTPUT_HANDLE);
            if ((!o || GetFileType(o) == FILE_TYPE_UNKNOWN) && AttachConsole(ATTACH_PARENT_PROCESS)) freopen("CONOUT$", "w", stdout);
            Igd u;
            string r;
            if (!igd_discover(u)) return printf("Không thấy router UPnP\n"), 1;
            igd_call(u, "GetExternalIPAddress", "", r);
            string ip = xml_tag(r, "NewExternalIPAddress");
            printf("Router: %s:%d%s (%s)\nPC phía router: %s\nIP công khai: %s%s\n", u.host.c_str(), u.port, u.control.c_str(),
                   u.service.c_str(), u.local.c_str(), ip.c_str(), is_private(ip) ? " (KHÔNG công khai: CGNAT/2 lớp router)" : "");
            printf("IP công khai thật của nhà: %s\n", public_ip().c_str());
            return 0;
        }
    }
    auto fail = [](const std::wstring& m) { MessageBoxW(nullptr, m.c_str(), L"Remote Desk", MB_ICONERROR); return 1; };
    load_settings();
    g_devfile = g_dir + L"\\remote_devices.txt";
    load_devices();
    bool created;
    if (!setup_tls(L"RemoteDesk Host", created)) return fail(L"Không tạo được chứng chỉ TLS.");
    if (created) logf("Đã tạo chứng chỉ mới (Safari cần cài lại, app iPhone sẽ hỏi tin chứng chỉ mới)");
    g_accfile = g_dir + L"\\remote_account.txt";
    load_account();
    SOCKET l = listen_on(g_port);
    if (l == INVALID_SOCKET) return fail(L"Không mở được cổng " + std::to_wstring(g_port) + L" (có thể app đang chạy rồi).");
    logf("Đang nghe cổng %d", g_port);
    for (auto& ip : reachable_ips())
        logf("%s: https://%s:%d", is_tailscale(ip) ? "Tailscale (ở ngoài)" : "Safari trong mạng nhà", ip.c_str(), g_port);
    mdns_register();
    std::thread(remote_loop).detach();
    std::thread([l] { for (SOCKET s; (s = accept(l, nullptr, nullptr)) != INVALID_SOCKET;) std::thread(client_thread, s).detach(); }).detach();
    run_ui();
    ExitProcess(0);  // các phiên đang chạy dở: thoát luôn
}
