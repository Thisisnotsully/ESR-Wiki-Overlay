#include "web_uniques_updater.hpp"

#include <atomic>
#include <sstream>
#include <vector>

#include "web_fetch.hpp"
#include "web_uniques_live.hpp"

namespace esrwiki {

namespace {

constexpr DWORD kRecheckMs = 6u * 60u * 60u * 1000u;  // ask the site again every six hours
constexpr wchar_t kOptOutFile[] = L"uniques-live-disabled.txt";
constexpr wchar_t kUserAgent[] = L"ESR-Wiki-Overlay/1.3 (website uniques)";
constexpr std::size_t kPageCount = 4;

std::wstring CacheName(std::size_t index, const wchar_t* suffix) {
    return L"uniques-web-" + Wide(kWebUniquePageNames[index]) + suffix;  // uniques-web-unique_armors.htm[.meta]
}

}  // namespace

struct WebUniquesUpdaterState {
    std::wstring cacheDir;
    std::wstring siteUrl;
    std::string shipped;
    std::string artTable;
    std::function<void(const std::string&)> apply;
    std::function<void(const std::string&)> log;
    HANDLE stop = nullptr;
    HANDLE thread = nullptr;
    std::atomic<bool> cancelled{false};  // set when stopping, so late pages are not handed on

    std::vector<std::string> pages = std::vector<std::string>(kPageCount);  // empty = not saved yet
    std::vector<Meta> metas = std::vector<Meta>(kPageCount);

    void Log(const std::string& message) const {
        if (log && !cancelled) log("ESR Wiki website uniques: " + message);
    }

    bool Complete(const std::vector<std::string>& set) const {
        for (const std::string& page : set) if (page.empty()) return false;
        return true;
    }

    // Builds wiki data from a set of pages and passes it on. False when the pages are not usable.
    bool Use(const std::vector<std::string>& set, const char* source) {
        if (cancelled) return false;
        std::string wikiData, error;
        if (!BuildWebUniquesWikiData(shipped, set, artTable, wikiData, error)) {
            Log(std::string("the ") + source + " pages were not used: " + error);
            return false;
        }
        if (cancelled) return false;
        apply(wikiData);
        return true;
    }

    void LoadSaved() {
        for (std::size_t i = 0; i < kPageCount; ++i) {
            std::string metaText;
            if (!ReadTextFile(cacheDir + L"\\" + CacheName(i, L""), pages[i])) {
                pages[i].clear();
                continue;
            }
            if (ReadTextFile(cacheDir + L"\\" + CacheName(i, L".meta"), metaText)) {
                std::istringstream lines(metaText);
                std::getline(lines, metas[i].etag);
                std::getline(lines, metas[i].lastModified);
                for (std::string* value : {&metas[i].etag, &metas[i].lastModified}) {
                    while (!value->empty() && (value->back() == '\r' || value->back() == '\n')) value->pop_back();
                }
            }
        }
        if (Complete(pages)) {
            if (Use(pages, "saved")) {
                Log("using the copies saved from the website");
                return;
            }
        }
        // Saved copies that are no good, or an incomplete set: ask for every page afresh.
        pages.assign(kPageCount, std::string());
        metas.assign(kPageCount, Meta{});
    }

    void Run() {
        LoadSaved();
        for (;;) {
            if (cancelled) return;
            std::vector<std::string> next = pages;
            std::vector<Meta> nextMeta = metas;
            bool changed = false;
            bool failed = false;
            for (std::size_t i = 0; i < kPageCount && !failed; ++i) {
                const FetchResult fetched =
                    Fetch(siteUrl + Wide(kWebUniquePageNames[i]), pages[i].empty() ? Meta{} : metas[i], stop, kUserAgent);
                if (fetched.kind == FetchResult::Kind::Page) {
                    next[i] = fetched.body;
                    nextMeta[i] = fetched.meta;
                    changed = true;
                } else if (fetched.kind == FetchResult::Kind::Failed) {
                    Log(std::string("could not check the website: ") + fetched.error);
                    failed = true;
                }
            }
            if (cancelled) return;
            if (!failed && changed && Complete(next)) {
                if (Use(next, "downloaded")) {
                    pages = next;
                    metas = nextMeta;
                    for (std::size_t i = 0; i < kPageCount; ++i) {
                        if (!WriteFileAtomic(cacheDir + L"\\" + CacheName(i, L""), pages[i])) {
                            Log("could not save a copy of a page");
                            break;
                        }
                        WriteFileAtomic(cacheDir + L"\\" + CacheName(i, L".meta"), metas[i].etag + "\n" + metas[i].lastModified + "\n");
                    }
                    Log("updated from the website");
                }
            } else if (!failed && !changed) {
                Log("the website pages are unchanged");
            }
            if (cancelled || WaitForSingleObject(stop, kRecheckMs) == WAIT_OBJECT_0 || cancelled) return;
        }
    }
};

namespace {
DWORD WINAPI UpdaterThread(LPVOID parameter) {
    static_cast<WebUniquesUpdaterState*>(parameter)->Run();
    return 0;
}
}  // namespace

WebUniquesUpdater::WebUniquesUpdater() : state_(std::make_unique<WebUniquesUpdaterState>()) {}

WebUniquesUpdater::~WebUniquesUpdater() { Stop(); }

void WebUniquesUpdater::Start(const std::wstring& cacheDir, const std::wstring& siteUrl, std::string shippedWikiData, std::string artTable,
                              std::function<void(const std::string&)> apply, std::function<void(const std::string&)> log) {
    if (!state_ || state_->thread != nullptr) return;
    state_->cacheDir = cacheDir;
    state_->siteUrl = siteUrl;
    state_->shipped = std::move(shippedWikiData);
    state_->artTable = std::move(artTable);
    state_->apply = std::move(apply);
    state_->log = std::move(log);
    // A file with this name next to the data turns the website check off.
    if (GetFileAttributesW((cacheDir + L"\\" + kOptOutFile).c_str()) != INVALID_FILE_ATTRIBUTES) {
        state_->Log("website updates are turned off (uniques-live-disabled.txt)");
        return;
    }
    state_->stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (state_->stop == nullptr) return;
    state_->thread = CreateThread(nullptr, 0, UpdaterThread, state_.get(), 0, nullptr);
}

void WebUniquesUpdater::Stop() {
    if (!state_) return;  // abandoned during a slow download
    if (state_->thread != nullptr) {
        state_->cancelled = true;
        SetEvent(state_->stop);
        if (WaitForSingleObject(state_->thread, 3000) != WAIT_OBJECT_0) {
            // Still inside a slow download. Leave its state allocated; it ends on its own and hands nothing on.
            CloseHandle(state_->thread);
            state_->thread = nullptr;
            (void)state_.release();
            return;
        }
        CloseHandle(state_->thread);
        state_->thread = nullptr;
    }
    if (state_->stop != nullptr) {
        CloseHandle(state_->stop);
        state_->stop = nullptr;
    }
}

}  // namespace esrwiki
