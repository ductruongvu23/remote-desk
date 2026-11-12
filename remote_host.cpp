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
#include <uxtheme.h>
#include <gdiplus.h>
#include <winhttp.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wincodec.h>
#include <zlib.h>  // thư viện tĩnh của MSYS2 (libz.a), không cần DLL đi kèm
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
#include <functional>
#include <deque>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <condition_variable>
#include <vector>

// mingw thiếu uuid này cho ComPtr::As
__CRT_UUID_DECL(ICodecAPI, 0x901db4c7, 0x31ce, 0x41a2, 0x85, 0xdc, 0x8f, 0xa0, 0xbf, 0x41, 0xb8, 0xda)
using Microsoft::WRL::ComPtr;
using std::string;
using Clock = std::chrono::steady_clock;

static std::atomic<int> g_fps{60}, g_mbps{20}, g_width{1920};
static const int RES_W[] = {1280, 1920, 2560, 0}, FPS_V[] = {30, 60, 90, 120}, RATE_V[] = {5, 10, 20, 30, 50};  // 0 = độ phân giải gốc
static int g_port = 8443;
static std::atomic<bool> g_hevc{true}, g_remote{false};  // mã hoá HEVC; cho vào từ Internet (mở cổng router)
static std::wstring g_dir;  // thư mục chứa exe
static CredHandle g_cred;
static PCCERT_CONTEXT g_cert;
static HWND g_hwnd;  // cửa sổ chính (null khi self-test)
static std::mutex g_logmu;
static std::vector<string> g_logq;
#define WM_APP_LOG (WM_APP + 1)
#define WM_APP_SET (WM_APP + 2)  // iPhone đổi chất lượng: wParam nhóm (0 độ phân giải, 1 fps, 2 bitrate, 3 mã hoá), lParam vị trí

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
// Chia hàng cho các nhân CPU (1080p một nhân mất ~9 ms, là một nửa thời gian của cả khung). Mỗi phần bắt đầu ở hàng
// chẵn vì hàng UV đi theo hàng chẵn, nên các phần không ghi đè nhau.
static void bgra_to_nv12(const uint8_t* src, int sw, int sh, uint8_t* dst, int dw, int dh) {
    std::vector<int> sxs(dw);
    for (int x = 0; x < dw; x++) sxs[x] = (int)((int64_t)x * sw / dw) * 4;
    uint8_t* uv = dst + (size_t)dw * dh;
    auto rows = [&](int y0, int y1) {
    for (int y = y0; y < y1; y++) {
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
    };
    int n = dh < 64 ? 1 : (int)std::clamp(std::thread::hardware_concurrency(), 1u, 8u);
    int step = ((dh + n - 1) / n + 1) & ~1;
    std::vector<std::thread> th;
    for (int y0 = step; y0 < dh; y0 += step) th.emplace_back(rows, y0, std::min(dh, y0 + step));
    rows(0, std::min(dh, step));
    for (auto& t : th) t.join();
}

// ======================= Mã hoá HEVC / H.264 (Media Foundation) =======================
struct Encoder {
    ComPtr<IMFTransform> mft;
    ComPtr<IMFMediaEventGenerator> gen;
    ComPtr<ICodecAPI> api;
    DWORD in_id = 0, out_id = 0, out_cb = 0;
    bool async = false, provides = false, hevc = false;  // HEVC: cùng chất lượng tốn khoảng nửa bitrate so với H.264
    int w = 0, h = 0, fps = 0, need_input = 0, pending = 0;  // pending: khung đã đưa vào, chưa lấy gói ra
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
                    drain(o, 200);  // gói của khung thử bỏ đi tại đây, không để lẫn vào lượt phát đầu
                }
                if (!ok) { if (mft) acts[i]->ShutdownObject(); mft.Reset(), gen.Reset(), api.Reset(), need_input = 0, pending = 0, hdrs.clear(); }
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
        while (!need_input) {  // encoder thường nhận khung mới ngay; gói xong trong lúc chờ thì lấy luôn
            if (!event(t, true)) return false;
            if (t == METransformNeedInput) need_input++;
            else if (t == METransformHaveOutput) pull(out), pending = std::max(0, pending - 1);
        }
        if (FAILED(mft->ProcessInput(in_id, s.Get(), 0))) return false;
        need_input--, pending++;
        // Không ngủ chờ gói của khung này (Sleep trên Windows có thể tới 15,6 ms, làm khung/s tụt còn ~30):
        // vòng phát gọi drain() mỗi lượt và chụp khung kế trong lúc NVENC đang mã hoá.
        drain(out);
        return true;
    }
    // Lấy các gói đã mã hoá xong; wait_ms > 0 thì chờ tới khi lấy hết hoặc hết giờ (dùng khi thử và khi kết thúc).
    void drain(std::vector<string>& out, int wait_ms = 0) {
        MediaEventType t;
        for (auto t0 = Clock::now(); async && pending > 0;) {
            if (!event(t, false)) {
                if (Clock::now() - t0 >= std::chrono::milliseconds(wait_ms)) break;
                Sleep(1);
                continue;
            }
            if (t == METransformNeedInput) need_input++;
            else if (t == METransformHaveOutput) pull(out), pending--;
        }
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
            int fn = k.size() >= 2 && k.size() <= 3 && k[0] == 'F' && isdigit((unsigned char)k[1]) ? atoi(k.c_str() + 1) : 0;
            WORD vk = VK.count(k) ? VK.at(k) : k.size() == 1 && isalnum((unsigned char)k[0]) ? (WORD)k[0]
                    : fn >= 1 && fn <= 24 ? (WORD)(VK_F1 + fn - 1) : 0;
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

// ======================= Mã QR =======================
// Bộ tạo mã QR nhỏ (ISO/IEC 18004: chế độ byte, mức sửa lỗi M, phiên bản 1–40), theo cách làm của qrcodegen (Nayuki).
// Dùng cho mã ghép iPhone hiện trên cửa sổ PC.
struct Qr {
    int size = 0;
    std::vector<char> m, fn;  // ô đen, ô thuộc mẫu cố định
    bool at(int x, int y) const { return m[y * size + x]; }
};
static const uint8_t QR_ECC[41] = {0,  10, 16, 26, 18, 24, 16, 18, 22, 22, 26, 30, 22, 22, 24, 24, 28, 28, 26, 26, 26,
                                   26, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28};
static const uint8_t QR_BLOCKS[41] = {0,  1,  1,  1,  2,  2,  4,  4,  4,  5,  5,  5,  8,  9,  9,  10, 10, 11, 13, 14, 16,
                                      17, 17, 18, 20, 21, 23, 25, 26, 28, 29, 31, 33, 35, 37, 38, 40, 43, 45, 47, 49};
static int qr_raw(int v) {  // số ô chứa dữ liệu + mã sửa lỗi
    int r = (16 * v + 128) * v + 64;
    if (v >= 2) {
        int n = v / 7 + 2;
        r -= (25 * n - 10) * n - 55;
        if (v >= 7) r -= 36;
    }
    return r;
}
static uint8_t gf_mul(uint8_t x, uint8_t y) {
    int z = 0;
    for (int i = 7; i >= 0; i--) z = ((z << 1) ^ ((z >> 7) * 0x11D)) ^ (((y >> i) & 1) * x);
    return (uint8_t)z;
}
static std::vector<uint8_t> rs_ecc(const std::vector<uint8_t>& data, int deg) {
    std::vector<uint8_t> div(deg), r(deg);
    div[deg - 1] = 1;
    for (int i = 0, root = 1; i < deg; i++, root = gf_mul((uint8_t)root, 2))
        for (int j = 0; j < deg; j++) div[j] = gf_mul(div[j], (uint8_t)root) ^ (j + 1 < deg ? div[j + 1] : 0);
    for (uint8_t b : data) {
        uint8_t f = b ^ r[0];
        r.erase(r.begin());
        r.push_back(0);
        for (int i = 0; i < deg; i++) r[i] ^= gf_mul(div[i], f);
    }
    return r;
}
// mask -1: tự chọn mẫu phủ ít điểm phạt nhất. Chuỗi quá dài (> 2331 byte) trả size 0.
static Qr qr_make(const string& text, int mask = -1) {
    int v = 1, n = (int)text.size();
    for (; v <= 40 && 4 + (v < 10 ? 8 : 16) + 8 * n > (qr_raw(v) / 8 - QR_ECC[v] * QR_BLOCKS[v]) * 8; v++) {}
    if (v > 40) return {};
    int cap = qr_raw(v) / 8 - QR_ECC[v] * QR_BLOCKS[v];
    std::vector<bool> bits;
    auto put = [&](uint32_t val, int len) { for (int i = len - 1; i >= 0; i--) bits.push_back((val >> i) & 1); };
    put(4, 4);
    put(n, v < 10 ? 8 : 16);
    for (unsigned char c : text) put(c, 8);
    put(0, std::min(4, cap * 8 - (int)bits.size()));
    put(0, (8 - bits.size() % 8) % 8);
    std::vector<uint8_t> data;
    for (size_t i = 0; i < bits.size(); i += 8) {
        uint8_t b = 0;
        for (int j = 0; j < 8; j++) b = b << 1 | bits[i + j];
        data.push_back(b);
    }
    for (uint8_t pad = 0xEC; (int)data.size() < cap; pad ^= 0xEC ^ 0x11) data.push_back(pad);

    // Chia khối, thêm mã sửa lỗi Reed–Solomon, xen kẽ các khối
    int nb = QR_BLOCKS[v], ecl = QR_ECC[v], raw = qr_raw(v) / 8, nshort = nb - raw % nb, slen = raw / nb;
    std::vector<std::vector<uint8_t>> blocks;
    for (int i = 0, k = 0; i < nb; i++) {
        int len = slen - ecl + (i < nshort ? 0 : 1);
        std::vector<uint8_t> d(data.begin() + k, data.begin() + k + len);
        k += len;
        auto e = rs_ecc(d, ecl);
        if (i < nshort) d.push_back(0);
        d.insert(d.end(), e.begin(), e.end());
        blocks.push_back(d);
    }
    std::vector<uint8_t> cw;
    for (size_t i = 0; i < blocks[0].size(); i++)
        for (int j = 0; j < nb; j++)
            if (i != (size_t)(slen - ecl) || j >= nshort) cw.push_back(blocks[j][i]);

    Qr q;
    int s = q.size = v * 4 + 17;
    q.m.assign(s * s, 0), q.fn.assign(s * s, 0);
    auto set = [&](int x, int y, bool dark) { q.m[y * s + x] = dark, q.fn[y * s + x] = 1; };
    for (int i = 0; i < s; i++) set(6, i, i % 2 == 0), set(i, 6, i % 2 == 0);  // vạch thời gian
    std::pair<int, int> finders[] = {{3, 3}, {s - 4, 3}, {3, s - 4}};
    for (auto [cx, cy] : finders)  // ba ô vuông định vị
        for (int dy = -4; dy <= 4; dy++)
            for (int dx = -4; dx <= 4; dx++) {
                int d = std::max(abs(dx), abs(dy)), x = cx + dx, y = cy + dy;
                if (x >= 0 && x < s && y >= 0 && y < s) set(x, y, d != 2 && d != 4);
            }
    if (v > 1) {  // ô căn chỉnh
        int na = v / 7 + 2, step = (v * 8 + na * 3 + 5) / (na * 4 - 4) * 2;
        std::vector<int> pos{6};
        for (int i = 0; i < na - 1; i++) pos.insert(pos.begin() + 1, s - 7 - i * step);
        for (int i = 0; i < na; i++)
            for (int j = 0; j < na; j++) {
                if ((i == 0 && j == 0) || (i == 0 && j == na - 1) || (i == na - 1 && j == 0)) continue;
                for (int dy = -2; dy <= 2; dy++)
                    for (int dx = -2; dx <= 2; dx++) set(pos[i] + dx, pos[j] + dy, std::max(abs(dx), abs(dy)) != 1);
            }
    }
    auto format = [&](int msk) {  // mức M = 00, cộng mẫu phủ, BCH(15,5)
        int d = msk, r = d;
        for (int i = 0; i < 10; i++) r = (r << 1) ^ ((r >> 9) * 0x537);
        int b = (d << 10 | r) ^ 0x5412;
        auto bit = [b](int i) { return (b >> i) & 1; };
        for (int i = 0; i <= 5; i++) set(8, i, bit(i));
        set(8, 7, bit(6)), set(8, 8, bit(7)), set(7, 8, bit(8));
        for (int i = 9; i < 15; i++) set(14 - i, 8, bit(i));
        for (int i = 0; i < 8; i++) set(s - 1 - i, 8, bit(i));
        for (int i = 8; i < 15; i++) set(8, s - 15 + i, bit(i));
        set(8, s - 8, true);
    };
    format(0);
    if (v >= 7) {  // thông tin phiên bản, BCH(18,6)
        int r = v;
        for (int i = 0; i < 12; i++) r = (r << 1) ^ ((r >> 11) * 0x1F25);
        long b = (long)v << 12 | r;
        for (int i = 0; i < 18; i++) {
            bool bit = (b >> i) & 1;
            set(s - 11 + i % 3, i / 3, bit), set(i / 3, s - 11 + i % 3, bit);
        }
    }
    // Rải dữ liệu theo hình zíc zắc từ góc dưới phải, từng cặp cột
    size_t bi = 0;
    for (int right = s - 1; right >= 1; right -= 2) {
        if (right == 6) right = 5;
        for (int vert = 0; vert < s; vert++)
            for (int j = 0; j < 2; j++) {
                int x = right - j, y = ((right + 1) & 2) == 0 ? s - 1 - vert : vert;
                if (!q.fn[y * s + x] && bi < cw.size() * 8) q.m[y * s + x] = (cw[bi >> 3] >> (7 - (bi & 7))) & 1, bi++;
            }
    }
    auto apply = [&](int msk) {  // phủ mẫu: đảo ô dữ liệu theo công thức (đảo lần hai là bỏ phủ)
        for (int y = 0; y < s; y++)
            for (int x = 0; x < s; x++) {
                bool inv = msk == 0 ? (x + y) % 2 == 0 : msk == 1 ? y % 2 == 0 : msk == 2 ? x % 3 == 0 : msk == 3 ? (x + y) % 3 == 0
                         : msk == 4 ? (x / 3 + y / 2) % 2 == 0 : msk == 5 ? x * y % 2 + x * y % 3 == 0
                         : msk == 6 ? (x * y % 2 + x * y % 3) % 2 == 0 : ((x + y) % 2 + x * y % 3) % 2 == 0;
                if (inv && !q.fn[y * s + x]) q.m[y * s + x] ^= 1;
            }
    };
    // Điểm phạt N1–N4 của chuẩn: hàng dài cùng màu, khối 2×2, mẫu giống ô định vị, lệch tỉ lệ đen/trắng
    auto penalty = [&] {
        long p = 0, dark = 0;
        for (int pass = 0; pass < 2; pass++)
            for (int a = 0; a < s; a++) {
                int run = 0;
                uint32_t win = 0;
                for (int b = 0; b < s; b++) {
                    bool c = pass ? q.at(a, b) : q.at(b, a);
                    run = b && c == (pass ? q.at(a, b - 1) : q.at(b - 1, a)) ? run + 1 : 1;
                    if (run == 5) p += 3; else if (run > 5) p++;
                    win = (win << 1 | c) & 0x7FF;
                    if (b >= 10 && (win == 0x5D0 || win == 0x05D)) p += 40;
                }
            }
        for (int y = 0; y < s; y++)
            for (int x = 0; x < s; x++) {
                dark += q.at(x, y);
                if (x && y && q.at(x, y) == q.at(x - 1, y) && q.at(x, y) == q.at(x, y - 1) && q.at(x, y) == q.at(x - 1, y - 1)) p += 3;
            }
        long total = (long)s * s;
        return p + ((std::abs(dark * 20 - total * 10) + total - 1) / total - 1) * 10;
    };
    if (mask < 0) {
        long best = -1;
        for (int i = 0; i < 8; i++) {
            apply(i), format(i);
            long p = penalty();
            if (best < 0 || p < best) best = p, mask = i;
            apply(i);
        }
    }
    apply(mask), format(mask);
    return q;
}

// ======================= Chuyển file và cập nhật: hàm phụ =======================
#ifndef RD_VERSION
#define RD_VERSION "dev"  // build_host.sh đặt theo tag khi phát hành (VERSION=1.2.0)
#endif
// a mới hơn b? So từng số: "1.10.0" > "1.9.2", bỏ chữ v đầu. Bản tự build ("dev") không so được nên không bao giờ cập nhật.
static bool newer(const string& a, const string& b) {
    auto nums = [](const string& v) {
        std::vector<int> n;
        size_t i = v.find_first_of("0123456789");
        while (i < v.size() && isdigit((unsigned char)v[i])) {
            n.push_back(atoi(v.c_str() + i));
            i = v.find_first_not_of("0123456789", i);
            if (i >= v.size() || v[i] != '.') break;
            i++;
        }
        return n;
    };
    auto x = nums(a), y = nums(b);
    if (x.empty() || y.empty()) return false;
    x.resize(std::max(x.size(), y.size())), y.resize(x.size());
    return x > y;
}
struct Release { string tag, url, sha; };  // tag, link bộ cài, SHA-256 bộ cài GitHub tính sẵn
// JSON của api.github.com/repos/.../releases/latest: tìm asset bộ cài (*-Setup.exe). Mỗi asset kết thúc bằng
// browser_download_url, digest nằm trước nó trong cùng asset.
static bool parse_release(const string& js, Release& r) {
    if (!jstr(js, "tag_name", r.tag)) return false;
    size_t from = 0;
    for (size_t p; (p = js.find("\"browser_download_url\":", from)) != string::npos; from = p + 1) {
        if (!jstr(js.substr(p), "browser_download_url", r.url) || !r.url.ends_with("-Setup.exe")) continue;
        size_t d = js.rfind("\"digest\":", p);
        string dig;
        r.sha = d != string::npos && d >= from && jstr(js.substr(d), "digest", dig) && dig.starts_with("sha256:") ? dig.substr(7) : "";
        return r.url.starts_with("https://github.com/");
    }
    return false;
}
// Tên file iPhone gửi lên: bỏ đường dẫn và ký tự Windows cấm, tránh tên thiết bị (CON, NUL...), không để rỗng.
static string safe_name(string n) {
    size_t sl = n.find_last_of("/\\");
    if (sl != string::npos) n.erase(0, sl + 1);
    string o;
    for (unsigned char c : n) o += c < 32 || strchr("<>:\"|?*", c) ? '_' : (char)c;
    while (!o.empty() && (o.back() == '.' || o.back() == ' ')) o.pop_back();
    o.erase(0, std::min(o.find_first_not_of(' '), o.size()));
    if (o.size() > 180) {  // giữ đuôi file, cắt đúng ranh giới ký tự UTF-8
        size_t d = o.rfind('.');
        string ext = d != string::npos && o.size() - d <= 10 ? o.substr(d) : "";
        size_t k = 180 - ext.size();
        while (k && ((unsigned char)o[k] & 0xC0) == 0x80) k--;
        o = o.substr(0, k) + ext;
    }
    string stem = o.substr(0, o.find('.'));
    std::transform(stem.begin(), stem.end(), stem.begin(), ::toupper);
    static const char* dev[] = {"CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
                                "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};
    for (auto x : dev) if (stem == x) o = "_" + o;
    return o.empty() ? "file" : o;
}
static string url_dec(const string& s) {
    string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '+') o += ' ';
        else if (s[i] == '%' && i + 2 < s.size() && isxdigit((unsigned char)s[i + 1]) && isxdigit((unsigned char)s[i + 2]))
            o += (char)std::stoi(s.substr(i + 1, 2), nullptr, 16), i += 2;
        else o += s[i];
    }
    return o;
}
static string qparam(const string& q, const string& key) {  // "a=1&name=x" -> giá trị (chưa giải mã %)
    for (size_t p = 0; p <= q.size();) {
        size_t e = q.find('&', p);
        if (e == string::npos) e = q.size();
        if (q.compare(p, key.size() + 1, key + "=") == 0) return q.substr(p + key.size() + 1, e - p - key.size() - 1);
        p = e + 1;
    }
    return "";
}
static string header(const string& req, const string& low, const string& name) {  // name viết thường
    size_t p = low.find("\r\n" + name + ":");
    if (p == string::npos) return "";
    p += name.size() + 3;
    string v = req.substr(p, req.find("\r\n", p) - p);
    v.erase(0, std::min(v.find_first_not_of(" \t"), v.size()));
    v.erase(v.find_last_not_of(" \t") + 1);
    return v;
}
static string human(uint64_t b) {
    const char* u[] = {"B", "KB", "MB", "GB", "TB"};
    double v = (double)b;
    int i = 0;
    while (v >= 1024 && i < 4) v /= 1024, i++;
    char s[32];
    snprintf(s, sizeof s, i ? "%.1f %s" : "%.0f %s", v, u[i]);
    return s;
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

// Ghép bằng mã QR: cửa sổ PC hiện mã chứa địa chỉ, chứng chỉ và một mã 128-bit dùng một lần (tối đa 10 phút, đóng hộp là
// hết hiệu lực). iPhone quét là được cấp khoá thiết bị như đăng nhập đúng mật khẩu. Chỉ ai nhìn thấy màn hình PC mới có mã,
// nên nhận từ mọi địa chỉ, kể cả Internet.
static std::mutex g_pairmu;
static string g_pair;  // mã đang hiện; rỗng = không ghép
static ULONGLONG g_pair_until;
static string pair_start() {
    uint8_t r[16];
    BCryptGenRandom(nullptr, r, sizeof r, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    std::lock_guard g(g_pairmu);
    g_pair_until = GetTickCount64() + 10 * 60 * 1000;
    return g_pair = hexs(r, sizeof r);
}
static void pair_stop() {
    std::lock_guard g(g_pairmu);
    g_pair.clear();
}
static bool pair_active() {
    std::lock_guard g(g_pairmu);
    return !g_pair.empty() && GetTickCount64() < g_pair_until;
}
// Đúng mã thì xoá ngay (dùng một lần). So hết chuỗi để thời gian trả lời không lộ đúng được bao nhiêu ký tự.
static bool pair_ok(const string& code) {
    std::lock_guard g(g_pairmu);
    if (g_pair.empty() || GetTickCount64() >= g_pair_until || code.size() != g_pair.size()) return false;
    int diff = 0;
    for (size_t i = 0; i < code.size(); i++) diff |= code[i] ^ g_pair[i];
    if (diff) return false;
    g_pair.clear();
    return true;
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
static string https_get(const wchar_t* host, const std::wstring& path) {
    string body;
    HINTERNET ses = WinHttpOpen(L"RemoteDesk", WINHTTP_ACCESS_TYPE_NO_PROXY, nullptr, nullptr, 0);
    HINTERNET con = ses ? WinHttpConnect(ses, host, INTERNET_DEFAULT_HTTPS_PORT, 0) : nullptr;
    HINTERNET req = con ? WinHttpOpenRequest(con, L"GET", path.c_str(), nullptr, nullptr, nullptr, WINHTTP_FLAG_SECURE) : nullptr;
    if (req) WinHttpSetTimeouts(req, 3000, 3000, 3000, 3000);
    if (req && WinHttpSendRequest(req, nullptr, 0, nullptr, 0, 0, 0) && WinHttpReceiveResponse(req, nullptr)) {
        char b[1024];
        DWORD n;
        while (WinHttpReadData(req, b, sizeof b, &n) && n) body.append(b, n);
    }
    for (HINTERNET h : {req, con, ses}) if (h) WinHttpCloseHandle(h);
    return body;
}
static string public_ip() {
    string ip, body = https_get(L"1.1.1.1", L"/cdn-cgi/trace");
    size_t p = body.find("\nip=");
    if (p != string::npos) ip = body.substr(p + 4, body.find('\n', p + 4) - p - 4);
    return ip.find_first_not_of("0123456789.") == string::npos ? ip : "";  // chỉ nhận IPv4
}
static SOCKET tcp_connect(const string& host, int port, int ms) {
    addrinfo hints{}, *ai = nullptr;
    hints.ai_family = AF_INET, hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &ai) || !ai) return INVALID_SOCKET;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    connect(s, ai->ai_addr, (int)ai->ai_addrlen);
    freeaddrinfo(ai);
    fd_set w, e;
    FD_ZERO(&w);  // FD_* là macro do{...}while(0): không nối bằng dấu phẩy được
    FD_ZERO(&e);
    FD_SET(s, &w);
    FD_SET(s, &e);
    timeval tv{ms / 1000, ms % 1000 * 1000};
    if (select(0, nullptr, &w, &e, &tv) != 1 || !FD_ISSET(s, &w)) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    nb = 0;
    ioctlsocket(s, FIONBIO, &nb);
    BOOL nd = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (char*)&nd, sizeof nd);
    return s;
}

// Thử nối TCP (chờ tối đa 3 giây). Từ PC nối vào IP công khai của chính nhà mình chỉ thông khi modem đã chuyển cổng
// và hỗ trợ NAT vòng (hairpin): thông là chắc chắn mở, không thông thì chưa chắc.
static bool port_open(const string& ip, int port) {
    SOCKET s = tcp_connect(ip, port, 3000);
    if (s == INVALID_SOCKET) return false;
    closesocket(s);
    return true;
}
// Tên miền riêng (DDNS): iPhone ở xa vào bằng tên cố định kể cả khi nhà đổi IP, không cần quét mã QR.
// Tên *.duckdns.org có token thì PC tự báo IP mới cho DuckDNS (miễn phí; chỉ là DNS, hình và điều khiển vẫn đi thẳng).
// Tên khác thì coi như modem/router tự cập nhật DDNS, PC chỉ gửi tên cho iPhone.
static std::mutex g_ddnsmu;
static string g_ddns, g_ddns_token, g_ddns_ip;  // g_ddns_ip: IP đã báo DuckDNS lần cuối
static std::atomic<bool> g_net_kick{false};    // đổi tên miền: kiểm tra mạng ngay, không chờ hết 20 phút
static string url_enc(const string& s);
static std::wstring wide(const string& s);
static string ddns_name() {
    std::lock_guard g(g_ddnsmu);
    return g_ddns;
}
// "nha" -> "nha.duckdns.org". Chỉ nhận chữ không dấu, số, dấu chấm, gạch ngang; sai trả "".
static string ddns_norm(string h) {
    h = trim(h);
    std::transform(h.begin(), h.end(), h.begin(), ::tolower);
    if (h.empty() || h.size() > 253 || h.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789.-") != string::npos ||
        h.front() == '.' || h.back() == '.' || h.front() == '-' || h.find("..") != string::npos)
        return "";
    return h.find('.') == string::npos ? h + ".duckdns.org" : h;
}
static void ddns_update() {
    string host, tok, last;
    {
        std::lock_guard g(g_ddnsmu);
        host = g_ddns, tok = g_ddns_token, last = g_ddns_ip;
    }
    const string suf = ".duckdns.org";
    if (tok.empty() || !host.ends_with(suf)) return;
    string ip = public_ip();
    if (ip.empty() || ip == last) return;
    string r = https_get(L"www.duckdns.org", wide("/update?domains=" + url_enc(host.substr(0, host.size() - suf.size())) +
                                                  "&token=" + url_enc(tok) + "&ip=" + ip));
    if (r.starts_with("OK")) {
        std::lock_guard g(g_ddnsmu);
        g_ddns_ip = ip;
    }
    if (r.starts_with("OK")) logf("Tên miền %s đã trỏ về %s", host.c_str(), ip.c_str());
    else logf("DuckDNS không nhận cập nhật cho %s (trả \"%s\"): kiểm tra tên và token", host.c_str(), r.substr(0, 16).c_str());
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
        if (!on || (was && Clock::now() < next && !g_net_kick.exchange(false))) continue;
        was = true;
        next = Clock::now() + std::chrono::minutes(1);  // lỗi thì một phút sau thử lại
        ddns_update();  // trước cả UPnP: router không có UPnP vẫn chuyển cổng tay được
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
// Địa chỉ gửi cho app: LAN, Tailscale, tên miền riêng, rồi IP công khai nếu đã mở cổng.
static std::vector<string> client_addrs() {
    std::vector<string> v;
    for (auto& ip : reachable_ips()) v.push_back(ip + ":" + std::to_string(g_port));
    string dn = ddns_name();
    std::lock_guard g(g_igdmu);
    if (g_remote && !dn.empty()) v.push_back(dn + ":" + std::to_string(g_igd.ext_port ? g_igd.ext_port : g_port));
    if (g_igd.ext_port) v.push_back(g_igd.ext_ip + ":" + std::to_string(g_igd.ext_port));
    return v;
}

static string url_enc(const string& s) {  // khai báo trước ở phần tên miền
    string o;
    char b[4];
    for (unsigned char c : s) {
        if (isalnum(c) || strchr("-._~:,", c)) o += (char)c;
        else snprintf(b, sizeof b, "%%%02X", c), o += b;
    }
    return o;
}
// remotedesk://pair?n=tên máy&a=ip[:cổng],...&c=mã dùng một lần&f=SHA-256 chứng chỉ. Cổng 8443 thì bỏ cho mã QR nhỏ hơn.
static string pair_url(const string& code) {
    string a, name = "PC";
    for (auto ip : client_addrs()) {
        if (ip.ends_with(":8443")) ip.resize(ip.size() - 5);
        a += (a.empty() ? "" : ",") + ip;
    }
    wchar_t comp[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD n = ARRAYSIZE(comp);
    if (GetComputerNameW(comp, &n)) name = utf8(comp);
    string f = g_cert ? sha256_hex(string((const char*)g_cert->pbCertEncoded, g_cert->cbCertEncoded)) : "";
    return "remotedesk://pair?n=" + url_enc(name) + "&a=" + url_enc(a) + "&c=" + code + (f.empty() ? "" : "&f=" + f);
}

// File PC mời iPhone tải (nút "Gửi tới iPhone" hoặc kéo thả vào cửa sổ). Mã ngẫu nhiên, hết hạn sau 1 giờ;
// không xoá sau khi tải để iPhone mất mạng giữa chừng còn tải lại được.
struct Offer {
    std::wstring path;
    string name;
    uint64_t size = 0;
    ULONGLONG until = 0;
};
static std::mutex g_offmu;
static std::map<string, Offer> g_offers;
static bool find_offer(const string& id, Offer& o) {
    std::lock_guard g(g_offmu);
    auto it = g_offers.find(id);
    if (it == g_offers.end() || GetTickCount64() > it->second.until) return false;
    o = it->second;
    return true;
}
// Thư mục nhận file từ iPhone: Tải về\Remote Desk.
static std::filesystem::path recv_dir() {
    PWSTR p = nullptr;
    std::filesystem::path d = SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &p)) ? std::filesystem::path(p) / L"Remote Desk"
                                                                                              : std::filesystem::path(g_dir) / L"Nhận từ iPhone";
    CoTaskMemFree(p);
    std::error_code ec;
    std::filesystem::create_directories(d, ec);
    return d;
}
static std::filesystem::path unique_path(const std::filesystem::path& dir, const string& name) {
    std::filesystem::path f = dir / wide(name), stem = f.stem(), ext = f.extension();
    for (int i = 1; std::filesystem::exists(f); i++) f = dir / (stem.wstring() + L" (" + std::to_wstring(i) + L")" + ext.wstring());
    return f;
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
        const size_t nvsz = (size_t)dw * dh * 3 / 2;
        std::vector<string> out;
        auto period = std::chrono::microseconds(1000000 / fps);
        auto win = Clock::now();
        Clock::duration busy{};  // thời gian bị kẹt khi gửi trong giây hiện tại
        int cur = mbps, calm = 0;
        bool abr_warned = false;
        live_mbps = mbps;
        // Hai luồng chồng nhau: luồng chụp lo chụp + con trỏ + đổi màu (~11 ms/khung 1080p), luồng này lo mã hoá và gửi.
        // Nối đuôi nhau thì mỗi khung ~22 ms, chỉ được ~45 khung/s dù game chạy 60+.
        std::mutex qm;
        std::condition_variable qc;
        std::vector<uint8_t> slot(nvsz), work(nvsz);  // slot: khung mới nhất chờ mã hoá; chưa kịp lấy thì bị khung sau ghi đè
        bool ready = false;
        std::thread capt([&] {
            std::vector<uint8_t> buf(nvsz);
            auto next = Clock::now();  // hạn khung kế theo lịch (giữ nhịp trung bình đúng fps)
            bool dirty = true;
            while (!stop && !restart) {
                int r = cap.grab((UINT)(period.count() / 2000 + 1));
                if (r < 0) { Sleep(100); if (!cap.init()) Sleep(500); dirty = true; continue; }  // UAC, đổi độ phân giải...
                dirty |= r > 0;
                bool key = want_key.load();  // xin khung khoá khi màn hình đứng yên: mã hoá lại hình cuối
                // Nhận sớm tới 1/4 nhịp rồi dời hạn theo lịch: game 64 khung/s mà đòi đủ 16,7 ms mỗi khung thì khung đến
                // sớm 1 ms bị bỏ, chỉ còn ~45 khung/s. Tụt nhịp thì không dồn khung bù.
                auto now = Clock::now();
                if ((!dirty && !key) || cap.bgra.empty() || (now < next - period / 4 && !key)) continue;
                next = std::max(next + period, now - period / 2), dirty = false;
                cap.cursor(true);
                bgra_to_nv12(cap.bgra.data(), cap.w, cap.h, buf.data(), dw, dh);
                cap.cursor(false);
                {
                    std::lock_guard g(qm);
                    slot.swap(buf), ready = true;
                }
                qc.notify_one();
            }
            qc.notify_one();
        });
        auto send = [&]() {
            for (auto& o : out) {
                auto t0 = Clock::now();
                // byte đầu: bit 0 = khung khoá, bit 1 = HEVC
                if (!ws_send(t, 2, string(1, (char)(is_key(o, enc.hevc) | (enc.hevc ? 2 : 0))) + o)) return false;
                busy += Clock::now() - t0;
                frames++, bytes += o.size();
            }
            out.clear();
            return true;
        };
        for (int64_t n = 1; !stop && !restart;) {
            bool got;
            {  // còn khung đang mã hoá thì chỉ chờ 2 ms để gói xong được gửi ngay
                std::unique_lock lk(qm);
                qc.wait_for(lk, enc.pending ? std::chrono::microseconds(2000) : period, [&] { return ready || stop || restart; });
                got = ready;
                if (got) work.swap(slot), ready = false;
            }
            enc.drain(out);
            if (!send()) { stop = true; break; }
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
            if (!got) continue;
            if (!enc.encode(work.data(), n++, want_key.exchange(false), out)) { logf("Encoder lỗi"); stop = true; break; }
            if (!send()) { stop = true; break; }
        }
        capt.join();
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

    // Chuyển file trên cùng cổng, mỗi file một kết nối HTTPS riêng để không chặn luồng hình:
    //   POST /upload?name=...  iPhone gửi lên, lưu vào Tải về\Remote Desk;   GET /file?id=...  iPhone tải file PC mời gửi.
    // Xác thực bằng khoá thiết bị (header X-Token) như lúc vào phiên, sai thì tính vào khoá IP chống dò.
    void files(const string& req, const string& low, const string& method, const string& path, const string& query) {
        auto reply = [&](const char* status, const string& body) {
            string r = string("HTTP/1.1 ") + status + "\r\nContent-Type: application/json\r\nContent-Length: " +
                       std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
            t.send(r.data(), r.size());
        };
        if (auth_locked(peer)) return reply("429 Too Many Requests", R"({"error":"locked"})");
        if (!token_ok(header(req, low, "x-token"))) {
            auth_failed(peer);
            return reply("401 Unauthorized", R"({"error":"token"})");
        }
        DWORD to = 30000;  // iPhone mất mạng giữa chừng thì bỏ, không treo luồng
        setsockopt(t.s, SOL_SOCKET, SO_RCVTIMEO, (char*)&to, sizeof to);
        if (method == "POST" && path == "/upload") {
            string cl = header(req, low, "content-length");
            if (cl.empty() || cl.size() > 15 || cl.find_first_not_of("0123456789") != string::npos)
                return reply("411 Length Required", R"({"error":"length"})");
            uint64_t len = std::stoull(cl), got = 0;
            auto dst = unique_path(recv_dir(), safe_name(url_dec(qparam(query, "name"))));
            auto part = dst;
            part += L".part";
            {
                std::ofstream f(part, std::ios::binary);
                while (f && got < len) {
                    if (t.plain.empty() && !t.fill()) break;
                    size_t k = (size_t)std::min<uint64_t>(t.plain.size(), len - got);
                    f.write(t.plain.data(), k);
                    got += k;
                    t.plain.erase(0, k);
                }
                if (!f) got = UINT64_MAX;  // không ghi được (đĩa đầy)
            }
            std::error_code ec;
            if (got == len) std::filesystem::rename(part, dst, ec);
            string name = utf8(dst.filename().wstring());
            if (got != len || ec) {
                std::filesystem::remove(part, ec);
                logf("Nhận \"%s\" từ iPhone không trọn vẹn, đã bỏ", name.c_str());
                return reply("400 Bad Request", R"({"error":"incomplete"})");
            }
            logf("Đã nhận \"%s\" (%s) từ iPhone, lưu trong Tải về\\Remote Desk", name.c_str(), human(len).c_str());
            return reply("200 OK", R"({"ok":true,"name":")" + jesc(name) + "\"}");
        }
        if (method == "GET" && path == "/file") {
            Offer o;
            std::ifstream f;
            if (find_offer(qparam(query, "id"), o)) f.open(std::filesystem::path(o.path), std::ios::binary);
            if (!f.is_open()) return reply("404 Not Found", R"({"error":"id"})");  // ifstream chưa mở thì !f vẫn là false
            string h = "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: " + std::to_string(o.size) +
                       "\r\nConnection: close\r\n\r\n";
            uint64_t sent = 0;
            std::vector<char> b(1 << 16);
            bool ok = t.send(h.data(), h.size());
            while (ok && (f.read(b.data(), b.size()) || f.gcount())) {
                ok = t.send(b.data(), (size_t)f.gcount());
                if (ok) sent += f.gcount();
            }
            logf(sent == o.size ? "Đã gửi \"%s\" tới iPhone" : "Gửi \"%s\" tới iPhone bị ngắt giữa chừng", o.name.c_str());
            return;
        }
        reply("405 Method Not Allowed", R"({"error":"method"})");
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
        string query = path.find('?') == string::npos ? "" : path.substr(path.find('?') + 1);
        path = path.substr(0, path.find('?'));
        if (path == "/upload" || path == "/file") return files(req, low, req.substr(0, sp1), path, query);
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
        string m, user, pass, tok, issued, pair;
        if (!ws_read(t, op, m)) return;
        jstr(m, "user", user);
        jstr(m, "pass", pass);
        jstr(m, "token", tok);
        jstr(m, "pair", pair);
        size_t hv = jfind(m, "hevc");
        client_hevc = hv != string::npos && m.compare(hv, 4, "true") == 0;
        bool local = is_private(peer);
        if (auth_locked(peer)) {
            ws_send(t, 1, R"({"t":"locked"})");
            return;
        }
        bool ok = token_ok(tok);
        if (!ok && !pair.empty()) {  // quét mã QR trên PC: mã dùng một lần thay cho mật khẩu
            if (!pair_ok(pair)) {
                auth_failed(peer);
                Sleep(1000);
                ws_send(t, 1, R"({"t":"badpair"})");
                return;
            }
            ok = true;
            issued = issue_token();
            logf("Đã ghép iPhone bằng mã QR (%s): đã cấp khoá cho thiết bị mới", peer.c_str());
        }
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
                                  jesc(enc_name) + R"(","size":[)" + std::to_string(vw) + "," + std::to_string(vh) + R"(],"q":[)" +
                                  std::to_string(g_width) + "," + std::to_string(g_fps) + "," + std::to_string(g_mbps) + "," +
                                  (g_hevc ? "1" : "0") + "]}");
            } else if (ty == "quality") {  // iPhone đổi chất lượng: chỉ nhận đúng các mức có trên cửa sổ PC
                const int* tab[] = {RES_W, FPS_V, RATE_V};
                const int cnt[] = {4, 4, 5};
                const char* key[] = {"w", "fps", "mbps", "hevc"};
                for (int grp = 0; grp < 4; grp++) {
                    double v;
                    if (!jnum(m, key[grp], v)) continue;
                    int idx = grp == 3 ? (v != 0) : -1;
                    for (int i = 0; grp < 3 && i < cnt[grp]; i++) if (tab[grp][i] == (int)v) idx = i;
                    if (idx >= 0 && g_hwnd) PostMessageW(g_hwnd, WM_APP_SET, grp, idx);  // áp dụng, lưu, phát lại ở luồng giao diện
                }
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

// PC gửi file: báo iPhone đang kết nối, iPhone tự tải về qua GET /file. Chạy ở luồng riêng: gửi tin có thể chờ luồng hình.
static void offer_files(std::vector<std::wstring> paths) {
    std::lock_guard g(g_cmu);  // giữ g_current sống trong lúc gửi
    if (!g_current) { logf("Chưa có iPhone nào đang kết nối để gửi file"); return; }
    for (auto& p : paths) {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(p, ec)) { logf("Bỏ qua thư mục %s: chỉ gửi được file", utf8(p).c_str()); continue; }
        Offer o{p, utf8(std::filesystem::path(p).filename().wstring()), std::filesystem::file_size(p, ec), GetTickCount64() + 3600000};
        uint8_t r[16];
        BCryptGenRandom(nullptr, r, sizeof r, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        string id = hexs(r, sizeof r);
        {
            std::lock_guard g2(g_offmu);
            g_offers[id] = o;
        }
        ws_send(g_current->t, 1, R"({"t":"file","id":")" + id + R"(","name":")" + jesc(o.name) + R"(","size":)" + std::to_string(o.size) + "}");
        logf("Đang gửi \"%s\" (%s) tới iPhone", o.name.c_str(), human(o.size).c_str());
    }
}

// ======================= Tự cập nhật =======================
// Kiểm GitHub Release lúc mở app (sau 1 phút) và mỗi 6 giờ. Bản cài bằng bộ cài thì tự tải bộ cài mới, kiểm SHA-256 theo
// GitHub, rồi chạy im lặng khi không có iPhone nào đang điều khiển; bộ cài đóng app này rồi mở lại bản mới (/R).
static const wchar_t* RELEASES = L"https://github.com/ductruongvu23/remote-desk/releases/latest";
static std::mutex g_updmu;
static Release g_rel;  // bản mới hơn đang có trên GitHub (tag rỗng: chưa có)
static std::atomic<bool> g_updating{false}, g_autoupdate{true};
static Release latest_update() {
    std::lock_guard g(g_updmu);
    return g_rel;
}
static bool installed_copy() {  // chạy từ thư mục bộ cài đã cài (không phải bản zip hay bản tự build)?
    wchar_t b[MAX_PATH];
    DWORD n = sizeof b;
    return !RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\RemoteDesk", L"InstallLocation",
                         RRF_RT_REG_SZ, nullptr, b, &n) &&
           !lstrcmpiW(b, g_dir.c_str());
}
static void update_now(Release r) {
    if (g_updating.exchange(true)) return;
    logf("Đang tải Remote Desk %s…", r.tag.c_str());
    size_t h = r.url.find('/', 8);
    string body = h == string::npos ? "" : https_get(wide(r.url.substr(8, h - 8)).c_str(), wide(r.url.substr(h)));
    bool ok = r.sha.empty() ? body.size() > 200000 && body.starts_with("MZ") : sha256_hex(body) == r.sha;
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring exe = std::wstring(tmp) + L"RemoteDesk-Setup-" + wide(safe_name(r.tag)) + L".exe";
    if (ok) ok = (bool)(std::ofstream(std::filesystem::path(exe), std::ios::binary) << body);
    // /D phải đứng cuối và không có ngoặc kép (quy ước NSIS)
    std::wstring args = L"/S /R /D=" + g_dir;
    if (ok) ok = (INT_PTR)ShellExecuteW(nullptr, L"open", exe.c_str(), args.c_str(), nullptr, SW_HIDE) > 32;
    if (ok) {
        logf("Đang cài Remote Desk %s, app sẽ tự mở lại", r.tag.c_str());
        return;  // bộ cài sẽ đóng app này
    }
    logf("Không cập nhật được lên %s (tải %s): thử lại sau", r.tag.c_str(), human(body.size()).c_str());
    {
        std::lock_guard g(g_updmu);
        g_rel = {};  // lần kiểm sau (6 giờ) tìm lại
    }
    g_updating = false;
}
static void update_loop() {
    if (!newer(RD_VERSION, "0")) return;  // bản tự build: không có số phiên bản để so
    auto next = Clock::now() + std::chrono::minutes(1);
    for (;; Sleep(60000)) {
        if (Clock::now() >= next) {
            Release r;
            bool got = parse_release(https_get(L"api.github.com", L"/repos/ductruongvu23/remote-desk/releases/latest"), r);
            next = Clock::now() + (got ? std::chrono::hours(6) : std::chrono::minutes(30));
            if (got && newer(r.tag, RD_VERSION) && latest_update().tag != r.tag) {
                {
                    std::lock_guard g(g_updmu);
                    g_rel = r;
                }
                logf("Có bản mới Remote Desk %s (đang dùng %s)", r.tag.c_str(), RD_VERSION);
            }
        }
        Release r = latest_update();
        bool idle;
        {
            std::lock_guard g(g_cmu);
            idle = !g_current;
        }
        if (!r.tag.empty() && g_autoupdate && idle && installed_copy()) update_now(r);
    }
}

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

// ======================= Giao diện Windows: kiểu Cài đặt của Windows 11/macOS, có bản tối =======================
// Thanh bên chọn trang; thẻ trắng viền mảnh chia hàng; nút viên thuốc (xanh cho việc chính); phân đoạn kiểu Apple.
// Theo chế độ sáng/tối của Windows (App mode) và tự đổi khi người dùng đổi.
enum { ID_ACCOUNT = 101, ID_KICK, ID_FORGET, ID_ACC_SAVE, ID_ACC_CANCEL, ID_PAIR, ID_DDNS, ID_SEND, ID_RECVDIR, ID_UPDATE, ID_VNC,
       ID_ACC_EDIT = 120, ID_SEG = 200, ID_NAV = 300 };  // phân đoạn: ID_SEG + nhóm*10 + vị trí; trang: ID_NAV + trang
static DNS_SERVICE_REGISTER_REQUEST g_reg{};

static std::wstring wide(const string& s) {
    std::wstring w(MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0), 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), (int)w.size());
    return w;
}
// ======================= Điều khiển iPhone qua TrollVNC (RFB): phần lõi =======================
// TrollVNC chạy trên iPhone đã jailbreak (LibVNCServer 0.9.15, cổng 5901, quảng bá _rfb._tcp qua Bonjour).
// Xin mã hoá Tight + JPEG: đo trên iPhone 11 Pro Max qua Wi‑Fi được ~25 khung/s toàn màn (112 KB/khung); chỉ zlib thì
// ~2,6 khung/s. Chuột trái = chạm, chuột phải = nút Home, chuột giữa = nút nguồn (quy ước của TrollVNC).
using ReadFn = std::function<bool(void*, size_t)>;

// Độ dài kiểu "compact" của Tight: 1–3 byte, mỗi byte 7 bit (byte thứ ba đủ 8 bit).
static bool tight_len(const ReadFn& rd, uint32_t& n) {
    n = 0;
    for (int i = 0; i < 3; i++) {
        uint8_t b;
        if (!rd(&b, 1)) return false;
        n |= (uint32_t)(i < 2 ? b & 0x7F : b) << (7 * i);
        if (i < 2 && !(b & 0x80)) break;
    }
    return true;
}

// Giải mã Tight. Bốn luồng zlib dùng tiếp suốt phiên (máy chủ chỉ reset khi báo), ô ảnh chụp/game là JPEG.
struct Tight {
    z_stream zs[4]{};
    bool live[4]{};
    ComPtr<IWICImagingFactory> wic;
    ~Tight() {
        for (int i = 0; i < 4; i++) if (live[i]) inflateEnd(&zs[i]);
    }
    bool unzip(int id, const std::vector<uint8_t>& in, uint8_t* out, size_t size) {
        z_stream& z = zs[id];
        if (!live[id]) {
            z = {};
            if (inflateInit(&z) != Z_OK) return false;
            live[id] = true;
        }
        z.next_in = (Bytef*)in.data(), z.avail_in = (uInt)in.size();
        size_t got = 0;
        while (z.avail_in) {  // sau dữ liệu còn dấu đồng bộ (00 00 FF FF): phải đọc hết nhưng không được ra thêm byte nào
            uint8_t sink;
            bool full = got == size;
            z.next_out = full ? &sink : out + got, z.avail_out = full ? 1 : (uInt)(size - got);
            uInt before = z.avail_out;
            int r = inflate(&z, Z_SYNC_FLUSH);
            if (full && z.avail_out != before) return false;
            got += before - z.avail_out;
            if (r == Z_STREAM_END || r == Z_BUF_ERROR) break;
            if (r != Z_OK) return false;
        }
        return got == size;
    }
    bool jpeg(const std::vector<uint8_t>& j, int w, int h, uint32_t* out) {
        if (!wic && FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return false;
        ComPtr<IWICStream> st;
        ComPtr<IWICBitmapDecoder> dec;
        ComPtr<IWICBitmapFrameDecode> fr;
        ComPtr<IWICFormatConverter> cv;
        UINT fw = 0, fh = 0;
        return SUCCEEDED(wic->CreateStream(&st)) && SUCCEEDED(st->InitializeFromMemory((BYTE*)j.data(), (DWORD)j.size())) &&
               SUCCEEDED(wic->CreateDecoderFromStream(st.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &dec)) &&
               SUCCEEDED(dec->GetFrame(0, &fr)) && SUCCEEDED(fr->GetSize(&fw, &fh)) && (int)fw == w && (int)fh == h &&
               SUCCEEDED(wic->CreateFormatConverter(&cv)) &&
               SUCCEEDED(cv->Initialize(fr.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                        WICBitmapPaletteTypeCustom)) &&
               SUCCEEDED(cv->CopyPixels(nullptr, w * 4, w * h * 4, (BYTE*)out));
    }
    // Một ô w×h ra out (BGRX). Điểm ảnh 3 byte theo thứ tự R, G, B (định dạng 32 bit, sâu 24 bit).
    bool rect(const ReadFn& rd, int w, int h, std::vector<uint32_t>& out) {
        auto px = [](const uint8_t* p) { return 0xFF000000u | p[0] << 16 | p[1] << 8 | p[2]; };
        uint8_t cc;
        if (!rd(&cc, 1)) return false;
        for (int i = 0; i < 4; i++)
            if ((cc >> i & 1) && live[i]) inflateEnd(&zs[i]), live[i] = false;
        cc >>= 4;
        out.assign((size_t)w * h, 0);
        uint32_t n;
        if (cc == 8) {  // cả ô một màu
            uint8_t c[3];
            if (!rd(c, 3)) return false;
            std::fill(out.begin(), out.end(), px(c));
            return true;
        }
        if (cc == 9) {
            if (!tight_len(rd, n) || n > (32u << 20)) return false;
            std::vector<uint8_t> j(n);
            return rd(j.data(), n) && jpeg(j, w, h, out.data());
        }
        if (cc > 9) return false;  // 0x0A là PNG của TightPNG: không xin
        uint8_t filter = 0;
        if ((cc & 4) && !rd(&filter, 1)) return false;
        std::vector<uint32_t> pal;
        int bits = 24;
        if (filter == 1) {
            uint8_t k;
            if (!rd(&k, 1)) return false;
            std::vector<uint8_t> p((k + 1) * 3);
            if (!rd(p.data(), p.size())) return false;
            for (size_t i = 0; i < p.size(); i += 3) pal.push_back(px(&p[i]));
            bits = pal.size() == 2 ? 1 : 8;
        } else if (filter > 2) {
            return false;
        }
        size_t row = bits == 24 ? (size_t)w * 3 : ((size_t)w * bits + 7) / 8, size = row * h;
        std::vector<uint8_t> d(size);
        if (size < 12) {  // dưới 12 byte thì gửi thẳng, không nén
            if (!rd(d.data(), size)) return false;
        } else {
            if (!tight_len(rd, n) || n > (32u << 20)) return false;
            std::vector<uint8_t> z(n);
            if (!rd(z.data(), n) || !unzip(cc & 3, z, d.data(), size)) return false;
        }
        if (filter == 1) {
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++) {
                    size_t i = bits == 1 ? d[y * row + x / 8] >> (7 - x % 8) & 1 : d[y * row + x];
                    if (i >= pal.size()) return false;
                    out[(size_t)y * w + x] = pal[i];
                }
        } else if (filter == 2) {  // gradient: mỗi điểm = dự đoán (trái + trên − trên trái, kẹp 0..255) + phần dư
            std::vector<uint8_t> prev((size_t)w * 3, 0), cur((size_t)w * 3);
            for (int y = 0; y < h; y++) {
                for (int x = 0; x < w; x++)
                    for (int c = 0; c < 3; c++) {
                        int est = x ? std::clamp(prev[x * 3 + c] + cur[(x - 1) * 3 + c] - prev[(x - 1) * 3 + c], 0, 255) : prev[c];
                        cur[x * 3 + c] = (uint8_t)(est + d[y * row + x * 3 + c]);
                    }
                for (int x = 0; x < w; x++) out[(size_t)y * w + x] = px(&cur[x * 3]);
                prev.swap(cur);
            }
        } else {
            for (size_t i = 0; i < (size_t)w * h; i++) out[i] = px(&d[i * 3]);
        }
        return true;
    }
};

// Xác thực VNC cổ điển: mã DES 16 byte thử thách bằng mật khẩu (tối đa 8 ký tự, mỗi byte đảo thứ tự bit).
static bool vnc_auth(const string& pass, const uint8_t ch[16], uint8_t out[16]) {
    uint8_t key[8] = {};
    for (size_t i = 0; i < 8 && i < pass.size(); i++)
        for (int b = 0; b < 8; b++) key[i] |= ((uint8_t)pass[i] >> b & 1) << (7 - b);
    BCRYPT_ALG_HANDLE a;
    BCRYPT_KEY_HANDLE k;
    ULONG n = 0;
    if (BCryptOpenAlgorithmProvider(&a, BCRYPT_DES_ALGORITHM, nullptr, 0)) return false;
    bool ok = !BCryptSetProperty(a, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_ECB, sizeof BCRYPT_CHAIN_MODE_ECB, 0) &&
              !BCryptGenerateSymmetricKey(a, &k, nullptr, 0, key, 8, 0);
    if (ok) {
        ok = !BCryptEncrypt(k, (PUCHAR)ch, 16, nullptr, nullptr, 0, out, 16, &n, 0) && n == 16;
        BCryptDestroyKey(k);
    }
    BCryptCloseAlgorithmProvider(a, 0);
    return ok;
}

// Phím PC -> keysym X11. Chữ gửi dạng thường, Shift gửi riêng như phím thật: TrollVNC đổi keysym ra phím HID gốc
// (':' thành phím ';'), không tự thêm Shift. Ctrl -> Command để Ctrl+C/V/A trên PC thành ⌘C/V/A trên iPhone.
static uint32_t vnc_keysym(UINT vk) {
    if ((vk >= '0' && vk <= '9') || vk == VK_SPACE) return vk;
    if (vk >= 'A' && vk <= 'Z') return vk + 32;
    if (vk >= VK_F1 && vk <= VK_F24) return 0xFFBE + (vk - VK_F1);
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return '0' + (vk - VK_NUMPAD0);
    switch (vk) {
    case VK_BACK: return 0xFF08;
    case VK_TAB: return 0xFF09;
    case VK_RETURN: return 0xFF0D;
    case VK_ESCAPE: return 0xFF1B;
    case VK_DELETE: return 0xFFFF;
    case VK_INSERT: return 0xFF63;
    case VK_HOME: return 0xFF50;
    case VK_END: return 0xFF57;
    case VK_PRIOR: return 0xFF55;
    case VK_NEXT: return 0xFF56;
    case VK_LEFT: return 0xFF51;
    case VK_UP: return 0xFF52;
    case VK_RIGHT: return 0xFF53;
    case VK_DOWN: return 0xFF54;
    case VK_SHIFT: return 0xFFE1;
    case VK_CONTROL: return 0xFFEB;  // Super = Command
    case VK_MENU: return 0xFFE9;     // Alt = Option
    case VK_CAPITAL: return 0xFFE5;
    case VK_OEM_1: return ';';
    case VK_OEM_PLUS: return '=';
    case VK_OEM_COMMA: return ',';
    case VK_OEM_MINUS: return '-';
    case VK_OEM_PERIOD: return '.';
    case VK_OEM_2: return '/';
    case VK_OEM_3: return '`';
    case VK_OEM_4: return '[';
    case VK_OEM_5: return '\\';
    case VK_OEM_6: return ']';
    case VK_OEM_7: return '\'';
    }
    return 0;
}

// Tìm iPhone đang bật TrollVNC trong mạng nhà (Bonjour _rfb._tcp), để không phải gõ IP. Giữ kết quả mới nhất.
static std::mutex g_rfbmu;
static string g_rfb_addr, g_rfb_name;
static VOID WINAPI rfb_resolved(DWORD st, PVOID, PDNS_SERVICE_INSTANCE in) {
    if (!st && in && in->ip4Address) {
        char b[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, in->ip4Address, b, sizeof b);
        string name = in->pszInstanceName ? utf8(in->pszInstanceName) : "iPhone";
        name = name.substr(0, name.find("._rfb."));
        std::lock_guard g(g_rfbmu);  // máy khác cũng quảng bá VNC (Mac bật Chia sẻ màn hình...): ưu tiên tên TrollVNC
        if (name.find("TrollVNC") != string::npos || g_rfb_name.find("TrollVNC") == string::npos)
            g_rfb_addr = string(b) + ":" + std::to_string(in->wPort), g_rfb_name = name;
    }
    if (in) DnsServiceFreeInstance(in);
}
static VOID WINAPI rfb_found(DWORD st, PVOID, PDNS_RECORD r) {
    for (auto p = (PDNS_RECORDW)r; !st && p; p = p->pNext)  // API này luôn trả chuỗi Unicode
        if (p->wType == DNS_TYPE_PTR && p->dwTtl) {  // TTL 0 là dịch vụ vừa tắt
            // shortcut: yêu cầu phân giải không giải phóng (vài chục byte mỗi lần thấy iPhone), đủ cho app chạy cả ngày
            struct Rq { DNS_SERVICE_RESOLVE_REQUEST q; DNS_SERVICE_CANCEL c; };
            auto* rq = new Rq{{DNS_QUERY_REQUEST_VERSION1, 0, _wcsdup(p->Data.PTR.pNameHost), rfb_resolved, nullptr}, {}};
            DnsServiceResolve(&rq->q, &rq->c);
        }
    if (r) DnsRecordListFree(r, DnsFreeRecordList);
}
static void rfb_browse() {
    static DNS_SERVICE_BROWSE_REQUEST q{};
    static DNS_SERVICE_CANCEL c;
    q.Version = DNS_QUERY_REQUEST_VERSION1, q.QueryName = L"_rfb._tcp.local", q.pBrowseCallback = rfb_found;
    if (DnsServiceBrowse(&q, &c) != DNS_REQUEST_PENDING) logf("Không dò được iPhone (TrollVNC) qua Bonjour; nhập địa chỉ bằng tay");
}

// Mật khẩu VNC lưu bằng DPAPI (chỉ tài khoản Windows này giải được), không để chữ rõ trong remote_settings.ini.
static string dpapi(const string& s, bool protect) {
    DATA_BLOB in{(DWORD)s.size(), (BYTE*)s.data()}, out{};
    if (s.empty() || !(protect ? CryptProtectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)
                               : CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)))
        return "";
    string r((char*)out.pbData, out.cbData);
    LocalFree(out.pbData);
    return r;
}
static string unhex(const string& h) {
    string r;
    for (size_t i = 0; i + 1 < h.size(); i += 2) {
        if (!isxdigit((unsigned char)h[i]) || !isxdigit((unsigned char)h[i + 1])) return "";  // file cài đặt bị sửa hỏng
        r += (char)std::stoi(h.substr(i, 2), nullptr, 16);
    }
    return r;
}
static string g_vnc_addr, g_vnc_pass;  // iPhone (TrollVNC) dùng lần trước

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
    g_autoupdate = GetPrivateProfileIntW(L"app", L"autoupdate", 1, ini.c_str()) != 0;  // 0: chỉ báo, không tự cài
    g_remote = GetPrivateProfileIntW(L"net", L"remote", 0, ini.c_str()) != 0;
    wchar_t b[300];
    GetPrivateProfileStringW(L"net", L"ddns", L"", b, 300, ini.c_str());
    g_ddns = ddns_norm(utf8(b));
    GetPrivateProfileStringW(L"net", L"ddns_token", L"", b, 300, ini.c_str());
    g_ddns_token = trim(utf8(b));
    GetPrivateProfileStringW(L"vnc", L"addr", L"", b, 300, ini.c_str());
    g_vnc_addr = trim(utf8(b));
    wchar_t pb[2048];  // mật khẩu đã mã hoá DPAPI, dạng hex
    GetPrivateProfileStringW(L"vnc", L"pass", L"", pb, 2048, ini.c_str());
    g_vnc_pass = dpapi(unhex(utf8(pb)), false);
}
static void save_settings() {
    auto ini = ini_path();
    WritePrivateProfileStringW(L"stream", L"width", std::to_wstring(g_width).c_str(), ini.c_str());
    WritePrivateProfileStringW(L"stream", L"fps", std::to_wstring(g_fps).c_str(), ini.c_str());
    WritePrivateProfileStringW(L"stream", L"mbps", std::to_wstring(g_mbps).c_str(), ini.c_str());
    WritePrivateProfileStringW(L"stream", L"hevc", g_hevc ? L"1" : L"0", ini.c_str());
    WritePrivateProfileStringW(L"net", L"remote", g_remote ? L"1" : L"0", ini.c_str());
    WritePrivateProfileStringW(L"vnc", L"addr", wide(g_vnc_addr).c_str(), ini.c_str());
    string vp = dpapi(g_vnc_pass, true);
    WritePrivateProfileStringW(L"vnc", L"pass", wide(hexs((const uint8_t*)vp.data(), vp.size())).c_str(), ini.c_str());
    std::lock_guard g(g_ddnsmu);
    WritePrivateProfileStringW(L"net", L"ddns", wide(g_ddns).c_str(), ini.c_str());
    WritePrivateProfileStringW(L"net", L"ddns_token", wide(g_ddns_token).c_str(), ini.c_str());
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

// ---- Bảng màu: nền xám nhạt như Cài đặt của Windows 11/macOS, thẻ trắng viền mảnh; bản tối theo macOS ----
// R_BAND: nút phụ đang bấm/có chuột; R_TRACK: nền nút phụ và rãnh phân đoạn; R_SEL: mục đang chọn ở thanh bên
enum Role { R_CANVAS, R_BAND, R_CARD, R_HAIR, R_TRACK, R_SEGON, R_INK, R_SLATE, R_STEEL, R_BLUE, R_ORANGE, R_SEL, R_GREEN, R_COUNT };
static const COLORREF LIGHT[R_COUNT] = {RGB(243, 243, 246), RGB(222, 222, 228), RGB(255, 255, 255), RGB(224, 224, 230),
                                        RGB(236, 236, 241), RGB(255, 255, 255), RGB(29, 29, 31),    RGB(108, 108, 113),
                                        RGB(142, 142, 147), RGB(0, 113, 227),   RGB(182, 68, 0),    RGB(226, 226, 232),
                                        RGB(40, 167, 69)};
static const COLORREF DARK[R_COUNT] = {RGB(24, 24, 27),    RGB(62, 62, 67),    RGB(36, 36, 40),    RGB(50, 50, 55),
                                       RGB(48, 48, 53),    RGB(96, 96, 101),   RGB(245, 245, 247), RGB(161, 161, 166),
                                       RGB(110, 110, 115), RGB(10, 132, 255),  RGB(255, 159, 10),  RGB(46, 46, 51),
                                       RGB(48, 209, 88)};
static const COLORREF* P = LIGHT;
static HBRUSH g_brush[R_COUNT];
static bool g_dark;

enum Font { F_BRAND, F_CAPTION, F_HEAD, F_BODY, F_SMALL, F_STAT, F_TITLE, F_SECTION, F_ICON, F_ICONL, F_COUNT };
static HFONT g_font[F_COUNT];

// Bố cục theo 96 DPI: thanh bên (tên app, các trang, trạng thái) và trang đang chọn ở bên phải.
constexpr int WIN_W = 840, WIN_H = 440, X0 = 248, X1 = 812, BTN_X = X1 - 156, SEG_H = 30;  // BTN_X: nút ở mép phải của hàng
enum Page { PG_HOME, PG_QUALITY, PG_REMOTE, PG_FILES, PG_VNC, PG_LOG };
constexpr int PG_COUNT = PG_LOG + 1;
static const struct { const wchar_t *name, *glyph; COLORREF tile; } NAV[PG_COUNT] = {  // glyph: mã của Segoe Fluent Icons
    {L"Tổng quan", L"\xE80F", RGB(0, 122, 255)},         {L"Chất lượng hình", L"\xE714", RGB(175, 82, 222)},
    {L"Truy cập từ xa", L"\xE774", RGB(40, 167, 69)},    {L"Chuyển file", L"\xE8B7", RGB(255, 149, 0)},
    {L"iPhone (TrollVNC)", L"\xE8EA", RGB(88, 86, 214)}, {L"Nhật ký", L"\xE81C", RGB(142, 142, 147)}};
struct Card { int page; RECT r; int rows, radius; bool dashed; };  // rows > 1: kẻ vạch giữa các hàng
struct Ctl { HWND h; int x, y, w, h_; Font font; Role fg, bg; HWND parent; int page; bool off; };  // fg = R_BLUE: nút chính
struct Seg { HWND h; int grp, idx, n, x, w, y, page; };  // rãnh phân đoạn của nhóm: x, w, y theo 96 DPI
static std::vector<Card> g_cards;
static std::vector<Ctl> g_ctls;
static std::vector<Seg> g_segs;
static int g_page, g_mkpage = -1;  // trang đang xem; trang của điều khiển đang dựng (-1: luôn hiện: thanh bên, hộp thoại)
static HWND h_status, h_detail, h_pair, h_kick, h_stat[4], h_cap[4], h_acct, h_accsub, h_accbtn, h_devs, h_forget, h_side,
    h_upd, h_log, h_net, h_ddns, h_send, h_sendhint, h_vncname, h_vnc;
static HWND g_hot;  // nút đang có chuột ở trên
static HICON g_icon;
static HWND g_dlg, h_dlgerr;  // hộp tạo/đổi tài khoản, tên miền, mã QR
static void (*g_dlg_save)(HWND);  // nút Lưu / phím Enter của hộp có ô nhập
static std::vector<HWND> g_dlg_edits;
static int g_sel[5], g_dot = -1;  // g_dot: màu chấm trạng thái (0 chưa có tài khoản, 1 chờ iPhone, 2 đang phát)
static bool g_streaming;
static ULONG_PTR g_gdip;

static int S(int v) { return MulDiv(v, GetDpiForWindow(g_hwnd), 96); }
static Gdiplus::Color gc(COLORREF c) { return Gdiplus::Color(255, GetRValue(c), GetGValue(c), GetBValue(c)); }
static COLORREF mix(COLORREF a, COLORREF b, int pct) {  // a pha pct% màu b
    auto m = [pct](int x, int y) { return x + (y - x) * pct / 100; };
    return RGB(m(GetRValue(a), GetRValue(b)), m(GetGValue(a), GetGValue(b)), m(GetBValue(a), GetBValue(b)));
}

static void rrect(Gdiplus::Graphics& g, float x, float y, float w, float h, float r, COLORREF fill, COLORREF line = CLR_INVALID,
                  float lw = 1, bool dash = false) {
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
        if (dash) pen.SetDashStyle(Gdiplus::DashStyleDash);
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
    if (h_log) SetWindowTheme(h_log, g_dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);  // thanh cuộn tối theo nền
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

static const wchar_t* icon_face() {  // Windows 11: Segoe Fluent Icons; Windows 10 chỉ có Segoe MDL2 Assets (cùng mã ký tự)
    static const wchar_t* face;
    if (!face) {
        LOGFONTW lf{};
        lf.lfCharSet = DEFAULT_CHARSET;
        wcscpy(lf.lfFaceName, L"Segoe Fluent Icons");
        bool found = false;
        HDC dc = GetDC(nullptr);
        EnumFontFamiliesExW(dc, &lf, [](const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM p) -> int { return *(bool*)p = true, 0; },
                            (LPARAM)&found, 0);
        ReleaseDC(nullptr, dc);
        face = found ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";
    }
    return face;
}
static void layout() {
    auto mk = [](const wchar_t* face, int px, int weight) {
        return CreateFontW(-S(px), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, face);
    };
    for (HFONT& f : g_font) if (f) DeleteObject(f), f = nullptr;
    const wchar_t *disp = L"Segoe UI Variable Display", *text = L"Segoe UI Variable Text";  // thay cho SF Pro
    g_font[F_BRAND] = mk(disp, 15, FW_SEMIBOLD), g_font[F_CAPTION] = mk(text, 12, FW_NORMAL);
    g_font[F_HEAD] = mk(disp, 26, FW_SEMIBOLD), g_font[F_BODY] = mk(text, 14, FW_NORMAL);
    g_font[F_SMALL] = mk(text, 13, FW_NORMAL), g_font[F_STAT] = mk(disp, 24, FW_SEMIBOLD);
    g_font[F_TITLE] = mk(disp, 24, FW_SEMIBOLD), g_font[F_SECTION] = mk(text, 13, FW_SEMIBOLD);
    g_font[F_ICON] = mk(icon_face(), 13, FW_NORMAL), g_font[F_ICONL] = mk(icon_face(), 32, FW_NORMAL);
    if (g_icon) DestroyIcon(g_icon);
    g_icon = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON, S(28), S(28), 0);
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

// Chuột vào/ra nút tự vẽ: vẽ lại để nút sáng lên như nút của Windows 11.
static LRESULT CALLBACK hover_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (msg == WM_MOUSEMOVE && g_hot != h) {
        if (g_hot) InvalidateRect(g_hot, nullptr, FALSE);
        g_hot = h;
        InvalidateRect(h, nullptr, FALSE);
        TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, h, 0};
        TrackMouseEvent(&t);
    } else if (msg == WM_MOUSELEAVE && g_hot == h) {
        g_hot = nullptr;
        InvalidateRect(h, nullptr, FALSE);
    }
    return DefSubclassProc(h, msg, wp, lp);
}
static HWND ctl(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id, Font f, Role fg, Role bg,
               HWND parent = nullptr) {
    if (!parent) parent = g_hwnd;
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, parent, (HMENU)(INT_PTR)id,
                             GetModuleHandleW(nullptr), nullptr);
    if (!wcscmp(cls, L"BUTTON") && (style & BS_TYPEMASK) == BS_OWNERDRAW) SetWindowSubclass(c, hover_proc, 1, 0);
    g_ctls.push_back({c, x, y, w, h, f, fg, bg, parent, parent == g_hwnd ? g_mkpage : -1, false});
    return c;
}
static HWND label(const wchar_t* text, int x, int y, int w, int h, Font f, Role fg, Role bg, DWORD style = SS_LEFT,
                  HWND parent = nullptr) {
    return ctl(L"STATIC", text, style | SS_NOPREFIX, x, y, w, h, 0, f, fg, bg, parent);
}
static void card(RECT r, int rows = 1, int radius = 12, bool dashed = false) { g_cards.push_back({g_mkpage, r, rows, radius, dashed}); }
static void page_title(const wchar_t* title, const wchar_t* sub) {
    label(title, X0, 22, X1 - X0, 34, F_TITLE, R_INK, R_CANVAS);
    if (sub) label(sub, X0, 60, X1 - X0, 20, F_SMALL, R_SLATE, R_CANVAS, SS_LEFT | SS_ENDELLIPSIS);
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
// Một hàng trên thẻ: tên ở trái, các phân đoạn khít nhau trên một rãnh ở phải.
static void segmented(int grp, const wchar_t* title, std::initializer_list<const wchar_t*> items, int row_y, int w) {
    label(title, X0 + 20, row_y + 18, 220, 20, F_BODY, R_INK, R_CARD);
    int i = 0;
    for (auto s : items) {
        HWND h = CreateWindowExW(0, L"BUTTON", s, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 0, 0, g_hwnd,
                                 (HMENU)(INT_PTR)(ID_SEG + grp * 10 + i), GetModuleHandleW(nullptr), nullptr);
        SetWindowSubclass(h, hover_proc, 1, 0);
        g_segs.push_back({h, grp, i++, (int)items.size(), X1 - 20 - w, w, row_y + 13, g_mkpage});
    }
}
static void apply_vis() {  // chỉ hiện điều khiển của trang đang xem (và của thanh bên)
    auto put = [](HWND h, bool v) {
        if (((GetWindowLongW(h, GWL_STYLE) & WS_VISIBLE) != 0) != v) ShowWindow(h, v ? SW_SHOWNA : SW_HIDE);
    };
    for (auto& c : g_ctls) if (c.parent == g_hwnd) put(c.h, (c.page < 0 || c.page == g_page) && !c.off);
    for (auto& s : g_segs) put(s.h, s.page == g_page);
}
static void set_off(HWND h, bool off) {  // ẩn theo tình trạng (vd. nút Ngắt chỉ có khi đang phát)
    for (auto& c : g_ctls) if (c.h == h && c.off != off) c.off = off, apply_vis();
}
static void show_page(int p) {
    g_page = p;
    apply_vis();
    RedrawWindow(g_hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);  // thẻ của trang mới, mục đang chọn ở thanh bên
}

// Nút tự vẽ: mục thanh bên, nút viên thuốc (xanh: việc chính; xám: việc phụ) hoặc một phân đoạn trên rãnh xám.
static void draw_item(const DRAWITEMSTRUCT* d) {
    int w = d->rcItem.right, h = d->rcItem.bottom, id = (int)d->CtlID;
    bool down = d->itemState & ODS_SELECTED, off = d->itemState & ODS_DISABLED, hot = d->hwndItem == g_hot && !off;
    bool focus = (d->itemState & ODS_FOCUS) && !(d->itemState & ODS_NOFOCUSRECT);  // vòng focus chỉ khi đi bằng bàn phím
    bool nav = id >= ID_NAV && id < ID_NAV + PG_COUNT, seg = id >= ID_SEG && id < ID_NAV;
    HDC m = CreateCompatibleDC(d->hDC);
    HBITMAP bmp = CreateCompatibleBitmap(d->hDC, w, h), old = (HBITMAP)SelectObject(m, bmp);
    Role bg = R_CARD, role = R_INK;  // nền phía sau nút: phân đoạn luôn trên thẻ; nút khác theo chỗ đặt
    for (auto& c : g_ctls) if (c.h == d->hwndItem) bg = c.bg, role = c.fg;
    bool primary = role == R_BLUE && !seg && !nav && !off;
    RECT all{0, 0, w, h};
    FillRect(m, &all, g_brush[bg]);  // tô nền bằng GDI: GDI+ khử răng cưa sẽ pha mép với nền
    {
        Gdiplus::Graphics g(m);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        if (nav) {
            bool cur = g_page == id - ID_NAV;
            if (cur || hot || down) rrect(g, 0, 0, (float)w, (float)h, (float)S(8), cur || down ? P[R_SEL] : mix(P[R_CANVAS], P[R_SEL], 60));
            if (cur) rrect(g, 0, h / 2.f - S(8), (float)S(3), (float)S(16), S(3) / 2.f, P[R_BLUE]);  // vạch xanh: trang đang xem
            rrect(g, (float)S(12), (h - S(22)) / 2.f, (float)S(22), (float)S(22), (float)S(6), NAV[id - ID_NAV].tile);
            if (focus) rrect(g, 1, 1, w - 2.f, h - 2.f, (float)S(8), CLR_INVALID, P[R_BLUE], 2);
        } else if (seg) {
            const Seg* s = nullptr;
            for (auto& x : g_segs) if (x.h == d->hwndItem) s = &x;
            RECT r;
            GetWindowRect(d->hwndItem, &r);
            MapWindowPoints(nullptr, g_hwnd, (POINT*)&r, 2);
            float tx = s ? (float)(S(s->x) - r.left) : 0;  // rãnh của cả nhóm, toạ độ theo nút này
            rrect(g, tx, 0, s ? (float)S(s->w) : (float)w, (float)h, h / 2.f, P[R_TRACK]);
            bool sel = s && g_sel[s->grp] == s->idx;
            if (sel || hot)
                rrect(g, 2.5f, 2.5f, w - 5.f, h - 5.f, (h - 5) / 2.f, sel ? P[R_SEGON] : mix(P[R_TRACK], P[R_SEGON], 45),
                      sel && !g_dark ? P[R_HAIR] : CLR_INVALID);
            if (focus) rrect(g, 1, 1, w - 2.f, h - 2.f, (h - 2) / 2.f, CLR_INVALID, P[R_BLUE], 2);
        } else if (primary) {
            rrect(g, 0, 0, (float)w, (float)h, h / 2.f,
                  down ? mix(P[R_BLUE], RGB(0, 0, 0), 20) : hot ? mix(P[R_BLUE], RGB(255, 255, 255), 12) : P[R_BLUE]);
            if (focus) rrect(g, 2, 2, w - 4.f, h - 4.f, (h - 4) / 2.f, CLR_INVALID, RGB(255, 255, 255), 1.5f);
        } else {  // nút phụ: xám trên thẻ trắng; trắng viền mảnh trên nền xám (như nút của macOS)
            bool on_card = bg == R_CARD;
            COLORREF f = on_card ? P[R_TRACK] : P[R_CARD];
            rrect(g, 0.5f, 0.5f, w - 1.f, h - 1.f, (h - 1) / 2.f, down || hot ? mix(f, P[R_BAND], on_card ? 100 : 50) : f,
                  on_card ? CLR_INVALID : P[R_HAIR]);
            if (focus) rrect(g, 1, 1, w - 2.f, h - 2.f, (h - 2) / 2.f, CLR_INVALID, P[R_BLUE], 2);
        }
    }
    wchar_t t[64];
    GetWindowTextW(d->hwndItem, t, 64);
    SetBkMode(m, TRANSPARENT);
    RECT tr{0, 0, w, h};
    UINT fmt = DT_CENTER | DT_VCENTER | DT_SINGLELINE;
    HFONT of = (HFONT)SelectObject(m, g_font[F_SMALL]);
    if (nav) {  // biểu tượng trắng trên ô màu, rồi tên trang
        SelectObject(m, g_font[F_ICON]);
        SetTextColor(m, RGB(255, 255, 255));
        RECT ic{S(12), (h - S(22)) / 2, S(34), (h - S(22)) / 2 + S(22)};
        DrawTextW(m, NAV[id - ID_NAV].glyph, -1, &ic, fmt);
        SelectObject(m, g_font[F_BODY]);
        tr.left = S(44), fmt = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS;
    }
    SetTextColor(m, primary ? RGB(255, 255, 255) : P[off ? R_STEEL : R_INK]);
    DrawTextW(m, t, -1, &tr, fmt);
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
    buffered_paint(h, [](HDC m, RECT) {
        if (g_icon) DrawIconEx(m, S(20), S(20), g_icon, S(28), S(28), 0, nullptr, DI_NORMAL);
        {
            Gdiplus::Graphics g(m);
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            Gdiplus::SolidBrush hair(gc(P[R_HAIR]));
            for (auto& c : g_cards) {
                if (c.page != g_page) continue;
                float x = S(c.r.left) + .5f, y = S(c.r.top) + .5f, w = S(c.r.right - c.r.left) - 1.f, hh = S(c.r.bottom - c.r.top) - 1.f;
                rrect(g, x, y, w, hh, (float)S(c.radius), c.dashed ? CLR_INVALID : P[R_CARD], P[c.dashed ? R_STEEL : R_HAIR],
                      c.dashed ? 1.5f : 1.f, c.dashed);
                for (int i = 1; i < c.rows; i++)  // vạch giữa các hàng, thụt vào như danh sách của Apple
                    g.FillRectangle(&hair, S(c.r.left + 20), S(c.r.top + (c.r.bottom - c.r.top) * i / c.rows),
                                    S(c.r.right - c.r.left - 20), 1);
            }
            Gdiplus::SolidBrush dot(gc(P[g_dot == 2 ? R_GREEN : g_dot == 1 ? R_BLUE : R_ORANGE]));
            g.FillEllipse(&dot, S(24), S(WIN_H - 40), S(8), S(8));  // trạng thái ở chân thanh bên
            if (g_page == PG_HOME) g.FillEllipse(&dot, S(270), S(57), S(12), S(12));
        }
        if (g_page == PG_FILES) {  // mũi tên tải lên giữa vùng kéo thả
            SetBkMode(m, TRANSPARENT);
            SetTextColor(m, P[R_STEEL]);
            HFONT of = (HFONT)SelectObject(m, g_font[F_ICONL]);
            RECT r{S(X0), S(112), S(X1), S(160)};
            DrawTextW(m, L"\xE898", -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(m, of);
        }
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
    static int shown = -1;
    bool acc = has_account(), live;
    std::wstring where, st[4], cap1 = L"Mbps", cap3 = L"mã hoá";
    {
        std::lock_guard g(g_cmu);
        live = g_current;
        if (live) {
            uint64_t f = g_current->frames, b = g_current->bytes;
            if (ps != g_current) pf = f, pb = b, ps = g_current;
            std::lock_guard g2(g_current->info_mu);
            const string &en = g_current->enc_name, &ip = g_current->peer;
            wchar_t mb[16];
            swprintf(mb, 16, L"%.1f", (b - pb) * 8 / 1e6);
            st[0] = std::to_wstring(f - pf), st[1] = mb;
            st[2] = g_current->vw ? std::to_wstring(g_current->vw) + L"×" + std::to_wstring(g_current->vh) : L"–";
            st[3] = en.find("HEVC") != string::npos || en.find("265") != string::npos ? L"HEVC" : L"H.264";
            for (const char* v : {"NVIDIA", "Intel", "AMD"})
                if (en.find(v) != string::npos) cap3 = L"mã hoá " + wide(v);
            int lm = g_current->live_mbps;
            if (lm && lm < g_mbps) cap1 = L"Mbps, tự hạ còn " + std::to_wstring(lm);
            where = L"iPhone ở " + wide(ip) +
                    (is_tailscale(ip) ? L", qua Tailscale." : is_private(ip) ? L", trong mạng nhà." : L", từ Internet.");
            pf = f, pb = b;
        } else ps = nullptr;
    }
    if ((int)live != shown) {  // đang phát: thông số và nút Ngắt thay cho nút ghép
        shown = g_streaming = live;
        set_off(h_pair, live);
        set_off(h_kick, !live);
        for (int i = 0; i < 4; i++) set_off(h_stat[i], !live), set_off(h_cap[i], !live);
        set_off(h_sendhint, live);
        EnableWindow(h_send, live);
    }
    int dot = live ? 2 : acc ? 1 : 0;
    if (dot != g_dot) g_dot = dot, InvalidateRect(g_hwnd, nullptr, FALSE);
    set_text(h_status, live ? L"Đang phát tới iPhone" : acc ? L"Sẵn sàng kết nối" : L"Chưa có tài khoản");
    set_text(h_detail, live ? where
                       : acc ? L"Trên iPhone, mở Remote Desk rồi chạm vào PC này. iPhone mới thì ghép bằng mã QR, "
                               L"không cần nhập IP hay mật khẩu."
                             : L"Tạo tài khoản ở dưới để iPhone đăng nhập vào PC này, hoặc ghép iPhone bằng mã QR.");
    for (int i = 0; i < 4; i++) set_text(h_stat[i], st[i]);
    set_text(h_cap[1], cap1);
    set_text(h_cap[3], cap3);
    set_text(h_side, live ? L"Đang phát tới iPhone" : L"Chờ iPhone kết nối");
    set_text(h_acct, acc ? wide(account_user()) : L"Chưa có tài khoản");
    set_text(h_accsub, acc ? L"iPhone đăng nhập bằng tài khoản này, ở nhà hay ở ngoài" : L"Cần để iPhone ở ngoài đăng nhập");
    set_text(h_accbtn, acc ? L"Đổi tài khoản" : L"Tạo tài khoản");
    for (auto& c : g_ctls)  // chưa có tài khoản: nút tạo tô xanh, là việc cần làm trước
        if (c.h == h_accbtn && (c.fg == R_BLUE) != !acc) c.fg = acc ? R_INK : R_BLUE, InvalidateRect(h_accbtn, nullptr, FALSE);
    size_t n = device_count();
    set_text(h_devs, n ? std::to_wstring(n) + L" thiết bị vào thẳng, không phải nhập mật khẩu" : L"Chưa có thiết bị nào");
    EnableWindow(h_forget, n > 0);
    string net;
    {
        std::lock_guard g(g_igdmu);
        net = g_netstatus;
    }
    set_text(h_net, wide(net));
    Release up = latest_update();
    set_off(h_upd, up.tag.empty() && !g_updating);
    set_text(h_upd, g_updating ? L"Đang cập nhật…" : L"Cập nhật lên " + wide(up.tag));
    string dn = ddns_name();
    set_text(h_ddns, dn.empty() ? L"Chưa đặt. Cần khi IP nhà hay đổi" : wide(dn));
    string rfb, rname;
    {
        std::lock_guard g(g_rfbmu);
        rfb = g_rfb_addr, rname = g_rfb_name;
    }
    set_text(h_vncname, !rfb.empty() ? wide(rname) : !g_vnc_addr.empty() ? L"iPhone dùng lần trước" : L"Chưa thấy iPhone nào");
    set_text(h_vnc, !rfb.empty() ? wide(rfb) + L", có trong mạng này"
                    : !g_vnc_addr.empty() ? wide(g_vnc_addr) : L"Bật TrollVNC trên iPhone, hoặc bấm Điều khiển để nhập IP");
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
        else if (LOWORD(wp) == ID_ACC_SAVE || LOWORD(wp) == IDOK) g_dlg_save(h);  // IDOK: phím Enter
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
// Hộp riêng (tài khoản, mã QR) đặt giữa cửa sổ chính; run_modal khoá cửa sổ chính tới khi hộp đóng (như hộp thoại hệ thống).
static void open_dialog(const wchar_t* cls, WNDPROC proc, const wchar_t* title, int w, int h) {
    HINSTANCE hi = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{sizeof wc};
    if (!GetClassInfoExW(hi, cls, &wc)) {
        wc = {sizeof wc};
        wc.lpfnWndProc = proc, wc.hInstance = hi, wc.lpszClassName = cls;
        wc.hIcon = LoadIconW(hi, MAKEINTRESOURCEW(1)), wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
        RegisterClassExW(&wc);
    }
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
    g_dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, cls, title, style, 0, 0, 100, 100, g_hwnd, nullptr, hi, nullptr);
    RECT rc{0, 0, S(w), S(h)}, o;
    AdjustWindowRectExForDpi(&rc, style, FALSE, WS_EX_DLGMODALFRAME, GetDpiForWindow(g_hwnd));
    GetWindowRect(g_hwnd, &o);
    int ww = rc.right - rc.left, hh = rc.bottom - rc.top;
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    RECT a = mi.rcWork;  // giữa cửa sổ chính, không lòi khỏi màn hình
    int x = std::clamp((int)(o.left + (o.right - o.left - ww) / 2), (int)a.left, std::max((int)a.left, (int)a.right - ww));
    int y = std::clamp((int)(o.top + (o.bottom - o.top - hh) / 2), (int)a.top, std::max((int)a.top, (int)a.bottom - hh));
    SetWindowPos(g_dlg, nullptr, x, y, ww, hh, SWP_NOZORDER);
}
static void run_modal(HWND focus) {
    layout();
    apply_theme();
    ShowWindow(g_dlg, SW_SHOW);
    SetFocus(focus);
    EnableWindow(g_hwnd, FALSE);
    MSG m{};
    while (g_dlg && GetMessageW(&m, nullptr, 0, 0) > 0)
        if (!IsDialogMessageW(g_dlg, &m)) TranslateMessage(&m), DispatchMessageW(&m);
    if (m.message == WM_QUIT) PostQuitMessage((int)m.wParam);
    EnableWindow(g_hwnd, TRUE);
    SetForegroundWindow(g_hwnd);
    refresh_status();
}
static void account_dialog() {
    if (g_dlg) return;
    bool change = has_account();
    open_dialog(L"RemoteDeskAccount", dlg_proc, change ? L"Đổi tài khoản" : L"Tạo tài khoản", DLG_W, DLG_H);
    g_dlg_save = account_save;

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
    run_modal(g_dlg_edits[change ? 1 : 0]);
}

// ---- Hộp tên miền riêng ----
constexpr int DDNS_H = 380;
static void ddns_save(HWND dlg) {
    string raw = trim(edit_text(g_dlg_edits[0])), host = ddns_norm(raw), tok = trim(edit_text(g_dlg_edits[1]));
    const wchar_t* err = !raw.empty() && host.empty() ? L"Tên miền chỉ gồm chữ không dấu, số, dấu chấm và gạch ngang."
                         : !tok.empty() && !host.ends_with(".duckdns.org") ? L"Token chỉ dùng cho tên miền .duckdns.org."
                         : nullptr;
    if (err) return (void)SetWindowTextW(h_dlgerr, err);
    {
        std::lock_guard g(g_ddnsmu);
        g_ddns = host, g_ddns_token = tok, g_ddns_ip.clear();
    }
    save_settings();
    g_net_kick = true;
    if (host.empty()) logf("Đã bỏ tên miền riêng");
    else logf("Tên miền riêng: %s%s", host.c_str(), g_remote ? "" : ". Bật \"Từ Internet\" để iPhone ở xa dùng được");
    DestroyWindow(dlg);
}
static void ddns_dialog() {
    if (g_dlg) return;
    open_dialog(L"RemoteDeskAccount", dlg_proc, L"Tên miền riêng", DLG_W, DDNS_H);
    g_dlg_save = ddns_save;
    label(L"Tên miền riêng", 28, 22, 304, 44, F_HEAD, R_INK, R_CANVAS, SS_LEFT, g_dlg);
    label(L"iPhone ở xa vào bằng tên này cùng tài khoản, kể cả khi nhà đổi IP. Tạo miễn phí ở duckdns.org rồi chép token vào "
          L"đây. Modem đã tự cập nhật DDNS thì chỉ cần điền tên.",
          28, 70, 304, 72, F_SMALL, R_SLATE, R_CANVAS, SS_LEFT, g_dlg);
    const wchar_t* names[] = {L"Tên miền, ví dụ nhacuatoi.duckdns.org", L"Token DuckDNS (bỏ trống nếu modem tự cập nhật)"};
    for (int i = 0; i < 2; i++) {
        int y = 150 + i * 70;
        label(names[i], 28, y, 304, 18, F_SMALL, R_SLATE, R_CANVAS, SS_LEFT, g_dlg);
        g_dlg_edits.push_back(ctl(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL | (i ? ES_PASSWORD : 0), 44, y + 30, 272, 22,
                                  ID_ACC_EDIT + i, F_BODY, R_INK, R_CANVAS, g_dlg));
        SendMessageW(g_dlg_edits.back(), EM_LIMITTEXT, 253, 0);
    }
    {
        std::lock_guard g(g_ddnsmu);
        SetWindowTextW(g_dlg_edits[0], wide(g_ddns).c_str());
        SetWindowTextW(g_dlg_edits[1], wide(g_ddns_token).c_str());
    }
    h_dlgerr = label(L"", 28, 290, 304, 20, F_SMALL, R_ORANGE, R_CANVAS, SS_LEFT, g_dlg);
    ctl(L"BUTTON", L"Huỷ", WS_TABSTOP | BS_OWNERDRAW, 152, 326, 84, 34, ID_ACC_CANCEL, F_SMALL, R_INK, R_CANVAS, g_dlg);
    ctl(L"BUTTON", L"Lưu", WS_TABSTOP | BS_OWNERDRAW, 246, 326, 86, 34, ID_ACC_SAVE, F_SMALL, R_BLUE, R_CANVAS, g_dlg);
    run_modal(g_dlg_edits[0]);
}

// ---- Cửa sổ điều khiển iPhone (TrollVNC) ----
#define WM_APP_VNCSIZE (WM_APP + 3)  // khung iPhone đổi cỡ (lúc vào, lúc xoay máy): chỉnh cửa sổ theo dáng mới
struct Vnc {
    std::mutex m;  // fb, w, h, status, title
    std::vector<uint32_t> fb;
    int w = 0, h = 0;
    std::wstring status = L"Đang kết nối tới iPhone…", title;
    std::mutex wm;  // ghi socket từ luồng giao diện (chạm, phím) và luồng mạng
    SOCKET s = INVALID_SOCKET;
    std::atomic<bool> stop{false};
    std::atomic<HWND> hwnd{nullptr};
    int buttons = 0, px = 0, py = 0;  // của luồng giao diện
    std::vector<uint32_t> keys;       // phím đang giữ: thả hết khi cửa sổ mất focus, tránh kẹt phím trên iPhone
    bool send(const void* p, size_t n) {
        std::lock_guard g(wm);
        return s != INVALID_SOCKET && send_all(s, (const char*)p, n);
    }
    void pointer(int mask, int x, int y) {
        uint8_t b[6] = {5, (uint8_t)mask, (uint8_t)(x >> 8), (uint8_t)x, (uint8_t)(y >> 8), (uint8_t)y};
        send(b, 6);
    }
    void key(uint32_t k, bool down) {
        uint8_t b[8] = {4, (uint8_t)down, 0, 0, (uint8_t)(k >> 24), (uint8_t)(k >> 16), (uint8_t)(k >> 8), (uint8_t)k};
        send(b, 8);
    }
    void say(const wchar_t* msg) {
        {
            std::lock_guard g(m);
            status = msg, w = h = 0, fb.clear();
        }
        if (HWND x = hwnd) InvalidateRect(x, nullptr, FALSE);
    }
};

// Một phiên RFB 3.8 tới đầu bên kia đã nối; trả lời lỗi để hiện trong cửa sổ (nullptr = người dùng đóng cửa sổ).
static const wchar_t* vnc_session(Vnc& v, SOCKET s, const string& pass) {
    ReadFn rd = [s](void* p, size_t n) {
        for (size_t got = 0; got < n;) {
            int r = recv(s, (char*)p + got, (int)std::min<size_t>(n - got, 1 << 20), 0);
            if (r <= 0) return false;
            got += r;
        }
        return true;
    };
    auto skip = [&](size_t n) {
        char tmp[4096];
        for (size_t k; n; n -= k)
            if (!rd(tmp, k = std::min(n, sizeof tmp))) return false;
        return true;
    };
    auto u16 = [](const uint8_t* p) { return p[0] << 8 | p[1]; };
    auto u32 = [](const uint8_t* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; };
    const wchar_t *lost = L"Mất kết nối với iPhone.", *bad = L"iPhone gửi dữ liệu hình không hợp lệ.";
    char ver[12];
    if (!rd(ver, 12) || memcmp(ver, "RFB 003.", 8)) return L"Địa chỉ này không phải TrollVNC (VNC).";
    if (atoi(ver + 8) < 8) return L"Máy chủ VNC quá cũ (cần RFB 3.8).";
    uint8_t n, b[24];
    if (!v.send("RFB 003.008\n", 12) || !rd(&n, 1)) return lost;
    if (!n) return L"iPhone từ chối kết nối.";
    std::vector<uint8_t> types(n);
    if (!rd(types.data(), n)) return lost;
    auto has = [&](uint8_t t) { return std::find(types.begin(), types.end(), t) != types.end(); };
    uint8_t sec = has(1) ? 1 : has(2) ? 2 : 0;  // 1 = không mật khẩu, 2 = mật khẩu VNC
    if (!sec) return L"iPhone dùng kiểu đăng nhập VNC chưa hỗ trợ.";
    if (sec == 2 && pass.empty()) return L"TrollVNC có đặt mật khẩu: bấm \"Điều khiển\" trên cửa sổ Remote Desk và nhập mật khẩu VNC.";
    if (!v.send(&sec, 1)) return lost;
    if (sec == 2) {
        uint8_t ch[16], r[16];
        if (!rd(ch, 16) || !vnc_auth(pass, ch, r) || !v.send(r, 16)) return lost;
    }
    if (!rd(b, 4)) return lost;
    if (u32(b)) return sec == 2 ? L"Sai mật khẩu VNC." : L"iPhone từ chối kết nối.";
    uint8_t shared = 1;
    if (!v.send(&shared, 1) || !rd(b, 24)) return lost;
    int W = u16(b), H = u16(b + 2);
    uint32_t nl = u32(b + 20);
    if (!W || !H || W > 8192 || H > 8192 || nl > 4096) return bad;
    string name(nl, '\0');
    if (!rd(name.data(), nl)) return lost;
    // 32 bit, little-endian, đỏ ở bit 16: trong bộ nhớ là B, G, R, X, đúng thứ tự DIB của Windows
    const uint8_t pf[20] = {0, 0, 0, 0, 32, 24, 0, 1, 0, 255, 0, 255, 0, 255, 16, 8, 0, 0, 0, 0};
    // Tight, CopyRect, Raw, đổi cỡ khung (xoay máy), JPEG chất lượng 6, nén zlib mức 2 (như mặc định của noVNC)
    const int32_t enc[] = {7, 1, 0, -223, -26, -254};
    std::vector<uint8_t> se = {2, 0, 0, (uint8_t)std::size(enc)};
    for (int32_t e : enc)
        for (int i = 3; i >= 0; i--) se.push_back((uint8_t)(e >> (8 * i)));
    if (!v.send(pf, 20) || !v.send(se.data(), se.size())) return lost;
    auto resize = [&](int w, int h) {
        {
            std::lock_guard g(v.m);
            v.w = w, v.h = h, v.fb.assign((size_t)w * h, 0);
            v.title = L"iPhone " + wide(name) + L" (TrollVNC)  ·  chuột phải: Home  ·  chuột giữa: nút nguồn  ·  Ctrl = ⌘";
        }
        if (HWND x = v.hwnd) PostMessageW(x, WM_APP_VNCSIZE, 0, 0);
    };
    resize(W, H);
    logf("Đang điều khiển iPhone \"%s\" (%dx%d) qua TrollVNC", name.c_str(), W, H);
    Tight tight;
    std::vector<uint32_t> px;
    for (bool full = true; !v.stop;) {  // full: xin cả màn (lúc đầu và sau khi khung đổi cỡ), không thì chỉ phần đổi
        {
            std::lock_guard g(v.m);
            W = v.w, H = v.h;
        }
        uint8_t q[10] = {3, (uint8_t)!full, 0, 0, 0, 0, (uint8_t)(W >> 8), (uint8_t)W, (uint8_t)(H >> 8), (uint8_t)H};
        full = false;
        if (!v.send(q, 10)) return v.stop ? nullptr : lost;
        uint8_t t;
        for (;;) {  // bỏ qua chuông, clipboard, bảng màu tới khi gặp khung hình
            if (!rd(&t, 1)) return v.stop ? nullptr : lost;
            if (t == 0) break;
            if (t == 2) continue;
            if (t == 1 ? !(rd(b, 5) && skip(u16(b + 3) * 6)) : t == 3 ? !(rd(b, 7) && skip(u32(b + 3))) : true)
                return v.stop ? nullptr : lost;
        }
        if (!rd(b, 3)) return v.stop ? nullptr : lost;
        for (int i = 0, nr = u16(b + 1); i < nr; i++) {
            if (!rd(b, 12)) return v.stop ? nullptr : lost;
            int x = u16(b), y = u16(b + 2), w = u16(b + 4), h = u16(b + 6);
            int32_t e = (int32_t)u32(b + 8);
            if (e == -223) {  // TrollVNC xoay khung theo máy: xin lại toàn màn với cỡ mới
                if (!w || !h || w > 8192 || h > 8192) return bad;
                resize(w, h);
                W = w, H = h, full = true;
                continue;
            }
            if (x + w > W || y + h > H) return bad;
            if (e == 1) {
                if (!rd(b, 4)) return lost;
                int sx = u16(b), sy = u16(b + 2);
                if (sx + w > W || sy + h > H) return bad;
                std::lock_guard g(v.m);
                px.resize((size_t)w * h);
                for (int r = 0; r < h; r++) memcpy(&px[(size_t)r * w], &v.fb[(size_t)(sy + r) * W + sx], w * 4);
                for (int r = 0; r < h; r++) memcpy(&v.fb[(size_t)(y + r) * W + x], &px[(size_t)r * w], w * 4);
                continue;
            }
            if (e == 0) {
                px.resize((size_t)w * h);
                if (!rd(px.data(), px.size() * 4)) return lost;
            } else if (e == 7) {
                if (!tight.rect(rd, w, h, px)) return bad;
            } else {
                return L"iPhone gửi kiểu mã hoá hình chưa hỗ trợ.";
            }
            std::lock_guard g(v.m);
            for (int r = 0; r < h; r++) memcpy(&v.fb[(size_t)(y + r) * W + x], &px[(size_t)r * w], w * 4);
        }
        if (HWND x = v.hwnd) InvalidateRect(x, nullptr, FALSE);
    }
    return nullptr;
}
static void vnc_thread(std::shared_ptr<Vnc> v, string host, int port, string pass) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);  // WIC giải JPEG
    const wchar_t* err = L"Không kết nối được iPhone. Kiểm tra TrollVNC đã bật và iPhone cùng mạng Wi‑Fi với PC.";
    if (SOCKET s = tcp_connect(host, port, 5000); s != INVALID_SOCKET) {
        {
            std::lock_guard g(v->wm);
            v->s = s;
        }
        if (v->stop) shutdown(s, SD_BOTH);  // cửa sổ đã đóng trong lúc đang nối
        err = vnc_session(*v, s, pass);
        {
            std::lock_guard g(v->wm);
            v->s = INVALID_SOCKET;
        }
        closesocket(s);
    }
    if (err && !v->stop) {
        v->say(err);
        logf("iPhone (TrollVNC): %s", utf8(err).c_str());
    }
    CoUninitialize();
}

static RECT vnc_fit(const RECT& cr, int w, int h) {  // giữ đúng tỉ lệ hình iPhone, viền đen hai bên
    int dw = cr.right, dh = (int)((int64_t)cr.right * h / w);
    if (dh > cr.bottom) dh = cr.bottom, dw = (int)((int64_t)cr.bottom * w / h);
    int x = (cr.right - dw) / 2, y = (cr.bottom - dh) / 2;
    return {x, y, x + dw, y + dh};
}
static std::shared_ptr<Vnc> g_vnc;  // một lúc một iPhone
static LRESULT CALLBACK vnc_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    auto* v = (Vnc*)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (!v) return DefWindowProcW(h, msg, wp, lp);
    auto at = [&](int cx, int cy) {  // toạ độ trong cửa sổ -> điểm ảnh trên iPhone
        RECT cr;
        GetClientRect(h, &cr);
        std::lock_guard g(v->m);
        if (!v->w) return false;
        RECT d = vnc_fit(cr, v->w, v->h);
        v->px = std::clamp((int)((int64_t)(cx - d.left) * v->w / std::max<LONG>(1, d.right - d.left)), 0, v->w - 1);
        v->py = std::clamp((int)((int64_t)(cy - d.top) * v->h / std::max<LONG>(1, d.bottom - d.top)), 0, v->h - 1);
        return true;
    };
    auto press = [&](int bit, bool down) {
        if (!at((short)LOWORD(lp), (short)HIWORD(lp))) return 0;
        v->buttons = down ? v->buttons | bit : v->buttons & ~bit;
        v->pointer(v->buttons, v->px, v->py);
        if (down) SetCapture(h);
        else if (!v->buttons) ReleaseCapture();
        return 0;
    };
    switch (msg) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT:
        buffered_paint(h, [v](HDC m, RECT cr) {
            FillRect(m, &cr, (HBRUSH)GetStockObject(BLACK_BRUSH));
            std::lock_guard g(v->m);
            if (v->w) {
                RECT d = vnc_fit(cr, v->w, v->h);
                BITMAPINFO bi{};
                bi.bmiHeader = {sizeof(BITMAPINFOHEADER), v->w, -v->h, 1, 32, BI_RGB};
                SetStretchBltMode(m, HALFTONE);
                SetBrushOrgEx(m, 0, 0, nullptr);
                StretchDIBits(m, d.left, d.top, d.right - d.left, d.bottom - d.top, 0, 0, v->w, v->h, v->fb.data(), &bi,
                              DIB_RGB_COLORS, SRCCOPY);
                return;
            }
            RECT r = cr, c{};
            InflateRect(&r, -S(28), 0);
            HGDIOBJ of = SelectObject(m, g_font[F_BODY]);
            SetTextColor(m, RGB(235, 235, 240));
            SetBkMode(m, TRANSPARENT);
            c = r;
            DrawTextW(m, v->status.c_str(), -1, &c, DT_CENTER | DT_WORDBREAK | DT_CALCRECT);
            OffsetRect(&r, 0, (cr.bottom - (c.bottom - c.top)) / 2);
            DrawTextW(m, v->status.c_str(), -1, &r, DT_CENTER | DT_WORDBREAK);
            SelectObject(m, of);
        });
        return 0;
    case WM_SIZE: InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_APP_VNCSIZE: {
        int w, hh;
        std::wstring title;
        {
            std::lock_guard g(v->m);
            w = v->w, hh = v->h, title = v->title;
        }
        if (!w) return 0;
        SetWindowTextW(h, title.c_str());
        MONITORINFO mi{sizeof mi};
        GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi);
        RECT wa = mi.rcWork, cur;
        double sc = std::min({(wa.right - wa.left) * 0.9 / w, (wa.bottom - wa.top) * 0.9 / hh, 1.0});
        RECT rc{0, 0, (LONG)(w * sc), (LONG)(hh * sc)};
        AdjustWindowRectExForDpi(&rc, WS_OVERLAPPEDWINDOW, FALSE, 0, GetDpiForWindow(h));
        int ww = rc.right - rc.left, wh = rc.bottom - rc.top;
        GetWindowRect(h, &cur);  // giữ tâm cửa sổ, không tràn khỏi màn hình
        int x = std::max<int>(wa.left, std::min<int>((cur.left + cur.right - ww) / 2, wa.right - ww));
        int y = std::max<int>(wa.top, std::min<int>((cur.top + cur.bottom - wh) / 2, wa.bottom - wh));
        SetWindowPos(h, nullptr, x, y, ww, wh, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_LBUTTONDOWN: return press(1, true);  // chạm
    case WM_LBUTTONUP: return press(1, false);
    case WM_MBUTTONDOWN: return press(2, true);  // nút nguồn (giữ lâu như bấm giữ thật)
    case WM_MBUTTONUP: return press(2, false);
    case WM_RBUTTONDOWN: return press(4, true);  // nút Home
    case WM_RBUTTONUP: return press(4, false);
    case WM_MOUSEMOVE:
        if (v->buttons && at((short)LOWORD(lp), (short)HIWORD(lp))) v->pointer(v->buttons, v->px, v->py);
        return 0;
    case WM_MOUSEWHEEL: {  // TrollVNC đổi một nấc bánh xe thành một lần vuốt ngắn
        POINT p{(short)LOWORD(lp), (short)HIWORD(lp)};
        ScreenToClient(h, &p);
        if (!at(p.x, p.y)) return 0;
        int bit = GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 8 : 16;
        v->pointer(v->buttons | bit, v->px, v->py);
        v->pointer(v->buttons, v->px, v->py);
        return 0;
    }
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYUP: {
        bool down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
        if (wp == VK_F4 && GetKeyState(VK_MENU) < 0) break;  // Alt+F4 vẫn đóng cửa sổ
        if (down && (lp & (1 << 30))) return 0;              // bỏ lặp phím của Windows: iPhone tự lặp khi giữ
        if (uint32_t k = vnc_keysym((UINT)wp)) {
            v->key(k, down);
            if (down) v->keys.push_back(k);
            else std::erase(v->keys, k);
        }
        return 0;
    }
    case WM_SYSCHAR: return 0;  // không kêu "bíp" khi giữ Alt (Option)
    case WM_KILLFOCUS:
        for (uint32_t k : v->keys) v->key(k, false);
        v->keys.clear();
        if (v->buttons) v->buttons = 0, v->pointer(0, v->px, v->py);
        return 0;
    case WM_DESTROY: {
        v->stop = true;
        {
            std::lock_guard g(v->wm);
            if (v->s != INVALID_SOCKET) shutdown(v->s, SD_BOTH);  // luồng mạng đang chờ đọc: cho nó thoát
        }
        v->hwnd = nullptr;
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        if (g_vnc.get() == v) g_vnc.reset();
        return 0;
    }
    }
    return DefWindowProcW(h, msg, wp, lp);
}
static void vnc_open(const string& addr, const string& pass) {
    if (g_vnc)
        if (HWND old = g_vnc->hwnd) DestroyWindow(old);
    HINSTANCE hi = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{sizeof wc};
    if (!GetClassInfoExW(hi, L"RemoteDeskVnc", &wc)) {
        wc = {sizeof wc};
        wc.lpfnWndProc = vnc_proc, wc.hInstance = hi, wc.lpszClassName = L"RemoteDeskVnc";
        wc.hIcon = LoadIconW(hi, MAKEINTRESOURCEW(1)), wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
        RegisterClassExW(&wc);
    }
    // Mở trên màn đang có cửa sổ chính (khi chơi game người dùng để cửa sổ chính ở màn phụ), cỡ tạm theo dáng iPhone
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    RECT wa = mi.rcWork;
    int h = (wa.bottom - wa.top) * 8 / 10, w = h * 9 / 19;
    HWND hw = CreateWindowExW(0, L"RemoteDeskVnc", L"iPhone (TrollVNC)", WS_OVERLAPPEDWINDOW, wa.left + (wa.right - wa.left - w) / 2,
                              wa.top + (wa.bottom - wa.top - h) / 2, w, h, nullptr, nullptr, hi, nullptr);
    if (!hw) return;
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hw, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof dark);
    auto v = std::make_shared<Vnc>();
    v->hwnd = hw;
    SetWindowLongPtrW(hw, GWLP_USERDATA, (LONG_PTR)v.get());
    g_vnc = v;
    size_t c = addr.rfind(':');
    ShowWindow(hw, SW_SHOW);
    std::thread(vnc_thread, v, addr.substr(0, c), c == string::npos ? 5901 : atoi(addr.c_str() + c + 1), pass).detach();
}

// ---- Hộp địa chỉ iPhone (TrollVNC) ----
static bool g_vnc_go;
static void vnc_save(HWND dlg) {
    string a = trim(edit_text(g_dlg_edits[0])), p = edit_text(g_dlg_edits[1]);
    size_t c = a.rfind(':');
    int port = c == string::npos ? 5901 : atoi(a.c_str() + c + 1);
    if (a.empty() || c == 0 || port <= 0 || port > 65535)
        return (void)SetWindowTextW(h_dlgerr, L"Nhập địa chỉ iPhone, ví dụ 192.168.0.101 hoặc 192.168.0.101:5901.");
    g_vnc_addr = a, g_vnc_pass = p;
    save_settings();
    g_vnc_go = true;
    DestroyWindow(dlg);
}
static void vnc_dialog() {
    if (g_dlg) return;
    g_vnc_go = false;
    string found, name;
    {
        std::lock_guard g(g_rfbmu);
        found = g_rfb_addr, name = g_rfb_name;
    }
    open_dialog(L"RemoteDeskAccount", dlg_proc, L"Điều khiển iPhone", DLG_W, DDNS_H);
    g_dlg_save = vnc_save;
    label(L"Điều khiển iPhone", 28, 22, 304, 44, F_HEAD, R_INK, R_CANVAS, SS_LEFT, g_dlg);
    std::wstring hint = found.empty() ? L"iPhone đã jailbreak và bật TrollVNC, cùng mạng Wi‑Fi với PC. Chưa thấy iPhone nào qua Bonjour: "
                                        L"xem IP ở Cài đặt > TrollVNC trên iPhone rồi nhập vào đây."
                                      : L"Đã tìm thấy " + wide(name) + L" trong mạng nhà. Chuột trái là chạm, chuột phải là nút Home, "
                                        L"chuột giữa là nút nguồn.";
    label(hint.c_str(), 28, 70, 304, 72, F_SMALL, R_SLATE, R_CANVAS, SS_LEFT, g_dlg);
    const wchar_t* names[] = {L"Địa chỉ iPhone (cổng mặc định 5901)", L"Mật khẩu VNC (bỏ trống nếu TrollVNC không đặt)"};
    for (int i = 0; i < 2; i++) {
        int y = 150 + i * 70;
        label(names[i], 28, y, 304, 18, F_SMALL, R_SLATE, R_CANVAS, SS_LEFT, g_dlg);
        g_dlg_edits.push_back(ctl(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL | (i ? ES_PASSWORD : 0), 44, y + 30, 272, 22,
                                  ID_ACC_EDIT + i, F_BODY, R_INK, R_CANVAS, g_dlg));
        SendMessageW(g_dlg_edits.back(), EM_LIMITTEXT, i ? 8 : 253, 0);  // VNC chỉ dùng 8 ký tự đầu của mật khẩu
    }
    SetWindowTextW(g_dlg_edits[0], wide(found.empty() ? g_vnc_addr : found).c_str());  // IP iPhone hay đổi: ưu tiên chỗ vừa thấy
    SetWindowTextW(g_dlg_edits[1], wide(g_vnc_pass).c_str());
    h_dlgerr = label(L"", 28, 290, 304, 20, F_SMALL, R_ORANGE, R_CANVAS, SS_LEFT, g_dlg);
    ctl(L"BUTTON", L"Huỷ", WS_TABSTOP | BS_OWNERDRAW, 152, 326, 84, 34, ID_ACC_CANCEL, F_SMALL, R_INK, R_CANVAS, g_dlg);
    ctl(L"BUTTON", L"Kết nối", WS_TABSTOP | BS_OWNERDRAW, 246, 326, 86, 34, ID_ACC_SAVE, F_SMALL, R_BLUE, R_CANVAS, g_dlg);
    run_modal(g_dlg_edits[0]);
    if (g_vnc_go) vnc_open(g_vnc_addr, g_vnc_pass);  // mở sau khi hộp đóng hẳn, để cửa sổ iPhone nằm trên cùng
}

// ---- Hộp mã QR ghép iPhone ----
constexpr int PAIR_H = 500, QR_X = 60, QR_Y = 128, QR_BOX = 240;
static Qr g_qr;
static LRESULT CALLBACK qr_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT:  // ô đen trên thẻ trắng cả khi giao diện tối (camera cần nền sáng), chừa lề 4 ô theo chuẩn
        buffered_paint(h, [](HDC m, RECT) {
            int box = S(QR_BOX), x0 = S(QR_X), y0 = S(QR_Y), cell = box / (g_qr.size + 8), off = (box - cell * g_qr.size) / 2;
            {
                Gdiplus::Graphics g(m);
                g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                rrect(g, (float)x0, (float)y0, (float)box, (float)box, (float)S(20), RGB(255, 255, 255), P[R_HAIR]);
            }
            for (int y = 0; y < g_qr.size; y++)
                for (int x = 0; x < g_qr.size; x++)
                    if (g_qr.at(x, y)) {
                        RECT r{x0 + off + x * cell, y0 + off + y * cell, x0 + off + (x + 1) * cell, y0 + off + (y + 1) * cell};
                        FillRect(m, &r, (HBRUSH)GetStockObject(BLACK_BRUSH));
                    }
        });
        return 0;
    case WM_TIMER:  // iPhone đã dùng mã, hoặc hết 10 phút: tự đóng
        if (!pair_active()) DestroyWindow(h);
        return 0;
    case WM_DRAWITEM: draw_item((DRAWITEMSTRUCT*)lp); return TRUE;
    case WM_CTLCOLORSTATIC:
        if (LRESULT r = ctl_color(wp, lp)) return r;
        break;
    case WM_COMMAND:
        if (LOWORD(wp) == ID_ACC_CANCEL || LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL) DestroyWindow(h);  // Enter, Esc
        return 0;
    case WM_DESTROY:
        pair_stop();  // đóng hộp là mã hết hiệu lực
        std::erase_if(g_ctls, [h](const Ctl& c) { return c.parent == h; });
        g_dlg = nullptr;
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}
static void pair_dialog() {
    if (g_dlg) return;
    g_qr = qr_make(pair_url(pair_start()));
    open_dialog(L"RemoteDeskPair", qr_proc, L"Ghép iPhone", DLG_W, PAIR_H);
    label(L"Ghép iPhone", 28, 22, 304, 44, F_HEAD, R_INK, R_CANVAS, SS_LEFT, g_dlg);
    label(L"Trên iPhone, mở Remote Desk và bấm \"Quét mã QR trên PC\", hoặc quét bằng app Camera. Không cần nhập IP hay mật khẩu.",
          28, 70, 304, 54, F_SMALL, R_SLATE, R_CANVAS, SS_LEFT, g_dlg);
    bool pub = false;
    std::wstring ips;
    for (auto& a : client_addrs()) {
        string ip = a.substr(0, a.rfind(':'));
        pub |= !is_private(ip);
        ips += (ips.empty() ? L"" : L", ") + wide(ip);
    }
    std::wstring note = L"Mã dùng một lần, hết hạn sau 10 phút. Địa chỉ trong mã: " + ips +
                        (pub ? L"." : L". Muốn dùng ở ngoài thì bật \"Từ Internet\" trước rồi mở lại mã.");
    label(note.c_str(), 28, 384, 304, 54, F_SMALL, R_SLATE, R_CANVAS, SS_LEFT, g_dlg);
    HWND done = ctl(L"BUTTON", L"Xong", WS_TABSTOP | BS_OWNERDRAW, 246, 450, 86, 34, ID_ACC_CANCEL, F_SMALL, R_BLUE, R_CANVAS, g_dlg);
    SetTimer(g_dlg, 1, 1000, nullptr);
    run_modal(done);
}

static std::vector<std::wstring> pick_files() {
    std::vector<wchar_t> buf(1 << 16);
    OPENFILENAMEW o{sizeof o};
    o.hwndOwner = g_hwnd, o.lpstrFile = buf.data(), o.nMaxFile = (DWORD)buf.size(), o.lpstrTitle = L"Chọn file gửi tới iPhone";
    o.Flags = OFN_ALLOWMULTISELECT | OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&o)) return {};
    std::wstring dir = buf.data();  // nhiều file: thư mục rồi từng tên, ngăn bằng \0; một file: đường dẫn đầy đủ
    std::vector<std::wstring> v;
    for (const wchar_t* f = buf.data() + dir.size() + 1; *f; f += wcslen(f) + 1) v.push_back(dir + L"\\" + f);
    if (v.empty()) v.push_back(dir);
    return v;
}

static LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_APP_LOG: append_logs(); return 0;
    case WM_APP_SET: apply_setting((int)wp, (int)lp); return 0;
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
        } else if (LOWORD(wp) == ID_PAIR) {
            pair_dialog();
        } else if (LOWORD(wp) == ID_DDNS) {
            ddns_dialog();
        } else if (LOWORD(wp) == ID_SEND) {
            auto f = pick_files();
            if (!f.empty()) std::thread(offer_files, f).detach();
        } else if (LOWORD(wp) == ID_RECVDIR) {
            ShellExecuteW(h, L"open", recv_dir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        } else if (LOWORD(wp) == ID_VNC) {
            vnc_dialog();
        } else if (LOWORD(wp) == ID_UPDATE) {  // bản zip/tự build: mở trang tải; bản đã cài: cập nhật ngay
            if (installed_copy()) std::thread(update_now, latest_update()).detach();
            else ShellExecuteW(h, L"open", RELEASES, nullptr, nullptr, SW_SHOWNORMAL);
        } else if (LOWORD(wp) == ID_FORGET) {
            forget_devices();
            logf("Đã đăng xuất mọi thiết bị: lần sau phải nhập mật khẩu");
        } else if (LOWORD(wp) == ID_KICK) {
            std::lock_guard g(g_cmu);
            if (g_current) g_current->kick(4001, "PC đã ngắt kết nối");
        } else if (LOWORD(wp) >= ID_NAV && LOWORD(wp) < ID_NAV + PG_COUNT) {
            show_page(LOWORD(wp) - ID_NAV);
        } else if (LOWORD(wp) >= ID_SEG && LOWORD(wp) < ID_NAV && HIWORD(wp) == BN_CLICKED) {
            apply_setting((LOWORD(wp) - ID_SEG) / 10, (LOWORD(wp) - ID_SEG) % 10);
        }
        return 0;
    case WM_DROPFILES: {  // kéo file vào cửa sổ: gửi tới iPhone
        HDROP d = (HDROP)wp;
        std::vector<std::wstring> f;
        for (UINT i = 0, n = DragQueryFileW(d, 0xFFFFFFFF, nullptr, 0); i < n; i++) {
            std::wstring b(DragQueryFileW(d, i, nullptr, 0) + 1, 0);
            b.resize(DragQueryFileW(d, i, b.data(), (UINT)b.size()));
            f.push_back(b);
        }
        DragFinish(d);
        std::thread(offer_files, f).detach();
        return 0;
    }
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

    // Thanh bên: tên app, các trang, nút cập nhật khi có bản mới, trạng thái ở chân
    std::wstring ver = newer(RD_VERSION, "0") ? L"Phiên bản " + wide(RD_VERSION) : L"Bản tự build";
    label(L"Remote Desk", 58, 19, 150, 20, F_BRAND, R_INK, R_CANVAS);
    label(ver.c_str(), 58, 39, 150, 16, F_CAPTION, R_SLATE, R_CANVAS);
    for (int i = 0; i < PG_COUNT; i++)
        ctl(L"BUTTON", NAV[i].name, WS_TABSTOP | BS_OWNERDRAW, 10, 74 + i * 38, 200, 34, ID_NAV + i, F_BODY, R_INK, R_CANVAS);
    h_upd = ctl(L"BUTTON", L"", WS_TABSTOP | BS_OWNERDRAW, 16, WIN_H - 96, 188, 32, ID_UPDATE, F_SMALL, R_BLUE, R_CANVAS);
    h_side = label(L"", 40, WIN_H - 45, 170, 18, F_SMALL, R_SLATE, R_CANVAS, SS_LEFT | SS_ENDELLIPSIS);

    // Tổng quan: thẻ trạng thái (đang phát thì hiện thông số), tài khoản và thiết bị
    g_mkpage = PG_HOME;
    card({X0, 28, X1, 220}, 1, 16);
    h_status = label(L"", 292, 44, 360, 36, F_HEAD, R_INK, R_CARD, SS_LEFT | SS_ENDELLIPSIS);
    h_detail = label(L"", 272, 86, X1 - 24 - 272, 40, F_BODY, R_SLATE, R_CARD);
    h_pair = ctl(L"BUTTON", L"Ghép iPhone bằng mã QR", WS_TABSTOP | BS_OWNERDRAW, 272, 160, 200, 36, ID_PAIR, F_SMALL, R_BLUE, R_CARD);
    h_kick = ctl(L"BUTTON", L"Ngắt kết nối", WS_TABSTOP | BS_OWNERDRAW, X1 - 24 - 124, 46, 124, 32, ID_KICK, F_SMALL, R_INK, R_CARD);
    const wchar_t* caps[4] = {L"khung/s", L"Mbps", L"độ phân giải", L"mã hoá"};
    const int col[5] = {272, 380, 488, 660, 788};  // cột độ phân giải rộng hơn: "2560×1440" không dính sang cột bên
    for (int i = 0; i < 4; i++) {
        h_stat[i] = label(L"", col[i], 128, col[i + 1] - col[i] - 8, 34, F_STAT, R_INK, R_CARD, SS_LEFT | SS_ENDELLIPSIS);
        h_cap[i] = label(caps[i], col[i], 164, col[i + 1] - col[i] - 8, 18, F_CAPTION, R_SLATE, R_CARD, SS_LEFT | SS_ENDELLIPSIS);
    }
    label(L"Tài khoản và thiết bị", X0 + 4, 240, 300, 20, F_SECTION, R_SLATE, R_CANVAS);
    card({X0, 266, X1, 378}, 2);
    h_acct = label(L"", X0 + 20, 274, 360, 20, F_BODY, R_INK, R_CARD, SS_LEFT | SS_ENDELLIPSIS);
    h_accsub = label(L"", X0 + 20, 296, 360, 18, F_CAPTION, R_SLATE, R_CARD, SS_LEFT | SS_ENDELLIPSIS);
    h_accbtn = ctl(L"BUTTON", L"", WS_TABSTOP | BS_OWNERDRAW, BTN_X, 278, 136, 32, ID_ACCOUNT, F_SMALL, R_INK, R_CARD);
    label(L"Thiết bị đã đăng nhập", X0 + 20, 330, 360, 20, F_BODY, R_INK, R_CARD);
    h_devs = label(L"", X0 + 20, 352, 360, 18, F_CAPTION, R_SLATE, R_CARD, SS_LEFT | SS_ENDELLIPSIS);
    h_forget = ctl(L"BUTTON", L"Đăng xuất hết", WS_TABSTOP | BS_OWNERDRAW, BTN_X, 334, 136, 32, ID_FORGET, F_SMALL, R_INK, R_CARD);

    g_mkpage = PG_QUALITY;
    page_title(L"Chất lượng hình", L"Đổi là iPhone thấy ngay, không phải kết nối lại.");
    card({X0, 96, X1, 320}, 4);
    segmented(0, L"Độ phân giải", {L"720p", L"1080p", L"1440p", L"Gốc"}, 96, 300);
    segmented(1, L"Khung hình", {L"30", L"60", L"90", L"120"}, 152, 300);
    segmented(2, L"Bitrate (Mbps)", {L"5", L"10", L"20", L"30", L"50"}, 208, 300);
    segmented(3, L"Mã hoá", {L"H.264", L"HEVC"}, 264, 300);
    label(L"Hình giật hay trễ thì hạ bitrate hoặc độ phân giải. HEVC nét hơn H.264 ở cùng bitrate. Mức 90 và 120 khung/s "
          L"hiện chỉ ra khoảng 60 khung/s và kém nét hơn mức 60.",
          X0 + 4, 334, X1 - X0 - 8, 36, F_CAPTION, R_SLATE, R_CANVAS);

    // Truy cập từ xa: tự mở cổng router (UPnP) để iPhone ở ngoài vào thẳng
    g_mkpage = PG_REMOTE;
    page_title(L"Truy cập từ xa", L"Cho iPhone ở ngoài (4G, Wi‑Fi nơi khác) vào thẳng PC này bằng tài khoản.");
    card({X0, 96, X1, 208}, 2);
    segmented(4, L"Từ Internet", {L"Tắt", L"Bật"}, 96, 140);
    label(L"Tên miền riêng", X0 + 20, 160, 360, 20, F_BODY, R_INK, R_CARD);
    h_ddns = label(L"", X0 + 20, 182, 360, 18, F_CAPTION, R_SLATE, R_CARD, SS_LEFT | SS_ENDELLIPSIS);
    ctl(L"BUTTON", L"Đặt tên miền", WS_TABSTOP | BS_OWNERDRAW, BTN_X, 164, 136, 32, ID_DDNS, F_SMALL, R_INK, R_CARD);
    label(L"Tình trạng", X0 + 4, 228, 300, 20, F_SECTION, R_SLATE, R_CANVAS);
    card({X0, 254, X1, 318});
    h_net = label(L"", X0 + 20, 266, X1 - X0 - 40, 40, F_SMALL, R_INK, R_CARD);

    // Chuyển file: kéo file vào cửa sổ (trang nào cũng được) hoặc chọn file; file iPhone gửi lên nằm trong Tải về\Remote Desk
    g_mkpage = PG_FILES;
    page_title(L"Chuyển file", L"Gửi file qua lại với iPhone đang kết nối.");
    card({X0, 96, X1, 268}, 1, 16, true);
    label(L"Kéo file vào cửa sổ này để gửi tới iPhone", X0 + 2, 166, X1 - X0 - 4, 22, F_BODY, R_INK, R_CANVAS, SS_CENTER);
    h_send = ctl(L"BUTTON", L"Chọn file…", WS_TABSTOP | BS_OWNERDRAW, (X0 + X1) / 2 - 60, 198, 120, 32, ID_SEND, F_SMALL, R_BLUE, R_CANVAS);
    h_sendhint = label(L"Chỉ gửi được khi iPhone đang kết nối.", X0 + 2, 238, X1 - X0 - 4, 18, F_CAPTION, R_SLATE, R_CANVAS, SS_CENTER);
    card({X0, 288, X1, 344});
    label(L"File iPhone gửi lên", X0 + 20, 296, 360, 20, F_BODY, R_INK, R_CARD);
    label(L"Lưu trong Tải về\\Remote Desk", X0 + 20, 318, 360, 18, F_CAPTION, R_SLATE, R_CARD);
    ctl(L"BUTTON", L"Mở thư mục", WS_TABSTOP | BS_OWNERDRAW, BTN_X, 300, 136, 32, ID_RECVDIR, F_SMALL, R_INK, R_CARD);

    // iPhone đã jailbreak có TrollVNC: xem và điều khiển ngay trên PC (tự tìm qua Bonjour)
    g_mkpage = PG_VNC;
    page_title(L"iPhone (TrollVNC)", L"Xem và điều khiển iPhone đã jailbreak có bật TrollVNC, trong cùng mạng Wi‑Fi.");
    card({X0, 96, X1, 152});
    h_vncname = label(L"", X0 + 20, 104, 360, 20, F_BODY, R_INK, R_CARD, SS_LEFT | SS_ENDELLIPSIS);
    h_vnc = label(L"", X0 + 20, 126, 360, 18, F_CAPTION, R_SLATE, R_CARD, SS_LEFT | SS_ENDELLIPSIS);
    ctl(L"BUTTON", L"Điều khiển", WS_TABSTOP | BS_OWNERDRAW, BTN_X, 108, 136, 32, ID_VNC, F_SMALL, R_BLUE, R_CARD);
    label(L"Trong cửa sổ iPhone", X0 + 4, 172, 300, 20, F_SECTION, R_SLATE, R_CANVAS);
    card({X0, 198, X1, 398}, 5);
    const wchar_t* keys[5][2] = {{L"Chuột trái", L"Chạm; giữ và kéo để vuốt"}, {L"Chuột phải", L"Nút Home"},
                                 {L"Chuột giữa", L"Nút nguồn (khoá màn hình)"}, {L"Cuộn chuột", L"Vuốt lên, xuống"},
                                 {L"Ctrl + phím", L"Command + phím, như chép và dán"}};
    for (int i = 0; i < 5; i++) {
        label(keys[i][0], X0 + 20, 208 + i * 40, 200, 20, F_BODY, R_INK, R_CARD);
        label(keys[i][1], X1 - 20 - 300, 209 + i * 40, 300, 20, F_SMALL, R_SLATE, R_CARD, SS_RIGHT);
    }

    g_mkpage = PG_LOG;
    page_title(L"Nhật ký", nullptr);
    card({X0, 72, X1, WIN_H - 28});
    h_log = ctl(L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, X0 + 16, 84, X1 - X0 - 24, WIN_H - 28 - 84 - 10, 0,
                F_SMALL, R_INK, R_CARD);
    g_mkpage = -1;
    apply_vis();

    layout();
    apply_theme();
    append_logs();
    refresh_status();
    SetTimer(g_hwnd, 1, 1000, nullptr);
    DragAcceptFiles(g_hwnd, TRUE);
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
    CHECK(combo("F5", true, in) && in[0].ki.wVk == VK_F5 && combo("f24", true, in) && in[1].ki.wVk == VK_F24 && !combo("F25", true, in) &&
          !combo("F0", true, in));
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
        // mã QR: dùng một lần, sai thì không mất mã, đóng hộp là hết hiệu lực
        string pc = pair_start();
        CHECK(pc.size() == 32 && pair_active() && !pair_ok("") && !pair_ok(string(32, '0')) && pair_ok(pc) && !pair_ok(pc) && !pair_active());
        pc = pair_start();
        pair_stop();
        CHECK(!pair_ok(pc));
        forget_devices();
        CHECK(!token_ok(tk) && device_count() == 0);
        DeleteFileW(g_devfile.c_str());
    }
    // Cập nhật: so phiên bản, đọc JSON Release (dạng có khoảng trắng như api.github.com trả về), lấy đúng asset bộ cài
    CHECK(newer("v1.10.0", "1.9.2") && newer("1.2", "1.1.9") && !newer("1.2.0", "1.2") && !newer("1.1.1", "v1.2.0") && !newer("1.0", "dev") &&
          !newer("dev", "0") && newer("1.2.0", "0"));
    {
        string js = R"({"tag_name": "v1.2.0", "assets": [
          {"name": "RemoteDesk-iOS-v1.2.0-unsigned.ipa", "digest": "sha256:aa", "browser_download_url": "https://github.com/a/b/x.ipa"},
          {"name": "RemoteDesk-Windows-v1.2.0-Setup.exe", "uploader": {"login": "x"}, "digest": "sha256:bb",
           "browser_download_url": "https://github.com/a/b/releases/download/v1.2.0/RemoteDesk-Windows-v1.2.0-Setup.exe"}]})";
        Release r;
        CHECK(parse_release(js, r) && r.tag == "v1.2.0" && r.sha == "bb" && r.url.ends_with("v1.2.0-Setup.exe"));
        string nodig = R"({"tag_name":"v2","assets":[{"digest":"sha256:aa","browser_download_url":"https://github.com/x.ipa"},
                          {"browser_download_url":"https://github.com/y-Setup.exe"}]})";
        CHECK(parse_release(nodig, r) && r.sha.empty() && !parse_release(R"({"tag_name":"v3","assets":[]})", r) &&
              !parse_release(R"({"tag_name":"v3","assets":[{"browser_download_url":"http://evil/x-Setup.exe"}]})", r));
    }
    // Chuyển file: tên an toàn, đọc query/header
    CHECK(safe_name("../../Windows/a.txt") == "a.txt" && safe_name("C:\\x\\b:c?.jpg") == "b_c_.jpg" && safe_name("con.txt") == "_con.txt" &&
          safe_name(" ..") == "file" && safe_name("ảnh đẹp.heic") == "ảnh đẹp.heic" && safe_name(string(300, 'a') + ".mp4").size() == 180 &&
          safe_name(string(300, 'a') + ".mp4").ends_with(".mp4"));
    CHECK(url_dec("%E1%BA%A3nh+1%2Fx.jpg") == "ảnh 1/x.jpg" && qparam("a=1&name=x%20y&id=z", "name") == "x%20y" && qparam("a=1", "id").empty());
    {
        string rq = "POST /upload HTTP/1.1\r\nX-Token:  abc \r\nContent-Length: 12\r\n", lw = rq;
        std::transform(lw.begin(), lw.end(), lw.begin(), ::tolower);
        CHECK(header(rq, lw, "x-token") == "abc" && header(rq, lw, "content-length") == "12" && header(rq, lw, "range").empty());
    }
    CHECK(human(512) == "512 B" && human(1536) == "1.5 KB" && human(3u << 20) == "3.0 MB");
    // Mã QR: khớp đúng từng ô với thư viện qrcode của Python (phiên bản 1, mask 2); link ghép mã hoá đúng
    {
        static const uint32_t want[21] = {0x1FC07F, 0x104341, 0x17545D, 0x17575D, 0x175B5D, 0x105141, 0x1FD57F,
                                          0x001F00, 0x17CD7C, 0x0901BD, 0x1BFE8E, 0x1A28AD, 0x0DCEC0, 0x00172A,
                                          0x1FC5CE, 0x10538D, 0x175941, 0x175578, 0x175AC8, 0x10482C, 0x1FDE0A};
        Qr q = qr_make("RemoteDesk", 2);
        bool same = q.size == 21;
        for (int y = 0; same && y < 21; y++)
            for (int x = 0; x < 21; x++) same &= q.at(x, y) == (bool)((want[y] >> (20 - x)) & 1);
        CHECK(same);
        CHECK(qr_make(string(200, 'a')).size == 57 && qr_make(string(2332, 'a')).size == 0);
        CHECK(url_enc("DESKTOP-1 é&=") == "DESKTOP-1%20%C3%A9%26%3D" && url_enc("1.2.3.4:9,5.6.7.8") == "1.2.3.4:9,5.6.7.8");
        CHECK(ddns_norm(" Nha ") == "nha.duckdns.org" && ddns_norm("pc.example.vn") == "pc.example.vn" && ddns_norm("a b").empty() &&
              ddns_norm("a..b").empty() && ddns_norm(".x").empty() && ddns_norm("nhà").empty() && ddns_norm("").empty());
    }
    CHECK(is_private("192.168.0.103") && is_private("100.99.1.1") && is_private("172.20.10.2") && is_private("127.0.0.1"));
    CHECK(!is_private("203.0.113.7") && !is_private("172.32.0.1") && !is_private("100.128.0.1"));
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
    CHECK(is_tailscale("100.100.1.2") && !is_tailscale("100.20.1.1") && !is_tailscale("192.168.0.103"));
    // Màu: trắng -> Y 235, đen -> Y 16, xám -> U/V 128
    std::vector<uint8_t> px = {255, 255, 255, 0, 0, 0, 0, 0, 128, 128, 128, 0, 128, 128, 128, 0}, nv(6);
    bgra_to_nv12(px.data(), 2, 2, nv.data(), 2, 2);
    CHECK(nv[0] == 235 && nv[1] == 16 && nv[4] == 128 && nv[5] == 128);
    {  // chia nhiều nhân phải ra đúng như một nhân (so với cách tính từng điểm)
        int W = 640, H = 362;
        std::vector<uint8_t> src((size_t)W * H * 4), a((size_t)W * H * 3 / 2);
        for (size_t i = 0; i < src.size(); i++) src[i] = (uint8_t)(i * 31 + i / 7);
        bgra_to_nv12(src.data(), W, H, a.data(), W, H);
        bool same = true;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                const uint8_t* q = &src[((size_t)y * W + x) * 4];
                same &= a[(size_t)y * W + x] == (uint8_t)(((47 * q[2] + 157 * q[1] + 16 * q[0] + 128) >> 8) + 16);
                if (!(y & 1) && !(x & 1))
                    same &= a[(size_t)W * H + (size_t)(y / 2) * W + x] == (uint8_t)(((-26 * q[2] - 86 * q[1] + 112 * q[0] + 128) >> 8) + 128);
            }
        CHECK(same);
    }
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
            CHECK(e.encode(f.data(), i, i == 1 || i == 5, o));  // init đã dùng IDR đầu cho khung thử
        }
        e.drain(o, 500);
        CHECK(e.pending == 0 && o.size() == 8);  // đủ một gói cho mỗi khung, không mất khung nào
        for (auto& x : o) keys.push_back(has_nal(x, 5));
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
            e.drain(o, 500);
            CHECK(!o.empty() && has_nal(o[0], 32, true) && has_nal(o[0], 33, true) && has_nal(o[0], 34, true) && is_key(o[0], true));
            printf("encoder HEVC: %s, %zu gói\n", e.name.c_str(), o.size());
        } else {
            printf("encoder HEVC: không có (sẽ dùng H.264)\n");
        }
    }
    // TrollVNC: độ dài compact, xác thực VNC (vector tính bằng thư viện cryptography của Python), phím, và giải mã
    // Tight: một màu, zlib dùng tiếp qua nhiều ô và sau khi reset luồng, bảng màu 1 bit, gradient, JPEG
    {
        auto reader = [](const std::vector<uint8_t>& d, size_t& pos) -> ReadFn {
            return [&d, &pos](void* p, size_t n) {
                if (pos + n > d.size()) return false;
                memcpy(p, d.data() + pos, n), pos += n;
                return true;
            };
        };
        size_t pos = 0;
        uint32_t n = 0;
        std::vector<uint8_t> msg = {0x90, 0x4E, 0x05};
        CHECK(tight_len(reader(msg, pos), n) && n == 10000 && pos == 2);
        msg = {0xFF, 0xFF, 0x01}, pos = 0;
        CHECK(tight_len(reader(msg, pos), n) && n == 0x7FFF && pos == 3);
        uint8_t ch[16], out[16];
        for (int i = 0; i < 16; i++) ch[i] = (uint8_t)i;
        CHECK(vnc_auth("trollvnc", ch, out) && hexs(out, 16) == "f274f5a243dfda6253d8eb67f74a554c");
        CHECK(vnc_keysym('A') == 'a' && vnc_keysym(VK_OEM_1) == ';' && vnc_keysym(VK_RETURN) == 0xFF0D && vnc_keysym(VK_CONTROL) == 0xFFEB &&
              vnc_keysym(VK_F5) == 0xFFC2 && vnc_keysym(VK_LWIN) == 0);
        CHECK(unhex("00ff7a") == string("\0\xff\x7a", 3) && unhex("zz").empty() && dpapi(dpapi("bí mật", true), false) == "bí mật");
        auto clen = [](std::vector<uint8_t>& m, size_t k) {
            m.push_back((uint8_t)((k & 0x7F) | (k > 0x7F ? 0x80 : 0)));
            if (k > 0x7F) m.push_back((uint8_t)((k >> 7 & 0x7F) | (k > 0x3FFF ? 0x80 : 0)));
            if (k > 0x3FFF) m.push_back((uint8_t)(k >> 14));
        };
        z_stream zs[2]{};
        deflateInit(&zs[0], 6), deflateInit(&zs[1], 6);
        auto zsync = [&zs](int id, const std::vector<uint8_t>& raw) {  // như máy chủ: nén tiếp luồng cũ, đẩy ra bằng Z_SYNC_FLUSH
            std::vector<uint8_t> o(raw.size() + 64);
            zs[id].next_in = (Bytef*)raw.data(), zs[id].avail_in = (uInt)raw.size();
            zs[id].next_out = o.data(), zs[id].avail_out = (uInt)o.size();
            deflate(&zs[id], Z_SYNC_FLUSH);
            o.resize(o.size() - zs[id].avail_out);
            return o;
        };
        auto same = [](const std::vector<uint8_t>& rgb, const std::vector<uint32_t>& px) {
            bool ok = px.size() * 3 == rgb.size();
            for (size_t i = 0; ok && i < px.size(); i++)
                ok = px[i] == (0xFF000000u | rgb[i * 3] << 16 | rgb[i * 3 + 1] << 8 | rgb[i * 3 + 2]);
            return ok;
        };
        Tight t;
        std::vector<uint32_t> px;
        msg = {0x80, 10, 20, 30}, pos = 0;  // cả ô một màu
        CHECK(t.rect(reader(msg, pos), 3, 2, px) && px.size() == 6 && px[5] == 0xFF0A141E);
        std::vector<uint8_t> rgb(4 * 3 * 3);
        for (size_t i = 0; i < rgb.size(); i++) rgb[i] = (uint8_t)(i * 7);
        for (int k = 0; k < 3; k++) {  // ô 2 nối tiếp luồng của ô 1; trước ô 3 máy chủ reset luồng 0 rồi nén lại từ đầu
            if (k == 2) deflateEnd(&zs[0]), zs[0] = {}, deflateInit(&zs[0], 6);
            auto z = zsync(0, rgb);
            msg = {(uint8_t)(k == 2 ? 0x01 : 0x00)}, clen(msg, z.size()), msg.insert(msg.end(), z.begin(), z.end()), pos = 0;
            CHECK(t.rect(reader(msg, pos), 4, 3, px) && pos == msg.size() && same(rgb, px));
            for (auto& c : rgb) c += 3;
        }
        msg = {0x40, 1, 1, 1, 2, 3, 200, 201, 202, 0b10100000, 0b01000000, 0, 0b10000000}, pos = 0;  // bảng 2 màu, 1 bit/điểm
        CHECK(t.rect(reader(msg, pos), 10, 2, px) && px[0] == 0xFFC8C9CA && px[1] == 0xFF010203 && px[2] == 0xFFC8C9CA &&
              px[8] == 0xFF010203 && px[9] == 0xFFC8C9CA && px[10] == 0xFF010203 && px[18] == 0xFFC8C9CA && px[19] == 0xFF010203);
        std::vector<uint8_t> res(3 * 2 * 3, 0);
        res[0] = 10, res[1] = 20, res[2] = 30;  // gradient: chỉ điểm đầu có phần dư nên cả ô thành một màu
        auto z = zsync(1, res);
        msg = {0x50, 2}, clen(msg, z.size()), msg.insert(msg.end(), z.begin(), z.end()), pos = 0;
        CHECK(t.rect(reader(msg, pos), 3, 2, px) && std::all_of(px.begin(), px.end(), [](uint32_t c) { return c == 0xFF0A141E; }));
        string j = unhex(  // JPEG 16×8 màu (200, 100, 50) tạo bằng Pillow
            "ffd8ffe000104a46494600010100000100010000ffdb0043000302020302020303030304030304050805050404050a070706080c0a0c0c0b0a0b0b0d0e12"
            "100d0e110e0b0b1016101113141515150c0f171816141812141514ffdb00430103040405040509050509140d0b0d14141414141414141414141414141414"
            "14141414141414141414141414141414141414141414141414141414141414141414ffc00011080008001003012200021101031101ffc4001500010100000000"
            "000000000000000000000004ffc40014100100000000000000000000000000000000ffc4001501010100000000000000000000000000000708ffc4001411010000"
            "0000000000000000000000000000ffda000c03010002110311003f0090007a907fffd9");
        msg = {0x90}, clen(msg, j.size()), msg.insert(msg.end(), j.begin(), j.end()), pos = 0;
        auto close_to = [](uint32_t c) { return abs((int)(c >> 16 & 255) - 200) < 6 && abs((int)(c >> 8 & 255) - 100) < 6 && abs((int)(c & 255) - 50) < 6; };
        CHECK(t.rect(reader(msg, pos), 16, 8, px) && close_to(px[0]) && close_to(px[127]));
        msg = {0xA0}, pos = 0;
        CHECK(!t.rect(reader(msg, pos), 4, 4, px));  // PNG của TightPNG (không xin) bị từ chối
        deflateEnd(&zs[0]), deflateEnd(&zs[1]);
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
    // Windows 11 bỏ qua timeBeginPeriod và hạ tốc tiến trình khi cửa sổ bị che, thu nhỏ hay ở màn khác (lúc người dùng
    // đang chơi game): Sleep(1) thành 15,6 ms và khung/s tụt còn ~30. Xin giữ độ chính xác bộ đếm giờ và tốc độ đầy đủ.
    PROCESS_POWER_THROTTLING_STATE pt{PROCESS_POWER_THROTTLING_CURRENT_VERSION,
                                      PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION, 0};
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &pt, sizeof pt);
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
    rfb_browse();
    std::thread(remote_loop).detach();
    std::thread(update_loop).detach();
    std::thread([l] { for (SOCKET s; (s = accept(l, nullptr, nullptr)) != INVALID_SOCKET;) std::thread(client_thread, s).detach(); }).detach();
    run_ui();
    ExitProcess(0);  // các phiên đang chạy dở: thoát luôn
}
