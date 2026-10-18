// Deterministic Win32 IOCP lifetime regressions for the vendored efsw backend.
// SPDX-License-Identifier: GPL-3.0-or-later
#define CATCH_CONFIG_RUNNER
#include <catch2/catch.hpp>

#include <efsw/FileWatcherWin32.hpp>
#include <efsw/Lock.hpp>

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cwchar>
#include <functional>
#include <string>
#include <thread>


namespace {

class TempDirectory {
public:
    TempDirectory()
    {
        wchar_t tempPath[MAX_PATH]{};
        REQUIRE(GetTempPathW(MAX_PATH, tempPath) != 0);
        wchar_t tempFile[MAX_PATH]{};
        REQUIRE(GetTempFileNameW(tempPath, L"efw", 0, tempFile) != 0);
        REQUIRE(DeleteFileW(tempFile) != FALSE);
        REQUIRE(CreateDirectoryW(tempFile, nullptr) != FALSE);
        path_ = tempFile;
    }

    ~TempDirectory()
    {
        if (path_.empty()) return;
        const std::wstring pattern = path_ + L"\\*";
        WIN32_FIND_DATAW entry{};
        HANDLE search = FindFirstFileW(pattern.c_str(), &entry);
        if (search != INVALID_HANDLE_VALUE) {
            do {
                if (wcscmp(entry.cFileName, L".") == 0 || wcscmp(entry.cFileName, L"..") == 0) continue;
                const std::wstring childPath = path_ + L"\\" + entry.cFileName;
                if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) RemoveDirectoryW(childPath.c_str());
                else DeleteFileW(childPath.c_str());
            } while (FindNextFileW(search, &entry));
            FindClose(search);
        }
        RemoveDirectoryW(path_.c_str());
    }
    std::wstring child(const wchar_t* name) const { return path_ + L"\\" + name; }

    std::string utf8() const
    {
        const int size = WideCharToMultiByte(CP_UTF8, 0, path_.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (size <= 1) return {};
        std::string result(static_cast<std::size_t>(size), '\0');
        WideCharToMultiByte(CP_UTF8, 0, path_.c_str(), -1, result.data(), size, nullptr, nullptr);
        result.resize(static_cast<std::size_t>(size - 1));
        return result;
    }

private:
    std::wstring path_;
};

class Listener final : public efsw::FileWatchListener {
public:
    void handleFileAction(efsw::WatchID, const std::string&, const std::string&,
                          efsw::Action, std::string) override
    {
        callbacks.fetch_add(1, std::memory_order_relaxed);
        if (onAction) onAction();
    }

    std::atomic<unsigned> callbacks{0};
    std::function<void()> onAction;
};

class InspectableFileWatcher final : public efsw::FileWatcherWin32 {
public:
    explicit InspectableFileWatcher(efsw::FileWatcher* parent)
        : FileWatcherWin32(parent)
    {
    }

    efsw::WatcherStructWin32* active(efsw::WatchID id)
    {
        efsw::Lock lock(mWatchesLock);
        for (auto* watch : mWatches) {
            if (watch->Watch->ID == id) return watch;
        }
        return nullptr;
    }

    struct Counts { std::size_t active; std::size_t retired; };

    Counts counts()
    {
        efsw::Lock lock(mWatchesLock);
        return {mWatches.size(), mRetiredWatches.size()};
    }

    bool isPending(efsw::WatcherStructWin32* watch)
    {
        efsw::Lock lock(mWatchesLock);
        return watch->Pending;
    }

    void dispatch(DWORD bytes, OVERLAPPED* overlapped, bool success)
    {
        dispatchCompletion(bytes, overlapped, success);
    }

    struct Completion {
        BOOL success;
        DWORD bytes;
        ULONG_PTR key;
        OVERLAPPED* overlapped;
        DWORD error;
    };

    Completion dequeue(DWORD timeoutMs = 5000)
    {
        Completion result{};
        result.success = GetQueuedCompletionStatus(mIOCP, &result.bytes, &result.key,
                                                   &result.overlapped, timeoutMs);
        result.error = result.success ? ERROR_SUCCESS : GetLastError();
        return result;
    }
};

struct WatcherFixture {
    // Keep the directory and parent alive until after the backend has drained.
    TempDirectory directory;
    efsw::FileWatcher parent;
    Listener listener;
    InspectableFileWatcher watcher{&parent};

    efsw::WatchID add()
    {
        return watcher.addWatch(directory.utf8(), &listener, false,
                                {{efsw::Option::WinNotifyFilter, FILE_NOTIFY_CHANGE_FILE_NAME}});
    }
};

bool createFile(const std::wstring& path)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    const char contents[] = "io completion";
    DWORD written = 0;
    const BOOL wrote = WriteFile(file, contents, sizeof(contents), &written, nullptr);
    const BOOL closed = CloseHandle(file);
    return wrote != FALSE && written == sizeof(contents) && closed != FALSE;
}

bool waitForRetiredEmpty(InspectableFileWatcher& watcher, DWORD timeoutMs)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    do {
        if (watcher.counts().retired == 0) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    } while (std::chrono::steady_clock::now() < deadline);
    return watcher.counts().retired == 0;
}

} // namespace

TEST_CASE("watch removal retains storage across the completion cancellation race", "[efsw][lifecycle]")
{
    WatcherFixture fixture;
    const auto id = fixture.add();
    REQUIRE(id > 0);
    auto* watch = fixture.watcher.active(id);
    REQUIRE(watch != nullptr);

    // File changes race with cancellation. Do not assume the directory handle is
    // waitable or claim the normal packet was queued before removeWatch.
    REQUIRE(createFile(fixture.directory.child(L"queued.txt")));
    OVERLAPPED* const expectedOverlapped = &watch->Overlapped;
    fixture.watcher.removeWatch(id);
    auto counts = fixture.watcher.counts();
    const bool retainedUntilCompletion = counts.retired == 1;
    CHECK(counts.active == 0);
    CHECK(retainedUntilCompletion);
    const auto packet = fixture.watcher.dequeue();
    if (retainedUntilCompletion && packet.overlapped) {
        fixture.watcher.dispatch(packet.bytes, packet.overlapped, packet.success != FALSE);
    }
    counts = fixture.watcher.counts();
    CHECK(counts.active == 0);
    REQUIRE(counts.retired == 0);
    CHECK(packet.overlapped == expectedOverlapped);
    CHECK((packet.success != FALSE || packet.error == ERROR_OPERATION_ABORTED));
    REQUIRE(fixture.listener.callbacks.load(std::memory_order_relaxed) == 0);
}

TEST_CASE("dequeued completion survives removal until production dispatch returns", "[efsw][lifecycle]")
{
    WatcherFixture fixture;
    const auto id = fixture.add();
    REQUIRE(id > 0);
    auto* watch = fixture.watcher.active(id);
    REQUIRE(watch != nullptr);
    REQUIRE(createFile(fixture.directory.child(L"dequeued.txt")));

    // GQCS removes the packet and updates OVERLAPPED state; dispatch is deliberately
    // held by the test so removeWatch races the exact post-dequeue/pre-dispatch window.
    const auto packet = fixture.watcher.dequeue();
    OVERLAPPED* const expectedOverlapped = &watch->Overlapped;
    fixture.watcher.removeWatch(id);
    const bool retainedUntilDispatch = fixture.watcher.counts().retired == 1;
    if (retainedUntilDispatch && packet.overlapped) {
        fixture.watcher.dispatch(packet.bytes, packet.overlapped, packet.success != FALSE);
    }
    CHECK(retainedUntilDispatch);
    CHECK(packet.overlapped == expectedOverlapped);
    CHECK(packet.success != FALSE);
    REQUIRE(fixture.watcher.counts().retired == 0);
    REQUIRE(fixture.listener.callbacks.load(std::memory_order_relaxed) == 0);
}

TEST_CASE("canceled IOCP completion retains watch until cancellation packet is consumed", "[efsw][lifecycle]")
{
    WatcherFixture fixture;
    const auto id = fixture.add();
    REQUIRE(id > 0);
    auto* watch = fixture.watcher.active(id);
    REQUIRE(watch != nullptr);

    fixture.watcher.removeWatch(id);
    const bool retainedUntilCancelPacket = fixture.watcher.counts().retired == 1;
    CHECK(retainedUntilCancelPacket);
    const auto packet = fixture.watcher.dequeue();
    OVERLAPPED* const expectedOverlapped = &watch->Overlapped;
    if (retainedUntilCancelPacket && packet.overlapped) {
        fixture.watcher.dispatch(packet.bytes, packet.overlapped, packet.success != FALSE);
    }
    CHECK(packet.overlapped == expectedOverlapped);
    CHECK(packet.success == FALSE);
    CHECK(packet.error == ERROR_OPERATION_ABORTED);
    CHECK(fixture.watcher.counts().active == 0);
    REQUIRE(fixture.watcher.counts().retired == 0);
}

TEST_CASE("zero-byte successful completion rearms exactly one outstanding read", "[efsw][lifecycle]")
{
    WatcherFixture fixture;
    const auto id = fixture.add();
    REQUIRE(id > 0);
    auto* watch = fixture.watcher.active(id);
    REQUIRE(watch != nullptr);
    REQUIRE(createFile(fixture.directory.child(L"zero-byte-path.txt")));

    const auto packet = fixture.watcher.dequeue();
    OVERLAPPED* const expectedOverlapped = &watch->Overlapped;

    // Feed the production handler a zero-byte successful completion after consuming
    // a genuine kernel completion. This tests the zero-byte branch, not OS overflow.
    if (packet.overlapped) {
        fixture.watcher.dispatch(0, packet.overlapped, packet.success != FALSE);
    }
    CHECK(packet.overlapped == expectedOverlapped);
    CHECK(packet.success != FALSE);
    REQUIRE(fixture.watcher.active(id) == watch);
    REQUIRE(fixture.watcher.isPending(watch));

    fixture.watcher.removeWatch(id);
    const bool retainedUntilCancelPacket = fixture.watcher.counts().retired == 1;
    const auto canceled = fixture.watcher.dequeue();
    if (retainedUntilCancelPacket && canceled.overlapped) {
        fixture.watcher.dispatch(canceled.bytes, canceled.overlapped, canceled.success != FALSE);
    }
    CHECK(retainedUntilCancelPacket);
    CHECK(canceled.overlapped == expectedOverlapped);
    CHECK(canceled.success == FALSE);
    REQUIRE(fixture.watcher.counts().retired == 0);
    const auto extra = fixture.watcher.dequeue(0);
    REQUIRE(extra.success == FALSE);
    REQUIRE(extra.overlapped == nullptr);
    REQUIRE(extra.error == WAIT_TIMEOUT);
}

TEST_CASE("destroying a backend before watch starts drains outstanding cancellations", "[efsw][lifecycle]")
{
    efsw::FileWatcher parent;
    Listener listener;
    TempDirectory directory;
    {
        InspectableFileWatcher watcher(&parent);
        const auto id = watcher.addWatch(directory.utf8(), &listener, false,
                                        {{efsw::Option::WinNotifyFilter, FILE_NOTIFY_CHANGE_FILE_NAME}});
        REQUIRE(id > 0);
        REQUIRE(watcher.active(id) != nullptr);
        // Deliberately do not call watch(): the destructor must drain completions
        // synchronously rather than leak a pending read or free its OVERLAPPED early.
    }
    SUCCEED("backend destruction returned without a worker thread");
}

TEST_CASE("listener may remove its own watch during completion dispatch", "[efsw][lifecycle]")
{
    WatcherFixture fixture;
    const auto id = fixture.add();
    REQUIRE(id > 0);
    auto* watch = fixture.watcher.active(id);
    REQUIRE(watch != nullptr);
    fixture.listener.onAction = [&] { fixture.watcher.removeWatch(id); };
    REQUIRE(createFile(fixture.directory.child(L"self-remove.txt")));

    OVERLAPPED* const expectedOverlapped = &watch->Overlapped;
    const auto packet = fixture.watcher.dequeue();
    if (packet.overlapped) {
        fixture.watcher.dispatch(packet.bytes, packet.overlapped, packet.success != FALSE);
    }
    CHECK(packet.overlapped == expectedOverlapped);
    CHECK(packet.success != FALSE);
    const auto counts = fixture.watcher.counts();
    REQUIRE(counts.active == 0);
    REQUIRE(counts.retired == 0);
    REQUIRE(fixture.listener.callbacks.load(std::memory_order_relaxed) == 1);
}

TEST_CASE("destroying a running backend joins after pending completion drain", "[efsw][lifecycle]")
{
    TempDirectory directory;
    efsw::FileWatcher parent;
    Listener listener;
    {
        InspectableFileWatcher watcher(&parent);
        const auto id = watcher.addWatch(directory.utf8(), &listener, false,
                                         {{efsw::Option::WinNotifyFilter, FILE_NOTIFY_CHANGE_FILE_NAME}});
        REQUIRE(id > 0);
        watcher.watch();
        // Destruction races the still-pending read and must join only after the
        // cancellation packet has been drained by the worker.
    }
    SUCCEED("running backend shutdown drained and joined");
}

TEST_CASE("rapid add/remove traffic drains all retired IOCP watches", "[efsw][lifecycle]")
{
    WatcherFixture fixture;
    constexpr int iterations = 128;
    for (int i = 0; i < iterations; ++i) {
        const auto id = fixture.add();
        REQUIRE(id > 0);
        fixture.watcher.removeWatch(id);
    }
    REQUIRE(fixture.watcher.counts().active == 0);
    REQUIRE(fixture.watcher.counts().retired == iterations);
    fixture.watcher.watch();
    REQUIRE(waitForRetiredEmpty(fixture.watcher, 10000));
}

int main(int argc, char** argv)
{
    Catch::Session session;
    return session.run(argc, argv);
}
