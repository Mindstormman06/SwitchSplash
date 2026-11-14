/*
 * SwitchSplash overlay (libultrahand): edit SwitchSplash's config.ini files on the fly.
 * The sysmodule re-reads every config on each launch, so changes apply to the next game
 * launch, no reboot needed.
 */
#define TESLA_INIT_IMPL
#include <tesla.hpp>

#include "splash_cfg.hpp"

using namespace sc;

static std::string filesSummary(const FolderInfo &fi) {
    std::string s;
    auto add = [&](bool on, const char *name) {
        if (!on)
            return;
        if (!s.empty())
            s += " · ";
        s += name;
    };
    add(fi.png, "image");
    add(fi.wav, "jingle");
    add(fi.ini, "config");
    return s.empty() ? "no files" : s;
}

/* List marker: "*" when the folder has its own config.ini (settings changed from the
 * inherited ones), nothing otherwise. */
static std::string modifiedMark(const Scope &scope) {
    return folderInfo(scope).ini ? "*" : "";
}

class SettingsGui;

/* ---------------- choice list for one setting ---------------- */

class ChoiceGui : public tsl::Gui {
public:
    ChoiceGui(SettingsGui *parent, Scope scope, const Setting *setting)
        : m_parent(parent), m_scope(scope), m_setting(setting) {}

    tsl::elm::Element *createUI() override;

private:
    void pick(const std::optional<std::string> &value);

    SettingsGui *m_parent;
    Scope m_scope;
    const Setting *m_setting;
};

/* ---------------- settings for one scope ---------------- */

class SettingsGui : public tsl::Gui {
public:
    explicit SettingsGui(Scope scope) : m_scope(scope) {}

    tsl::elm::Element *createUI() override {
        std::string subtitle = m_scope.kind == ScopeKind::Title ? hexId(m_scope.tid) : "Applies on next launch";
        auto *frame = new tsl::elm::OverlayFrame(m_scope.label(), subtitle);
        auto *list = new tsl::elm::List();

        list->addItem(new tsl::elm::CategoryHeader(
            m_scope.kind == ScopeKind::Global ? "Settings" : "Settings (faint = inherited)"));
        for (const auto &s : settings()) {
            if (s.globalOnly && m_scope.kind != ScopeKind::Global)
                continue;
            auto *item = new tsl::elm::ListItem(s.label);
            const Setting *sp = &s;
            item->setClickListener([this, sp](u64 keys) {
                if (keys & HidNpadButton_A) {
                    tsl::changeTo<ChoiceGui>(this, m_scope, sp);
                    return true;
                }
                return false;
            });
            m_items.push_back({sp, item});
            list->addItem(item);
        }

        if (m_scope.kind != ScopeKind::Global) {
            list->addItem(new tsl::elm::CategoryHeader("Files"));
            m_files = new tsl::elm::ListItem("In this folder");
            list->addItem(m_files);

            auto *reset = new tsl::elm::ListItem("Reset to inherited");
            reset->setClickListener([this, reset](u64 keys) {
                if (keys & HidNpadButton_A) {
                    reset->setValue(resetScope(m_scope) ? "Done" : "Nothing to reset", true);
                    refresh();
                    return true;
                }
                return false;
            });
            list->addItem(reset);

            auto *cache = new tsl::elm::ListItem("Clear image cache");
            cache->setClickListener([this, cache](u64 keys) {
                if (keys & HidNpadButton_A) {
                    cache->setValue(clearCache(m_scope) ? "Cleared" : "No cache", true);
                    refresh();
                    return true;
                }
                return false;
            });
            list->addItem(cache);
        }

        refresh();
        frame->setContent(list);
        return frame;
    }

    void refresh() {
        for (auto &[s, item] : m_items) {
            const Resolved r = resolve(m_scope, *s);
            item->setValue(displayValue(*s, r.value), !r.setHere && m_scope.kind != ScopeKind::Global);
        }
        if (m_files)
            m_files->setValue(filesSummary(folderInfo(m_scope)), true);
    }

private:
    Scope m_scope;
    std::vector<std::pair<const Setting *, tsl::elm::ListItem *>> m_items;
    tsl::elm::ListItem *m_files = nullptr;
};

tsl::elm::Element *ChoiceGui::createUI() {
    auto *frame = new tsl::elm::OverlayFrame(m_setting->label, m_scope.label());
    auto *list = new tsl::elm::List();

    const Resolved r = resolve(m_scope, *m_setting);

    if (m_scope.kind != ScopeKind::Global) {
        /* What the value would be without this folder's own setting. */
        const std::string inherited = resolve(m_scope, *m_setting, false).value;
        auto *item = new tsl::elm::ListItem("Inherit", displayValue(*m_setting, inherited));
        if (!r.setHere)
            item->setValue(ult::CHECKMARK_SYMBOL);
        item->setClickListener([this](u64 keys) {
            if (keys & HidNpadButton_A) {
                pick(std::nullopt);
                return true;
            }
            return false;
        });
        list->addItem(item);
    }

    for (const auto &c : m_setting->choices) {
        auto *item = new tsl::elm::ListItem(displayValue(*m_setting, c.value));
        if (r.setHere || m_scope.kind == ScopeKind::Global) {
            if (c.value == r.value)
                item->setValue(ult::CHECKMARK_SYMBOL);
        }
        const std::string v = c.value;
        item->setClickListener([this, v](u64 keys) {
            if (keys & HidNpadButton_A) {
                pick(v);
                return true;
            }
            return false;
        });
        list->addItem(item);
    }

    frame->setContent(list);
    return frame;
}

void ChoiceGui::pick(const std::optional<std::string> &value) {
    writeSetting(m_scope, *m_setting, value);
    m_parent->refresh(); /* parent stays alive underneath; refresh before popping this page */
    tsl::goBack();
    /* nothing may touch `this` after goBack */
}

/* ---------------- main menu ---------------- */

class MainGui : public tsl::Gui {
public:
    tsl::elm::Element *createUI() override {
        auto *frame = new tsl::elm::OverlayFrame("SwitchSplash", "v" APP_VERSION);
        auto *list = new tsl::elm::List();

        auto open = [](tsl::elm::ListItem *item, Scope scope) {
            item->setClickListener([scope](u64 keys) {
                if (keys & HidNpadButton_A) {
                    tsl::changeTo<SettingsGui>(scope);
                    return true;
                }
                return false;
            });
        };

        list->addItem(new tsl::elm::CategoryHeader("Running game"));
        if (auto tid = runningApplication()) {
            const Scope scope{ScopeKind::Title, *tid};
            auto *item = new tsl::elm::ListItem(titleName(*tid), modifiedMark(scope));
            open(item, scope);
            list->addItem(item);
        } else {
            list->addItem(new tsl::elm::ListItem("No game running"));
        }

        list->addItem(new tsl::elm::CategoryHeader("Settings"));
        {
            auto *g = new tsl::elm::ListItem("All games (global)");
            open(g, Scope{ScopeKind::Global});
            list->addItem(g);
            const Scope def{ScopeKind::Default}, hbl{ScopeKind::Hbl};
            auto *d = new tsl::elm::ListItem("Default splash", modifiedMark(def));
            open(d, def);
            list->addItem(d);
            auto *h = new tsl::elm::ListItem("hbmenu (hbl)", modifiedMark(hbl));
            open(h, hbl);
            list->addItem(h);
        }

        const auto titles = titleFolders();
        list->addItem(new tsl::elm::CategoryHeader("Games (" + std::to_string(titles.size()) + ")"));
        for (u64 tid : titles) {
            const Scope scope{ScopeKind::Title, tid};
            auto *item = new tsl::elm::ListItem(titleName(tid), modifiedMark(scope));
            open(item, scope);
            list->addItem(item);
        }

        list->addItem(new tsl::elm::CategoryHeader("Sysmodule"));
        auto *running = new tsl::elm::ToggleListItem("Running", sysmoduleRunning());
        running->setStateChangedListener([running](bool on) {
            if (!sysmoduleSetRunning(on))
                running->setState(sysmoduleRunning());
        });
        list->addItem(running);
        auto *boot = new tsl::elm::ToggleListItem("Start at boot", sysmoduleBootEnabled());
        boot->setStateChangedListener([boot](bool on) {
            if (!sysmoduleSetBoot(on))
                boot->setState(sysmoduleBootEnabled());
        });
        list->addItem(boot);
        auto *clear = new tsl::elm::ListItem("Clear all image caches");
        clear->setClickListener([clear](u64 keys) {
            if (keys & HidNpadButton_A) {
                clear->setValue("Cleared " + std::to_string(clearAllCaches()), true);
                return true;
            }
            return false;
        });
        list->addItem(clear);

        frame->setContent(list);
        return frame;
    }
};

/* ---------------- overlay ---------------- */

class SplashOverlay : public tsl::Overlay {
public:
    /* libultrahand already initialises fs/sdmc, hid, pmdmnt, hid:sys and set:sys. */
    void initServices() override {
        pminfoInitialize();
        pmshellInitialize();
        nsInitialize();
    }
    void exitServices() override {
        nsExit();
        pmshellExit();
        pminfoExit();
    }
    std::unique_ptr<tsl::Gui> loadInitialGui() override { return initially<MainGui>(); }
};

int main(int argc, char **argv) {
    return tsl::loop<SplashOverlay>(argc, argv);
}
