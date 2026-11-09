// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/fs/dstorage_queue.h"

#ifdef LICHII_DIRECTSTORAGE

#include <algorithm>
#include <mutex>

// INITGUID before any Windows header: defines the DirectStorage IIDs in this translation unit,
// so dstorage.lib does not need to be linked (dstorage.dll is loaded at run time).
#define INITGUID
#include <initguid.h>
#include <windows.h>
#include <dstorage.h>

#include "common/fs/fs_util.h"
#include "common/logging.h"

namespace Common::FS::DirectStorage {
namespace {

using DStorageGetFactoryFn = HRESULT(WINAPI*)(REFIID, void**);

struct Runtime {
    ~Runtime() {
        if (queue1) {
            queue1->Release();
        }
        if (queue) {
            queue->Release();
        }
        if (factory) {
            factory->Release();
        }
        if (dll) {
            FreeLibrary(dll);
        }
    }

    HMODULE dll{};
    IDStorageFactory* factory{};
    IDStorageQueue* queue{};
    IDStorageQueue1* queue1{};
    std::once_flag init_once{};
    std::mutex queue_mutex{};
    bool available{};
};

Runtime& GetRuntime() {
    static Runtime runtime;
    return runtime;
}

void Initialize() {
    auto& runtime = GetRuntime();
    runtime.dll = LoadLibraryW(L"dstorage.dll");
    if (!runtime.dll) {
        LOG_WARNING(Common_Filesystem, "DirectStorage: dstorage.dll not found, using regular reads");
        return;
    }
    const auto get_factory =
        reinterpret_cast<DStorageGetFactoryFn>(GetProcAddress(runtime.dll, "DStorageGetFactory"));
    if (!get_factory) {
        LOG_WARNING(Common_Filesystem, "DirectStorage: DStorageGetFactory not exported");
        return;
    }
    if (FAILED(get_factory(__uuidof(IDStorageFactory), reinterpret_cast<void**>(&runtime.factory)))) {
        LOG_WARNING(Common_Filesystem, "DirectStorage: DStorageGetFactory failed");
        return;
    }
    DSTORAGE_QUEUE_DESC desc{};
    desc.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
    desc.Capacity = DSTORAGE_MAX_QUEUE_CAPACITY;
    desc.Priority = DSTORAGE_PRIORITY_NORMAL;
    desc.Name = "lichii";
    if (FAILED(runtime.factory->CreateQueue(&desc, __uuidof(IDStorageQueue),
                                             reinterpret_cast<void**>(&runtime.queue)))) {
        LOG_WARNING(Common_Filesystem, "DirectStorage: CreateQueue failed");
        return;
    }
    // EnqueueSetEvent needs IDStorageQueue1 (DirectStorage 1.1+).
    if (FAILED(runtime.queue->QueryInterface(__uuidof(IDStorageQueue1),
                                             reinterpret_cast<void**>(&runtime.queue1)))) {
        LOG_WARNING(Common_Filesystem, "DirectStorage: IDStorageQueue1 unavailable");
        return;
    }
    runtime.available = true;
    LOG_INFO(Common_Filesystem,
             "DirectStorage runtime loaded: file reads of at least {} KiB go through its queue",
             MinDirectStorageReadSize / 1024);
}

} // namespace

bool IsAvailable() {
    auto& runtime = GetRuntime();
    std::call_once(runtime.init_once, Initialize);
    return runtime.available;
}

bool ReadFileAt(const std::filesystem::path& path, u64 offset, void* dst, size_t size) {
    auto& runtime = GetRuntime();
    std::call_once(runtime.init_once, Initialize);
    if (!runtime.available || size == 0) {
        return false;
    }
    // One synchronous read at a time: the queue has a fixed capacity and EnqueueRequest blocks
    // when it is full, so serializing keeps the request structs (stack) alive until completion.
    std::lock_guard lock{runtime.queue_mutex};

    IDStorageFile* file = nullptr;
    if (FAILED(runtime.factory->OpenFile(path.c_str(), __uuidof(IDStorageFile),
                                         reinterpret_cast<void**>(&file)))) {
        return false;
    }
    IDStorageStatusArray* status = nullptr;
    if (FAILED(runtime.factory->CreateStatusArray(1, "lichii-read", __uuidof(IDStorageStatusArray),
                                                  reinterpret_cast<void**>(&status)))) {
        file->Release();
        return false;
    }
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!event) {
        status->Release();
        file->Release();
        return false;
    }

    // Large reads are split into small requests: single requests above a few tens of MiB were
    // rejected by the runtime (observed failures at 42 MiB and 64 MiB), and small requests let
    // it pipeline. A failed chunk aborts the whole read; the caller then re-reads the full
    // range with std::fread, which overwrites any partial data.
    constexpr u64 kMaxRequestSize = 16 * 1024 * 1024;
    u8* out = static_cast<u8*>(dst);
    bool ok = true;
    HRESULT request_hr = S_OK;
    for (u64 done = 0; done < size; done += kMaxRequestSize) {
        const u32 chunk = static_cast<u32>(std::min<u64>(kMaxRequestSize, size - done));
        DSTORAGE_REQUEST request{};
        request.Options.CompressionFormat = DSTORAGE_COMPRESSION_FORMAT_NONE;
        request.Options.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
        request.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_MEMORY;
        request.Source.File.Source = file;
        request.Source.File.Offset = offset + done;
        request.Source.File.Size = chunk;
        request.Destination.Memory.Buffer = out + done;
        request.Destination.Memory.Size = chunk;
        request.Name = "lichii-read";

        ResetEvent(event);
        runtime.queue->EnqueueRequest(&request);
        runtime.queue->EnqueueStatus(status, 0);
        runtime.queue1->EnqueueSetEvent(event);
        runtime.queue->Submit();

        if (WaitForSingleObject(event, INFINITE) != WAIT_OBJECT_0 || !status->IsComplete(0) ||
            FAILED(request_hr = status->GetHResult(0))) {
            ok = false;
            break;
        }
    }
    if (!ok) {
        LOG_WARNING(Common_Filesystem,
                    "DirectStorage: read of {} bytes at {:#x} in \"{}\" failed (hr={:#x})", size,
                    offset, Common::FS::PathToUTF8String(path), static_cast<u32>(request_hr));
    }

    CloseHandle(event);
    status->Release();
    file->Release();
    return ok;
}

} // namespace Common::FS::DirectStorage

#endif // LICHII_DIRECTSTORAGE
