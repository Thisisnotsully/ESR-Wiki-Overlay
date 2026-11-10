// ---------------------------------------------------------------------------------------------------------------
// The official Maps page on the Campaign Progression map. The plugin reads the page in the loader's language (the
// copy shipped in locales\<code>\maps.htm or maps.htm first, then the website's live page), matches its rows to the
// map's areas (campaign_live.hpp) and hands the result over with Overlay::SetMapsPageData. With it:
//   - a Special areas card that the page lists shows the page's size and location lines instead of the map's English
//     "how to get in" text;
//   - an area with a Teleport Restrictions row shows that row's text among its caveats (in place of the English
//     "No teleport across the walls");
//   - under the Special areas tab, the page's closing note replaces the map's warning notes taken from it, and rows or
//     cards of the page that match no area are listed, so nothing the page says is lost.
// The text is shown exactly as the page writes it. Without the page (or before it arrives) the map's own text shows.
// Included by overlay.cpp after overlay_campaign.inc.
// ---------------------------------------------------------------------------------------------------------------

void Overlay::SetMapsPageData(std::shared_ptr<const campaign::LiveMaps> data) {
    if (!impl_ || !data) return;
    {
        std::lock_guard<std::mutex> guard(impl_->mapsLock);
        impl_->mapsPending = std::move(data);
    }
    // Adopted on the UI thread (ApplyMapsData); a window not made yet adopts it at its first layout of the map.
    if (impl_->window != nullptr) PostMessageW(impl_->window, kMsgMapsData, 0, 0);
}

bool OverlayState::AdoptMapsData() {
    std::shared_ptr<const campaign::LiveMaps> next;
    {
        std::lock_guard<std::mutex> guard(mapsLock);
        next = std::move(mapsPending);
        mapsPending.reset();
    }
    if (!next) return false;
    mapsData = std::move(next);
    return true;
}

void OverlayState::ApplyMapsData() {
    if (!AdoptMapsData()) return;
    if (window != nullptr) InvalidateRect(window, nullptr, FALSE);  // the next paint lays the map out again
}

std::wstring OverlayState::CampaignLiveCardEntry(const campaign::Tab& tab, const campaign::Area& area) const {
    if (mapsData && tab.key == "special") {
        const auto it = mapsData->cardOf.find(area.id);
        if (it != mapsData->cardOf.end() && it->second < mapsData->page.cards.size()) {
            const std::string text = campaign::MapsCardText(mapsData->page.cards[it->second]);
            if (!text.empty()) return Wide(text);
        }
    }
    return Wide(area.entry);
}

bool OverlayState::CampaignLiveTeleport(const campaign::Area& area, std::vector<std::wstring>& out) const {
    if (!mapsData) return false;
    const auto it = mapsData->teleportOf.find(area.id);
    if (it == mapsData->teleportOf.end() || it->second >= mapsData->page.teleport.size()) return false;
    const std::string& text = mapsData->page.teleport[it->second].text;
    if (text.empty()) return false;
    out.push_back(Wide(text));
    return true;
}

bool OverlayState::CampaignLiveHidesNote(const campaign::Tab& tab, const campaign::Note& note) const {
    // The Special areas tab's warnings from the official page ('O' or 'o') paraphrase the page's closing note, which
    // is shown instead (RenderCampaignLiveNotes).
    return mapsData && tab.key == "special" && !mapsData->note.empty() && note.warning &&
           (note.source == 'O' || note.source == 'o');
}

int OverlayState::RenderCampaignLiveNotes(HDC dc, const campaign::Tab& tab, int left, int paneW, int y, bool draw) {
    if (!mapsData || tab.key != "special") return y;
    const campaign::LiveMaps& live = *mapsData;
    const auto line = [&](const std::wstring& text, COLORREF color) {
        if (text.empty()) return;
        const int h = MeasureHeight(dc, text, paneW, fontSnippet);
        if (draw && y + h >= clipTop && y <= clipBottom)
            Text(dc, text, Rect(left, y, left + paneW, y + h), color, fontSnippet, DT_LEFT | DT_WORDBREAK);
        y += h + S(2);
    };
    const std::string note = campaign::MapsNoteText(live.note);
    if (!note.empty()) line(L"\u2022 " + Wide(note), kCampaignOrange);
    // Rows and cards of the page that match no area of the map, in the page's words.
    for (const std::size_t i : live.unmatchedCards) {
        if (i >= live.page.cards.size()) continue;
        const mapspage::SpecialCard& card = live.page.cards[i];
        std::wstring text = L"\u2022 " + Wide(card.name);
        for (const std::string* part : {&card.size, &card.location})
            if (!part->empty()) text += L" \u00B7 " + Wide(*part);
        line(text, kDim);
    }
    for (const std::size_t i : live.unmatchedTeleport) {
        if (i >= live.page.teleport.size()) continue;
        const mapspage::TeleportRow& row = live.page.teleport[i];
        std::wstring text = L"\u2022 ";
        if (!live.page.teleportTitle.empty()) text += Wide(live.page.teleportTitle) + L": ";
        text += Wide(row.area);
        if (!row.text.empty()) text += L" \u2014 " + Wide(row.text);
        line(text, kCampaignOrange);
    }
    return y;
}
