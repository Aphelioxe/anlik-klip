#include "video_engine.h"
#include "com_ptr.h"

#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <dxgi1_4.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <codecapi.h>
#include <icodecapi.h>
#include <timeapi.h>

#include <algorithm>
#include <cstring>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace {

bool IsH264IDR(const uint8_t* p, size_t n) {
    for (size_t i = 0; i + 4 < n; ++i) {
        if (p[i] == 0 && p[i + 1] == 0) {
            size_t sc = 0;
            if (p[i + 2] == 1) sc = 3;
            else if (i + 5 <= n && p[i + 2] == 0 && p[i + 3] == 1) sc = 4;
            if (sc > 0 && i + sc < n) {
                uint8_t nal = p[i + sc] & 0x1F;
                if (nal == 5) return true;
                i += sc - 1;
            }
        }
    }
    return false;
}

struct DesktopCapture {
    ComPtr<IDXGIOutputDuplication> dupl;
    ComPtr<ID3D11Texture2D> srcCopyTex;
    ComPtr<ID3D11Texture2D> composeTex;
    ComPtr<IDXGISurface1> composeSurf1;
    ComPtr<ID3D11VideoProcessorEnumerator> vpEnum;
    ComPtr<ID3D11VideoProcessor> vp;
    ComPtr<ID3D11VideoProcessorInputView> inView;
    RECT monRect{};
    uint32_t srcW = 0;
    uint32_t srcH = 0;
    bool hasFrame = false;

    void ReleaseDupl() {
        dupl.Reset();
    }

    void ReleaseAll() {
        dupl.Reset();
        inView.Reset();
        composeSurf1.Reset();
        composeTex.Reset();
        srcCopyTex.Reset();
        vp.Reset();
        vpEnum.Reset();
        monRect = {};
        srcW = 0;
        srcH = 0;
        hasFrame = false;
    }

    bool EnsureInitialized(ID3D11Device* dev, ID3D11VideoDevice* vdev, ID3D11VideoContext* vctx,
                           uint32_t dstW, uint32_t dstH, uint32_t fps) {
        if (dupl) return true;

        ComPtr<IDXGIDevice> dxgiDev;
        if (FAILED(dev->QueryInterface(__uuidof(IDXGIDevice), dxgiDev.PutVoid())) || !dxgiDev) return false;

        ComPtr<IDXGIAdapter> adapter;
        if (FAILED(dxgiDev->GetAdapter(adapter.Put())) || !adapter) return false;

        ComPtr<IDXGIOutput> output;
        if (FAILED(adapter->EnumOutputs(0, output.Put())) || !output) return false;

        DXGI_OUTPUT_DESC od{};
        if (SUCCEEDED(output->GetDesc(&od))) {
            monRect = od.DesktopCoordinates;
        }

        ComPtr<IDXGIOutput1> output1;
        if (FAILED(output->QueryInterface(__uuidof(IDXGIOutput1), output1.PutVoid())) || !output1) return false;

        HRESULT hr = output1->DuplicateOutput(dev, dupl.Put());
        if (FAILED(hr) || !dupl) {
            dupl.Reset();
            return false;
        }

        DXGI_OUTDUPL_DESC dd{};
        dupl->GetDesc(&dd);
        uint32_t newW = dd.ModeDesc.Width;
        uint32_t newH = dd.ModeDesc.Height;
        if (newW == 0 || newH == 0) {
            dupl.Reset();
            return false;
        }

        if (newW != srcW || newH != srcH || !vp || !inView || !srcCopyTex || !composeTex) {
            inView.Reset();
            composeSurf1.Reset();
            composeTex.Reset();
            srcCopyTex.Reset();
            vp.Reset();
            vpEnum.Reset();
            srcW = newW;
            srcH = newH;
            hasFrame = false;

            D3D11_TEXTURE2D_DESC sd{};
            sd.Width = srcW;
            sd.Height = srcH;
            sd.MipLevels = 1;
            sd.ArraySize = 1;
            sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            sd.SampleDesc.Count = 1;
            sd.Usage = D3D11_USAGE_DEFAULT;
            sd.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            if (FAILED(dev->CreateTexture2D(&sd, nullptr, srcCopyTex.Put()))) {
                ReleaseAll();
                return false;
            }

            D3D11_TEXTURE2D_DESC cd = sd;
            cd.MiscFlags = D3D11_RESOURCE_MISC_GDI_COMPATIBLE;
            if (SUCCEEDED(dev->CreateTexture2D(&cd, nullptr, composeTex.Put())) && composeTex) {
                composeTex->QueryInterface(__uuidof(IDXGISurface1), composeSurf1.PutVoid());
            } else {
                composeTex.Reset();
                if (FAILED(dev->CreateTexture2D(&sd, nullptr, composeTex.Put()))) {
                    ReleaseAll();
                    return false;
                }
            }

            D3D11_VIDEO_PROCESSOR_CONTENT_DESC vcd{};
            vcd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
            vcd.InputWidth = srcW;
            vcd.InputHeight = srcH;
            vcd.OutputWidth = dstW;
            vcd.OutputHeight = dstH;
            vcd.InputFrameRate = { fps, 1 };
            vcd.OutputFrameRate = { fps, 1 };
            vcd.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;
            if (FAILED(vdev->CreateVideoProcessorEnumerator(&vcd, vpEnum.Put())) ||
                FAILED(vdev->CreateVideoProcessor(vpEnum.Get(), 0, vp.Put()))) {
                ReleaseAll();
                return false;
            }

            D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd{};
            ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
            if (FAILED(vdev->CreateVideoProcessorInputView(composeTex.Get(), vpEnum.Get(), &ivd, inView.Put()))) {
                // Fallback without GDI_COMPATIBLE if driver forbids it on VideoProcessorInputView
                composeSurf1.Reset();
                composeTex.Reset();
                if (FAILED(dev->CreateTexture2D(&sd, nullptr, composeTex.Put())) ||
                    FAILED(vdev->CreateVideoProcessorInputView(composeTex.Get(), vpEnum.Get(), &ivd, inView.Put()))) {
                    ReleaseAll();
                    return false;
                }
            }

            // Aspect-ratio preserving destination rect
            RECT srcRect{ 0, 0, static_cast<LONG>(srcW), static_cast<LONG>(srcH) };
            double scaleX = static_cast<double>(dstW) / static_cast<double>(srcW);
            double scaleY = static_cast<double>(dstH) / static_cast<double>(srcH);
            double scale = std::min(scaleX, scaleY);
            int fitW = std::max(2, static_cast<int>(srcW * scale + 0.5) & ~1);
            int fitH = std::max(2, static_cast<int>(srcH * scale + 0.5) & ~1);
            int offX = (static_cast<int>(dstW) - fitW) / 2;
            int offY = (static_cast<int>(dstH) - fitH) / 2;
            RECT dstRect{ offX, offY, offX + fitW, offY + fitH };

            vctx->VideoProcessorSetStreamSourceRect(vp.Get(), 0, TRUE, &srcRect);
            vctx->VideoProcessorSetStreamDestRect(vp.Get(), 0, TRUE, &dstRect);
            vctx->VideoProcessorSetOutputTargetRect(vp.Get(), TRUE, &dstRect);

            D3D11_VIDEO_COLOR bg{};
            bg.RGBA = { 0.0f, 0.0f, 0.0f, 1.0f };
            vctx->VideoProcessorSetOutputBackgroundColor(vp.Get(), FALSE, &bg);
            vctx->VideoProcessorSetStreamAutoProcessingMode(vp.Get(), 0, FALSE);

            // Convert full-range sRGB (0-255) accurately to BT.709 Studio YCbCr (16-235)
            // so Windows Media Player, Chrome, VLC, and Discord render identical colors.
            ComPtr<ID3D11VideoContext1> vctx1;
            if (SUCCEEDED(vctx->QueryInterface(__uuidof(ID3D11VideoContext1), vctx1.PutVoid())) && vctx1) {
                vctx1->VideoProcessorSetStreamColorSpace1(vp.Get(), 0, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
                vctx1->VideoProcessorSetOutputColorSpace1(vp.Get(), DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709);
            } else {
                D3D11_VIDEO_PROCESSOR_COLOR_SPACE inCs{};
                inCs.RGB_Range = 0; // Full 0-255
                inCs.YCbCr_Matrix = 1; // BT.709
                inCs.Nominal_Range = 2; // 0-255
                D3D11_VIDEO_PROCESSOR_COLOR_SPACE outCs{};
                outCs.RGB_Range = 0;
                outCs.YCbCr_Matrix = 1; // BT.709
                outCs.Nominal_Range = 1; // Studio 16-235
                vctx->VideoProcessorSetStreamColorSpace(vp.Get(), 0, &inCs);
                vctx->VideoProcessorSetOutputColorSpace(vp.Get(), &outCs);
            }
        }
        return true;
    }

    // Returns false if duplication was lost and needs recreation
    bool GrabLatest(ID3D11DeviceContext* ctx, UINT firstWaitMs = 0) {
        if (!dupl) return false;
        // Drain any queued desktop frames up to 4 so we encode the freshest frame
        for (int iter = 0; iter < 4; ++iter) {
            DXGI_OUTDUPL_FRAME_INFO fi{};
            ComPtr<IDXGIResource> res;
            HRESULT hr = dupl->AcquireNextFrame(iter == 0 ? firstWaitMs : 0, &fi, res.Put());
            if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
                return true; // keep using previous srcCopyTex
            }
            if (hr == DXGI_ERROR_ACCESS_LOST || hr == DXGI_ERROR_INVALID_CALL || FAILED(hr)) {
                dupl.Reset();
                return false;
            }

            ComPtr<ID3D11Texture2D> frameTex;
            if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), frameTex.PutVoid())) && frameTex) {
                ctx->CopyResource(srcCopyTex.Get(), frameTex.Get());
                hasFrame = true;
            }
            dupl->ReleaseFrame();
        }
        return true;
    }

    void PrepareFrameWithCursor(ID3D11DeviceContext* ctx) {
        if (!ctx || !srcCopyTex || !composeTex) return;
        ctx->CopyResource(composeTex.Get(), srcCopyTex.Get());
        if (!composeSurf1) return;

        CURSORINFO ci{};
        ci.cbSize = sizeof(ci);
        if (!GetCursorInfo(&ci) || !(ci.flags & CURSOR_SHOWING) || !ci.hCursor) return;

        int cx = ci.ptScreenPos.x - monRect.left;
        int cy = ci.ptScreenPos.y - monRect.top;
        if (cx < -64 || cy < -64 || cx > static_cast<int>(srcW) + 64 || cy > static_cast<int>(srcH) + 64) {
            return;
        }

        ICONINFO ii{};
        if (GetIconInfo(ci.hCursor, &ii)) {
            cx -= static_cast<int>(ii.xHotspot);
            cy -= static_cast<int>(ii.yHotspot);
            if (ii.hbmMask) DeleteObject(ii.hbmMask);
            if (ii.hbmColor) DeleteObject(ii.hbmColor);
        }

        HDC hdc = nullptr;
        if (SUCCEEDED(composeSurf1->GetDC(FALSE, &hdc)) && hdc) {
            DrawIconEx(hdc, cx, cy, ci.hCursor, 0, 0, 0, nullptr, DI_NORMAL | DI_DEFAULTSIZE);
            composeSurf1->ReleaseDC(nullptr);
        }
    }
};

} // namespace

VideoEngine::VideoEngine(RingBuffer& ring) : ring_(ring) {}

VideoEngine::~VideoEngine() {
    Stop();
}

bool VideoEngine::Start(int64_t masterStartQpc, uint32_t width, uint32_t height, uint32_t fps, uint32_t bitrateKbps) {
    Stop();
    stop_.store(false);
    SetStatus(false, false, 0, 0, 0.0, L"", L"Başlatılıyor...");
    thread_ = std::thread(&VideoEngine::ThreadMain, this, masterStartQpc, width, height, fps, bitrateKbps);
    return true;
}

void VideoEngine::Stop() {
    stop_.store(true);
    if (thread_.joinable()) {
        thread_.join();
    }
}

VideoStats VideoEngine::GetStats() const {
    std::lock_guard<std::mutex> lk(statsMutex_);
    return stats_;
}

void VideoEngine::SetStatus(bool capturing, bool hw, uint32_t sw, uint32_t sh, double fps,
                            const std::wstring& encName, const std::wstring& text) {
    std::lock_guard<std::mutex> lk(statsMutex_);
    stats_.capturingDesktop = capturing;
    stats_.hardwareEncoder = hw;
    stats_.srcWidth = sw;
    stats_.srcHeight = sh;
    stats_.actualFps = fps;
    stats_.encoderName = encName;
    stats_.statusText = text;
}

void VideoEngine::ThreadMain(int64_t masterStartQpc, uint32_t dstW, uint32_t dstH, uint32_t fps, uint32_t bitrateKbps) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    struct TimePeriodScope {
        TimePeriodScope() { timeBeginPeriod(1); }
        ~TimePeriodScope() { timeEndPeriod(1); }
    } timePeriodScope;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    ComScope comScope(COINIT_MULTITHREADED);

    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    D3D_FEATURE_LEVEL flOut = D3D_FEATURE_LEVEL_11_0;
    D3D_FEATURE_LEVEL fls[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
        fls, 2, D3D11_SDK_VERSION, dev.Put(), &flOut, ctx.Put());
    if (FAILED(hr) || !dev) {
        Logger::Log("VideoEngine: D3D11CreateDevice failed hr=0x%08lx", (unsigned long)hr);
        SetStatus(false, false, 0, 0, 0.0, L"", L"Direct3D 11 başlatılamadı.");
        return;
    }

    {
        ComPtr<ID3D10Multithread> mt;
        if (SUCCEEDED(dev->QueryInterface(__uuidof(ID3D10Multithread), mt.PutVoid())) && mt) {
            mt->SetMultithreadProtected(TRUE);
        }
    }

    UINT resetToken = 0;
    ComPtr<IMFDXGIDeviceManager> dxgiMgr;
    hr = MFCreateDXGIDeviceManager(&resetToken, dxgiMgr.Put());
    if (SUCCEEDED(hr) && dxgiMgr) {
        hr = dxgiMgr->ResetDevice(dev.Get(), resetToken);
    }
    if (FAILED(hr)) {
        Logger::Log("VideoEngine: MFCreateDXGIDeviceManager failed hr=0x%08lx", (unsigned long)hr);
        SetStatus(false, false, 0, 0, 0.0, L"", L"DXGI aygıt yöneticisi hatası.");
        return;
    }

    ComPtr<ID3D11VideoDevice> vdev;
    ComPtr<ID3D11VideoContext> vctx;
    dev->QueryInterface(__uuidof(ID3D11VideoDevice), vdev.PutVoid());
    ctx->QueryInterface(__uuidof(ID3D11VideoContext), vctx.PutVoid());
    if (!vdev || !vctx) {
        Logger::Log("VideoEngine: VideoDevice/Context unavailable");
        SetStatus(false, false, 0, 0, 0.0, L"", L"D3D11 VideoProcessor bulunamadı.");
        return;
    }

    // Create 3-texture NV12 pool in VRAM for zero-copy MFT pipelining
    constexpr int kPoolSize = 3;
    ComPtr<ID3D11Texture2D> nv12Tex[kPoolSize];
    ComPtr<ID3D11VideoProcessorOutputView> outViews[kPoolSize];
    D3D11_TEXTURE2D_DESC nvDesc{};
    nvDesc.Width = dstW;
    nvDesc.Height = dstH;
    nvDesc.MipLevels = 1;
    nvDesc.ArraySize = 1;
    nvDesc.Format = DXGI_FORMAT_NV12;
    nvDesc.SampleDesc.Count = 1;
    nvDesc.Usage = D3D11_USAGE_DEFAULT;
    nvDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    for (int i = 0; i < kPoolSize; ++i) {
        if (FAILED(dev->CreateTexture2D(&nvDesc, nullptr, nv12Tex[i].Put()))) {
            Logger::Log("VideoEngine: CreateTexture2D NV12 failed");
            SetStatus(false, false, 0, 0, 0.0, L"", L"NV12 GPU dokusu oluşturulamadı.");
            return;
        }
    }

    // Activate Hardware H.264 MFT (NVIDIA H.264 Encoder MFT)
    MFT_REGISTER_TYPE_INFO outType = { MFMediaType_Video, MFVideoFormat_H264 };
    IMFActivate** acts = nullptr;
    UINT32 count = 0;
    bool isHw = true;
    std::wstring encName = L"Donanım H.264";

    hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
        MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
        nullptr, &outType, &acts, &count);
    if (FAILED(hr) || count == 0) {
        if (acts) CoTaskMemFree(acts);
        acts = nullptr;
        count = 0;
        isHw = false;
        encName = L"Yazılım H.264";
        hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
            MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER,
            nullptr, &outType, &acts, &count);
    }

    ComPtr<IMFTransform> venc;
    if (SUCCEEDED(hr) && count > 0) {
        WCHAR nameBuf[256] = {};
        UINT32 nLen = 0;
        if (SUCCEEDED(acts[0]->GetString(MFT_FRIENDLY_NAME_Attribute, nameBuf, 256, &nLen)) && nLen > 0) {
            encName = nameBuf;
        }
        hr = acts[0]->ActivateObject(__uuidof(IMFTransform), venc.PutVoid());
        for (UINT32 i = 0; i < count; ++i) acts[i]->Release();
        CoTaskMemFree(acts);
    } else {
        if (acts) CoTaskMemFree(acts);
        Logger::Log("VideoEngine: No H.264 encoder MFT found hr=0x%08lx", (unsigned long)hr);
        SetStatus(false, false, 0, 0, 0.0, L"", L"H.264 kodlayıcı bulunamadı.");
        return;
    }

    if (FAILED(hr) || !venc) {
        Logger::Log("VideoEngine: ActivateObject H.264 failed hr=0x%08lx", (unsigned long)hr);
        SetStatus(false, false, 0, 0, 0.0, L"", L"H.264 kodlayıcı başlatılamadı.");
        return;
    }

    bool isAsync = false;
    {
        ComPtr<IMFAttributes> attr;
        if (SUCCEEDED(venc->GetAttributes(attr.Put())) && attr) {
            UINT32 async = 0;
            attr->GetUINT32(MF_TRANSFORM_ASYNC, &async);
            if (async) {
                isAsync = true;
                attr->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
            }
        }
    }

    bool useD3dInput = false;
    if (isHw) {
        HRESULT dhr = venc->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, reinterpret_cast<ULONG_PTR>(dxgiMgr.Get()));
        useD3dInput = SUCCEEDED(dhr);
        Logger::Log("VideoEngine: SET_D3D_MANAGER hr=0x%08lx", (unsigned long)dhr);
    }

    const uint32_t bitrateBps = bitrateKbps * 1000;
    {
        ComPtr<ICodecAPI> codec;
        if (SUCCEEDED(venc->QueryInterface(IID_ICodecAPI, codec.PutVoid())) && codec) {
            VARIANT v;
            VariantInit(&v);
            v.vt = VT_UI4; v.ulVal = eAVEncCommonRateControlMode_CBR;
            codec->SetValue(&CODECAPI_AVEncCommonRateControlMode, &v);
            v.ulVal = bitrateBps;
            codec->SetValue(&CODECAPI_AVEncCommonMeanBitRate, &v);
            v.ulVal = fps; // 1-second IDR GOP
            codec->SetValue(&CODECAPI_AVEncMPVGOPSize, &v);
            v.vt = VT_BOOL; v.boolVal = VARIANT_TRUE;
            codec->SetValue(&CODECAPI_AVLowLatencyMode, &v);
        }
    }

    ComPtr<IMFMediaType> outMT;
    MFCreateMediaType(outMT.Put());
    outMT->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    outMT->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    MFSetAttributeSize(outMT.Get(), MF_MT_FRAME_SIZE, dstW, dstH);
    MFSetAttributeRatio(outMT.Get(), MF_MT_FRAME_RATE, fps, 1);
    outMT->SetUINT32(MF_MT_AVG_BITRATE, bitrateBps);
    outMT->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    outMT->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
    outMT->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
    outMT->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
    outMT->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
    outMT->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
    hr = venc->SetOutputType(0, outMT.Get(), 0);
    if (FAILED(hr)) {
        Logger::Log("VideoEngine: SetOutputType failed hr=0x%08lx", (unsigned long)hr);
        SetStatus(false, isHw, 0, 0, 0.0, encName, L"Video çıkış formatı ayarlanamadı.");
        return;
    }

    ComPtr<IMFMediaType> inMT;
    MFCreateMediaType(inMT.Put());
    inMT->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    inMT->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    MFSetAttributeSize(inMT.Get(), MF_MT_FRAME_SIZE, dstW, dstH);
    MFSetAttributeRatio(inMT.Get(), MF_MT_FRAME_RATE, fps, 1);
    inMT->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    inMT->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
    inMT->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
    inMT->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
    inMT->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
    hr = venc->SetInputType(0, inMT.Get(), 0);
    if (FAILED(hr)) {
        Logger::Log("VideoEngine: SetInputType failed hr=0x%08lx", (unsigned long)hr);
        SetStatus(false, isHw, 0, 0, 0.0, encName, L"Video giriş formatı ayarlanamadı.");
        return;
    }

    std::vector<uint8_t> seqHdr;
    {
        ComPtr<IMFMediaType> curOut;
        if (SUCCEEDED(venc->GetOutputCurrentType(0, curOut.Put())) && curOut) {
            UINT32 seqLen = 0;
            if (SUCCEEDED(curOut->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &seqLen)) && seqLen > 0) {
                seqHdr.resize(seqLen);
                curOut->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, seqHdr.data(), seqLen, nullptr);
            }
        }
    }
    ring_.SetStreamFormats(std::move(seqHdr), {}, dstW, dstH, fps, bitrateBps);

    venc->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    venc->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

    ComPtr<IMFMediaEventGenerator> evGen;
    if (isAsync) {
        venc->QueryInterface(__uuidof(IMFMediaEventGenerator), evGen.PutVoid());
    }

    MFT_OUTPUT_STREAM_INFO osi{};
    venc->GetOutputStreamInfo(0, &osi);
    const bool mftProvidesSamples = (osi.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0;

    auto drainOneOutput = [&]() -> bool {
        MFT_OUTPUT_DATA_BUFFER odb{};
        ComPtr<IMFSample> localOut;
        if (!mftProvidesSamples) {
            MFCreateSample(localOut.Put());
            ComPtr<IMFMediaBuffer> ob;
            MFCreateMemoryBuffer(osi.cbSize > 0 ? osi.cbSize : 1024 * 1024, ob.Put());
            localOut->AddBuffer(ob.Get());
            odb.pSample = localOut.Get();
        }
        DWORD st = 0;
        HRESULT ohr = venc->ProcessOutput(0, 1, &odb, &st);
        if (odb.pEvents) odb.pEvents->Release();
        IMFSample* gotSample = odb.pSample;
        if (SUCCEEDED(ohr) && gotSample) {
            ComPtr<IMFMediaBuffer> contig;
            if (SUCCEEDED(gotSample->ConvertToContiguousBuffer(contig.Put()))) {
                BYTE* ptr = nullptr;
                DWORD len = 0;
                if (SUCCEEDED(contig->Lock(&ptr, nullptr, &len)) && len > 0) {
                    EncodedPacket pkt;
                    pkt.data = std::make_shared<const std::vector<uint8_t>>(ptr, ptr + len);
                    LONGLONG pts = 0, dur = 0;
                    gotSample->GetSampleTime(&pts);
                    gotSample->GetSampleDuration(&dur);
                    UINT32 cp = 0;
                    gotSample->GetUINT32(MFSampleExtension_CleanPoint, &cp);
                    pkt.pts100ns = pts;
                    pkt.dur100ns = dur > 0 ? dur : (10000000LL / fps);
                    pkt.keyframe = (cp != 0) || IsH264IDR(ptr, len);
                    ring_.PushVideo(std::move(pkt));
                    contig->Unlock();
                }
            }
        }
        if (mftProvidesSamples && gotSample) {
            gotSample->Release();
        }
        return SUCCEEDED(ohr);
    };

    // CPU staging fallback only if D3D11 input wasn't accepted (unlikely on GTX 1050 Ti)
    ComPtr<ID3D11Texture2D> cpuStagingTex;
    if (!useD3dInput) {
        D3D11_TEXTURE2D_DESC stDesc = nvDesc;
        stDesc.Usage = D3D11_USAGE_STAGING;
        stDesc.BindFlags = 0;
        stDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        dev->CreateTexture2D(&stDesc, nullptr, cpuStagingTex.Put());
    }

    DesktopCapture cap;
    ID3D11VideoProcessorEnumerator* boundEnum = nullptr;

    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer) timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);

    const int64_t frameDur100Ns = 10000000LL / fps;
    int64_t frameIdx = 0;
    int64_t lastRetryQpc = 0;
    int64_t fpsWindowStartQpc = GetQpcNow();
    int fpsWindowFrames = 0;
    double measuredFps = static_cast<double>(fps);

    while (!stop_.load()) {
        int64_t target100Ns = (frameIdx * 10000000LL) / fps;
        int64_t now100Ns = QpcTo100Ns(GetQpcNow() - masterStartQpc);
        if (now100Ns < 0) now100Ns = 0;
        int64_t wait100Ns = target100Ns - now100Ns;
        if (wait100Ns > 10000LL && timer) { // > 1ms
            LARGE_INTEGER due{};
            due.QuadPart = -wait100Ns;
            SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
            WaitForSingleObject(timer, 50);
        } else if (wait100Ns < -frameDur100Ns * 3) {
            // Skip ahead cleanly if stalled by UAC / mode switch
            int64_t jumpIdx = (now100Ns * fps) / 10000000LL;
            if (jumpIdx > frameIdx) frameIdx = jumpIdx;
            target100Ns = (frameIdx * 10000000LL) / fps;
        }
        const int64_t stepDur100Ns = ((frameIdx + 1) * 10000000LL) / fps - target100Ns;
        if (stop_.load()) break;

        if (!cap.dupl) {
            int64_t nowQpc = GetQpcNow();
            if (lastRetryQpc == 0 || QpcTo100Ns(nowQpc - lastRetryQpc) >= 2500000LL) {
                lastRetryQpc = nowQpc;
                if (cap.EnsureInitialized(dev.Get(), vdev.Get(), vctx.Get(), dstW, dstH, fps)) {
                    if (boundEnum != cap.vpEnum.Get()) {
                        for (int i = 0; i < kPoolSize; ++i) {
                            outViews[i].Reset();
                            D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd{};
                            ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
                            vdev->CreateVideoProcessorOutputView(nv12Tex[i].Get(), cap.vpEnum.Get(), &ovd, outViews[i].Put());
                        }
                        boundEnum = cap.vpEnum.Get();
                    }
                    Logger::Log("VideoEngine: DXGI duplication ready (%ux%u -> %ux%u@%u)",
                        cap.srcW, cap.srcH, dstW, dstH, fps);
                } else {
                    SetStatus(false, isHw, cap.srcW, cap.srcH, 0.0, encName,
                        L"Ekran yakalama bekleniyor (UAC/Tam Ekran/Kilit)");
                }
            }
        }

        if (cap.dupl) {
            const UINT waitMs = (wait100Ns > -20000LL) ? 2 : 0; // catch frames landing near tick boundary
            if (!cap.GrabLatest(ctx.Get(), waitMs)) {
                Logger::Log("VideoEngine: DXGI duplication lost, will reacquire");
                lastRetryQpc = 0;
                if (cap.EnsureInitialized(dev.Get(), vdev.Get(), vctx.Get(), dstW, dstH, fps)) {
                    if (boundEnum != cap.vpEnum.Get()) {
                        for (int i = 0; i < kPoolSize; ++i) {
                            outViews[i].Reset();
                            D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd{};
                            ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
                            vdev->CreateVideoProcessorOutputView(nv12Tex[i].Get(), cap.vpEnum.Get(), &ovd, outViews[i].Put());
                        }
                        boundEnum = cap.vpEnum.Get();
                    }
                    Logger::Log("VideoEngine: DXGI duplication ready (%ux%u -> %ux%u@%u)",
                        cap.srcW, cap.srcH, dstW, dstH, fps);
                    cap.GrabLatest(ctx.Get(), 0);
                } else {
                    SetStatus(false, isHw, cap.srcW, cap.srcH, measuredFps, encName,
                        L"Ekran yenileniyor...");
                }
            }
        }

        if (!cap.hasFrame || !cap.vp || !cap.inView) {
            ++frameIdx;
            continue;
        }

        const int poolIdx = static_cast<int>(frameIdx % kPoolSize);
        if (!outViews[poolIdx]) {
            ++frameIdx;
            continue;
        }

        cap.PrepareFrameWithCursor(ctx.Get());

        D3D11_VIDEO_PROCESSOR_STREAM stream{};
        stream.Enable = TRUE;
        stream.pInputSurface = cap.inView.Get();
        vctx->VideoProcessorBlt(cap.vp.Get(), outViews[poolIdx].Get(), 0, 1, &stream);

        if (evGen) {
            bool gotNeedInput = false;
            while (!stop_.load()) {
                ComPtr<IMFMediaEvent> ev;
                HRESULT ehr = evGen->GetEvent(0, ev.Put());
                if (FAILED(ehr) || !ev) break;
                MediaEventType met = MEUnknown;
                ev->GetType(&met);
                if (met == METransformNeedInput) {
                    gotNeedInput = true;
                    break;
                }
                if (met == METransformHaveOutput) {
                    drainOneOutput();
                }
            }
            if (stop_.load() || !gotNeedInput) break;
        }

        ComPtr<IMFMediaBuffer> inBuf;
        if (useD3dInput) {
            MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), nv12Tex[poolIdx].Get(), 0, FALSE, inBuf.Put());
        } else if (cpuStagingTex) {
            ctx->CopyResource(cpuStagingTex.Get(), nv12Tex[poolIdx].Get());
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (SUCCEEDED(ctx->Map(cpuStagingTex.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
                DWORD nvSize = dstW * dstH * 3 / 2;
                MFCreateMemoryBuffer(nvSize, inBuf.Put());
                BYTE* pDst = nullptr;
                if (inBuf && SUCCEEDED(inBuf->Lock(&pDst, nullptr, nullptr))) {
                    const BYTE* srcY = reinterpret_cast<const BYTE*>(mapped.pData);
                    for (uint32_t y = 0; y < dstH; ++y) {
                        memcpy(pDst + y * dstW, srcY + y * mapped.RowPitch, dstW);
                    }
                    const BYTE* srcUV = srcY + dstH * mapped.RowPitch;
                    BYTE* dstUV = pDst + dstW * dstH;
                    for (uint32_t y = 0; y < dstH / 2; ++y) {
                        memcpy(dstUV + y * dstW, srcUV + y * mapped.RowPitch, dstW);
                    }
                    inBuf->Unlock();
                    inBuf->SetCurrentLength(nvSize);
                }
                ctx->Unmap(cpuStagingTex.Get(), 0);
            }
        }

        if (inBuf) {
            ComPtr<IMFSample> samp;
            MFCreateSample(samp.Put());
            samp->AddBuffer(inBuf.Get());
            samp->SetSampleTime(target100Ns);
            samp->SetSampleDuration(stepDur100Ns);
            HRESULT phr = venc->ProcessInput(0, samp.Get(), 0);
            if (FAILED(phr)) {
                Logger::Log("VideoEngine: ProcessInput failed hr=0x%08lx", (unsigned long)phr);
            }
        }

        if (!evGen) {
            while (drainOneOutput()) {}
        }

        ++fpsWindowFrames;
        int64_t nowQpc = GetQpcNow();
        int64_t winNs = QpcTo100Ns(nowQpc - fpsWindowStartQpc);
        if (winNs >= 10000000LL) {
            measuredFps = static_cast<double>(fpsWindowFrames) * 1e7 / static_cast<double>(winNs);
            fpsWindowFrames = 0;
            fpsWindowStartQpc = nowQpc;
            wchar_t msg[128] = {};
            swprintf(msg, 128, L"Kayıt Aktif (%ux%u -> %ux%u @ %.0f fps)",
                cap.srcW, cap.srcH, dstW, dstH, measuredFps);
            SetStatus(true, isHw, cap.srcW, cap.srcH, measuredFps, encName, msg);
        }

        ++frameIdx;
    }

    if (timer) CloseHandle(timer);

    venc->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
    venc->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
    if (evGen) {
        for (int iter = 0; iter < 64; ++iter) {
            ComPtr<IMFMediaEvent> ev;
            if (FAILED(evGen->GetEvent(MF_EVENT_FLAG_NO_WAIT, ev.Put())) || !ev) {
                Sleep(5);
                continue;
            }
            MediaEventType met = MEUnknown;
            ev->GetType(&met);
            if (met == METransformHaveOutput) drainOneOutput();
            else if (met == METransformDrainComplete) break;
        }
    } else {
        while (drainOneOutput()) {}
    }

    for (int i = 0; i < kPoolSize; ++i) {
        outViews[i].Reset();
        nv12Tex[i].Reset();
    }
    cap.ReleaseAll();
    venc.Reset();
    vctx.Reset();
    vdev.Reset();
    dxgiMgr.Reset();
    ctx.Reset();
    dev.Reset();
}
