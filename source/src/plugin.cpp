// D2RLoader client plugin entry points for the ESR wiki overlay. Mirrors the
// D2RR Item Database plugin: load the data, start the overlay, and register the
// bindable action (unbound by default) and an `eswiki` console command.
#include <D2RLPlugin/api.h>

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <cstdio>
#include <cstring>
#include <exception>
#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>

#include "overlay.hpp"
#include "wiki_model.hpp"

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace {
using namespace D2RL;

constexpr D2RL::PluginInfo EsrOverlayPluginInfo {
    .infoSize = D2RL::PluginInfoSize,
    .abiVersion = D2RL_PLUGIN_ABI_VERSION,
    .id = "esr-wiki-overlay",
    .name = "ESR Wiki Overlay",
    .version = "1.1.2",
    .author = "Sully",
    .description = "Read-only ESR wiki reference overlay. Open it with the eswiki command, or a key bound in the controls.",
    .flags = D2RL::PluginFlags::Client,
};

const D2RL::PluginContext* pluginContext = nullptr;
const D2RL::InputService* input = nullptr;
D2RL::Input::ActionHandle openAction = D2RL::Input::InvalidHandle;
std::unique_ptr<esrwiki::Database> database;
std::unique_ptr<esrwiki::Overlay> overlay;
std::unique_ptr<esrwiki::Database> cubeDatabase;   // Horadric Cube recipes (cube-wiki-data.txt)
std::unique_ptr<esrwiki::Overlay> cubeOverlay;
bool lastOpenedCube = false;  // which panel was opened most recently; the wiki key reopens it

// Folder that holds this DLL. The wiki data sits in a folder next to it.
std::wstring PluginDirectory() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(reinterpret_cast<HMODULE>(&__ImageBase), path, MAX_PATH);
    const std::wstring full(path);
    const auto slash = full.find_last_of(L"\\/");
    return slash == std::wstring::npos ? full : full.substr(0, slash);
}

std::string ReadWholeFile(const std::wstring& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("cannot open wiki-data.txt next to the plugin");
    std::ostringstream content;
    content << file.rdbuf();
    return content.str();
}

// Toggles the panel that was opened most recently (the main wiki unless cube recipes was last).
void toggleLastPanel() {
    if (lastOpenedCube && cubeOverlay != nullptr) {
        cubeOverlay->Toggle();
    } else if (overlay != nullptr) {
        lastOpenedCube = false;
        overlay->Toggle();
    }
}

D2RL::Input::ActionResult __cdecl onOpenAction(const D2RL::PluginContext* plugin,
                                                const D2RL::Input::ActionEvent* event,
                                                void*) noexcept {
    if (plugin == nullptr || overlay == nullptr ||
        !D2RL::Input::HasActionEventField(event, D2RL::Input::ActionEventRequiredSize) ||
        event->kind != D2RL::Input::ActionEventKind::Pressed) {
        return D2RL::Input::ActionResult::Ignored;
    }
    toggleLastPanel();
    return D2RL::Input::ActionResult::Handled;
}

// The key bound to the open action, read from the game's controls. Game actions do not fire while
// the overlay's search box has focus, so the overlay closes itself on the same key.
bool matchesOpenKey(unsigned int virtualKey) {
    if (pluginContext == nullptr || input == nullptr || openAction == D2RL::Input::InvalidHandle) return false;
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool control = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
    for (auto slot : {D2RL::Input::BindingSlot::Primary, D2RL::Input::BindingSlot::Secondary}) {
        D2RL::Input::Binding binding{};
        if (input->getBinding(pluginContext, openAction, slot, &binding) != D2RL::Input::Result::Success) continue;
        if (static_cast<unsigned int>(binding.key) != virtualKey) continue;
        switch (binding.modifier) {
            case D2RL::Input::Modifier::None: if (!shift && !control && !alt) return true; break;
            case D2RL::Input::Modifier::Shift: if (shift && !control && !alt) return true; break;
            case D2RL::Input::Modifier::Control: if (control && !shift && !alt) return true; break;
            case D2RL::Input::Modifier::Alt: if (alt && !shift && !control) return true; break;
        }
    }
    return false;
}

D2RL::ConsoleCommandResult toggleCommand(D2R::Game::Client*, const D2RL::ConsoleCommandContext* command, void*) noexcept {
    if (command == nullptr || command->plugin == nullptr || overlay == nullptr)
        return D2RL::ConsoleCommandResult::Failed;
    toggleLastPanel();
    return D2RL::ConsoleCommandResult::Handled;
}

// ---- Pause-menu button ------------------------------------------------------
// The "ESR Wiki" button on the bare pause menu. Its click sends
// PanelManager:OpenPanel:esr-wiki-overlay/OpenOverlay, which the listener below
// turns into Open(). The button is shown only while PauseLayoutGarden is open,
// and hidden while a vanilla pause sub-panel covers it.
const PanelService* panels = nullptr;
const SharedEventService* events = nullptr;
const ThreadService* threads = nullptr;
const ResourceService* resources = nullptr;
Panels::RegistrationHandle pauseButtonPanel = Panels::InvalidHandle;
Panels::RegistrationHandle openSelectorPanel = Panels::InvalidHandle;
SharedEvents::ListenerHandle messageHandle = SharedEvents::InvalidHandle;
bool pauseMenuOpen = false;
char obscuringPanel[64] = {};

constexpr const char* kPauseMenuPanel = "PauseLayoutGarden";
constexpr const char* kPauseButtonPanel = "esr-wiki-overlay/PauseButton";
constexpr const char* kOpenSelectorPanel = "esr-wiki-overlay/OpenOverlay";
// Vanilla pause sub-panels that cover the pause screen. The button hides while one is open.
constexpr const char* kObscuringPanels[] = {"SettingsPanel", "ChroniclePanel", "LootFilterOptionsPanel"};

constexpr char kPauseButtonLayout[] = R"JSON({
    "type": "Panel", "name": "esr-wiki-overlay/PauseButton",
    "fields": { "priority": 9500, "fitToParent": true },
    "children": [ {
        "type": "TableWidget", "name": "EsrWikiButtonTable",
        "fields": {
            "anchor": { "x": 0.5, "y": 0.5 },
            "rect": { "x": -329, "y": 395 },
            "columns": [ { "width": 658, "alignment": { "h": "center", "v": "center" } } ],
            "rowHeight": 140,
            "generateFocusLinks": true
        },
        "children": [ {
            "type": "TableRowWidget", "name": "Row ESR Wiki",
            "children": [ {
                "type": "ButtonWidget", "name": "OpenWikiButton",
                "fields": {
                    "filename": "PauseMenu\\PauseButton",
                    "hoveredFrame": 3,
                    "textString": "ESR Wiki",
                    "onClickMessage": "PanelManager:OpenPanel:esr-wiki-overlay/OpenOverlay",
                    "textColor": "$LightButtonTextColor",
                    "text/style": "$StyleFEButtonText",
                    "acceptsReturnKey": true,
                    "focusOnMouseOver": true,
                    "sound": "select"
                }
            } ]
        } ]
    } ]
})JSON";

constexpr char kSelectorLayout[] =
    R"JSON({"type":"Panel","name":"esr-wiki-overlay/OpenOverlay","fields":{"priority":1,"rect":{"x":0,"y":0,"width":1,"height":1}},"children":[]})JSON";

// Registers a layout from memory and a panel that uses it. Plugin layouts need
// their local id prefixed by the plugin id, so these names are esr-wiki-overlay/<id>.
bool RegisterPanelLayout(const PluginContext* plugin, const char* path, const char* json, const char* localId,
                         Panels::RegistrationHandle* handle) {
    const Resources::ResourceRegistration resource{Resources::ResourceRegistrationSize, 0, path, json, std::strlen(json)};
    Resources::RegistrationHandle resourceHandle = Resources::InvalidHandle;
    if (resources->registerResource(plugin, &resource, &resourceHandle) != Resources::Result::Success) return false;
    const Panels::PanelRegistration registration{Panels::PanelRegistrationSize, Panels::PanelFlags::None, localId};
    return panels->registerPanel(plugin, &registration, handle) == Panels::Result::Success;
}

// Panel open/close must run from a UI callback, so these are queued with runOnUiThread.
void __cdecl DoShowPauseButton(const PluginContext* plugin, void*) noexcept {
    if (plugin == nullptr || panels == nullptr || pauseButtonPanel == Panels::InvalidHandle) return;
    const Panels::Result result = panels->openPanel(plugin, pauseButtonPanel);
    if (result != Panels::Result::Success && result != Panels::Result::Conflict)
        plugin->LogError("ESR Wiki: could not show the pause button.");
}

void __cdecl DoHidePauseButton(const PluginContext* plugin, void*) noexcept {
    if (plugin == nullptr || panels == nullptr || pauseButtonPanel == Panels::InvalidHandle) return;
    const Panels::Result result = panels->closePanel(plugin, pauseButtonPanel);
    if (result != Panels::Result::Success && result != Panels::Result::Conflict)
        plugin->LogError("ESR Wiki: could not hide the pause button.");
}

// The selector is 1x1 and invisible, so close it right after it has been used.
void __cdecl DoCloseOpenSelector(const PluginContext* plugin, void*) noexcept {
    if (plugin == nullptr || panels == nullptr || openSelectorPanel == Panels::InvalidHandle) return;
    panels->closePanel(plugin, openSelectorPanel);
}

// ---- Horadric Cube recipes button -------------------------------------------
// The "Recipes" button on the Horadric Cube panel. It is shown while the native
// HoradricCubeLayout panel is open and opens the cube recipes overlay.
Panels::RegistrationHandle cubeButtonPanel = Panels::InvalidHandle;
Panels::RegistrationHandle openCubeSelectorPanel = Panels::InvalidHandle;
bool cubeOpen = false;

constexpr const char* kCubePanelName = "HoradricCubeLayout";
constexpr const char* kCubeButtonPanel = "esr-wiki-overlay/HoradricCubeButton";
constexpr const char* kOpenCubeSelectorPanel = "esr-wiki-overlay/OpenCubeOverlay";

constexpr char kCubeButtonLayout[] = R"JSON({
    "type": "Panel", "name": "esr-wiki-overlay/HoradricCubeButton",
    "fields": { "priority": 9500, "rect": "$LeftPanelRectS", "anchor": "$LeftPanelAnchor" },
    "children": [ {
        "type": "ButtonWidget", "name": "recipes",
        "fields": {
            "rect": { "x": 1502, "y": 1518, "scale": 1.6 },
            "filename": "panel\\hud_02\\questlog",
            "tooltipString": "Recipes",
            "hoveredFrame": 2,
            "onClickMessage": "PanelManager:OpenPanel:esr-wiki-overlay/OpenCubeOverlay",
            "frame": 5
        }
    } ]
})JSON";

constexpr char kCubeSelectorLayout[] =
    R"JSON({"type":"Panel","name":"esr-wiki-overlay/OpenCubeOverlay","fields":{"priority":1,"rect":{"x":0,"y":0,"width":1,"height":1}},"children":[]})JSON";

void __cdecl DoShowCubeButton(const PluginContext* plugin, void*) noexcept {
    if (plugin == nullptr || panels == nullptr || cubeButtonPanel == Panels::InvalidHandle) return;
    const Panels::Result result = panels->openPanel(plugin, cubeButtonPanel);
    if (result != Panels::Result::Success && result != Panels::Result::Conflict)
        plugin->LogError("ESR Wiki: could not show the cube recipes button.");
}

void __cdecl DoHideCubeButton(const PluginContext* plugin, void*) noexcept {
    if (plugin == nullptr || panels == nullptr || cubeButtonPanel == Panels::InvalidHandle) return;
    const Panels::Result result = panels->closePanel(plugin, cubeButtonPanel);
    if (result != Panels::Result::Success && result != Panels::Result::Conflict)
        plugin->LogError("ESR Wiki: could not hide the cube recipes button.");
}

void __cdecl DoCloseCubeSelector(const PluginContext* plugin, void*) noexcept {
    if (plugin == nullptr || panels == nullptr || openCubeSelectorPanel == Panels::InvalidHandle) return;
    panels->closePanel(plugin, openCubeSelectorPanel);
}

bool IsObscuringPanel(const char* text) noexcept {
    for (const char* name : kObscuringPanels) {
        if (std::strcmp(text, name) == 0) return true;
    }
    return false;
}

SharedEvents::UiMessageAction __cdecl OnUiMessage(const PluginContext* plugin, const SharedEvents::UiMessageEvent* event,
                                                  void*) noexcept {
    if (plugin == nullptr || event == nullptr || event->structSize < SharedEvents::UiMessageEventRequiredSize ||
        threads == nullptr) {
        return SharedEvents::UiMessageAction::Continue;
    }
    const char* target = event->target != nullptr ? event->target : "";
    const char* command = event->command != nullptr ? event->command : "";
    const char* text = event->text != nullptr ? event->text : "";
    if (std::strcmp(target, "PanelManager") != 0) return SharedEvents::UiMessageAction::Continue;

    if (std::strcmp(command, "OpenPanel") == 0 && std::strcmp(text, kOpenSelectorPanel) == 0) {
        if (threads->runOnUiThread(plugin, DoCloseOpenSelector, nullptr) != Threads::Result::Success)
            plugin->LogError("ESR Wiki: failed to queue closing the selector panel.");
        lastOpenedCube = false;
        if (overlay != nullptr) overlay->Open();
        return SharedEvents::UiMessageAction::Continue;
    }

    if (std::strcmp(command, "OpenPanel") == 0 && std::strcmp(text, kOpenCubeSelectorPanel) == 0) {
        if (threads->runOnUiThread(plugin, DoCloseCubeSelector, nullptr) != Threads::Result::Success)
            plugin->LogError("ESR Wiki: failed to queue closing the cube selector panel.");
        lastOpenedCube = true;
        if (cubeOverlay != nullptr) cubeOverlay->Open();
        return SharedEvents::UiMessageAction::Continue;
    }

    if (std::strcmp(text, kCubePanelName) == 0) {
        if (std::strcmp(command, "OpenPanel") == 0) {
            cubeOpen = true;
            if (threads->runOnUiThread(plugin, DoShowCubeButton, nullptr) != Threads::Result::Success)
                plugin->LogError("ESR Wiki: failed to queue showing the cube recipes button.");
        } else if (std::strcmp(command, "ClosePanel") == 0) {
            cubeOpen = false;
            if (threads->runOnUiThread(plugin, DoHideCubeButton, nullptr) != Threads::Result::Success)
                plugin->LogError("ESR Wiki: failed to queue hiding the cube recipes button.");
        }
        return SharedEvents::UiMessageAction::Continue;
    }

    if (std::strcmp(text, kPauseMenuPanel) == 0) {
        if (std::strcmp(command, "OpenPanel") == 0) {
            pauseMenuOpen = true;
            obscuringPanel[0] = '\0';
            if (threads->runOnUiThread(plugin, DoShowPauseButton, nullptr) != Threads::Result::Success)
                plugin->LogError("ESR Wiki: failed to queue showing the pause button.");
        } else if (std::strcmp(command, "ClosePanel") == 0) {
            pauseMenuOpen = false;
            obscuringPanel[0] = '\0';
            if (threads->runOnUiThread(plugin, DoHidePauseButton, nullptr) != Threads::Result::Success)
                plugin->LogError("ESR Wiki: failed to queue hiding the pause button.");
        }
    } else if (pauseMenuOpen && std::strcmp(text, kPauseButtonPanel) != 0 && IsObscuringPanel(text)) {
        if (std::strcmp(command, "OpenPanel") == 0) {
            std::snprintf(obscuringPanel, sizeof(obscuringPanel), "%s", text);
            if (threads->runOnUiThread(plugin, DoHidePauseButton, nullptr) != Threads::Result::Success)
                plugin->LogError("ESR Wiki: failed to queue hiding the pause button.");
        } else if ((std::strcmp(command, "ClosePanel") == 0 || std::strcmp(command, "UnloadPanel") == 0) &&
                   obscuringPanel[0] != '\0' && std::strcmp(text, obscuringPanel) == 0) {
            obscuringPanel[0] = '\0';
            if (threads->runOnUiThread(plugin, DoShowPauseButton, nullptr) != Threads::Result::Success)
                plugin->LogError("ESR Wiki: failed to queue showing the pause button.");
        }
    }
    return SharedEvents::UiMessageAction::Continue;
}

// Optional feature: if the services are missing, log it and leave the overlay usable through the eswiki command.
// Registers the cube button and its selector. Needs the services SetupPauseButton queries.
bool SetupCubeButton(const PluginContext* plugin) {
    if (panels == nullptr || resources == nullptr || threads == nullptr) return false;
    if (!RegisterPanelLayout(plugin, "data/global/ui/layouts/esr-wiki-overlay/HoradricCubeButtonhd.json", kCubeButtonLayout,
                             "HoradricCubeButton", &cubeButtonPanel)) {
        plugin->LogError("ESR Wiki: failed to register the cube recipes button panel.");
        return false;
    }
    if (!RegisterPanelLayout(plugin, "data/global/ui/layouts/esr-wiki-overlay/OpenCubeOverlayhd.json", kCubeSelectorLayout,
                             "OpenCubeOverlay", &openCubeSelectorPanel)) {
        plugin->LogError("ESR Wiki: failed to register the cube selector panel.");
        return false;
    }
    return true;
}

// Optional feature: if the services are missing, log it and leave the overlay usable through the eswiki command.
bool SetupPauseButton(const PluginContext* plugin) {
    if (plugin->QueryService(&panels) != ServiceQueryResult::Success || !HasPanelServiceField(panels, PanelServiceRequiredSize)) {
        panels = nullptr;
        plugin->LogWarn("ESR Wiki pause button unavailable: PanelService missing.");
        return false;
    }
    if (plugin->QueryService(&events) != ServiceQueryResult::Success || !HasSharedEventServiceField(events, SharedEventServiceRequiredSize)) {
        events = nullptr;
        plugin->LogWarn("ESR Wiki pause button unavailable: SharedEventService missing.");
        return false;
    }
    if (plugin->QueryService(&threads) != ServiceQueryResult::Success || !HasThreadServiceField(threads, ThreadServiceRequiredSize)) {
        threads = nullptr;
        plugin->LogWarn("ESR Wiki pause button unavailable: ThreadService missing.");
        return false;
    }
    if (plugin->QueryService(&resources) != ServiceQueryResult::Success || !HasResourceServiceField(resources, ResourceServiceRequiredSize)) {
        resources = nullptr;
        plugin->LogWarn("ESR Wiki pause button unavailable: ResourceService missing.");
        return false;
    }
    if (!RegisterPanelLayout(plugin, "data/global/ui/layouts/esr-wiki-overlay/PauseButtonhd.json", kPauseButtonLayout,
                             "PauseButton", &pauseButtonPanel)) {
        plugin->LogError("ESR Wiki: failed to register the pause-button panel.");
        return false;
    }
    if (!RegisterPanelLayout(plugin, "data/global/ui/layouts/esr-wiki-overlay/OpenOverlayhd.json", kSelectorLayout,
                             "OpenOverlay", &openSelectorPanel)) {
        plugin->LogError("ESR Wiki: failed to register the open-overlay selector panel.");
        return false;
    }
    const SharedEvents::UiMessageListener listener{SharedEvents::UiMessageListenerSize, 0, 0, 0, OnUiMessage, nullptr};
    if (events->registerUiMessageListener(plugin, &listener, &messageHandle) != SharedEvents::Result::Success) {
        plugin->LogError("ESR Wiki: failed to register the UI message listener.");
        return false;
    }
    return true;
}

void Release() noexcept {
    if (cubeOverlay) cubeOverlay->Stop();
    cubeOverlay.reset();
    cubeDatabase.reset();
    if (overlay) overlay->Stop();
    overlay.reset();
    database.reset();
}
}  // namespace

D2RL_PLUGIN_EXPORT auto D2RLoaderGetPluginInfo() noexcept -> const D2RL::PluginInfo* { return &EsrOverlayPluginInfo; }

D2RL_PLUGIN_EXPORT auto D2RLoaderLoadPlugin(const D2RL::PluginContext* plugin) noexcept -> bool {
    try {
        if (!D2RL::HasContext(plugin)) return false;
        pluginContext = plugin;
        plugin->LogInfo("ESR Wiki overlay load started");

        const std::wstring dataDir = PluginDirectory() + L"\\esr-wiki-overlay";
        database = std::make_unique<esrwiki::Database>();
        std::string error;
        if (!esrwiki::ParseDatabase(ReadWholeFile(dataDir + L"\\wiki-data.txt"), *database, error)) {
            const std::string message = "ESR Wiki data rejected: " + error;
            plugin->LogError(message.c_str());
            database.reset();
            return false;
        }

        overlay = std::make_unique<esrwiki::Overlay>();
        overlay->SetCloseKey(matchesOpenKey);
        if (!overlay->Start(*database, dataDir, L"ESR Wiki", L"main", L"Cube Recipes", [] {
                lastOpenedCube = true;
                if (cubeOverlay != nullptr) cubeOverlay->Open();
            })) {
            plugin->LogError("ESR Wiki overlay window could not be created");
            Release();
            return false;
        }

        const std::string loaded = "ESR Wiki overlay ready: " + std::to_string(database->pages.size()) +
                                   " pages, " + std::to_string(database->entryCount()) + " entries";
        plugin->LogInfo(loaded.c_str());

        // Horadric Cube recipes: a second overlay with its own data. Optional, so failures only log.
        try {
            cubeDatabase = std::make_unique<esrwiki::Database>();
            std::string cubeError;
            if (esrwiki::ParseDatabase(ReadWholeFile(dataDir + L"\\cube-wiki-data.txt"), *cubeDatabase, cubeError)) {
                cubeOverlay = std::make_unique<esrwiki::Overlay>();
                cubeOverlay->SetCloseKey(matchesOpenKey);
                const bool started = cubeOverlay->Start(*cubeDatabase, dataDir, L"Horadric Cube Recipes", L"cube",
                                                        L"Main Wiki", [] {
                                                            lastOpenedCube = false;
                                                            if (overlay != nullptr) overlay->Open();
                                                        });
                if (!started) {
                    plugin->LogError("ESR Wiki cube recipes window could not be created");
                    cubeOverlay.reset();
                }
            } else {
                const std::string message = "ESR Wiki cube recipes data rejected: " + cubeError;
                plugin->LogError(message.c_str());
                cubeDatabase.reset();
            }
        } catch (const std::exception& error) {
            const std::string message = std::string("ESR Wiki cube recipes unavailable: ") + error.what();
            plugin->LogError(message.c_str());
            cubeOverlay.reset();
            cubeDatabase.reset();
        }

        if (!plugin->RegisterConsoleCommand("eswiki", toggleCommand, "Open or close the ESR Wiki overlay.")) {
            plugin->LogError("ESR Wiki console command registration failed");
            Release();
            return false;
        }

        if (plugin->QueryService(&input) == D2RL::ServiceQueryResult::Success &&
            D2RL::HasInputServiceField(input, D2RL::InputServiceRequiredSize)) {
            const D2RL::Input::ActionRegistration action{
                .structSize = D2RL::Input::ActionRegistrationSize,
                .logicalId = "open-esr-wiki",
                .displayName = "Open ESR Wiki",
                .category = "ESR Wiki",
                .defaultPrimary = {.key = D2RL::Input::Key::None},  // unbound by default; set a key in the game's controls
                .defaultSecondary = {.key = D2RL::Input::Key::None},
                .callback = onOpenAction,
            };
            if (input->registerAction(plugin, &action, &openAction) != D2RL::Input::Result::Success)
                plugin->LogWarn("ESR Wiki action was not registered; use the eswiki console command");
        } else {
            plugin->LogWarn("ESR Wiki input service unavailable; use the eswiki console command");
        }

        if (!SetupPauseButton(plugin)) plugin->LogWarn("ESR Wiki: use the eswiki command to open the wiki.");
        if (!SetupCubeButton(plugin)) plugin->LogWarn("ESR Wiki: Horadric Cube recipes button unavailable.");
        plugin->LogInfo("ESR Wiki overlay load complete");
        return true;
    } catch (const std::exception& error) {
        if (plugin != nullptr) {
            const std::string message = std::string("ESR Wiki overlay initialization failure: ") + error.what();
            plugin->LogError(message.c_str());
        }
        Release();
        return false;
    } catch (...) {
        if (plugin != nullptr) plugin->LogError("ESR Wiki overlay initialization failure: unknown exception");
        Release();
        return false;
    }
}

D2RL_PLUGIN_EXPORT void D2RLoaderUnloadPlugin() noexcept {
    if (events != nullptr && messageHandle != SharedEvents::InvalidHandle && pluginContext != nullptr) {
        events->unregisterUiMessageListener(pluginContext, messageHandle);
        messageHandle = SharedEvents::InvalidHandle;
    }
    Release();
    if (pluginContext != nullptr) pluginContext->LogInfo("ESR Wiki overlay unloaded");
    pluginContext = nullptr;
}
