// Keeps the Uniques page in step with the unique item pages on the ESR website.
//
// A background thread reads the pages it saved last time, then asks the site for newer ones (conditional
// requests, so nothing is downloaded when a page is unchanged) and again every few hours while the game runs.
// Whenever the four pages are usable they are turned into wiki data by BuildWebUniquesWikiData and handed to
// `apply`. A failed download or a page that cannot be read leaves the data as it was.
#pragma once

#include <functional>
#include <memory>
#include <string>

namespace esrwiki {

struct WebUniquesUpdaterState;

class WebUniquesUpdater {
public:
    WebUniquesUpdater();
    ~WebUniquesUpdater();
    WebUniquesUpdater(const WebUniquesUpdater&) = delete;
    WebUniquesUpdater& operator=(const WebUniquesUpdater&) = delete;

    // `cacheDir` holds the saved copies of the pages. `siteUrl` ends with a slash. `shippedWikiData` is the
    // wiki-data.txt shipped with the plugin, which the new page is merged into, and `artTable` the
    // web-unique-art.txt shipped with it (art, tier and weapon family the website does not give). `apply` runs on the updater's
    // thread with the whole new wiki data. `log` reports what happened, one line at a time.
    void Start(const std::wstring& cacheDir, const std::wstring& siteUrl, std::string shippedWikiData, std::string artTable,
               std::function<void(const std::string& wikiData)> apply, std::function<void(const std::string& message)> log);
    void Stop();

private:
    std::unique_ptr<WebUniquesUpdaterState> state_;
};

}  // namespace esrwiki
