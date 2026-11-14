#pragma once
/*
 * SwitchSplash config access for the overlay. Mirrors the sysmodule's keys, defaults and
 * layering (source/config.c + main.c) so the overlay shows what a launch will actually use:
 *   game:   config.ini -> default/config.ini (if the default image is shown) -> <TID>/config.ini
 *   hbmenu: config.ini -> hbl_duration_ms as duration -> hbl/config.ini
 */
#include <switch.h>

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace sc {

inline constexpr const char *ROOT = "sdmc:/config/bootsplash";
inline constexpr u64 SYSMODULE_ID = 0x420000000000B5A5ULL;

/* Minimal line-preserving INI editor (comments and unknown keys survive). Sections are
 * ignored, as in the sysmodule. */
class IniFile {
public:
    explicit IniFile(std::string path);
    bool exists() const { return m_exists; }
    std::optional<std::string> get(const std::string &key) const;
    void set(const std::string &key, const std::string &value);
    void remove(const std::string &key);
    bool save() const; /* creates the folder if needed */

private:
    std::string m_path;
    std::vector<std::string> m_lines;
    bool m_exists = false;
};

enum class ScopeKind { Global, Default, Hbl, Title };

struct Scope {
    ScopeKind kind;
    u64 tid = 0;

    std::string folder() const;     /* sdmc:/config/bootsplash[/<x>] */
    std::string configPath() const; /* <folder>/config.ini */
    std::string label() const;      /* for headers */
};

struct Choice {
    std::string value, label;
};

struct Setting {
    const char *key;
    const char *label;
    const char *def;
    std::vector<Choice> choices;
    bool globalOnly;
};

const std::vector<Setting> &settings();
std::string displayValue(const Setting &s, const std::string &value);

/* Value a launch in `scope` would use, and whether `scope`'s own config.ini sets it.
 * With includeOwn=false, the value it would inherit if its own config.ini didn't set it. */
struct Resolved {
    std::string value;
    bool setHere;
};
Resolved resolve(const Scope &scope, const Setting &s, bool includeOwn = true);

/* Writes (or, with nullopt, removes) a key in the scope's own config.ini. */
bool writeSetting(const Scope &scope, const Setting &s, const std::optional<std::string> &value);

/* Folder contents. */
struct FolderInfo {
    bool png, wav, ini, cache;
};
FolderInfo folderInfo(const Scope &scope);
bool resetScope(const Scope &scope);      /* deletes the scope's config.ini */
bool clearCache(const Scope &scope);      /* deletes splash.cache */
int clearAllCaches();

/* Title-ID folders under the root, sorted by name. */
std::vector<u64> titleFolders();

/* Running application's program ID, if any. */
std::optional<u64> runningApplication();

/* Game name from ns (cached), or the hex ID if unavailable. */
std::string titleName(u64 tid);

/* Sysmodule control. */
bool sysmoduleRunning();
bool sysmoduleSetRunning(bool run);
bool sysmoduleBootEnabled();
bool sysmoduleSetBoot(bool enabled);

std::string hexId(u64 tid);

} // namespace sc
