// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <utility>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJSEngine>
#include <QJSValue>
#include <QSaveFile>

#include "core/core.h"
#include "core/debugger/nx2_debug_tools.h"
#include "core/hle/kernel/k_process.h"
#include "qt_common/qt_common.h"
#include "render/adventure_script.h"

namespace {
/// A module that never returns must not hold the emulator's window.
class Watchdog {
public:
    explicit Watchdog(QJSEngine& engine) : thread{[this, &engine] {
        std::unique_lock lock{mutex};
        if (!done.wait_for(lock, std::chrono::seconds{5}, [this] { return finished; })) {
            engine.setInterrupted(true);
        }
    }} {}

    ~Watchdog() {
        {
            std::scoped_lock lock{mutex};
            finished = true;
        }
        done.notify_all();
        thread.join();
    }

private:
    std::mutex mutex;
    std::condition_variable done;
    bool finished = false;
    std::thread thread;
};

QString Describe(const QJSValue& value) {
    const QString name = QFileInfo{value.property(QStringLiteral("fileName")).toString()}.fileName();
    return QStringLiteral("%1, line %2: %3")
        .arg(name)
        .arg(value.property(QStringLiteral("lineNumber")).toInt())
        .arg(value.property(QStringLiteral("message")).toString());
}

/// A plain name: a module cannot climb out of the folders it is given.
bool PlainName(const QString& name) {
    return !name.isEmpty() && name != QStringLiteral("..") && !name.contains(QLatin1Char('/')) &&
           !name.contains(QLatin1Char('\\'));
}

int Length(const QJSValue& array) {
    return array.property(QStringLiteral("length")).toInt();
}

NotesRow ParseRow(const QJSValue& object) {
    NotesRow row;
    row.title = object.property(QStringLiteral("title")).toString();
    const QJSValue value = object.property(QStringLiteral("value"));
    if (!value.isUndefined()) {
        row.value = value.toString();
    }
    const QJSValue check = object.property(QStringLiteral("check"));
    if (!check.isUndefined()) {
        row.check = check.toBool() ? 1 : 0;
    }
    const QJSValue done = object.property(QStringLiteral("done"));
    if (!done.isUndefined()) {
        row.done = done.toInt();
        row.total = object.property(QStringLiteral("total")).toInt();
    }
    return row;
}

NotesReport ParseReport(const QJSValue& result) {
    NotesReport report;
    report.title = result.property(QStringLiteral("title")).toString();
    const QJSValue keys = result.property(QStringLiteral("keys"));
    for (int k = 0; k < Length(keys); ++k) {
        report.keys.push_back(ParseRow(keys.property(static_cast<quint32>(k))));
    }
    const QJSValue tabs = result.property(QStringLiteral("tabs"));
    for (int t = 0; t < Length(tabs); ++t) {
        const QJSValue tab_object = tabs.property(static_cast<quint32>(t));
        NotesTab tab{tab_object.property(QStringLiteral("name")).toString(), {}};
        const QJSValue views = tab_object.property(QStringLiteral("views"));
        for (int v = 0; v < Length(views); ++v) {
            const QJSValue view_object = views.property(static_cast<quint32>(v));
            NotesView view{view_object.property(QStringLiteral("name")).toString(), {}};
            const QJSValue rows = view_object.property(QStringLiteral("rows"));
            for (int r = 0; r < Length(rows); ++r) {
                view.rows.push_back(ParseRow(rows.property(static_cast<quint32>(r))));
            }
            tab.views.push_back(std::move(view));
        }
        report.tabs.push_back(std::move(tab));
    }
    return report;
}

// Plain functions in front of the host: a script sees these, not the QObject and its methods.
constexpr const char* Bootstrap = R"((function (host) {
    return {
        unzip: (data, offset, end) =>
            host.unzip(data, offset === undefined ? 0 : offset, end === undefined ? -1 : end),
        utf8: (data, offset, size) =>
            host.utf8(data, offset === undefined ? 0 : offset, size === undefined ? -1 : size),
        file: (name) => host.file(name),
        include: (name) => host.include(name),
        memory: (address, size) => host.memory(String(address), size),
        search: (pattern) => host.search(pattern),
        load: (name) => host.load(name),
        store: (name, text) => host.store(name, String(text)),
    };
}))";
/// The running game, or null while none runs or it is stopping.
Kernel::KProcess* RunningGame() {
    Core::System* const system = QtCommon::system.get();
    if (system == nullptr || system->IsShuttingDown()) {
        return nullptr;
    }
    return system->ApplicationProcess();
}

/// The heap addresses where pattern starts, at most 64. Scans in slices so that a stopping
/// panel or game ends it within one slice.
std::vector<u64> SearchHeap(const std::vector<u8>& pattern, const std::atomic<bool>& stopping) {
    std::vector<u64> hits;
    Kernel::KProcess* const process = RunningGame();
    if (process == nullptr || pattern.empty()) {
        return hits;
    }
    const u64 start = GetInteger(process->GetPageTable().GetHeapRegionStart());
    const u64 end = start + process->GetPageTable().GetHeapRegionSize();
    // Small slices: a stopping game or panel ends the search within a few milliseconds.
    constexpr u64 Slice = u64{16} << 20;
    for (u64 at = start; at < end && hits.size() < 64; at += Slice) {
        if (stopping.load(std::memory_order_relaxed) || RunningGame() == nullptr) {
            break;
        }
        const u64 slice_end = (std::min)(end, at + Slice);
        // A match may start before slice_end and end after it.
        const u64 read_end = (std::min)(end, slice_end + pattern.size() - 1);
        const auto found = Core::Nx2Debug::SearchGuestMemory(
            process->GetMemory(), pattern, at, read_end, 64,
            std::chrono::steady_clock::now() + std::chrono::seconds{10});
        for (const u64 hit : found.hits) {
            if (hit < slice_end && hits.size() < 64) {
                hits.push_back(hit);
            }
        }
    }
    return hits;
}
} // Anonymous namespace

NotesHost::NotesHost(QJSEngine* engine_, QStringList library_dirs)
    : engine{engine_}, libraries{std::move(library_dirs)} {}

NotesHost::~NotesHost() {
    stopping = true;
    if (search_job.valid()) {
        search_job.wait();
    }
}

QJSValue NotesHost::memory(const QString& address, int size) {
    bool ok = false;
    const u64 start = QString{address}.remove(QStringLiteral("0x")).toULongLong(&ok, 16);
    if (!ok || size <= 0 || size > 4096) {
        engine->throwError(QStringLiteral("memory: want a hex address and a size up to 4096"));
        return {};
    }
    Kernel::KProcess* const process = RunningGame();
    if (process == nullptr) {
        return QJSValue{QJSValue::NullValue};
    }
    QByteArray bytes(size, '\0');
    if (Core::Nx2Debug::ReadGuestMemory(
            process->GetMemory(), start,
            std::span<u8>{reinterpret_cast<u8*>(bytes.data()), static_cast<size_t>(size)})) {
        return QJSValue{QJSValue::NullValue};
    }
    return engine->toScriptValue(bytes);
}

QJSValue NotesHost::search(const QString& pattern) {
    if (search_job.valid()) {
        if (search_job.wait_for(std::chrono::seconds{0}) != std::future_status::ready) {
            return QJSValue{QJSValue::NullValue};
        }
        const std::vector<u64> hits = search_job.get();
        if (pattern != search_pattern) {
            // A finished search for another pattern: drop it and start this one.
            return search(pattern);
        }
        QJSValue result = engine->newArray(static_cast<uint>(hits.size()));
        for (size_t i = 0; i < hits.size(); ++i) {
            result.setProperty(static_cast<quint32>(i),
                               QStringLiteral("0x%1").arg(hits[i], 0, 16));
        }
        return result;
    }
    const QByteArray bytes = QByteArray::fromHex(pattern.toLatin1());
    if (bytes.isEmpty() || bytes.size() > 256) {
        engine->throwError(QStringLiteral("search: want 1 to 256 bytes in hex"));
        return {};
    }
    search_pattern = pattern;
    std::vector<u8> wanted(bytes.begin(), bytes.end());
    search_job = std::async(std::launch::async, [this, wanted = std::move(wanted)] {
        return SearchHeap(wanted, stopping);
    });
    return QJSValue{QJSValue::NullValue};
}

QString NotesHost::StatePath(const QString& name) const {
    if (!PlainName(name) || libraries.isEmpty()) {
        return {};
    }
    // libraries[0] is notes/lib: the state sits next to it.
    QDir state{libraries.front()};
    state.cdUp();
    state.mkpath(QStringLiteral("state"));
    return state.filePath(QStringLiteral("state/%1").arg(name));
}

QString NotesHost::load(const QString& name) {
    QFile input{StatePath(name)};
    if (!input.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QString::fromUtf8(input.readAll());
}

void NotesHost::store(const QString& name, const QString& text) {
    const QString path = StatePath(name);
    if (path.isEmpty()) {
        engine->throwError(QStringLiteral("store: want a plain file name"));
        return;
    }
    QSaveFile output{path};
    if (output.open(QIODevice::WriteOnly)) {
        output.write(text.toUtf8());
        static_cast<void>(output.commit());
    }
}

void NotesHost::SetSlot(const QString& directory) {
    slot = directory;
}

QByteArray NotesHost::unzip(const QByteArray& data, int offset, int end) {
    const qsizetype stop = end < 0 ? data.size() : std::min<qsizetype>(end, data.size());
    if (offset < 0 || offset >= stop) {
        engine->throwError(QStringLiteral("unzip: offset outside the data"));
        return {};
    }
    // qUncompress wants the size up front; it is only a hint and grows when it is short.
    QByteArray packed(4, '\0');
    packed[1] = '\x40';
    packed.append(data.constData() + offset, stop - offset);
    const QByteArray result = qUncompress(packed);
    if (result.isEmpty()) {
        engine->throwError(QStringLiteral("unzip: not a zlib stream"));
    }
    return result;
}

QString NotesHost::utf8(const QByteArray& data, int offset, int size) {
    if (offset < 0 || offset >= data.size()) {
        return {};
    }
    const qsizetype room = data.size() - offset;
    const qsizetype limit = size < 0 ? room : std::min<qsizetype>(size, room);
    const char* text = data.constData() + offset;
    return QString::fromUtf8(text, std::find(text, text + limit, '\0') - text);
}

QByteArray NotesHost::file(const QString& name) {
    QFile input{QDir{slot}.filePath(name)};
    if (!PlainName(name) || !input.open(QIODevice::ReadOnly)) {
        engine->throwError(QStringLiteral("file: no %1 in this save").arg(name));
        return {};
    }
    return input.readAll();
}

void NotesHost::include(const QString& name) {
    for (const QString& directory : libraries) {
        QFile input{QDir{directory}.filePath(name)};
        if (PlainName(name) && input.open(QIODevice::ReadOnly)) {
            const QJSValue result =
                engine->evaluate(QString::fromUtf8(input.readAll()), input.fileName());
            if (result.isError()) {
                engine->throwError(Describe(result));
            }
            return;
        }
    }
    engine->throwError(QStringLiteral("include: no library named %1").arg(name));
}

NotesScript::NotesScript() = default;
NotesScript::~NotesScript() = default;

bool NotesScript::Load(const QString& path, const QStringList& library_dirs, QString& error) {
    QFile input{path};
    if (!input.open(QIODevice::ReadOnly)) {
        return false;
    }
    host.reset();
    engine = std::make_unique<QJSEngine>();
    host = std::make_unique<NotesHost>(engine.get(), library_dirs);
    QJSEngine::setObjectOwnership(host.get(), QJSEngine::CppOwnership);
    const QJSValue make = engine->evaluate(QString::fromLatin1(Bootstrap));
    engine->globalObject().setProperty(QStringLiteral("notes"),
                                       make.call({engine->newQObject(host.get())}));
    QJSValue game;
    {
        Watchdog watchdog{*engine};
        const QJSValue result = engine->evaluate(QString::fromUtf8(input.readAll()), path);
        if (result.isError()) {
            error = Describe(result);
            engine->setInterrupted(false);
            return false;
        }
        game = engine->evaluate(QStringLiteral("game"));
    }
    engine->setInterrupted(false);
    title = game.property(QStringLiteral("title")).toString();
    accent = game.property(QStringLiteral("accent")).toString();
    folder = game.property(QStringLiteral("folder")).toString();
    file = game.property(QStringLiteral("file")).toString();
    if (game.isError() || !engine->globalObject().property(QStringLiteral("read")).isCallable() ||
        folder.isEmpty() || file.isEmpty()) {
        error = QStringLiteral("A module needs game, with folder and file, and a read function");
        return false;
    }
    // Plain names only: a module reads its own game's save and nothing else.
    if (!PlainName(folder) || folder == QStringLiteral(".") || !PlainName(file) ||
        file == QStringLiteral(".")) {
        error = QStringLiteral("A module's folder and file must be plain names");
        return false;
    }
    return true;
}

std::optional<NotesReport> NotesScript::Read(const QString& path, QString& error) {
    QFile input{path};
    if (!engine || !input.open(QIODevice::ReadOnly)) {
        error = QStringLiteral("Unreadable save");
        return std::nullopt;
    }
    const QFileInfo info{path};
    host->SetSlot(info.absolutePath());
    QJSValue save = engine->newObject();
    save.setProperty(QStringLiteral("data"), engine->toScriptValue(input.readAll()));
    save.setProperty(QStringLiteral("slot"), info.dir().dirName());
    QJSValue result;
    {
        Watchdog watchdog{*engine};
        result = engine->globalObject().property(QStringLiteral("read")).call({save});
    }
    engine->setInterrupted(false);
    if (result.isError()) {
        error = Describe(result);
        return std::nullopt;
    }
    return ParseReport(result);
}

bool NotesScript::Tick(QString& error) {
    if (!engine) {
        return false;
    }
    const QJSValue tick = engine->globalObject().property(QStringLiteral("tick"));
    if (!tick.isCallable()) {
        return false;
    }
    QJSValue result;
    {
        Watchdog watchdog{*engine};
        result = tick.call();
    }
    engine->setInterrupted(false);
    if (result.isError()) {
        error = Describe(result);
        return false;
    }
    return result.toBool();
}
