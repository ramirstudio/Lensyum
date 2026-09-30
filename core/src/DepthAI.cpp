#include "lensyum/DepthAI.h"
#include "lensyum/Math.h"
#include "lensyum/Parallel.h"

#include <algorithm>
#include <cmath>
#include <list>
#include <mutex>
#include <utility>

#ifdef LENSYUM_WITH_ORT
#include "onnxruntime_c_api.h"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#endif

namespace lensyum {

// ---------------------------------------------------------------------------------------
// Guided filter (He, Sun, Tang): the depth follows the edges of the picture.
// ---------------------------------------------------------------------------------------
namespace {

void boxFilter(const std::vector<float>& in, std::vector<float>& out, int w, int h, int r) {
    std::vector<float> tmp(in.size());
    parallelFor(h, [&](int y) {
        const float* row = &in[static_cast<size_t>(y) * w];
        float* t = &tmp[static_cast<size_t>(y) * w];
        double acc = 0;
        for (int x = -r; x <= r; ++x) acc += row[clampv(x, 0, w - 1)];
        for (int x = 0; x < w; ++x) {
            t[x] = static_cast<float>(acc / (2 * r + 1));
            acc += row[std::min(x + r + 1, w - 1)] - row[std::max(x - r, 0)];
        }
    });
    out.resize(in.size());
    parallelFor(w, [&](int x) {
        double acc = 0;
        for (int y = -r; y <= r; ++y) acc += tmp[static_cast<size_t>(clampv(y, 0, h - 1)) * w + x];
        for (int y = 0; y < h; ++y) {
            out[static_cast<size_t>(y) * w + x] = static_cast<float>(acc / (2 * r + 1));
            acc += tmp[static_cast<size_t>(std::min(y + r + 1, h - 1)) * w + x] - tmp[static_cast<size_t>(std::max(y - r, 0)) * w + x];
        }
    });
}

} // namespace

void refineDepth(const float* rgb, int w, int h, std::vector<float>& depth, int radius, float eps) {
    const size_t n = static_cast<size_t>(w) * h;
    std::vector<float> I(n), Ip(n), II(n);
    for (size_t i = 0; i < n; ++i) {
        I[i] = 0.2126f * rgb[i * 3] + 0.7152f * rgb[i * 3 + 1] + 0.0722f * rgb[i * 3 + 2];
        Ip[i] = I[i] * depth[i];
        II[i] = I[i] * I[i];
    }
    std::vector<float> mI, mp, mIp, mII;
    boxFilter(I, mI, w, h, radius);
    boxFilter(depth, mp, w, h, radius);
    boxFilter(Ip, mIp, w, h, radius);
    boxFilter(II, mII, w, h, radius);
    std::vector<float> a(n), b(n);
    for (size_t i = 0; i < n; ++i) {
        const float cov = mIp[i] - mI[i] * mp[i];
        const float var = mII[i] - mI[i] * mI[i];
        a[i] = cov / (var + eps);
        b[i] = mp[i] - a[i] * mI[i];
    }
    std::vector<float> ma, mb;
    boxFilter(a, ma, w, h, radius);
    boxFilter(b, mb, w, h, radius);
    for (size_t i = 0; i < n; ++i) depth[i] = clampv(ma[i] * I[i] + mb[i], 0.0f, 1.0f);
}

#ifndef LENSYUM_WITH_ORT

bool depthAIInit(const DepthAIConfig&, std::string& err) {
    err = "Lensyum was built without ONNX Runtime support";
    return false;
}

bool depthAIEstimate(const float*, int, int, int, bool, std::vector<float>&, std::string& err) {
    err = "Lensyum was built without ONNX Runtime support";
    return false;
}

#else

namespace {

struct OrtState {
    std::mutex mtx;
    bool ready = false;
    std::string loadedModel;
    const OrtApi* api = nullptr;
    OrtEnv* env = nullptr;
    OrtSession* session = nullptr;
    std::string inputName, outputName;
    std::list<std::pair<uint64_t, std::vector<float>>> cache; // raw network output per image
    std::list<std::pair<uint64_t, std::pair<int, int>>> cacheDims;
};

OrtState& state() {
    static OrtState s;
    return s;
}

bool check(const OrtApi* api, OrtStatus* st, std::string& err) {
    if (!st) return true;
    err = api->GetErrorMessage(st);
    api->ReleaseStatus(st);
    return false;
}

// Runs a call whose failure is not fatal and frees its status.
void soft(const OrtApi* api, OrtStatus* st) {
    if (st) api->ReleaseStatus(st);
}

#ifdef _WIN32
std::wstring widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    if (!w.empty() && w.back() == L'\0') w.pop_back();
    return w;
}
#endif

} // namespace

bool depthAIInit(const DepthAIConfig& cfg, std::string& err) {
    OrtState& S = state();
    std::lock_guard<std::mutex> lock(S.mtx);
    if (S.ready && S.loadedModel == cfg.modelPath) return true;

    if (!S.api) {
        typedef const OrtApiBase*(ORT_API_CALL * GetApiBaseFn)();
        GetApiBaseFn getBase = nullptr;
#ifdef _WIN32
        // Altered search path: DirectML.dll and other dependencies are looked up next to the
        // runtime, not in the host application's folder.
        HMODULE lib = LoadLibraryExW(widen(cfg.runtimeLib).c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!lib) { err = "cannot load " + cfg.runtimeLib + " (Windows error " + std::to_string(GetLastError()) + ", 126 = a dependency such as DirectML.dll is missing, 193 = wrong architecture, 2 = file not found)"; return false; }
        getBase = reinterpret_cast<GetApiBaseFn>(GetProcAddress(lib, "OrtGetApiBase"));
#else
        void* lib = dlopen(cfg.runtimeLib.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!lib) { err = "cannot load " + cfg.runtimeLib; return false; }
        getBase = reinterpret_cast<GetApiBaseFn>(dlsym(lib, "OrtGetApiBase"));
#endif
        if (!getBase) { err = "OrtGetApiBase not found in " + cfg.runtimeLib; return false; }
        S.api = getBase()->GetApi(ORT_API_VERSION);
        if (!S.api) { err = "ONNX Runtime too old for this build"; return false; }
        if (!check(S.api, S.api->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "lensyum", &S.env), err)) return false;
    }
    const OrtApi* api = S.api;
    if (S.session) { api->ReleaseSession(S.session); S.session = nullptr; }

    OrtSessionOptions* so = nullptr;
    if (!check(api, api->CreateSessionOptions(&so), err)) return false;
    soft(api, api->SetSessionGraphOptimizationLevel(so, ORT_ENABLE_ALL));
#ifdef _WIN32
    if (cfg.useGpu) {
        // DirectML runs on any DirectX 12 GPU; it needs sequential execution without memory patterns.
        soft(api, api->DisableMemPattern(so));
        soft(api, api->SetSessionExecutionMode(so, ORT_SEQUENTIAL));
        typedef OrtStatus*(ORT_API_CALL * AppendDmlFn)(OrtSessionOptions*, int);
        HMODULE lib = GetModuleHandleW(widen(cfg.runtimeLib.substr(cfg.runtimeLib.find_last_of("\\/") + 1)).c_str());
        AppendDmlFn appendDml = lib ? reinterpret_cast<AppendDmlFn>(GetProcAddress(lib, "OrtSessionOptionsAppendExecutionProvider_DML")) : nullptr;
        if (appendDml) {
            OrtStatus* st = appendDml(so, 0);
            if (st) api->ReleaseStatus(st); // fall back to the CPU provider
        }
    }
    OrtStatus* st = api->CreateSession(S.env, widen(cfg.modelPath).c_str(), so, &S.session);
#else
    (void)cfg.useGpu;
    OrtStatus* st = api->CreateSession(S.env, cfg.modelPath.c_str(), so, &S.session);
#endif
    api->ReleaseSessionOptions(so);
    if (!check(api, st, err)) { S.session = nullptr; return false; }

    OrtAllocator* alloc = nullptr;
    soft(api, api->GetAllocatorWithDefaultOptions(&alloc));
    char* name = nullptr;
    if (check(api, api->SessionGetInputName(S.session, 0, alloc, &name), err)) { S.inputName = name; soft(api, api->AllocatorFree(alloc, name)); }
    if (check(api, api->SessionGetOutputName(S.session, 0, alloc, &name), err)) { S.outputName = name; soft(api, api->AllocatorFree(alloc, name)); }
    S.cache.clear();
    S.cacheDims.clear();
    S.loadedModel = cfg.modelPath;
    S.ready = true;
    return true;
}

bool depthAIEstimate(const float* rgb, int w, int h, int inferLongSide, bool refine, std::vector<float>& out, std::string& err) {
    OrtState& S = state();
    if (w < 2 || h < 2) { err = "image too small"; return false; }

    // Network input size: long side as requested, both sides multiples of 14.
    const double sc = std::max(inferLongSide, 56) / static_cast<double>(std::max(w, h));
    const int tw = std::max(14, static_cast<int>(std::lround(w * sc / 14.0)) * 14);
    const int th = std::max(14, static_cast<int>(std::lround(h * sc / 14.0)) * 14);

    std::vector<float> input(static_cast<size_t>(3) * tw * th);
    const float mean[3] = {0.485f, 0.456f, 0.406f}, stdv[3] = {0.229f, 0.224f, 0.225f};
    parallelFor(th, [&](int y) {
        const double sy = clampv((y + 0.5) * h / th - 0.5, 0.0, h - 1.001);
        const int y0 = static_cast<int>(sy);
        const float fy = static_cast<float>(sy - y0);
        for (int x = 0; x < tw; ++x) {
            const double sx = clampv((x + 0.5) * w / tw - 0.5, 0.0, w - 1.001);
            const int x0 = static_cast<int>(sx);
            const float fx = static_cast<float>(sx - x0);
            for (int c = 0; c < 3; ++c) {
                auto at = [&](int xx, int yy) { return rgb[(static_cast<size_t>(yy) * w + xx) * 3 + c]; };
                const float a = at(x0, y0) + (at(x0 + 1, y0) - at(x0, y0)) * fx;
                const float b = at(x0, y0 + 1) + (at(x0 + 1, y0 + 1) - at(x0, y0 + 1)) * fx;
                const float v = clampv(a + (b - a) * fy, 0.0f, 1.0f);
                input[(static_cast<size_t>(c) * th + y) * tw + x] = (v - mean[c]) / stdv[c];
            }
        }
    });

    Hasher hs;
    hs.add(tw); hs.add(th);
    for (size_t i = 0; i < input.size(); i += 97) hs.add(input[i]);
    const uint64_t key = hs.h;

    std::vector<float> net;
    int nw = 0, nh = 0;
    {
        std::lock_guard<std::mutex> lock(S.mtx);
        if (!S.ready) { err = "depth model not loaded"; return false; }
        auto dims = S.cacheDims.begin();
        for (auto it = S.cache.begin(); it != S.cache.end(); ++it, ++dims) {
            if (it->first == key) { net = it->second; nw = dims->second.first; nh = dims->second.second; break; }
        }
        if (net.empty()) {
            const OrtApi* api = S.api;
            OrtMemoryInfo* mem = nullptr;
            if (!check(api, api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &mem), err)) return false;
            const int64_t shape[4] = {1, 3, th, tw};
            OrtValue* in = nullptr;
            OrtStatus* st = api->CreateTensorWithDataAsOrtValue(mem, input.data(), input.size() * sizeof(float), shape, 4,
                                                                ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &in);
            api->ReleaseMemoryInfo(mem);
            if (!check(api, st, err)) return false;
            const char* inNames[1] = {S.inputName.c_str()};
            const char* outNames[1] = {S.outputName.c_str()};
            OrtValue* outV = nullptr;
            st = api->Run(S.session, nullptr, inNames, &in, 1, outNames, 1, &outV);
            api->ReleaseValue(in);
            if (!check(api, st, err)) return false;
            OrtTensorTypeAndShapeInfo* info = nullptr;
            soft(api, api->GetTensorTypeAndShape(outV, &info));
            size_t nd = 0;
            soft(api, api->GetDimensionsCount(info, &nd));
            std::vector<int64_t> d(nd);
            soft(api, api->GetDimensions(info, d.data(), nd));
            api->ReleaseTensorTypeAndShapeInfo(info);
            if (nd < 2) { api->ReleaseValue(outV); err = "unexpected model output"; return false; }
            nh = static_cast<int>(d[nd - 2]);
            nw = static_cast<int>(d[nd - 1]);
            float* p = nullptr;
            soft(api, api->GetTensorMutableData(outV, reinterpret_cast<void**>(&p)));
            if (!p) { api->ReleaseValue(outV); err = "empty model output"; return false; }
            net.assign(p, p + static_cast<size_t>(nw) * nh);
            api->ReleaseValue(outV);
            S.cache.emplace_front(key, net);
            S.cacheDims.emplace_front(key, std::make_pair(nw, nh));
            while (S.cache.size() > 6) { S.cache.pop_back(); S.cacheDims.pop_back(); }
        }
    }

    // Robust normalisation (1st..99th percentile) so single outliers do not flatten the map.
    std::vector<float> sorted(net);
    std::sort(sorted.begin(), sorted.end());
    const float lo = sorted[sorted.size() / 100], hi = sorted[sorted.size() - 1 - sorted.size() / 100];
    const float inv = hi > lo ? 1.0f / (hi - lo) : 1.0f;

    out.resize(static_cast<size_t>(w) * h);
    parallelFor(h, [&](int y) {
        const double sy = clampv((y + 0.5) * nh / h - 0.5, 0.0, nh - 1.001);
        const int y0 = static_cast<int>(sy);
        const float fy = static_cast<float>(sy - y0);
        for (int x = 0; x < w; ++x) {
            const double sx = clampv((x + 0.5) * nw / w - 0.5, 0.0, nw - 1.001);
            const int x0 = static_cast<int>(sx);
            const float fx = static_cast<float>(sx - x0);
            auto at = [&](int xx, int yy) { return net[static_cast<size_t>(yy) * nw + xx]; };
            const float a = at(x0, y0) + (at(x0 + 1, y0) - at(x0, y0)) * fx;
            const float b = at(x0, y0 + 1) + (at(x0 + 1, y0 + 1) - at(x0, y0 + 1)) * fx;
            out[static_cast<size_t>(y) * w + x] = clampv((a + (b - a) * fy - lo) * inv, 0.0f, 1.0f);
        }
    });

    if (refine) refineDepth(rgb, w, h, out, std::max(2, std::max(w, h) / 240), 2e-3f);
    return true;
}

#endif

} // namespace lensyum
