#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objbase.h>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <string>
#include <mutex>

template <typename T>
class ComPtr {
public:
    ComPtr() noexcept : ptr_(nullptr) {}
    ComPtr(std::nullptr_t) noexcept : ptr_(nullptr) {}
    ComPtr(const ComPtr& other) noexcept : ptr_(other.ptr_) {
        if (ptr_) ptr_->AddRef();
    }
    ComPtr(ComPtr&& other) noexcept : ptr_(other.ptr_) {
        other.ptr_ = nullptr;
    }
    ~ComPtr() { Reset(); }

    ComPtr& operator=(std::nullptr_t) noexcept {
        Reset();
        return *this;
    }
    ComPtr& operator=(const ComPtr& other) noexcept {
        if (this != &other) {
            ComPtr tmp(other);
            Swap(tmp);
        }
        return *this;
    }
    ComPtr& operator=(ComPtr&& other) noexcept {
        if (this != &other) {
            Reset();
            ptr_ = other.ptr_;
            other.ptr_ = nullptr;
        }
        return *this;
    }

    void Swap(ComPtr& other) noexcept {
        T* t = ptr_;
        ptr_ = other.ptr_;
        other.ptr_ = t;
    }

    T* Get() const noexcept { return ptr_; }
    T* operator->() const noexcept { return ptr_; }
    explicit operator bool() const noexcept { return ptr_ != nullptr; }

    T** Put() noexcept {
        Reset();
        return &ptr_;
    }

    void** PutVoid() noexcept {
        Reset();
        return reinterpret_cast<void**>(&ptr_);
    }

    void Attach(T* p) noexcept {
        Reset();
        ptr_ = p;
    }

    T* Detach() noexcept {
        T* p = ptr_;
        ptr_ = nullptr;
        return p;
    }

    void Reset() noexcept {
        if (ptr_) {
            ptr_->Release();
            ptr_ = nullptr;
        }
    }

private:
    T* ptr_;
};

struct ComScope {
    explicit ComScope(DWORD flags = COINIT_MULTITHREADED) noexcept {
        hr_ = CoInitializeEx(nullptr, flags);
    }
    ~ComScope() {
        if (SUCCEEDED(hr_)) CoUninitialize();
    }
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;
    HRESULT hr_ = E_FAIL;
};

inline int64_t GetQpcNow() noexcept {
    LARGE_INTEGER li{};
    QueryPerformanceCounter(&li);
    return li.QuadPart;
}

inline int64_t GetQpcFreq() noexcept {
    static int64_t freq = []() {
        LARGE_INTEGER li{};
        QueryPerformanceFrequency(&li);
        return li.QuadPart > 0 ? li.QuadPart : 10000000LL;
    }();
    return freq;
}

inline int64_t QpcTo100Ns(int64_t deltaTicks) noexcept {
    const int64_t freq = GetQpcFreq();
    // Avoid 64-bit overflow for long uptimes by splitting seconds and remainder.
    int64_t secs = deltaTicks / freq;
    int64_t rem = deltaTicks % freq;
    return secs * 10000000LL + (rem * 10000000LL) / freq;
}

class Logger {
public:
    static void Init(const std::wstring& logPath) {
        std::lock_guard<std::mutex> lk(Mutex());
        Path() = logPath;
        // Truncate if previous log exceeded 512 KB
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (GetFileAttributesExW(logPath.c_str(), GetFileExInfoStandard, &fad)) {
            if (fad.nFileSizeHigh > 0 || fad.nFileSizeLow > 512 * 1024) {
                DeleteFileW(logPath.c_str());
            }
        }
    }

    static void Log(const char* fmt, ...) {
        std::lock_guard<std::mutex> lk(Mutex());
        if (Path().empty()) return;
        FILE* f = _wfopen(Path().c_str(), L"a");
        if (!f) return;
        SYSTEMTIME st{};
        GetLocalTime(&st);
        fprintf(f, "[%04u-%02u-%02u %02u:%02u:%02u.%03u] ",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        va_list ap;
        va_start(ap, fmt);
        vfprintf(f, fmt, ap);
        va_end(ap);
        fputc('\n', f);
        fclose(f);
    }

private:
    static std::mutex& Mutex() {
        static std::mutex m;
        return m;
    }
    static std::wstring& Path() {
        static std::wstring p;
        return p;
    }
};
