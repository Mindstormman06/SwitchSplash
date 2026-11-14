#include "splash_cfg.hpp"

#include <algorithm>
#include <cctype>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>

#include <dirent.h>
#include <sys/stat.h>

namespace sc {

/* ---------------- helpers ---------------- */

static std::string trim(const std::string &s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) a++;
    while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

static bool isFile(const std::string &p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

static void mkdirs(const std::string &path) {
    /* sdmc:/a/b/c -> create each level */
    for (size_t i = std::strlen("sdmc:/"); i <= path.size(); i++) {
        if (i == path.size() || path[i] == '/')
            mkdir(path.substr(0, i).c_str(), 0777);
    }
}

std::string hexId(u64 tid) {
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016" PRIX64, (uint64_t)tid);
    return buf;
}

/* Same key=value parsing as the sysmodule (source/config.c). Returns the key of a line,
 * or "" for comments/sections/blank lines. */
static std::string lineKey(const std::string &line, std::string *valueOut = nullptr) {
    const std::string s = trim(line);
    if (s.empty() || s[0] == ';' || s[0] == '#' || s[0] == '[')
        return "";
    const size_t eq = s.find('=');
    if (eq == std::string::npos)
        return "";
    if (valueOut) {
        std::string v = trim(s.substr(eq + 1));
        for (size_t i = 0; i < v.size(); i++) {
            if (v[i] == ';' || (v[i] == '#' && i > 0 && std::isspace((unsigned char)v[i - 1]))) {
                v = trim(v.substr(0, i));
                break;
            }
        }
        *valueOut = v;
    }
    return trim(s.substr(0, eq));
}

/* ---------------- IniFile ---------------- */

IniFile::IniFile(std::string path) : m_path(std::move(path)) {
    FILE *f = std::fopen(m_path.c_str(), "r");
    if (!f)
        return;
    m_exists = true;
    char buf[512];
    while (std::fgets(buf, sizeof(buf), f)) {
        std::string l(buf);
        while (!l.empty() && (l.back() == '\n' || l.back() == '\r'))
            l.pop_back();
        m_lines.push_back(l);
    }
    std::fclose(f);
}

std::optional<std::string> IniFile::get(const std::string &key) const {
    std::optional<std::string> out;
    for (const auto &l : m_lines) {
        std::string v;
        if (lineKey(l, &v) == key)
            out = v; /* last one wins, like the sysmodule */
    }
    return out;
}

void IniFile::set(const std::string &key, const std::string &value) {
    bool done = false;
    for (auto &l : m_lines) {
        if (lineKey(l) == key) {
            if (!done) {
                l = key + "=" + value;
                done = true;
            } else {
                l = "; " + l; /* neutralise duplicates */
            }
        }
    }
    if (!done)
        m_lines.push_back(key + "=" + value);
}

void IniFile::remove(const std::string &key) {
    m_lines.erase(std::remove_if(m_lines.begin(), m_lines.end(),
                                 [&](const std::string &l) { return lineKey(l) == key; }),
                  m_lines.end());
}

bool IniFile::save() const {
    const size_t slash = m_path.rfind('/');
    if (slash != std::string::npos)
        mkdirs(m_path.substr(0, slash));
    FILE *f = std::fopen(m_path.c_str(), "w");
    if (!f)
        return false;
    for (const auto &l : m_lines)
        std::fprintf(f, "%s\n", l.c_str());
    std::fclose(f);
    return true;
}

/* ---------------- scopes ---------------- */

std::string Scope::folder() const {
    switch (kind) {
        case ScopeKind::Global:  return ROOT;
        case ScopeKind::Default: return std::string(ROOT) + "/default";
        case ScopeKind::Hbl:     return std::string(ROOT) + "/hbl";
        case ScopeKind::Title:   return std::string(ROOT) + "/" + hexId(tid);
    }
    return ROOT;
}

std::string Scope::configPath() const {
    return folder() + "/config.ini";
}

std::string Scope::label() const {
    switch (kind) {
        case ScopeKind::Global:  return "All games (global)";
        case ScopeKind::Default: return "Default splash";
        case ScopeKind::Hbl:     return "hbmenu (hbl)";
        case ScopeKind::Title:   return titleName(tid);
    }
    return "";
}

/* ---------------- settings table ---------------- */

static std::vector<Choice> durationChoices(bool allowJingle) {
    std::vector<Choice> c;
    if (allowJingle)
        c.push_back({"0", "Jingle length"});
    for (int ms : {1000, 1500, 2000, 2500, 3000, 3500, 4000, 5000, 6000, 7000, 8000, 8500, 9000, 10000, 12000, 15000, 20000})
        c.push_back({std::to_string(ms), ""});
    return c;
}

const std::vector<Setting> &settings() {
    static const std::vector<Setting> s = [] {
        std::vector<Setting> v;
        v.push_back({"enabled", "Splash", "1", {{"1", "On"}, {"0", "Off"}}, false});
        v.push_back({"duration_ms", "Duration", "6000", durationChoices(true), false});
        v.push_back({"audio", "Jingle", "1", {{"1", "On"}, {"0", "Off"}}, false});
        std::vector<Choice> vol;
        for (int i = 100; i >= 0; i -= 10)
            vol.push_back({std::to_string(i), std::to_string(i) + "%"});
        v.push_back({"volume", "Jingle volume", "100", vol, false});
        std::vector<Choice> afade, ifade;
        for (int ms : {0, 100, 250, 500, 750, 1000, 1500, 2000, 3000})
            afade.push_back({std::to_string(ms), ""});
        for (int ms : {0, 250, 500, 750, 1000, 1500, 2000})
            ifade.push_back({std::to_string(ms), ""});
        v.push_back({"audio_fade_ms", "Jingle fade-out", "500", afade, false});
        v.push_back({"fade_ms", "Image fade-out", "500", ifade, false});
        v.push_back({"background", "Background", "000000",
                     {{"000000", "Black"}, {"FFFFFF", "White"}, {"202020", "Dark gray"}, {"101830", "Navy"}}, false});
        v.push_back({"hbl_duration_ms", "hbmenu duration", "2500", durationChoices(true), true});
        v.push_back({"skip_hbl", "hbmenu splash", "0", {{"0", "Use hbl/ if present"}, {"1", "Never"}}, true});
        v.push_back({"home_cancel", "HOME cancels splash", "1", {{"1", "On"}, {"0", "Off"}}, true});
        return v;
    }();
    return s;
}

static bool isMsKey(const char *key) {
    return std::strstr(key, "_ms") != nullptr;
}

std::string displayValue(const Setting &s, const std::string &value) {
    for (const auto &c : s.choices)
        if (c.value == value && !c.label.empty())
            return c.label;
    if (isMsKey(s.key)) {
        if (value == "0")
            return std::strcmp(s.key, "duration_ms") == 0 || std::strcmp(s.key, "hbl_duration_ms") == 0
                       ? "Jingle length" : "Off";
        const long ms = std::strtol(value.c_str(), nullptr, 10);
        char buf[32];
        if (ms % 1000 == 0)
            std::snprintf(buf, sizeof(buf), "%ld s", ms / 1000);
        else
            std::snprintf(buf, sizeof(buf), "%.2g s", ms / 1000.0);
        return buf;
    }
    if (std::strcmp(s.key, "background") == 0)
        return "#" + value;
    return value;
}

/* ---------------- resolution ---------------- */

static std::string normalise(const Setting &s, std::string v) {
    /* Accept the same spellings as the sysmodule for booleans / colours. */
    if (s.choices.size() == 2 && (s.choices[0].value == "1" || s.choices[0].value == "0")) {
        const char c = v.empty() ? '0' : v[0];
        return (c == '1' || c == 'y' || c == 'Y' || c == 't' || c == 'T') ? "1" : "0";
    }
    if (std::strcmp(s.key, "background") == 0) {
        if (!v.empty() && v[0] == '#')
            v = v.substr(1);
        for (auto &ch : v)
            ch = (char)std::toupper((unsigned char)ch);
    }
    return v;
}

Resolved resolve(const Scope &scope, const Setting &s, bool includeOwn) {
    std::string value = s.def;
    auto apply = [&](const std::string &path) {
        IniFile f(path);
        if (auto v = f.get(s.key))
            value = normalise(s, *v);
    };

    const Scope global{ScopeKind::Global};
    if (scope.kind == ScopeKind::Global && !includeOwn)
        return {s.def, false};
    apply(global.configPath());

    if (scope.kind == ScopeKind::Hbl && std::strcmp(s.key, "duration_ms") == 0) {
        /* hbmenu launches use hbl_duration_ms as their duration. */
        std::string hbl = "2500";
        if (auto v = IniFile(global.configPath()).get("hbl_duration_ms"))
            hbl = *v;
        value = hbl;
    }

    if (scope.kind == ScopeKind::Title) {
        const bool own = isFile(scope.folder() + "/splash.png");
        const bool def = isFile(std::string(ROOT) + "/default/splash.png");
        if (!own && def)
            apply(Scope{ScopeKind::Default}.configPath());
    }
    bool here = false;
    if (scope.kind != ScopeKind::Global) {
        IniFile f(scope.configPath());
        if (auto v = f.get(s.key)) {
            here = true;
            if (includeOwn)
                value = normalise(s, *v);
        }
    } else {
        here = IniFile(global.configPath()).get(s.key).has_value();
    }
    return {value, here};
}

bool writeSetting(const Scope &scope, const Setting &s, const std::optional<std::string> &value) {
    IniFile f(scope.configPath());
    if (value)
        f.set(s.key, *value);
    else
        f.remove(s.key);
    return f.save();
}

/* ---------------- folders ---------------- */

FolderInfo folderInfo(const Scope &scope) {
    const std::string d = scope.folder();
    return {isFile(d + "/splash.png"), isFile(d + "/splash.wav"), isFile(d + "/config.ini"),
            isFile(d + "/splash.cache")};
}

bool resetScope(const Scope &scope) {
    if (scope.kind == ScopeKind::Global)
        return false;
    return std::remove(scope.configPath().c_str()) == 0;
}

bool clearCache(const Scope &scope) {
    return std::remove((scope.folder() + "/splash.cache").c_str()) == 0;
}

int clearAllCaches() {
    int n = 0;
    if (clearCache(Scope{ScopeKind::Default})) n++;
    if (clearCache(Scope{ScopeKind::Hbl})) n++;
    for (u64 t : titleFolders())
        if (clearCache(Scope{ScopeKind::Title, t})) n++;
    return n;
}

std::vector<u64> titleFolders() {
    std::vector<u64> out;
    DIR *d = opendir(ROOT);
    if (!d)
        return out;
    while (struct dirent *e = readdir(d)) {
        const char *n = e->d_name;
        if (std::strlen(n) != 16)
            continue;
        bool hex = true;
        for (int i = 0; i < 16; i++)
            hex &= std::isxdigit((unsigned char)n[i]) != 0;
        if (!hex)
            continue;
        const std::string full = std::string(ROOT) + "/" + n;
        struct stat st;
        if (stat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
            out.push_back(std::strtoull(n, nullptr, 16));
    }
    closedir(d);
    std::sort(out.begin(), out.end(), [](u64 a, u64 b) { return titleName(a) < titleName(b); });
    return out;
}

std::optional<u64> runningApplication() {
    u64 pid = 0, tid = 0;
    if (R_FAILED(pmdmntGetApplicationProcessId(&pid)))
        return std::nullopt;
    if (R_FAILED(pminfoGetProgramId(&tid, pid)))
        return std::nullopt;
    return tid;
}

std::string titleName(u64 tid) {
    static std::map<u64, std::string> cache;
    if (auto it = cache.find(tid); it != cache.end())
        return it->second;

    std::string name = hexId(tid);
    auto data = std::make_unique<NsApplicationControlData>();
    u64 size = 0;
    if (R_SUCCEEDED(nsGetApplicationControlData(NsApplicationControlSource_Storage, tid & ~0xFFFULL, data.get(),
                                                sizeof(NsApplicationControlData), &size))) {
        NacpLanguageEntry *entry = nullptr;
        if (R_SUCCEEDED(nacpGetLanguageEntry(&data->nacp, &entry)) && entry && entry->name[0])
            name = entry->name;
    }
    cache[tid] = name;
    return name;
}

/* ---------------- sysmodule ---------------- */

bool sysmoduleRunning() {
    u64 pid = 0;
    return R_SUCCEEDED(pmdmntGetProcessId(&pid, SYSMODULE_ID)) && pid != 0;
}

bool sysmoduleSetRunning(bool run) {
    if (run == sysmoduleRunning())
        return true;
    if (run) {
        const NcmProgramLocation loc = {.program_id = SYSMODULE_ID, .storageID = NcmStorageId_None};
        u64 pid = 0;
        return R_SUCCEEDED(pmshellLaunchProgram(0, &loc, &pid));
    }
    return R_SUCCEEDED(pmshellTerminateProgram(SYSMODULE_ID));
}

static std::string bootFlagPath() {
    return "sdmc:/atmosphere/contents/" + hexId(SYSMODULE_ID) + "/flags/boot2.flag";
}

bool sysmoduleBootEnabled() {
    return isFile(bootFlagPath());
}

bool sysmoduleSetBoot(bool enabled) {
    const std::string p = bootFlagPath();
    if (enabled) {
        mkdirs(p.substr(0, p.rfind('/')));
        FILE *f = std::fopen(p.c_str(), "w");
        if (!f)
            return false;
        std::fclose(f);
        return true;
    }
    return std::remove(p.c_str()) == 0 || !isFile(p);
}

} // namespace sc
