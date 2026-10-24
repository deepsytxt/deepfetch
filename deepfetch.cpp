#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <poll.h>
#include <pwd.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using std::string;
using std::vector;

static constexpr double SPIN_FPS = 15;
static constexpr double DEPTH = 0.09;
static constexpr int LAYERS = 5;
static constexpr int SHELL_ROWS = 8;
static constexpr int FREEZE_ROWS = 5;
static constexpr double FPS_MIN = 6;
static constexpr double PERSPECTIVE = 3.0;
static constexpr int SS = 4;
static constexpr double EXACT_SHARE = 0.6;
static constexpr double AMBIENT = 0.5;
static constexpr int GAP = 3;
static constexpr double SAMPLE_SEC = 1.0;
static constexpr double SAMPLE_BG_SEC = 3.0;
static constexpr size_t HIST = 80;

static double now_sec() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

static string read_file(const string& path) {
    std::ifstream f(path);
    if (!f) return "";
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static string trim(string s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

static string first_line(const string& path) {
    std::ifstream f(path);
    string l;
    std::getline(f, l);
    return trim(l);
}

static string run_cmd(const string& cmd) {
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return "";
    string out;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, p)) > 0) out.append(buf, n);
    pclose(p);
    return out;
}

static bool in_path(const string& name) {
    const char* p = getenv("PATH");
    if (!p) return false;
    std::stringstream ss(p);
    string dir;
    while (std::getline(ss, dir, ':')) {
        if (!dir.empty() && access((dir + "/" + name).c_str(), X_OK) == 0) return true;
    }
    return false;
}

static string env_or(const char* k, const string& d = "") {
    const char* v = getenv(k);
    return (v && *v) ? string(v) : d;
}

static string g_shell_exe;
static int g_top = 0;

static bool icontains(const string& h, const string& n) {
    auto low = [](string s) {
        for (char& c : s) c = (char)tolower((unsigned char)c);
        return s;
    };
    return low(h).find(low(n)) != string::npos;
}

static void append_utf8(string& out, char32_t c) {
    if (c < 0x80) {
        out += (char)c;
    } else if (c < 0x800) {
        out += (char)(0xC0 | (c >> 6));
        out += (char)(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        out += (char)(0xE0 | (c >> 12));
        out += (char)(0x80 | ((c >> 6) & 0x3F));
        out += (char)(0x80 | (c & 0x3F));
    } else {
        out += (char)(0xF0 | (c >> 18));
        out += (char)(0x80 | ((c >> 12) & 0x3F));
        out += (char)(0x80 | ((c >> 6) & 0x3F));
        out += (char)(0x80 | (c & 0x3F));
    }
}

static vector<char32_t> decode_utf8(const string& s) {
    vector<char32_t> out;
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = (unsigned char)s[i];
        char32_t cp;
        int len;
        if (c < 0x80) { cp = c; len = 1; }
        else if ((c >> 5) == 6) { cp = c & 0x1F; len = 2; }
        else if ((c >> 4) == 14) { cp = c & 0x0F; len = 3; }
        else if ((c >> 3) == 30) { cp = c & 0x07; len = 4; }
        else { cp = U'?'; len = 1; }
        for (int k = 1; k < len && i + k < s.size(); k++) cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
        i += len;
        out.push_back(cp);
    }
    return out;
}

static string pad_str(const string& s, int w) {
    return (int)s.size() >= w ? s : s + string(w - (int)s.size(), ' ');
}

static string fmt(const char* f, double v) {
    char b[48];
    snprintf(b, sizeof b, f, v);
    return b;
}

enum Depth { D_NONE = 0, D_16 = 1, D_256 = 2, D_TRUE = 3 };

struct Caps {
    Depth depth = D_TRUE;
    bool unicode = true;
    bool attrs = true;
    bool plain = false;
    bool bright_bold = false;
    bool modern = true;
};
static Caps g_caps;

static void detect_caps() {
    Caps c;
    string term = env_or("TERM");
    bool tty = isatty(0) && isatty(1);
    bool dumb = term.empty() || term == "dumb";
    c.plain = !tty || dumb;

    string loc = env_or("LC_ALL");
    if (loc.empty()) loc = env_or("LC_CTYPE");
    if (loc.empty()) loc = env_or("LANG");
    c.unicode = icontains(loc, "utf-8") || icontains(loc, "utf8");
    if (term == "linux") c.unicode = false;

    string ct = env_or("COLORTERM"), tp = env_or("TERM_PROGRAM");
    bool force = !env_or("FORCE_COLOR").empty() || !env_or("CLICOLOR_FORCE").empty();
    Depth d;
    if (!env_or("NO_COLOR").empty() || (c.plain && !force)) {
        d = D_NONE;
    } else if (icontains(ct, "truecolor") || icontains(ct, "24bit")) {
        d = D_TRUE;
    } else if (icontains(term, "kitty") || icontains(term, "alacritty") || icontains(term, "foot") ||
               icontains(term, "wezterm") || icontains(term, "ghostty") || icontains(term, "contour") ||
               tp == "iTerm.app" || tp == "WezTerm" || tp == "vscode" || tp == "ghostty") {
        d = D_TRUE;
    } else if (icontains(term, "256color")) {
        d = D_256;
    } else if (term.rfind("vt", 0) == 0) {
        d = D_NONE;
    } else {
        d = D_16;
    }
    c.depth = d;
    c.attrs = !c.plain;
    c.bright_bold = (term == "linux");
    c.modern = !c.plain && term != "linux";

    if (!env_or("FFSPIN_ASCII").empty()) c.unicode = false;
    if (!env_or("FFSPIN_UNICODE").empty()) c.unicode = true;
    string fc = env_or("FFSPIN_COLORS");
    if (fc == "none") c.depth = D_NONE;
    else if (fc == "16") c.depth = D_16;
    else if (fc == "256") c.depth = D_256;
    else if (fc == "truecolor") c.depth = D_TRUE;
    g_caps = c;
}

struct Color {
    uint8_t k = 0;
    uint8_t a = 0, b = 0, c = 0;
    bool operator==(const Color& o) const { return k == o.k && a == o.a && b == o.b && c == o.c; }
    bool operator!=(const Color& o) const { return !(*this == o); }
};
static Color c16(int i) { Color c; c.k = 1; c.a = (uint8_t)i; return c; }
static Color c256(int i) { Color c; c.k = 2; c.a = (uint8_t)i; return c; }
static Color rgbc(int r, int g, int b) {
    Color c;
    c.k = 3;
    c.a = (uint8_t)std::max(0, std::min(255, r));
    c.b = (uint8_t)std::max(0, std::min(255, g));
    c.c = (uint8_t)std::max(0, std::min(255, b));
    return c;
}

struct RGB {
    int r, g, b;
};
static Color col(RGB c) { return rgbc(c.r, c.g, c.b); }
static RGB lerp(RGB a, RGB b, float t) {
    return {(int)std::lround(a.r + (b.r - a.r) * t), (int)std::lround(a.g + (b.g - a.g) * t),
            (int)std::lround(a.b + (b.b - a.b) * t)};
}
static float clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

static RGB heat_rgb(float t) {
    t = clamp01(t);
    const RGB g{105, 208, 128}, y{238, 202, 94}, r{240, 98, 94};
    return t < 0.5f ? lerp(g, y, t * 2) : lerp(y, r, (t - 0.5f) * 2);
}
static Color heat(float t) { return col(heat_rgb(t)); }
static Color cool(float t) { return col(lerp(RGB{84, 126, 224}, RGB{128, 226, 240}, clamp01(t))); }

static const Color TXT = rgbc(222, 224, 232);
static const Color DIMC = rgbc(112, 117, 130);
static const Color FAINT = rgbc(62, 66, 78);
static const Color TRACK = rgbc(44, 47, 56);
static const Color CACHE_C = rgbc(74, 108, 176);

enum : uint8_t { A_BOLD = 1, A_DIM = 2 };

struct Cell {
    char32_t ch = U' ';
    Color fg, bg;
    uint8_t attr = 0;
    bool operator==(const Cell& o) const { return ch == o.ch && fg == o.fg && bg == o.bg && attr == o.attr; }
    bool operator!=(const Cell& o) const { return !(*this == o); }
};

struct Grid {
    int w = 0, h = 0;
    vector<Cell> c;
    Grid() {}
    Grid(int W, int H) : w(W), h(H), c((size_t)W * H) {}
    Cell& at(int x, int y) { return c[(size_t)y * w + x]; }
    const Cell& at(int x, int y) const { return c[(size_t)y * w + x]; }
};

struct Line {
    vector<Cell> c;
    void add(char32_t cp, Color fg = Color(), Color bg = Color(), uint8_t at = 0) {
        Cell x;
        x.ch = cp;
        x.fg = fg;
        x.bg = bg;
        x.attr = at;
        c.push_back(x);
    }
    void add(const string& s, Color fg = Color(), Color bg = Color(), uint8_t at = 0) {
        for (char32_t cp : decode_utf8(s)) add(cp, fg, bg, at);
    }
    void pad(int w) {
        while ((int)c.size() < w) c.push_back(Cell());
    }
    int len() const { return (int)c.size(); }
};

static const int PAL16[16][3] = {{0, 0, 0},       {205, 0, 0},     {0, 205, 0},     {205, 205, 0},
                                 {0, 0, 238},     {205, 0, 205},   {0, 205, 205},   {229, 229, 229},
                                 {127, 127, 127}, {255, 0, 0},     {0, 255, 0},     {255, 255, 0},
                                 {92, 92, 255},   {255, 0, 255},   {0, 255, 255},   {255, 255, 255}};

static void idx256_rgb(int i, int& r, int& g, int& b) {
    if (i < 16) {
        r = PAL16[i][0]; g = PAL16[i][1]; b = PAL16[i][2];
    } else if (i < 232) {
        static const int lv[6] = {0, 95, 135, 175, 215, 255};
        int n = i - 16;
        r = lv[n / 36]; g = lv[(n / 6) % 6]; b = lv[n % 6];
    } else {
        r = g = b = 8 + 10 * (i - 232);
    }
}

static int nearest16(int r, int g, int b) {
    int mx = std::max(r, std::max(g, b)), mn = std::min(r, std::min(g, b));
    if (mx - mn < 50) {
        int lum = (r + g + b) / 3;
        return lum < 30 ? 0 : (lum < 150 ? 8 : (lum < 235 ? 7 : 15));
    }
    double h;
    if (mx == r) h = 60.0 * std::fmod((double)(g - b) / (mx - mn) + 6.0, 6.0);
    else if (mx == g) h = 60.0 * ((double)(b - r) / (mx - mn) + 2.0);
    else h = 60.0 * ((double)(r - g) / (mx - mn) + 4.0);
    static const int base[6] = {1, 3, 2, 6, 4, 5};
    int idx = base[((int)std::lround(h / 60.0)) % 6];
    return mx >= 170 ? idx + 8 : idx;
}

static int rgb_to_256(int r, int g, int b) {
    auto q = [](int v) { return v < 48 ? 0 : (v < 115 ? 1 : (v - 35) / 40); };
    static const int lv[6] = {0, 95, 135, 175, 215, 255};
    int qr = q(r), qg = q(g), qb = q(b);
    int cr = lv[qr], cg = lv[qg], cb = lv[qb];
    long dc = (long)(r - cr) * (r - cr) + (long)(g - cg) * (g - cg) + (long)(b - cb) * (b - cb);
    int avg = (r + g + b) / 3;
    int gi = std::max(0, std::min(23, (avg - 8 + 5) / 10));
    int gv = 8 + 10 * gi;
    long dg = (long)(r - gv) * (r - gv) + (long)(g - gv) * (g - gv) + (long)(b - gv) * (b - gv);
    return dg < dc ? 232 + gi : 16 + 36 * qr + 6 * qg + qb;
}

static string idx16_param(int idx, bool fg, bool& bold) {
    int base = fg ? 30 : 40;
    if (idx < 8) return std::to_string(base + idx);
    if (g_caps.bright_bold) {
        if (fg) bold = true;
        return std::to_string(base + idx - 8);
    }
    return std::to_string((fg ? 90 : 100) + idx - 8);
}

static string color_param(const Color& c, bool fg, bool& bold) {
    if (c.k == 0) return "";
    int r = 0, g = 0, b = 0;
    if (c.k == 1) {
        return idx16_param(c.a, fg, bold);
    } else if (c.k == 2) {
        if (g_caps.depth >= D_256) return string(fg ? "38;5;" : "48;5;") + std::to_string(c.a);
        idx256_rgb(c.a, r, g, b);
    } else {
        r = c.a; g = c.b; b = c.c;
        if (g_caps.depth == D_TRUE) {
            char buf[48];
            snprintf(buf, sizeof buf, "%d;2;%d;%d;%d", fg ? 38 : 48, r, g, b);
            return buf;
        }
        if (g_caps.depth == D_256) return string(fg ? "38;5;" : "48;5;") + std::to_string(rgb_to_256(r, g, b));
    }
    return idx16_param(nearest16(r, g, b), fg, bold);
}

struct SgrState {
    Color fg, bg;
    uint8_t attr = 0;
};

static void apply_sgr(string& out, SgrState& st, const Cell& c) {
    Color fg = c.fg, bg = c.bg;
    uint8_t at = g_caps.attrs ? c.attr : 0;
    if (g_caps.depth == D_NONE) fg = bg = Color();
    if (c.ch == U' ' && bg == Color() && st.bg == Color()) return;
    if (fg == st.fg && bg == st.bg && at == st.attr) return;
    bool bold = (at & A_BOLD) != 0;
    string fp = color_param(fg, true, bold), bp = color_param(bg, false, bold);
    string p = "0";
    if (bold) p += ";1";
    if (at & A_DIM) p += ";2";
    if (!fp.empty()) p += ";" + fp;
    if (!bp.empty()) p += ";" + bp;
    out += "\x1b[" + p + "m";
    st.fg = fg;
    st.bg = bg;
    st.attr = at;
}

static const char* ascii_fallback(char32_t c) {
    switch (c) {
        case U'█': case U'▉': case U'▊': case U'▋': case U'▌': case U'▍': case U'▎': case U'▏': case U'▓': return "#";
        case U'▒': return "=";
        case U'░': return ".";
        case U'▁': return "_";
        case U'▂': return ".";
        case U'▃': return ":";
        case U'▄': return "-";
        case U'▅': return "=";
        case U'▆': return "+";
        case U'▇': return "*";
        case U'─': case U'╶': case U'━': return "-";
        case U'│': case U'·': case U'┃': return "|";
        case U'●': case U'•': return "*";
        case U'…': return ".";
        case U'°': return "o";
        default: return "?";
    }
}

static void put_cp(string& out, char32_t c) {
    if (c < 0x80 || g_caps.unicode) append_utf8(out, c);
    else out += ascii_fallback(c);
}

static bool is_default_cell(const Cell& c) {
    return c.ch == U' ' && c.bg == Color();
}

static string grid_text(const Grid& g) {
    string out;
    for (int y = 0; y < g.h; y++) {
        int last = -1;
        for (int x = 0; x < g.w; x++)
            if (!is_default_cell(g.at(x, y))) last = x;
        SgrState st;
        for (int x = 0; x <= last; x++) {
            const Cell& c = g.at(x, y);
            if (g_caps.depth != D_NONE || g_caps.attrs) apply_sgr(out, st, c);
            put_cp(out, c.ch);
        }
        if (st.fg != Color() || st.bg != Color() || st.attr) out += "\x1b[0m";
        out += '\n';
    }
    return out;
}

struct Pinned {
    Grid prev;
    bool valid = false;

    void invalidate() { valid = false; }
    static string cup(int y, int x) { return "\x1b[" + std::to_string(y + g_top + 1) + ";" + std::to_string(x + 1) + "H"; }

    string update(const Grid& g) {
        string out = "\0337\x1b[0m";
        if (!valid || prev.w != g.w || prev.h != g.h) {
            prev = Grid(g.w, g.h);
            for (Cell& c : prev.c) c.ch = U'\x01';
            valid = true;
        }
        SgrState st;
        auto changed = [&](int y, int x) { return x < g.w && g.at(x, y) != prev.at(x, y); };
        for (int y = 0; y < g.h; y++) {
            int x = 0;
            while (x < g.w) {
                if (!changed(y, x)) { x++; continue; }
                out += cup(y, x);
                int cx = x;
                while (true) {
                    const Cell& c = g.at(cx, y);
                    apply_sgr(out, st, c);
                    put_cp(out, c.ch);
                    cx++;
                    if (cx >= g.w) break;
                    if (!(changed(y, cx) || changed(y, cx + 1) || changed(y, cx + 2))) break;
                }
                x = cx;
            }
        }
        if (st.fg != Color() || st.bg != Color() || st.attr) out += "\x1b[0m";
        out += "\0338";
        prev = g;
        return out;
    }
};

static bool rich_bars() { return g_caps.unicode && g_caps.depth >= D_256; }

static const char32_t PART[8] = {U' ', U'▏', U'▎', U'▍', U'▌', U'▋', U'▊', U'▉'};
static const char32_t BLOCKS[8] = {U'▁', U'▂', U'▃', U'▄', U'▅', U'▆', U'▇', U'█'};
static const char ASCII_SPARK[9] = "_.:-=+*#";

static void bar_cells(Line& L, float frac, int width) {
    frac = clamp01(frac);
    if (rich_bars()) {
        int eighths = (int)std::lround(frac * width * 8);
        for (int i = 0; i < width; i++) {
            Color c = heat((i + 0.5f) / width);
            if ((i + 1) * 8 <= eighths) L.add(U'█', c, TRACK);
            else if (i * 8 < eighths) L.add(PART[eighths - i * 8], c, TRACK);
            else L.add(U' ', Color(), TRACK);
        }
        return;
    }
    int inner = width;
    if (!g_caps.unicode) {
        L.add(U'[', DIMC);
        inner = width - 2;
    }
    int filled = (int)std::lround(frac * inner);
    for (int i = 0; i < inner; i++) {
        if (i < filled) L.add(g_caps.unicode ? U'█' : U'#', heat((i + 0.5f) / inner));
        else L.add(g_caps.unicode ? U'░' : U'-', FAINT, Color(), 0);
    }
    if (!g_caps.unicode) L.add(U']', DIMC);
}

static void stacked_cells(Line& L, float used, float cache, Color usedc, int width) {
    used = clamp01(used);
    cache = clamp01(std::min(cache, 1.0f - used));
    if (rich_bars()) {
        int e1 = (int)std::lround(used * width * 8);
        int e2 = (int)std::lround((used + cache) * width * 8);
        int ends[2] = {e1, e2};
        Color cols[2] = {usedc, CACHE_C};
        auto seg_at = [&](int pos) {
            for (int j = 0; j < 2; j++)
                if (ends[j] > pos) return j;
            return -1;
        };
        for (int i = 0; i < width; i++) {
            int start = i * 8, end = start + 8;
            int a = seg_at(start);
            if (a < 0) { L.add(U' ', Color(), TRACK); continue; }
            if (ends[a] >= end) { L.add(U'█', cols[a], TRACK); continue; }
            int k = ends[a] - start;
            int b = seg_at(ends[a]);
            L.add(PART[k], cols[a], b < 0 ? TRACK : cols[b]);
        }
        return;
    }
    int inner = width;
    if (!g_caps.unicode) {
        L.add(U'[', DIMC);
        inner = width - 2;
    }
    int n1 = (int)std::lround(used * inner), n2 = (int)std::lround((used + cache) * inner);
    for (int i = 0; i < inner; i++) {
        if (i < n1) L.add(g_caps.unicode ? U'█' : U'#', usedc);
        else if (i < n2) L.add(g_caps.unicode ? U'▒' : U'=', CACHE_C);
        else L.add(g_caps.unicode ? U'░' : U'-', FAINT);
    }
    if (!g_caps.unicode) L.add(U']', DIMC);
}

static void pct_cells(Line& L, float f) {
    char b[16];
    snprintf(b, sizeof b, "%3d%%", (int)std::lround(clamp01(f) * 100));
    L.add(string(b), heat(f));
}

struct Mat3 {
    double m[3][3];
};
static Mat3 identity() { return {{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}}; }
static Mat3 mul(const Mat3& a, const Mat3& b) {
    Mat3 r{};
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            double s = 0;
            for (int k = 0; k < 3; k++) s += a.m[i][k] * b.m[k][j];
            r.m[i][j] = s;
        }
    return r;
}
static Mat3 rotX(double a) { double c = cos(a), s = sin(a); return {{{1, 0, 0}, {0, c, -s}, {0, s, c}}}; }
static Mat3 rotY(double a) { double c = cos(a), s = sin(a); return {{{c, 0, s}, {0, 1, 0}, {-s, 0, c}}}; }
static Mat3 rotZ(double a) { double c = cos(a), s = sin(a); return {{{c, -s, 0}, {s, c, 0}, {0, 0, 1}}}; }
static Mat3 orthonormalize(const Mat3& R) {
    auto norm = [](double* v) {
        double n = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (n < 1e-12) n = 1;
        for (int i = 0; i < 3; i++) v[i] /= n;
    };
    double r0[3] = {R.m[0][0], R.m[0][1], R.m[0][2]};
    double r1[3] = {R.m[1][0], R.m[1][1], R.m[1][2]};
    norm(r0);
    double d = r1[0] * r0[0] + r1[1] * r0[1] + r1[2] * r0[2];
    for (int i = 0; i < 3; i++) r1[i] -= d * r0[i];
    norm(r1);
    double r2[3] = {r0[1] * r1[2] - r0[2] * r1[1], r0[2] * r1[0] - r0[0] * r1[2], r0[0] * r1[1] - r0[1] * r1[0]};
    Mat3 o{};
    for (int i = 0; i < 3; i++) {
        o.m[0][i] = r0[i];
        o.m[1][i] = r1[i];
        o.m[2][i] = r2[i];
    }
    return o;
}

static char32_t mirror_char(char32_t c) {
    switch (c) {
        case U'/': return U'\\';
        case U'\\': return U'/';
        case U'(': return U')';
        case U')': return U'(';
        case U'<': return U'>';
        case U'>': return U'<';
        case U'[': return U']';
        case U']': return U'[';
        case U'{': return U'}';
        case U'}': return U'{';
        case U'▌': return U'▐';
        case U'▐': return U'▌';
        case U'▀': return U'▀';
        default: return c;
    }
}

static void parse_sgr(const string& params, Color& fg, Color& bg, uint8_t& at) {
    vector<int> v;
    {
        std::stringstream ss(params);
        string t;
        while (std::getline(ss, t, ';')) v.push_back(t.empty() ? 0 : atoi(t.c_str()));
        if (params.empty() || params.back() == ';') v.push_back(0);
    }
    for (size_t i = 0; i < v.size(); i++) {
        int p = v[i];
        if (p == 0) { fg = bg = Color(); at = 0; }
        else if (p == 1) at |= A_BOLD;
        else if (p == 2) at |= A_DIM;
        else if (p == 22) at &= (uint8_t)~(A_BOLD | A_DIM);
        else if (p >= 30 && p <= 37) fg = c16(p - 30);
        else if (p == 39) fg = Color();
        else if (p >= 40 && p <= 47) bg = c16(p - 40);
        else if (p == 49) bg = Color();
        else if (p >= 90 && p <= 97) fg = c16(p - 90 + 8);
        else if (p >= 100 && p <= 107) bg = c16(p - 100 + 8);
        else if (p == 38 || p == 48) {
            Color& t = (p == 38) ? fg : bg;
            if (i + 2 < v.size() && v[i + 1] == 5) { t = c256(v[i + 2]); i += 2; }
            else if (i + 4 < v.size() && v[i + 1] == 2) { t = rgbc(v[i + 2], v[i + 3], v[i + 4]); i += 4; }
        }
    }
}

static vector<Cell> parse_line(const string& line) {
    vector<Cell> cells;
    Color fg, bg;
    uint8_t at = 0;
    size_t i = 0;
    while (i < line.size()) {
        unsigned char c = (unsigned char)line[i];
        if (c == 0x1b && i + 1 < line.size() && line[i + 1] == '[') {
            size_t j = i + 2;
            while (j < line.size() && !(line[j] >= 0x40 && line[j] <= 0x7E)) j++;
            if (j >= line.size()) break;
            if (line[j] == 'm') parse_sgr(line.substr(i + 2, j - i - 2), fg, bg, at);
            i = j + 1;
            continue;
        }
        if (c == '\r' || c == '\n') { i++; continue; }
        size_t len = 1;
        if (c >= 0xF0) len = 4;
        else if (c >= 0xE0) len = 3;
        else if (c >= 0xC0) len = 2;
        auto cps = decode_utf8(line.substr(i, len));
        i += len;
        Cell cell;
        cell.ch = cps.empty() ? U'?' : cps[0];
        cell.fg = fg;
        cell.bg = bg;
        cell.attr = at;
        cells.push_back(cell);
    }
    return cells;
}

struct Logo {
    vector<vector<Cell>> rows;
    int W = 0, H = 0;
    Color accent = c16(6);
};

static Logo load_logo() {
    string raw = run_cmd("fastfetch -s none --pipe false 2>/dev/null");
    if (trim(raw).empty()) raw = run_cmd("fastfetch -s '' --pipe false 2>/dev/null");
    if (trim(raw).empty()) {

        raw = string("\x1b[36m") +
              "     .-\"\"\"\"\"\"-.\n"
              "   .'  .----.  '.\n"
              "  /   /  ()  \\   \\\n"
              " |   |   /\\   |   |\n"
              " |   |  /  \\  |   |\n"
              "  \\   \\ '--' /   /\n"
              "   '.  '----'  .'\n"
              "     '-......-'\n";
    }
    Logo lg;
    {
        std::stringstream ss(raw);
        string l;
        while (std::getline(ss, l)) lg.rows.push_back(parse_line(l));
    }
    auto blank = [](const vector<Cell>& r) {
        for (auto& c : r)
            if (c.ch != U' ') return false;
        return true;
    };
    while (!lg.rows.empty() && blank(lg.rows.back())) lg.rows.pop_back();
    while (!lg.rows.empty() && blank(lg.rows.front())) lg.rows.erase(lg.rows.begin());
    for (auto& r : lg.rows) lg.W = std::max(lg.W, (int)r.size());
    for (auto& r : lg.rows) r.resize(lg.W);
    lg.H = (int)lg.rows.size();

    vector<std::pair<Color, int>> cnt;
    for (auto& r : lg.rows)
        for (auto& c : r) {
            if (c.ch == U' ' || c.fg.k == 0) continue;
            bool found = false;
            for (auto& e : cnt)
                if (e.first == c.fg) { e.second++; found = true; break; }
            if (!found) cnt.push_back({c.fg, 1});
        }
    int best = 0;
    for (auto& e : cnt) best = std::max(best, e.second);
    for (auto& e : cnt)
        if (e.second == best) { lg.accent = e.first; break; }
    return lg;
}

static float glyph_ink(char32_t c) {
    static const char RAMP[] = " .'`^\",:;Il!i><~+_-?][}{1)(|\\/tfjrxnuvczXYUJCLQ0OZmwqpdbkhao*#MW&8%B@$";
    if (c == U' ') return 0;
    if (c >= 0x2800 && c <= 0x28FF) return __builtin_popcount((unsigned)(c - 0x2800)) / 8.0f;
    switch (c) {
        case U'.': return 0.10f;
        case U':': return 0.20f;
        case U';': return 0.26f;
        case U'l': return 0.30f;
        case U'c': return 0.33f;
        case U'o': return 0.40f;
        case U'x': return 0.45f;
        case U'd': return 0.52f;
        case U'k': return 0.56f;
        case U'O': return 0.62f;
        case U'X': return 0.66f;
        case U'K': return 0.68f;
        case U'0': return 0.72f;
        case U'N': return 0.80f;
        case U'W': return 0.86f;
        case U'M': return 0.90f;
        case U'█': return 1.0f;
        case U'▉': case U'▊': return 0.85f;
        case U'▓': return 0.75f;
        case U'▋': case U'▌': case U'▐': case U'▀': case U'▄': case U'▒': return 0.5f;
        case U'▍': case U'▎': case U'▏': case U'░': return 0.25f;
        default: break;
    }
    if (c < 128) {
        const char* p = strchr(RAMP, (int)c);
        if (p) return (float)(p - RAMP) / (float)(sizeof RAMP - 2);
        if (c >= 'A' && c <= 'Z') return 0.62f;
        if (c >= 'a' && c <= 'z') return 0.42f;
    }
    return 0.5f;
}

struct RGBf {
    int r, g, b;
};
static RGBf color_rgb(const Color& c);
static RGBf gfx_rgb(const Color& c) {
    static const int P[16][3] = {{40, 42, 54},    {255, 85, 85},   {80, 220, 120},  {240, 220, 110},
                                 {98, 148, 255},  {255, 121, 198}, {110, 220, 240}, {215, 218, 228},
                                 {110, 114, 130}, {255, 130, 130}, {130, 255, 160}, {255, 245, 150},
                                 {140, 180, 255}, {255, 160, 220}, {160, 240, 255}, {255, 255, 255}};
    if (c.k == 1) return {P[c.a & 15][0], P[c.a & 15][1], P[c.a & 15][2]};
    return color_rgb(c);
}
static RGBf color_rgb(const Color& c) {
    int r = 208, g = 208, b = 208;
    if (c.k == 1) { r = PAL16[c.a & 15][0]; g = PAL16[c.a & 15][1]; b = PAL16[c.a & 15][2]; }
    else if (c.k == 2) idx256_rgb(c.a, r, g, b);
    else if (c.k == 3) { r = c.a; g = c.b; b = c.c; }
    return {r, g, b};
}

static void apply_light(Cell& cell, float b) {
    if (b >= 0.97f) return;
    b = std::max(0.18f, b);
    if (g_caps.depth >= D_256) {
        RGBf f = color_rgb(cell.fg);
        cell.fg = rgbc((int)std::lround(f.r * b), (int)std::lround(f.g * b), (int)std::lround(f.b * b));
        if (cell.bg.k != 0) {
            RGBf g = color_rgb(cell.bg);
            cell.bg = rgbc((int)std::lround(g.r * b), (int)std::lround(g.g * b), (int)std::lround(g.b * b));
        }
    } else if (b < 0.72f) {
        cell.attr |= A_DIM;
    }
}

struct Scene {
    struct PalEntry {
        float ink;
        char32_t ch;
        int cnt;
    };
    Logo logo;
    int W = 0, H = 0, mx = 0, my = 1, CW = 0, CH = 0;
    double D = 0, T = 0, S = 1;
    double zl[LAYERS];
    vector<double> ssx, ssy;
    vector<float> ink;
    vector<PalEntry> pal;

    void init(const Logo& l) {
        logo = l;
        W = l.W;
        H = l.H;
        mx = W / 8 + 2;
        CW = W + 2 * mx;
        CH = H + 2 * my + 2;
        D = PERSPECTIVE * W;
        T = DEPTH * W;
        for (int i = 0; i < LAYERS; i++) zl[i] = T / 2 - T * i / (LAYERS - 1);
        S = D / (D - T / 2);
        for (int c = 0; c < CW; c++)
            for (int i = 0; i < SS; i++) ssx.push_back(c + (i + 0.5) / SS - CW / 2.0);
        for (int r = 0; r < CH; r++)
            for (int j = 0; j < SS; j++) ssy.push_back((r + (j + 0.5) / SS - CH / 2.0) * 2);

        ink.assign((size_t)W * H, 0.f);
        vector<PalEntry> all;
        for (int r = 0; r < H; r++)
            for (int c = 0; c < W; c++) {
                const Cell& x = logo.rows[r][c];
                float v = glyph_ink(x.ch);
                if (x.ch == U' ' && x.bg != Color()) v = 0.35f;
                ink[(size_t)r * W + c] = v;
                if (x.ch == U' ') continue;
                bool found = false;
                for (auto& e : all)
                    if (e.ch == x.ch) { e.cnt++; found = true; break; }
                if (!found) all.push_back({v, x.ch, 1});
            }
        auto directional = [](char32_t c) { return c == U'/' || c == U'\\' || c == U'(' || c == U')' || c == U'<' || c == U'>' ||
                                                   c == U'[' || c == U']' || c == U'{' || c == U'}' || c == U'|'; };
        vector<PalEntry> use;
        for (auto& e : all) if (!directional(e.ch)) use.push_back(e);
        if (use.size() < 3) use = all;
        std::sort(use.begin(), use.end(), [](const PalEntry& x, const PalEntry& y) { return x.ink < y.ink; });
        for (auto& e : use) {
            if (!pal.empty() && fabs(e.ink - pal.back().ink) < 0.04f) {
                if (e.cnt > pal.back().cnt) pal.back() = e;
                continue;
            }
            pal.push_back(e);
        }
        if (pal.size() < 4) {
            for (auto e : {PalEntry{0.10f, U'.', 0}, PalEntry{0.20f, U':', 0}, PalEntry{0.36f, U'+', 0}, PalEntry{0.80f, U'#', 0}}) {
                bool near = false;
                for (auto& p : pal) if (fabs(p.ink - e.ink) < 0.08f) near = true;
                if (!near) pal.push_back(e);
            }
            std::sort(pal.begin(), pal.end(), [](const PalEntry& x, const PalEntry& y) { return x.ink < y.ink; });
        }
        if (pal.empty()) pal.push_back({0.5f, U'#', 1});
    }

    char32_t pick(float target) const {
        size_t best = 0;
        float bd = 1e9f;
        for (size_t i = 0; i < pal.size(); i++) {
            float d = fabs(pal[i].ink - target);
            if (d < bd) { bd = d; best = i; }
        }
        return pal[best].ch;
    }
    void canvas(const Mat3& R, Grid& out) const {
        double nx = R.m[0][2], ny = R.m[1][2], nz = R.m[2][2];
        bool back = nz < 0;
        double nzD = nz * D, halfW = W / 2.0, halfH = H / 2.0;
        int faceL = back ? LAYERS - 1 : 0;

        const double Lx = -0.35, Ly = -0.45, Lz = 0.82;
        double vs = back ? -1.0 : 1.0;
        double ndl = vs * (nx * Lx + ny * Ly + nz * Lz);
        double lit = std::max(0.0, std::min(1.0, ndl / Lz));
        float faceB = (float)(AMBIENT + (1.0 - AMBIENT) * lit);
        if (back) faceB *= 0.85f;
        float sideB = (float)(0.52 + 0.12 * lit);

        struct Grp {
            char32_t ch;
            Color fg, bg;
            uint8_t attr;
            int n;
            float w;
        };
        const int total = SS * SS;

        for (int r = 0; r < CH; r++) {
            for (int c = 0; c < CW; c++) {
                Cell& dst = out.at(c, r);
                dst = Cell();
                Grp g[SS * SS];
                int ng = 0, hits = 0;
                float inkSum = 0, bSum = 0;
                for (int j = 0; j < SS; j++) {
                    double py0 = ssy[(size_t)r * SS + j];
                    for (int i = 0; i < SS; i++) {
                        double px0 = ssx[(size_t)c * SS + i];
                        double den = nzD - nx * px0 - ny * py0;
                        if (fabs(den) < 1e-6) continue;
                        double bestT = 1e18;
                        int bl = -1, bcol = 0, brw = 0;
                        for (int l = 0; l < LAYERS; l++) {
                            double t = (nzD - zl[l]) / den;
                            if (t <= 0 || t >= bestT) continue;
                            double px = t * px0, py = t * py0, pz = D * (1 - t);
                            double lx = S * (R.m[0][0] * px + R.m[1][0] * py + R.m[2][0] * pz);
                            double ly = S * (R.m[0][1] * px + R.m[1][1] * py + R.m[2][1] * pz);
                            int col = (int)floor(lx + halfW), rw = (int)floor(ly / 2 + halfH);
                            if (col < 0 || col >= W || rw < 0 || rw >= H) continue;
                            const Cell& src = logo.rows[rw][col];
                            if (src.ch == U' ' && src.bg == Color()) continue;
                            bestT = t; bl = l; bcol = col; brw = rw;
                        }
                        if (bl < 0) continue;
                        const Cell& src = logo.rows[brw][bcol];
                        float v = ink[(size_t)brw * W + bcol];
                        float b = (bl == faceL) ? faceB : sideB * (1.0f - 0.09f * std::abs(bl - faceL));
                        b *= (float)std::max(0.55, std::min(1.0, 1.0 - 0.9 * (bestT - 1.0)));
                        hits++;
                        inkSum += v;
                        bSum += b;
                        int k = 0;
                        while (k < ng && !(g[k].ch == src.ch && g[k].fg == src.fg && g[k].bg == src.bg)) k++;
                        if (k == ng) { g[ng++] = {src.ch, src.fg, src.bg, src.attr, 0, 0.f}; }
                        g[k].n++;
                        g[k].w += v;
                    }
                }
                if (hits == 0) continue;
                float mean = inkSum / total;
                float minInk = pal.front().ink;
                int top = 0, dom = 0;
                for (int k = 1; k < ng; k++) {
                    if (g[k].n > g[top].n) top = k;
                    if (g[k].w > g[dom].w) dom = k;
                }
                Cell cell;
                if (g[top].n >= EXACT_SHARE * total) {
                    cell.ch = g[top].ch; cell.fg = g[top].fg; cell.bg = g[top].bg; cell.attr = g[top].attr;
                } else {
                    if (mean < minInk * 0.5f) continue;
                    cell.ch = pick(mean); cell.fg = g[dom].fg; cell.bg = g[dom].bg; cell.attr = g[dom].attr;
                }
                if (back) cell.ch = mirror_char(cell.ch);
                apply_light(cell, bSum / hits);
                dst = cell;
            }
        }
    }
};

struct Spin {
    double t = 0, yaw = 0, next_move;
    uint64_t rng;
    int axis = -1;
    double m_t0 = 0, m_dur = 1, m_amt = 0;
    Mat3 R = identity();

    Spin() {
        rng = (((uint64_t)(now_sec() * 1e6) * 0x9E3779B97F4A7C15ull) ^ ((uint64_t)getpid() << 20)) | 1;
        next_move = 6 + 6 * rnd();
    }
    double rnd() {
        rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
        return (double)(rng >> 11) / 9007199254740992.0;
    }
    static double ease(double u) {
        u = u < 0 ? 0 : (u > 1 ? 1 : u);
        return u * u * u * (u * (u * 6 - 15) + 10);
    }
    void step(double dt) {
        t += dt;
        double a = ease(t / 5.0);
        double rate = 0.75 + 0.45 * sin(0.37 * t + 1.1) + 0.25 * sin(0.91 * t + 0.3);
        yaw += std::max(0.10, rate) * dt * a;
        double pitch = a * (0.36 * sin(0.29 * t + 0.7) + 0.13 * sin(0.77 * t + 2.0));
        double roll = a * (0.14 * sin(0.21 * t + 4.0) + 0.05 * sin(0.59 * t));
        double extra = 0;
        if (axis < 0 && t >= next_move) {
            double p = rnd();
            axis = p < 0.45 ? 0 : (p < 0.85 ? 1 : 2);
            m_amt = (rnd() < 0.5 ? -1 : 1) * 2 * M_PI;
            m_dur = 2.8 + 1.6 * rnd();
            m_t0 = t;
        }
        if (axis >= 0) {
            double u = (t - m_t0) / m_dur;
            extra = m_amt * ease(u);
            if (u >= 1) {
                axis = -1;
                extra = 0;
                next_move = t + 8 + 14 * rnd();
            }
        }
        Mat3 base = mul(rotZ(roll), mul(rotX(pitch), rotY(yaw)));
        Mat3 m = identity();
        if (axis == 0) m = rotX(extra);
        else if (axis == 1) m = rotY(extra);
        else if (axis == 2) m = rotZ(extra);
        R = orthonormalize(mul(m, base));
    }
};

struct Gauge {
    float target = 0, shown = 0;
    void step(double dt) { shown += (target - shown) * (float)(1 - exp(-dt * 10)); }
    bool settled() const { return fabs(shown - target) < 0.004f; }
    void snap() { shown = target; }
};

struct Hist {
    vector<float> v;
    void push(float x) {
        v.push_back(x);
        if (v.size() > HIST) v.erase(v.begin());
    }
    float max() const {
        float m = 0;
        for (float x : v) m = std::max(m, x);
        return m;
    }
};

struct DiskInfo {
    string mount, fs, source;
    double used = 0, total = 0;
    Gauge g;
};

struct Stats {
    Gauge cpu;
    vector<Gauge> cores;
    Hist cpuH;
    double mhz = 0, tempC = -1, load1 = 0, load5 = 0, load15 = 0;
    int procs = 0;
    Gauge memUsed, memCache, swap;
    double memTotal = 0, memUsedKiB = 0, memCacheKiB = 0, swapTotal = 0, swapUsed = 0;
    Hist memH;
    bool hasGpu = false, gpuAsleep = false;
    string gpuName;
    Gauge gpuUtil, vram;
    double vramUsed = 0, vramTotal = 0, gpuTemp = -1, gpuPower = -1, gpuClock = -1, gpuFan = -1;
    Hist gpuH, vramH;
    vector<DiskInfo> disks;
    double ioR = 0, ioW = 0, netRx = 0, netTx = 0;
    Hist rxH, txH;
    double uptime = 0;
    bool pulse = false;

    vector<Gauge*> gauges() {
        vector<Gauge*> g{&cpu, &memUsed, &memCache, &swap, &gpuUtil, &vram};
        for (auto& c : cores) g.push_back(&c);
        for (auto& d : disks) g.push_back(&d.g);
        return g;
    }
};

struct Nvml {
    struct Util { unsigned gpu, memory; };
    struct Mem { unsigned long long total, free, used; };
    void* lib = nullptr;
    void* dev = nullptr;
    bool ok = false;
    int (*Init)() = nullptr;
    int (*Shutdown)() = nullptr;
    int (*ByIndex)(unsigned, void**) = nullptr;
    int (*Name)(void*, char*, unsigned) = nullptr;
    int (*GetUtil)(void*, Util*) = nullptr;
    int (*GetMem)(void*, Mem*) = nullptr;
    int (*GetTemp)(void*, int, unsigned*) = nullptr;
    int (*GetPower)(void*, unsigned*) = nullptr;
    int (*GetClock)(void*, int, unsigned*) = nullptr;
    int (*GetFan)(void*, unsigned*) = nullptr;

    template <class T>
    bool sym(T& f, const char* a, const char* b = nullptr) {
        void* p = dlsym(lib, a);
        if (!p && b) p = dlsym(lib, b);
        f = reinterpret_cast<T>(p);
        return p != nullptr;
    }

    bool init(string& name) {
        if (ok) return true;
        if (!lib) lib = dlopen("libnvidia-ml.so.1", RTLD_LAZY);
        if (!lib) return false;
        bool good = sym(Init, "nvmlInit_v2", "nvmlInit") && sym(Shutdown, "nvmlShutdown") &&
                    sym(ByIndex, "nvmlDeviceGetHandleByIndex_v2", "nvmlDeviceGetHandleByIndex") &&
                    sym(Name, "nvmlDeviceGetName") && sym(GetUtil, "nvmlDeviceGetUtilizationRates") &&
                    sym(GetMem, "nvmlDeviceGetMemoryInfo");
        if (!good) return false;
        sym(GetTemp, "nvmlDeviceGetTemperature");
        sym(GetPower, "nvmlDeviceGetPowerUsage");
        sym(GetClock, "nvmlDeviceGetClockInfo");
        sym(GetFan, "nvmlDeviceGetFanSpeed");
        if (Init() != 0) return false;
        if (ByIndex(0, &dev) != 0) return false;
        char nb[128] = {0};
        if (Name(dev, nb, sizeof nb) == 0) name = nb;
        ok = true;
        return true;
    }

    void close() {
        if (ok && Shutdown) Shutdown();
        ok = false;
    }
};

struct GpuFeed {
    pid_t pid = -1;
    int fd = -1;
    std::thread th;
    std::mutex mu;
    bool have = false;
    string name;
    double util = 0, memUsed = 0, memTotal = 0, temp = -1, power = -1, clock = -1, fan = -1;

    bool start() {
        if (!in_path("nvidia-smi")) return false;
        int p[2];
        if (pipe(p) != 0) return false;
        pid = fork();
        if (pid < 0) return false;
        if (pid == 0) {
            dup2(p[1], 1);
            int dn = open("/dev/null", O_WRONLY);
            if (dn >= 0) dup2(dn, 2);
            close(p[0]);
            close(p[1]);
            execlp("nvidia-smi", "nvidia-smi",
                   "--query-gpu=name,utilization.gpu,memory.used,memory.total,temperature.gpu,"
                   "power.draw,clocks.gr,fan.speed",
                   "--format=csv,noheader,nounits", "-lms", "1000", (char*)nullptr);
            _exit(127);
        }
        close(p[1]);
        fd = p[0];
        th = std::thread([this] { reader(); });
        return true;
    }

    static double num(const string& s) {
        string t = trim(s);
        if (t.empty() || t[0] == '[') return -1;
        return atof(t.c_str());
    }

    void reader() {
        string buf;
        char tmp[1024];
        ssize_t n;
        while ((n = read(fd, tmp, sizeof tmp)) > 0) {
            buf.append(tmp, n);
            size_t pos;
            while ((pos = buf.find('\n')) != string::npos) {
                string line = buf.substr(0, pos);
                buf.erase(0, pos + 1);
                vector<string> f;
                std::stringstream ss(line);
                string item;
                while (std::getline(ss, item, ',')) f.push_back(item);
                if (f.size() < 8) continue;
                std::lock_guard<std::mutex> lk(mu);
                name = trim(f[0]);
                util = num(f[1]);
                memUsed = num(f[2]);
                memTotal = num(f[3]);
                temp = num(f[4]);
                power = num(f[5]);
                clock = num(f[6]);
                fan = num(f[7]);
                have = true;
            }
        }
    }

    bool ready() {
        std::lock_guard<std::mutex> lk(mu);
        return have;
    }

    void stop() {
        if (pid > 0) {
            kill(pid, SIGTERM);
            waitpid(pid, nullptr, 0);
            pid = -1;
        }
        if (th.joinable()) th.join();
        if (fd >= 0) close(fd);
        fd = -1;
    }
};

struct Async {
    std::mutex mu;
    string packages = "...";
    string shellVer;
    std::atomic<int> version{0};
};
static Async g_async;

static string extract_version(const string& s) {
    for (size_t i = 0; i < s.size(); i++) {
        if (isdigit((unsigned char)s[i])) {
            size_t j = i;
            int dots = 0;
            while (j < s.size() && (isdigit((unsigned char)s[j]) || s[j] == '.')) {
                if (s[j] == '.') dots++;
                j++;
            }
            if (dots >= 1) return s.substr(i, j - i);
            i = j;
        }
    }
    return "";
}

static void async_worker() {
    vector<string> parts;
    auto count = [](const string& cmd) { return trim(run_cmd(cmd)); };
    if (in_path("rpm")) parts.push_back(count("rpm -qa 2>/dev/null | wc -l") + " (rpm)");
    if (in_path("dpkg-query")) parts.push_back(count("dpkg-query -f '.\\n' -W 2>/dev/null | wc -l") + " (dpkg)");
    if (in_path("pacman")) parts.push_back(count("pacman -Qq 2>/dev/null | wc -l") + " (pacman)");
    if (in_path("apk")) parts.push_back(count("apk info 2>/dev/null | wc -l") + " (apk)");
    if (in_path("xbps-query")) parts.push_back(count("xbps-query -l 2>/dev/null | wc -l") + " (xbps)");
    if (in_path("qlist")) parts.push_back(count("qlist -I 2>/dev/null | wc -l") + " (portage)");
    if (in_path("nix-store")) parts.push_back(count("nix-store -q --requisites /run/current-system/sw 2>/dev/null | wc -l") + " (nix)");
    if (in_path("snap")) {
        string n = count("snap list 2>/dev/null | tail -n +2 | wc -l");
        if (n != "0" && !n.empty()) parts.push_back(n + " (snap)");
    }
    if (in_path("flatpak")) {
        string n = count("flatpak list --app 2>/dev/null | wc -l");
        if (n != "0" && !n.empty()) parts.push_back(n + " (flatpak)");
    }
    string pk;
    for (size_t i = 0; i < parts.size(); i++) pk += (i ? ", " : "") + parts[i];
    if (pk.empty()) pk = "-";
    string sh = !g_shell_exe.empty() ? g_shell_exe : env_or("SHELL");
    string ver;
    if (!sh.empty()) ver = extract_version(run_cmd("'" + sh + "' --version 2>&1 | head -n1"));
    {
        std::lock_guard<std::mutex> lk(g_async.mu);
        g_async.packages = pk;
        g_async.shellVer = ver;
    }
    g_async.version++;
}

struct StaticInfo {
    string user, host, os, hostModel, kernel, shell, wm, terminal, cpuModel;
};

static StaticInfo collect_static(int threads) {
    StaticInfo si;
    struct passwd* pw = getpwuid(getuid());
    si.user = env_or("USER", pw ? pw->pw_name : "user");
    char hn[256] = {0};
    gethostname(hn, sizeof hn - 1);
    si.host = hn;
    struct utsname u;
    uname(&u);
    si.kernel = string(u.sysname) + " " + u.release;
    string pretty;
    {
        std::stringstream ss(read_file("/etc/os-release"));
        string l;
        while (std::getline(ss, l)) {
            if (l.rfind("PRETTY_NAME=", 0) == 0) {
                pretty = l.substr(12);
                pretty.erase(std::remove(pretty.begin(), pretty.end(), '"'), pretty.end());
            }
        }
    }
    si.os = (pretty.empty() ? string(u.sysname) : pretty) + " " + u.machine;
    string hm = first_line("/sys/devices/virtual/dmi/id/product_name");
    auto generic = [](const string& s) {
        return s.empty() || s.find("O.E.M") != string::npos || s == "System Product Name" ||
               s == "Default string";
    };
    if (generic(hm)) hm = first_line("/sys/devices/virtual/dmi/id/board_name");
    if (generic(hm)) hm.clear();
    si.hostModel = hm;
    string sh = !g_shell_exe.empty() ? g_shell_exe : env_or("SHELL");
    si.shell = sh.empty() ? "?" : sh.substr(sh.rfind('/') + 1);
    string de = env_or("XDG_CURRENT_DESKTOP", env_or("DESKTOP_SESSION", ""));
    string sess = env_or("XDG_SESSION_TYPE");
    if (!sess.empty()) sess[0] = (char)toupper(sess[0]);
    if (sess == "Wayland" || sess == "X11") si.wm = de.empty() ? sess : de + " (" + sess + ")";
    else si.wm = de.empty() ? "-" : de;
    string term = env_or("TERM_PROGRAM");
    if (term.empty() && !env_or("KITTY_WINDOW_ID").empty()) term = "kitty";
    if (term.empty()) term = env_or("TERM", "?");
    si.terminal = term;

    string model;
    {
        std::stringstream ss(read_file("/proc/cpuinfo"));
        string l;
        while (std::getline(ss, l)) {
            if (l.rfind("model name", 0) == 0) {
                size_t p = l.find(':');
                if (p != string::npos) model = trim(l.substr(p + 1));
                break;
            }
        }
    }
    for (const char* junk : {"(R)", "(TM)", "(tm)", " CPU"}) {
        size_t p;
        while ((p = model.find(junk)) != string::npos) model.erase(p, strlen(junk));
    }
    size_t p = model.find("-Core");
    if (p != string::npos) {
        size_t q = model.rfind(' ', p);
        if (q != string::npos) model.erase(q);
    }
    p = model.find(" @ ");
    if (p != string::npos) model.erase(p);
    while (model.find("  ") != string::npos) model.replace(model.find("  "), 2, " ");
    model = trim(model);
    if (model.empty()) model = "CPU";
    model += " (" + std::to_string(threads) + ")";
    double maxkhz = atof(first_line("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq").c_str());
    if (maxkhz > 0) model += fmt(" @ %.2f", maxkhz / 1e6) + " GHz";
    si.cpuModel = model;
    return si;
}

struct CpuT {
    unsigned long long idle = 0, total = 0;
};

static vector<CpuT> read_cpu_times() {
    vector<CpuT> v;
    std::ifstream f("/proc/stat");
    string line;
    while (std::getline(f, line)) {
        if (line.compare(0, 3, "cpu") != 0) break;
        const char* p = line.c_str() + 3;
        while (isdigit((unsigned char)*p)) p++;
        unsigned long long a[10] = {0};
        sscanf(p, "%llu %llu %llu %llu %llu %llu %llu %llu %llu %llu", &a[0], &a[1], &a[2], &a[3], &a[4], &a[5],
               &a[6], &a[7], &a[8], &a[9]);
        CpuT t;
        t.idle = a[3] + a[4];
        t.total = a[0] + a[1] + a[2] + a[3] + a[4] + a[5] + a[6] + a[7];
        v.push_back(t);
    }
    return v;
}

static std::unordered_map<string, double> read_meminfo() {
    std::unordered_map<string, double> m;
    std::ifstream f("/proc/meminfo");
    string line;
    while (std::getline(f, line)) {
        size_t p = line.find(':');
        if (p == string::npos) continue;
        m[line.substr(0, p)] = atof(line.c_str() + p + 1);
    }
    return m;
}

static bool is_physical_disk(const string& n) {
    auto all_alpha_after = [&](size_t from) {
        if (from >= n.size()) return false;
        for (size_t i = from; i < n.size(); i++)
            if (!isalpha((unsigned char)n[i])) return false;
        return true;
    };
    if (n.rfind("sd", 0) == 0) return all_alpha_after(2);
    if (n.rfind("vd", 0) == 0) return all_alpha_after(2);
    if (n.rfind("xvd", 0) == 0) return all_alpha_after(3);
    if (n.rfind("nvme", 0) == 0) return n.find('p', 4) == string::npos && n.find('n', 4) != string::npos;
    if (n.rfind("mmcblk", 0) == 0) return n.find('p') == string::npos;
    return false;
}

enum GpuKind { GK_NONE, GK_NVML, GK_SMI, GK_AMD };

class Sampler {
public:
    Stats s;
    GpuFeed feed;
    Nvml nvml;
    GpuKind gk = GK_NONE;
    int threads = 0;

    void init() {
        auto t = read_cpu_times();
        prevCpu = t;
        s.cores.assign(t.size() > 1 ? t.size() - 1 : 0, Gauge());
        threads = (int)s.cores.size();
        find_cpu_temp();
        find_disks();
        find_gpu();
        net_prev = read_net();
        io_prev = read_io();
        t_prev = now_sec();
    }

    void wait_gpu(double timeout) {
        if (gk != GK_SMI) return;
        double t0 = now_sec();
        while (!feed.ready() && now_sec() - t0 < timeout) usleep(20000);
        if (!feed.ready()) {
            gk = pciNvidia.empty() ? GK_NONE : gk;
        }
    }

    void finalize_layout() { s.hasGpu = (gk != GK_NONE); }

    void shutdown() {
        feed.stop();
        nvml.close();
    }

    void sample() {
        double t = now_sec();
        double dt = std::max(0.05, t - t_prev);
        t_prev = t;

        auto cur = read_cpu_times();
        if (cur.size() == prevCpu.size() && !cur.empty()) {
            auto usage = [&](size_t i) {
                double dtot = (double)cur[i].total - (double)prevCpu[i].total;
                double didle = (double)cur[i].idle - (double)prevCpu[i].idle;
                return dtot > 0 ? clamp01((float)(1.0 - didle / dtot)) : 0.f;
            };
            s.cpu.target = usage(0);
            s.cpuH.push(s.cpu.target);
            for (size_t i = 0; i < s.cores.size(); i++) s.cores[i].target = usage(i + 1);
        }
        prevCpu = cur;
        double maxk = 0;
        for (int i = 0; i < threads; i++) {
            double k = atof(first_line("/sys/devices/system/cpu/cpu" + std::to_string(i) + "/cpufreq/scaling_cur_freq").c_str());
            maxk = std::max(maxk, k);
        }
        s.mhz = maxk > 0 ? maxk / 1000.0 : 0;
        if (!tempPath.empty()) {
            double v = atof(first_line(tempPath).c_str());
            s.tempC = v > 0 ? v / 1000.0 : -1;
        }
        {
            string la = first_line("/proc/loadavg");
            int running = 0, total = 0;
            sscanf(la.c_str(), "%lf %lf %lf %d/%d", &s.load1, &s.load5, &s.load15, &running, &total);
            s.procs = total;
        }

        auto mi = read_meminfo();
        s.memTotal = mi["MemTotal"];
        double avail = mi.count("MemAvailable") ? mi["MemAvailable"] : mi["MemFree"];
        s.memUsedKiB = s.memTotal - avail;
        s.memCacheKiB = std::max(0.0, avail - mi["MemFree"]);
        if (s.memTotal > 0) {
            s.memUsed.target = clamp01((float)(s.memUsedKiB / s.memTotal));
            s.memCache.target = clamp01((float)(s.memCacheKiB / s.memTotal));
            s.memH.push(s.memUsed.target);
        }
        s.swapTotal = mi["SwapTotal"];
        s.swapUsed = s.swapTotal - mi["SwapFree"];
        s.swap.target = s.swapTotal > 0 ? clamp01((float)(s.swapUsed / s.swapTotal)) : 0;

        sample_gpu();

        for (auto& d : s.disks) {
            struct statvfs sv;
            if (statvfs(d.mount.c_str(), &sv) == 0) {
                d.total = (double)sv.f_blocks * sv.f_frsize;
                d.used = d.total - (double)sv.f_bfree * sv.f_frsize;
                d.g.target = d.total > 0 ? clamp01((float)(d.used / d.total)) : 0;
            }
        }
        auto io = read_io();
        s.ioR = std::max(0.0, (io.first - io_prev.first) / dt);
        s.ioW = std::max(0.0, (io.second - io_prev.second) / dt);
        io_prev = io;
        auto nt = read_net();
        s.netRx = std::max(0.0, (nt.first - net_prev.first) / dt);
        s.netTx = std::max(0.0, (nt.second - net_prev.second) / dt);
        net_prev = nt;
        s.rxH.push((float)s.netRx);
        s.txH.push((float)s.netTx);
        s.uptime = atof(first_line("/proc/uptime").c_str());
    }

private:
    vector<CpuT> prevCpu;
    string tempPath, amdPath, amdName, amdHwmon, pciNvidia;
    std::pair<double, double> net_prev{0, 0}, io_prev{0, 0};
    double t_prev = 0;

    bool gpu_asleep() {
        string p;
        if (gk == GK_AMD) p = amdPath;
        else if (!pciNvidia.empty()) p = pciNvidia;
        if (p.empty()) return false;
        return first_line(p + "/power/runtime_status") == "suspended";
    }

    void find_gpu() {
        if (DIR* d = opendir("/sys/bus/pci/devices")) {
            while (dirent* e = readdir(d)) {
                if (e->d_name[0] == '.') continue;
                string base = string("/sys/bus/pci/devices/") + e->d_name;
                if (first_line(base + "/vendor") == "0x10de" && first_line(base + "/class").rfind("0x03", 0) == 0) {
                    pciNvidia = base;
                    break;
                }
            }
            closedir(d);
        }
        bool asleep = !pciNvidia.empty() && first_line(pciNvidia + "/power/runtime_status") == "suspended";
        if (asleep) {
            gk = GK_NVML;
            s.gpuName = "NVIDIA GPU";
            return;
        }
        string nm;
        if (nvml.init(nm)) {
            gk = GK_NVML;
            s.gpuName = nm;
            return;
        }
        if (in_path("nvidia-smi") && feed.start()) {
            gk = GK_SMI;
            return;
        }
        for (int i = 0; i < 6; i++) {
            string base = "/sys/class/drm/card" + std::to_string(i) + "/device";
            if (access((base + "/gpu_busy_percent").c_str(), R_OK) == 0 &&
                access((base + "/mem_info_vram_total").c_str(), R_OK) == 0) {
                amdPath = base;
                amdName = first_line(base + "/product_name");
                if (amdName.empty()) amdName = "AMD Radeon GPU";
                for (int h = 0; h < 8; h++) {
                    string hb = base + "/hwmon/hwmon" + std::to_string(h);
                    if (access((hb + "/temp1_input").c_str(), R_OK) == 0) {
                        amdHwmon = hb;
                        break;
                    }
                }
                gk = GK_AMD;
                s.gpuName = amdName;
                return;
            }
        }
    }

    void sample_gpu() {
        if (gk == GK_NONE) return;
        if (gk != GK_SMI && gpu_asleep()) {
            s.gpuAsleep = true;
            s.gpuUtil.target = 0;
            s.gpuTemp = s.gpuPower = s.gpuClock = s.gpuFan = -1;
            s.gpuH.push(0);
            s.vramH.push(s.vram.target);
            return;
        }
        s.gpuAsleep = false;
        if (gk == GK_NVML) {
            string nm;
            if (!nvml.ok && !nvml.init(nm)) return;
            if (!nm.empty()) s.gpuName = nm;
            Nvml::Util u{0, 0};
            Nvml::Mem m{0, 0, 0};
            unsigned v = 0;
            if (nvml.GetUtil(nvml.dev, &u) == 0) s.gpuUtil.target = clamp01(u.gpu / 100.0f);
            if (nvml.GetMem(nvml.dev, &m) == 0) {
                s.vramUsed = m.used / 1048576.0;
                s.vramTotal = m.total / 1048576.0;
                s.vram.target = m.total ? clamp01((float)((double)m.used / (double)m.total)) : 0;
            }
            s.gpuTemp = (nvml.GetTemp && nvml.GetTemp(nvml.dev, 0, &v) == 0) ? (double)v : -1;
            s.gpuPower = (nvml.GetPower && nvml.GetPower(nvml.dev, &v) == 0) ? v / 1000.0 : -1;
            s.gpuClock = (nvml.GetClock && nvml.GetClock(nvml.dev, 0, &v) == 0) ? (double)v : -1;
            s.gpuFan = (nvml.GetFan && nvml.GetFan(nvml.dev, &v) == 0) ? (double)v : -1;
        } else if (gk == GK_SMI) {
            std::lock_guard<std::mutex> lk(feed.mu);
            s.gpuName = feed.name;
            s.gpuUtil.target = clamp01((float)(std::max(0.0, feed.util) / 100.0));
            s.vramUsed = std::max(0.0, feed.memUsed);
            s.vramTotal = feed.memTotal;
            s.vram.target = s.vramTotal > 0 ? clamp01((float)(s.vramUsed / s.vramTotal)) : 0;
            s.gpuTemp = feed.temp;
            s.gpuPower = feed.power;
            s.gpuClock = feed.clock;
            s.gpuFan = feed.fan;
        } else if (gk == GK_AMD) {
            s.gpuUtil.target = clamp01((float)(atof(first_line(amdPath + "/gpu_busy_percent").c_str()) / 100.0));
            s.vramUsed = atof(first_line(amdPath + "/mem_info_vram_used").c_str()) / 1048576.0;
            s.vramTotal = atof(first_line(amdPath + "/mem_info_vram_total").c_str()) / 1048576.0;
            s.vram.target = s.vramTotal > 0 ? clamp01((float)(s.vramUsed / s.vramTotal)) : 0;
            if (!amdHwmon.empty()) {
                double tt = atof(first_line(amdHwmon + "/temp1_input").c_str());
                s.gpuTemp = tt > 0 ? tt / 1000.0 : -1;
                double pw = atof(first_line(amdHwmon + "/power1_average").c_str());
                s.gpuPower = pw > 0 ? pw / 1e6 : -1;
            }
        }
        s.gpuH.push(s.gpuUtil.target);
        s.vramH.push(s.vram.target);
    }

    void find_cpu_temp() {
        for (int i = 0; i < 24 && tempPath.empty(); i++) {
            string base = "/sys/class/hwmon/hwmon" + std::to_string(i);
            string name = first_line(base + "/name");
            if (name == "k10temp" || name == "zenpower") {
                tempPath = base + "/temp1_input";
            } else if (name == "coretemp") {
                for (int j = 1; j < 12; j++) {
                    string lab = first_line(base + "/temp" + std::to_string(j) + "_label");
                    if (lab.rfind("Package", 0) == 0) tempPath = base + "/temp" + std::to_string(j) + "_input";
                }
            }
        }
        if (tempPath.empty()) {
            for (int i = 0; i < 12; i++) {
                string base = "/sys/class/thermal/thermal_zone" + std::to_string(i);
                string type = first_line(base + "/type");
                if (type == "x86_pkg_temp" || type == "cpu-thermal") {
                    tempPath = base + "/temp";
                    break;
                }
            }
        }
    }

    void find_disks() {
        static const std::set<string> fsok = {"ext2", "ext3", "ext4", "btrfs", "xfs", "f2fs", "ntfs", "ntfs3",
                                              "fuseblk", "exfat", "zfs", "bcachefs", "jfs"};
        std::ifstream f("/proc/mounts");
        string line;
        std::set<string> seen;
        while (std::getline(f, line) && s.disks.size() < 4) {
            std::stringstream ss(line);
            string src, mnt, fs;
            ss >> src >> mnt >> fs;
            if (!fsok.count(fs)) continue;
            for (size_t p; (p = mnt.find("\\040")) != string::npos;) mnt.replace(p, 4, " ");
            if (mnt == "/boot" || mnt == "/boot/efi") continue;
            bool user_mount = mnt.rfind("/run/media", 0) == 0 || mnt.rfind("/mnt", 0) == 0 ||
                              mnt.rfind("/media", 0) == 0 || mnt == "/home" || mnt == "/";
            if (!user_mount || seen.count(src)) continue;
            seen.insert(src);
            DiskInfo d;
            d.mount = mnt;
            d.fs = fs;
            d.source = src;
            s.disks.push_back(d);
        }
    }

    std::pair<double, double> read_net() {
        std::ifstream f("/proc/net/dev");
        string line;
        double rx = 0, tx = 0;
        std::getline(f, line);
        std::getline(f, line);
        while (std::getline(f, line)) {
            size_t p = line.find(':');
            if (p == string::npos) continue;
            string ifc = trim(line.substr(0, p));
            if (ifc == "lo" || ifc.rfind("docker", 0) == 0 || ifc.rfind("veth", 0) == 0 || ifc.rfind("br-", 0) == 0 ||
                ifc.rfind("virbr", 0) == 0 || ifc.rfind("cni", 0) == 0 || ifc.rfind("flannel", 0) == 0)
                continue;
            unsigned long long v[16] = {0};
            sscanf(line.c_str() + p + 1, "%llu %llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2], &v[3],
                   &v[4], &v[5], &v[6], &v[7], &v[8]);
            rx += (double)v[0];
            tx += (double)v[8];
        }
        return {rx, tx};
    }

    std::pair<double, double> read_io() {
        std::ifstream f("/proc/diskstats");
        string line;
        double rd = 0, wr = 0;
        while (std::getline(f, line)) {
            std::stringstream ss(line);
            int maj, min;
            string name;
            unsigned long long a[8] = {0};
            ss >> maj >> min >> name >> a[0] >> a[1] >> a[2] >> a[3] >> a[4] >> a[5] >> a[6];
            if (!is_physical_disk(name)) continue;
            rd += (double)a[2] * 512;
            wr += (double)a[6] * 512;
        }
        return {rd, wr};
    }
};

static string fmt_pair_kib(double used_kib, double total_kib) {
    if (total_kib >= 1048576.0) {
        const char* f = total_kib >= 100.0 * 1048576.0 ? "%.0f" : "%.2f";
        return fmt(f, used_kib / 1048576.0) + " / " + fmt(f, total_kib / 1048576.0) + " GiB";
    }
    return fmt("%.0f", used_kib / 1024.0) + " / " + fmt("%.0f", total_kib / 1024.0) + " MiB";
}
static string fmt_rate(double bps) {
    if (bps >= 1073741824.0) return fmt("%.2f", bps / 1073741824.0) + " GiB/s";
    if (bps >= 1048576.0) return fmt("%.2f", bps / 1048576.0) + " MiB/s";
    if (bps >= 1024.0) return fmt("%.1f", bps / 1024.0) + " KiB/s";
    return fmt("%.0f", bps) + " B/s";
}
static string fmt_uptime(double sec) {
    long s = (long)sec;
    long d = s / 86400, h = (s % 86400) / 3600, m = (s % 3600) / 60, x = s % 60;
    string o;
    auto add = [&](long n, const char* one, const char* many, bool force) {
        if (n == 0 && o.empty() && !force) return;
        if (!o.empty()) o += ", ";
        o += std::to_string(n) + " " + (n == 1 ? one : many);
    };
    add(d, "day", "days", false);
    add(h, "hour", "hours", false);
    add(m, "min", "mins", false);
    add(x, "sec", "secs", true);
    return o;
}
static string deg_c(double v) { return fmt("%.0f", v) + (g_caps.unicode ? " °C" : " C"); }

struct Layout {
    int bw = 24;
    int level = 0;
    int panelw = 0;
};
static constexpr int LBL = 9;
static constexpr int VALW = 22;

static vector<Line> build_info(const Stats& s, const StaticInfo& si, const Layout& L, Color accent) {
    vector<Line> out;
    const bool blanks = L.level <= 0, cores_row = L.level <= 0, io_row = L.level <= 0;
    const bool pk_row = L.level <= 1, term_row = L.level <= 1, swap_row = L.level <= 1;
    const bool wm_row = L.level <= 2, sep_row = L.level <= 2;

    auto label = [&](Line& l, const string& t) {
        l.add("  ");
        l.add(pad_str(t, LBL), accent, Color(), A_BOLD);
    };
    auto kv = [&](const string& k, const string& v, Color c = TXT) {
        Line l;
        label(l, k);
        l.add(v, c);
        if (l.len() > L.panelw) l.c.resize(L.panelw);
        out.push_back(l);
    };
    auto blank = [&]() {
        if (blanks) out.push_back(Line());
    };

    {
        Line l;
        l.add("  ");
        l.add(si.user, accent, Color(), A_BOLD);
        l.add("@", DIMC);
        l.add(si.host, accent, Color(), A_BOLD);
        int title = l.len() - 2;
        int pad = L.panelw - l.len() - 6;
        for (int i = 0; i < pad; i++) l.add(U' ');
        l.add(g_caps.unicode ? U'●' : U'*', s.pulse ? heat(0.0f) : col(RGB{50, 110, 66}));
        l.add(" live", DIMC);
        out.push_back(l);
        if (sep_row) {
            Line sep;
            sep.add("  ");
            for (int i = 0; i < title; i++) sep.add(g_caps.unicode ? U'─' : U'-', FAINT);
            out.push_back(sep);
        }
    }

    kv("OS", si.os);
    kv("Kernel", si.kernel);
    kv("Uptime", fmt_uptime(s.uptime));
    string pk, shv;
    {
        std::lock_guard<std::mutex> lk(g_async.mu);
        pk = g_async.packages;
        shv = g_async.shellVer;
    }
    if (pk_row) kv("Packages", pk);
    if (L.level <= 2) kv("Shell", si.shell + (shv.empty() ? "" : " " + shv));
    if (wm_row) kv("WM", si.wm);
    if (term_row) kv("Terminal", si.terminal);
    blank();

    kv("CPU", si.cpuModel);
    {
        Line l;
        label(l, "Usage");
        bar_cells(l, s.cpu.shown, L.bw);
        l.add(" ");
        pct_cells(l, s.cpu.target);
        if (s.tempC >= 0) l.add("  " + deg_c(s.tempC), heat((float)((s.tempC - 35) / 55)));
        out.push_back(l);
    }
    if (cores_row) {
        Line l;
        label(l, "Cores");
        for (auto& c : s.cores) {
            int lvl = (int)std::lround(clamp01(c.shown) * 7);
            l.add(g_caps.unicode ? BLOCKS[lvl] : (char32_t)ASCII_SPARK[lvl], heat(c.shown));
        }
        if (s.mhz > 0) l.add("  " + fmt("%.2f", s.mhz / 1000.0) + " GHz", DIMC);
        out.push_back(l);
    }

    if (s.hasGpu) {
        kv("GPU", s.gpuName.empty() ? "GPU" : s.gpuName);
        {
            Line l;
            label(l, "Usage");
            bar_cells(l, s.gpuUtil.shown, L.bw);
            l.add(" ");
            if (s.gpuAsleep) {
                l.add("asleep", DIMC);
            } else {
                pct_cells(l, s.gpuUtil.target);
                if (s.gpuTemp >= 0) l.add("  " + deg_c(s.gpuTemp), heat((float)((s.gpuTemp - 35) / 55)));
                if (s.gpuPower >= 0) l.add("  " + fmt("%.0f", s.gpuPower) + " W", DIMC);
            }
            out.push_back(l);
        }
        {
            Line l;
            label(l, "VRAM");
            bar_cells(l, s.vram.shown, L.bw);
            l.add(" " + fmt("%.0f", s.vramUsed) + " / " + fmt("%.0f", s.vramTotal) + " MiB ", TXT);
            pct_cells(l, s.vram.target);
            out.push_back(l);
        }
    }
    {
        Line l;
        label(l, "RAM");
        stacked_cells(l, s.memUsed.shown, s.memCache.shown, heat(s.memUsed.shown), L.bw);
        l.add(" " + fmt_pair_kib(s.memUsedKiB, s.memTotal) + " ", TXT);
        pct_cells(l, s.memUsed.target);
        out.push_back(l);
    }
    if (swap_row && s.swapTotal > 0) {
        Line l;
        label(l, "Swap");
        bar_cells(l, s.swap.shown, L.bw);
        l.add(" " + fmt_pair_kib(s.swapUsed, s.swapTotal), TXT);
        out.push_back(l);
    }
    blank();

    for (auto& d : s.disks) {
        string name = d.mount;
        if (name.size() > (size_t)LBL - 1) {
            size_t p = name.rfind('/');
            name = (p != string::npos && p + 1 < name.size()) ? name.substr(p + 1) : name;
            if (name.size() > (size_t)LBL - 1) name = name.substr(0, LBL - 1);
        }
        Line l;
        label(l, name);
        bar_cells(l, d.g.shown, L.bw);
        l.add(" " + fmt_pair_kib(d.used / 1024.0, d.total / 1024.0) + " ", TXT);
        pct_cells(l, d.g.target);
        out.push_back(l);
    }
    if (io_row) {
        Line l;
        label(l, "Disk I/O");
        l.add("R ", DIMC);
        l.add(fmt_rate(s.ioR), TXT);
        l.add("  W ", DIMC);
        l.add(fmt_rate(s.ioW), TXT);
        out.push_back(l);
    }
    {
        Line l;
        label(l, "Network");
        l.add("Down ", DIMC);
        l.add(fmt_rate(s.netRx), cool(0.9f));
        l.add("  Up ", DIMC);
        l.add(fmt_rate(s.netTx), cool(0.5f));
        out.push_back(l);
    }
    return out;
}

static void compose(const Scene& sc, const Mat3& R, const vector<Line>& info, int frameH, const Layout& L, Grid& g,
                    Grid& canvas, bool skip_logo = false) {
    int W = sc.CW + GAP + L.panelw;
    if (g.w != W || g.h != frameH) g = Grid(W, frameH);
    else std::fill(g.c.begin(), g.c.end(), Cell());
    if (canvas.w != sc.CW || canvas.h != sc.CH) canvas = Grid(sc.CW, sc.CH);
    if (!skip_logo) sc.canvas(R, canvas);
    int logo_off = (frameH - sc.CH) / 2;
    int info_off = (frameH - (int)info.size()) / 2;
    for (int r = 0; r < frameH; r++) {
        int ci = r - logo_off, ii = r - info_off;
        if (!skip_logo && ci >= 0 && ci < sc.CH)
            for (int x = 0; x < sc.CW; x++) g.at(x, r) = canvas.at(x, ci);
        if (ii >= 0 && ii < (int)info.size()) {
            Cell d;
            d.ch = g_caps.unicode ? U'│' : U'|';
            d.fg = FAINT;
            g.at(sc.CW + GAP / 2, r) = d;
            const Line& ln = info[ii];
            for (int x = 0; x < ln.len() && sc.CW + GAP + x < W; x++) g.at(sc.CW + GAP + x, r) = ln.c[x];
        }
    }
}

static volatile sig_atomic_t g_stop = 0;
static void on_stop(int) { g_stop = 1; }
static volatile sig_atomic_t g_pause = 0;
static void on_release(int) { g_pause = 1; }
static bool g_write_failed = false;

static void write_all(const string& s) {
    size_t off = 0;
    while (off < s.size()) {
        ssize_t n = write(1, s.data() + off, s.size() - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN) { usleep(1000); continue; }
            g_write_failed = true;
            return;
        }
        off += (size_t)n;
    }
}

static void term_size(int& cols, int& rows) {
    winsize ws{};
    if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
        cols = ws.ws_col;
        rows = ws.ws_row;
    } else {
        cols = 80;
        rows = 24;
    }
}

namespace kg {

static const uint8_t FONT[95][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00}, {0x00, 0x00, 0x5F, 0x00, 0x00}, {0x00, 0x07, 0x00, 0x07, 0x00},
    {0x14, 0x7F, 0x14, 0x7F, 0x14}, {0x24, 0x2A, 0x7F, 0x2A, 0x12}, {0x23, 0x13, 0x08, 0x64, 0x62},
    {0x36, 0x49, 0x56, 0x20, 0x50}, {0x00, 0x05, 0x03, 0x00, 0x00}, {0x00, 0x1C, 0x22, 0x41, 0x00},
    {0x00, 0x41, 0x22, 0x1C, 0x00}, {0x14, 0x08, 0x3E, 0x08, 0x14}, {0x08, 0x08, 0x3E, 0x08, 0x08},
    {0x00, 0x50, 0x30, 0x00, 0x00}, {0x08, 0x08, 0x08, 0x08, 0x08}, {0x00, 0x60, 0x60, 0x00, 0x00},
    {0x20, 0x10, 0x08, 0x04, 0x02}, {0x3E, 0x51, 0x49, 0x45, 0x3E}, {0x00, 0x42, 0x7F, 0x40, 0x00},
    {0x42, 0x61, 0x51, 0x49, 0x46}, {0x21, 0x41, 0x45, 0x4B, 0x31}, {0x18, 0x14, 0x12, 0x7F, 0x10},
    {0x27, 0x45, 0x45, 0x45, 0x39}, {0x3C, 0x4A, 0x49, 0x49, 0x30}, {0x01, 0x71, 0x09, 0x05, 0x03},
    {0x36, 0x49, 0x49, 0x49, 0x36}, {0x06, 0x49, 0x49, 0x29, 0x1E}, {0x00, 0x36, 0x36, 0x00, 0x00},
    {0x00, 0x56, 0x36, 0x00, 0x00}, {0x08, 0x14, 0x22, 0x41, 0x00}, {0x14, 0x14, 0x14, 0x14, 0x14},
    {0x00, 0x41, 0x22, 0x14, 0x08}, {0x02, 0x01, 0x51, 0x09, 0x06}, {0x32, 0x49, 0x79, 0x41, 0x3E},
    {0x7E, 0x11, 0x11, 0x11, 0x7E}, {0x7F, 0x49, 0x49, 0x49, 0x36}, {0x3E, 0x41, 0x41, 0x41, 0x22},
    {0x7F, 0x41, 0x41, 0x22, 0x1C}, {0x7F, 0x49, 0x49, 0x49, 0x41}, {0x7F, 0x09, 0x09, 0x09, 0x01},
    {0x3E, 0x41, 0x49, 0x49, 0x7A}, {0x7F, 0x08, 0x08, 0x08, 0x7F}, {0x00, 0x41, 0x7F, 0x41, 0x00},
    {0x20, 0x40, 0x41, 0x3F, 0x01}, {0x7F, 0x08, 0x14, 0x22, 0x41}, {0x7F, 0x40, 0x40, 0x40, 0x40},
    {0x7F, 0x02, 0x0C, 0x02, 0x7F}, {0x7F, 0x04, 0x08, 0x10, 0x7F}, {0x3E, 0x41, 0x41, 0x41, 0x3E},
    {0x7F, 0x09, 0x09, 0x09, 0x06}, {0x3E, 0x41, 0x51, 0x21, 0x5E}, {0x7F, 0x09, 0x19, 0x29, 0x46},
    {0x46, 0x49, 0x49, 0x49, 0x31}, {0x01, 0x01, 0x7F, 0x01, 0x01}, {0x3F, 0x40, 0x40, 0x40, 0x3F},
    {0x1F, 0x20, 0x40, 0x20, 0x1F}, {0x3F, 0x40, 0x38, 0x40, 0x3F}, {0x63, 0x14, 0x08, 0x14, 0x63},
    {0x07, 0x08, 0x70, 0x08, 0x07}, {0x61, 0x51, 0x49, 0x45, 0x43}, {0x00, 0x7F, 0x41, 0x41, 0x00},
    {0x02, 0x04, 0x08, 0x10, 0x20}, {0x00, 0x41, 0x41, 0x7F, 0x00}, {0x04, 0x02, 0x01, 0x02, 0x04},
    {0x40, 0x40, 0x40, 0x40, 0x40}, {0x00, 0x01, 0x02, 0x04, 0x00}, {0x20, 0x54, 0x54, 0x54, 0x78},
    {0x7F, 0x48, 0x44, 0x44, 0x38}, {0x38, 0x44, 0x44, 0x44, 0x20}, {0x38, 0x44, 0x44, 0x48, 0x7F},
    {0x38, 0x54, 0x54, 0x54, 0x18}, {0x08, 0x7E, 0x09, 0x01, 0x02}, {0x08, 0x14, 0x54, 0x54, 0x3C},
    {0x7F, 0x08, 0x04, 0x04, 0x78}, {0x00, 0x44, 0x7D, 0x40, 0x00}, {0x20, 0x40, 0x44, 0x3D, 0x00},
    {0x00, 0x7F, 0x10, 0x28, 0x44}, {0x00, 0x41, 0x7F, 0x40, 0x00}, {0x7C, 0x04, 0x18, 0x04, 0x78},
    {0x7C, 0x08, 0x04, 0x04, 0x78}, {0x38, 0x44, 0x44, 0x44, 0x38}, {0x7C, 0x14, 0x14, 0x14, 0x08},
    {0x08, 0x14, 0x14, 0x18, 0x7C}, {0x7C, 0x08, 0x04, 0x04, 0x08}, {0x48, 0x54, 0x54, 0x54, 0x20},
    {0x04, 0x3F, 0x44, 0x40, 0x20}, {0x3C, 0x40, 0x40, 0x20, 0x7C}, {0x1C, 0x20, 0x40, 0x20, 0x1C},
    {0x3C, 0x40, 0x30, 0x40, 0x3C}, {0x44, 0x28, 0x10, 0x28, 0x44}, {0x0C, 0x50, 0x50, 0x50, 0x3C},
    {0x44, 0x64, 0x54, 0x4C, 0x44}, {0x00, 0x08, 0x36, 0x41, 0x00}, {0x00, 0x00, 0x7F, 0x00, 0x00},
    {0x00, 0x41, 0x36, 0x08, 0x00}, {0x02, 0x01, 0x02, 0x04, 0x02}};

static constexpr int KLAYERS = 6;
static constexpr double KDEPTH = 0.04;
static constexpr int KSS = 2;
static constexpr double PX_MAX = 300000.0;
static constexpr int DEFAULT_FRAMES = 80;

typedef int (*Compress2Fn)(unsigned char*, unsigned long*, const unsigned char*, unsigned long, int);
typedef unsigned long (*BoundFn)(unsigned long);
static Compress2Fn g_compress = nullptr;
static BoundFn g_bound = nullptr;

static bool load_zlib() {
    if (g_compress && g_bound) return true;
    void* lib = dlopen("libz.so.1", RTLD_LAZY);
    if (!lib) lib = dlopen("libz.so", RTLD_LAZY);
    if (!lib) return false;
    g_compress = reinterpret_cast<Compress2Fn>(dlsym(lib, "compress2"));
    g_bound = reinterpret_cast<BoundFn>(dlsym(lib, "compressBound"));
    return g_compress && g_bound;
}

static string b64(const string& in) {
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    string o;
    o.reserve((in.size() + 2) / 3 * 4);
    auto B = [&](size_t i) { return (uint32_t)(uint8_t)in[i]; };
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        uint32_t v = (B(i) << 16) | (B(i + 1) << 8) | B(i + 2);
        o += T[v >> 18];
        o += T[(v >> 12) & 63];
        o += T[(v >> 6) & 63];
        o += T[v & 63];
    }
    if (i + 1 == in.size()) {
        uint32_t v = B(i) << 16;
        o += T[v >> 18];
        o += T[(v >> 12) & 63];
        o += "==";
    } else if (i + 2 == in.size()) {
        uint32_t v = (B(i) << 16) | (B(i + 1) << 8);
        o += T[v >> 18];
        o += T[(v >> 12) & 63];
        o += T[(v >> 6) & 63];
        o += '=';
    }
    return o;
}

static string apc(const string& ctrl, const string& payload) {
    string b = b64(payload), o;
    size_t off = 0;
    bool first = true;
    do {
        size_t n = std::min<size_t>(4096, b.size() - off);
        bool more = off + n < b.size();
        o += "\x1b_G";
        if (first) o += ctrl + ",";
        o += more ? "m=1" : "m=0";
        o += ';';
        o.append(b, off, n);
        o += "\x1b\\";
        off += n;
        first = false;
    } while (off < b.size());
    return o;
}

static float sstep(float a, float b, float x) {
    float t = clamp01((x - a) / (b - a));
    return t * t * (3 - 2 * t);
}

static float glyph_alpha(char32_t c, int x, int y, int cw, int ch) {
    float fx = (x + 0.5f) / cw, fy = (y + 0.5f) / ch;
    if (c == U' ') return 0;
    if (c >= 0x2800 && c <= 0x28FF) {
        static const int bit_col[8] = {0, 0, 0, 1, 1, 1, 0, 1};
        static const int bit_row[8] = {0, 1, 2, 0, 1, 2, 3, 3};
        float rad = std::min(cw / 4.0f, ch / 8.0f) * 0.62f, best = 0;
        for (int b = 0; b < 8; b++) {
            if (!((c - 0x2800) & (1u << b))) continue;
            float dx = x + 0.5f - (bit_col[b] + 0.5f) * cw / 2.0f, dy = y + 0.5f - (bit_row[b] + 0.5f) * ch / 4.0f;
            best = std::max(best, clamp01(rad + 0.5f - std::sqrt(dx * dx + dy * dy)));
        }
        return best;
    }
    switch (c) {
        case U'█': return 1;
        case U'▀': return fy < 0.5f ? 1.f : 0.f;
        case U'▄': return fy >= 0.5f ? 1.f : 0.f;
        case U'▌': return fx < 0.5f ? 1.f : 0.f;
        case U'▐': return fx >= 0.5f ? 1.f : 0.f;
        case U'░': return 0.25f;
        case U'▒': return 0.5f;
        case U'▓': return 0.75f;
        default: break;
    }
    if (c >= 0x2581 && c <= 0x2587) return fy >= 1.0f - (float)(c - 0x2580) / 8.0f ? 1.f : 0.f;
    if (c >= 0x2589 && c <= 0x258F) return fx < (float)(0x2590 - c) / 8.0f ? 1.f : 0.f;
    if (c >= 33 && c <= 126) {
        const uint8_t* g = FONT[c - 32];
        float u = (fx - 0.08f) / 0.84f * 5.f - 0.5f, v = (fy - 0.10f) / 0.80f * 7.f - 0.5f;
        auto bit = [&](int i, int j) -> float {
            if (i < 0 || i >= 5 || j < 0 || j >= 7) return 0.f;
            return (float)((g[i] >> j) & 1);
        };
        int i0 = (int)std::floor(u), j0 = (int)std::floor(v);
        float du = u - i0, dv = v - j0;
        float val = bit(i0, j0) * (1 - du) * (1 - dv) + bit(i0 + 1, j0) * du * (1 - dv) +
                    bit(i0, j0 + 1) * (1 - du) * dv + bit(i0 + 1, j0 + 1) * du * dv;
        return sstep(0.1f, 0.5f, val);
    }
    float dx = (fx - 0.5f) * cw, dy = (fy - 0.5f) * ch;
    float rad = std::min(cw, ch) * 0.35f;
    return clamp01(rad + 0.5f - std::sqrt(dx * dx + dy * dy)) * std::max(0.4f, glyph_ink(c));
}

struct Tex {
    int w = 0, h = 0;
    vector<float> p;
};

static Tex build_tex(const Scene& sc, int cw, int ch) {
    Tex t;
    t.w = sc.W * cw;
    t.h = sc.H * ch;
    t.p.assign((size_t)t.w * t.h * 4, 0.f);
    for (int r = 0; r < sc.H; r++)
        for (int c = 0; c < sc.W; c++) {
            const Cell& cell = sc.logo.rows[r][c];
            bool has_bg = cell.bg != Color();
            if (cell.ch == U' ' && !has_bg) continue;
            RGBf f = gfx_rgb(cell.fg), b = gfx_rgb(cell.bg);
            for (int y = 0; y < ch; y++)
                for (int x = 0; x < cw; x++) {
                    float a = glyph_alpha(cell.ch, x, y, cw, ch);
                    float R, G, B, A;
                    if (has_bg) {
                        R = f.r * a + b.r * (1 - a);
                        G = f.g * a + b.g * (1 - a);
                        B = f.b * a + b.b * (1 - a);
                        A = 1;
                    } else {
                        R = f.r * a;
                        G = f.g * a;
                        B = f.b * a;
                        A = a;
                    }
                    float* q = &t.p[(((size_t)(r * ch + y)) * t.w + (c * cw + x)) * 4];
                    q[0] = R;
                    q[1] = G;
                    q[2] = B;
                    q[3] = A;
                }
        }
    return t;
}

static void render(const Scene& sc, const Tex& tx, int cw, int chh, const Mat3& R, uint8_t* out) {
    const int fw = sc.CW * cw, fh = sc.CH * chh;
    const double D = sc.D, KT = KDEPTH * sc.W, S = D / (D - KT / 2), halfW = sc.W / 2.0, halfH = sc.H / 2.0;
    double zl[KLAYERS];
    for (int i = 0; i < KLAYERS; i++) zl[i] = KT / 2 - KT * i / (KLAYERS - 1);
    const double nx = R.m[0][2], ny = R.m[1][2], nz = R.m[2][2];
    const bool back = nz < 0;
    const double nzD = nz * D;
    const int faceL = back ? KLAYERS - 1 : 0;
    const double Lx = -0.35, Ly = -0.45, Lz = 0.82;
    double vs = back ? -1.0 : 1.0;
    double lit = std::max(0.0, std::min(1.0, vs * (nx * Lx + ny * Ly + nz * Lz) / Lz));
    float faceB = (float)(AMBIENT + (1.0 - AMBIENT) * lit);
    if (back) faceB *= 0.85f;
    float sideB = (float)(0.52 + 0.12 * lit);

    struct Hit {
        double t;
        float a, r, g, b;
    };
    std::memset(out, 0, (size_t)fw * fh * 4);
    int bx0 = fw, by0 = fh, bx1 = -1, by1 = -1;
    for (int cx = 0; cx < 2; cx++)
        for (int cy = 0; cy < 2; cy++)
            for (int cz = 0; cz < 2; cz++) {
                double l0 = (cx ? 1 : -1) * halfW, l1 = (cy ? 1 : -1) * (double)sc.H, l2 = (cz ? 1 : -1) * S * KT / 2;
                double x = (R.m[0][0] * l0 + R.m[0][1] * l1 + R.m[0][2] * l2) / S;
                double y = (R.m[1][0] * l0 + R.m[1][1] * l1 + R.m[1][2] * l2) / S;
                double z = (R.m[2][0] * l0 + R.m[2][1] * l1 + R.m[2][2] * l2) / S;
                double dd = D - z;
                if (dd < D * 0.2) dd = D * 0.2;
                double px0 = x * D / dd, py0 = y * D / dd;
                int X = (int)std::lround((px0 + sc.CW / 2.0) * cw), Y = (int)std::lround((py0 / 2 + sc.CH / 2.0) * chh);
                bx0 = std::min(bx0, X);
                bx1 = std::max(bx1, X);
                by0 = std::min(by0, Y);
                by1 = std::max(by1, Y);
            }
    bx0 = std::max(0, bx0 - 3);
    by0 = std::max(0, by0 - 3);
    bx1 = std::min(fw - 1, bx1 + 3);
    by1 = std::min(fh - 1, by1 + 3);
    for (int Y = by0; Y <= by1; Y++) {
        for (int X = bx0; X <= bx1; X++) {
            float sumA = 0, sr = 0, sg = 0, sb = 0;
            for (int j = 0; j < KSS; j++) {
                double py0 = ((Y + (j + 0.5) / KSS) / chh - sc.CH / 2.0) * 2;
                for (int i = 0; i < KSS; i++) {
                    double px0 = (X + (i + 0.5) / KSS) / cw - sc.CW / 2.0;
                    double den = nzD - nx * px0 - ny * py0;
                    if (std::fabs(den) < 1e-6) continue;
                    Hit hs[KLAYERS];
                    int nh = 0;
                    for (int l = 0; l < KLAYERS; l++) {
                        double t = (nzD - zl[l]) / den;
                        if (t <= 0) continue;
                        double px = t * px0, py = t * py0, pz = D * (1 - t);
                        double lx = S * (R.m[0][0] * px + R.m[1][0] * py + R.m[2][0] * pz);
                        double ly = S * (R.m[0][1] * px + R.m[1][1] * py + R.m[2][1] * pz);
                        double u = (lx + halfW) * cw - 0.5, v = (ly / 2 + halfH) * chh - 0.5;
                        if (u < -0.5 || v < -0.5 || u >= tx.w - 0.5 || v >= tx.h - 0.5) continue;
                        int u0 = (int)std::floor(u), v0 = (int)std::floor(v);
                        float fu = (float)(u - u0), fv = (float)(v - v0);
                        int u1 = std::min(tx.w - 1, u0 + 1), v1 = std::min(tx.h - 1, v0 + 1);
                        u0 = std::max(0, u0);
                        v0 = std::max(0, v0);
                        const float* p00 = &tx.p[((size_t)v0 * tx.w + u0) * 4];
                        const float* p10 = &tx.p[((size_t)v0 * tx.w + u1) * 4];
                        const float* p01 = &tx.p[((size_t)v1 * tx.w + u0) * 4];
                        const float* p11 = &tx.p[((size_t)v1 * tx.w + u1) * 4];
                        float w00 = (1 - fu) * (1 - fv), w10 = fu * (1 - fv), w01 = (1 - fu) * fv, w11 = fu * fv;
                        float pr = p00[0] * w00 + p10[0] * w10 + p01[0] * w01 + p11[0] * w11;
                        float pg = p00[1] * w00 + p10[1] * w10 + p01[1] * w01 + p11[1] * w11;
                        float pb = p00[2] * w00 + p10[2] * w10 + p01[2] * w01 + p11[2] * w11;
                        float a = p00[3] * w00 + p10[3] * w10 + p01[3] * w01 + p11[3] * w11;
                        if (a < 0.02f) continue;
                        float b = (l == faceL) ? faceB : sideB * (1.0f - 0.09f * (float)std::abs(l - faceL));
                        b *= (float)std::max(0.55, std::min(1.0, 1.0 - 0.9 * (t - 1.0)));
                        float k = b / a;
                        Hit h{t, a, pr * k, pg * k, pb * k};
                        int pos = nh++;
                        while (pos > 0 && hs[pos - 1].t > h.t) {
                            hs[pos] = hs[pos - 1];
                            pos--;
                        }
                        hs[pos] = h;
                    }
                    float acc = 0, cr = 0, cg = 0, cb = 0;
                    for (int h = 0; h < nh; h++) {
                        if (hs[h].a > acc) {
                            float w = hs[h].a - acc;
                            cr += w * hs[h].r;
                            cg += w * hs[h].g;
                            cb += w * hs[h].b;
                            acc = hs[h].a;
                        }
                    }
                    if (acc > 0) {
                        sumA += acc;
                        sr += cr;
                        sg += cg;
                        sb += cb;
                    }
                }
            }
            uint8_t* o = out + ((size_t)Y * fw + X) * 4;
            if (sumA <= 0.0005f) {
                o[0] = o[1] = o[2] = o[3] = 0;
                continue;
            }
            auto cl = [](float v) { return (uint8_t)std::max(0.f, std::min(255.f, v + 0.5f)); };
            o[0] = cl(sr / sumA);
            o[1] = cl(sg / sumA);
            o[2] = cl(sb / sumA);
            o[3] = cl(sumA / (KSS * KSS) * 255.0f);
        }
    }
}

static Mat3 pose(double u) {
    const double TAU = 2 * M_PI;
    auto sm = [](double x) {
        x = x < 0 ? 0 : (x > 1 ? 1 : x);
        return x * x * (3 - 2 * x);
    };
    double yaw = 2 * TAU * (u + 0.07 * std::sin(2 * TAU * u + 0.8));
    double pitch = 0.36 * std::sin(TAU * u + 0.7) + 0.13 * std::sin(3 * TAU * u + 2.0);
    double roll = 0.14 * std::sin(2 * TAU * u + 4.0) + 0.05 * std::sin(3 * TAU * u);
    double extra = 0;
    if (u > 0.62 && u < 0.88) extra = TAU * sm((u - 0.62) / 0.26);
    else if (u >= 0.88) extra = TAU;
    Mat3 base = mul(rotZ(roll), mul(rotX(pitch), rotY(yaw)));
    return orthonormalize(mul(rotX(extra), base));
}

static vector<string> build(const Scene& sc, int cw0, int ch0, int N, int& fw, int& fh) {
    if (!load_zlib() || N < 2) return {};
    double s = 1.0, px = (double)sc.CW * cw0 * sc.CH * ch0;
    if (px > PX_MAX) s = std::sqrt(PX_MAX / px);
    int cw = std::max(5, (int)(cw0 * s)), chh = std::max(10, (int)(ch0 * s));
    fw = sc.CW * cw;
    fh = sc.CH * chh;
    Tex tx = build_tex(sc, cw, chh);
    vector<string> out((size_t)N);
    std::atomic<int> next{0};
    auto work = [&]() {
        vector<uint8_t> buf((size_t)fw * fh * 4);
        vector<uint8_t> zb(g_bound(buf.size()));
        for (;;) {
            int k = next++;
            if (k >= N) break;
            render(sc, tx, cw, chh, pose((double)k / N), buf.data());
            unsigned long zl = zb.size();
            if (g_compress(zb.data(), &zl, buf.data(), buf.size(), 1) != 0) continue;
            out[(size_t)k].assign((const char*)zb.data(), zl);
        }
    };
    int nt = std::max(1, std::min(8, (int)std::thread::hardware_concurrency()));
    vector<std::thread> th;
    for (int i = 1; i < nt; i++) th.emplace_back(work);
    work();
    for (auto& t : th) t.join();
    for (auto& f : out)
        if (f.empty()) return {};
    return out;
}

struct Probe {
    bool ok = false;
    int cw = 10, ch = 20;
};

static bool hinted() {
    return env_or("TERM") == "xterm-kitty" || !env_or("KITTY_WINDOW_ID").empty();
}

static Probe probe() {
    Probe p;
    string pf = env_or("DEEPFETCH_GRAPHICS");
    if (pf == "0" || !isatty(0) || !isatty(1)) return p;
    bool force = pf == "1";
    if (!hinted() && !force) return p;
    if (!force && (!env_or("TMUX").empty() || !env_or("STY").empty())) return p;
    if (!load_zlib()) return p;
    winsize ws{};
    int cw = 0, ch = 0;
    if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0 && ws.ws_xpixel > 0 && ws.ws_ypixel > 0) {
        cw = ws.ws_xpixel / ws.ws_col;
        ch = ws.ws_ypixel / ws.ws_row;
    }
    termios old{};
    if (tcgetattr(0, &old) != 0) return p;
    termios raw = old;
    raw.c_lflag &= ~(tcflag_t)(ICANON | ECHO);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(0, TCSANOW, &raw) != 0) return p;
    const string q = "\x1b_Gi=31,s=1,v=1,a=q,t=d,f=24;AAAA\x1b\\\x1b[16t\x1b[c";
    ssize_t w = write(1, q.data(), q.size());
    (void)w;
    string buf;
    double end = now_sec() + 0.6;
    bool got_da = false;
    while (now_sec() < end && !got_da) {
        pollfd pf2{0, POLLIN, 0};
        if (poll(&pf2, 1, 50) <= 0) continue;
        char tmp[256];
        ssize_t n = read(0, tmp, sizeof tmp);
        if (n <= 0) break;
        buf.append(tmp, (size_t)n);
        size_t a = buf.find("\x1b[?");
        if (a != string::npos && buf.find('c', a) != string::npos) got_da = true;
    }
    tcsetattr(0, TCSANOW, &old);
    bool gfx = buf.find("_Gi=31;OK") != string::npos;
    size_t t = buf.find("\x1b[6;");
    if (t != string::npos) {
        int hh = 0, ww = 0;
        if (sscanf(buf.c_str() + t + 4, "%d;%d", &hh, &ww) == 2 && hh > 0 && ww > 0) {
            cw = ww;
            ch = hh;
        }
    }
    p.ok = gfx || force;
    if (cw > 0 && ch > 0) {
        p.cw = cw;
        p.ch = ch;
    }
    return p;
}

static void upload(const vector<string>& fr, int fw, int fh, unsigned id, int gap_ms) {
    char c[200];
    for (size_t k = 0; k < fr.size(); k++) {
        if (k == 0) snprintf(c, sizeof c, "a=t,f=32,o=z,s=%d,v=%d,i=%u,q=2", fw, fh, id);
        else snprintf(c, sizeof c, "a=f,f=32,o=z,s=%d,v=%d,i=%u,z=%d,X=1,q=2", fw, fh, id, gap_ms);
        write_all(apc(c, fr[k]));
    }
    string s;
    snprintf(c, sizeof c, "\x1b_Ga=a,i=%u,r=1,z=%d,q=2\x1b\\", id, gap_ms);
    s += c;
    snprintf(c, sizeof c, "\x1b_Ga=a,i=%u,s=3,v=1,q=2\x1b\\", id);
    s += c;
    write_all(s);
}

static string place_cmd(unsigned id, int cols, int rows) {
    char c[160];
    snprintf(c, sizeof c, "\x1b_Ga=p,i=%u,p=1,c=%d,r=%d,C=1,z=-1,q=2\x1b\\", id, cols, rows);
    return c;
}

static string place_at(unsigned id, int cols, int rows, int row1, int col1) {
    return "\x1b[" + std::to_string(row1) + ";" + std::to_string(col1) + "H" + place_cmd(id, cols, rows);
}

static string place_rel(unsigned id, int cols, int rows, int block_h, int logo_off) {
    string s = "\r\x1b[" + std::to_string(block_h) + "A";
    if (logo_off > 0) s += "\x1b[" + std::to_string(logo_off) + "B";
    s += place_cmd(id, cols, rows);
    s += "\x1b[" + std::to_string(block_h - logo_off) + "B\r";
    return s;
}

static unsigned new_id() {
    return (unsigned)(((uint64_t)(now_sec() * 1000.0) ^ ((uint64_t)getpid() << 8)) % 2000000000ull) + 1u;
}

}  // namespace kg

static string state_path() {
    const char* tn = nullptr;
    for (int fd : {1, 0, 2})
        if (isatty(fd) && (tn = ttyname(fd))) break;
    if (!tn) return "";
    string key = tn;
    for (char& c : key)
        if (c == '/') c = '_';
    string dir = env_or("XDG_RUNTIME_DIR");
    if (dir.empty() || access(dir.c_str(), W_OK) != 0) dir = "/tmp";
    return dir + "/deepfetch-" + std::to_string((int)getuid()) + key + ".state";
}

static bool read_state(const string& path, pid_t& pid, int& h) {
    std::ifstream f(path);
    long p = 0;
    int hh = 0;
    if (!(f >> p >> hh) || p <= 0) return false;
    pid = (pid_t)p;
    h = hh;
    return true;
}

static bool is_ours(pid_t pid) {
    if (pid <= 0 || kill(pid, 0) != 0) return false;
    return trim(first_line("/proc/" + std::to_string(pid) + "/comm")) == trim(first_line("/proc/self/comm"));
}

static void write_state(const string& path, int h, int paused = 0) {
    if (path.empty()) return;
    std::ofstream f(path, std::ios::trunc);
    f << (long)getpid() << " " << h << " " << paused << "\n";
}

static bool state_paused(const string& path) {
    std::ifstream f(path);
    long p = 0;
    int h = 0, z = 0;
    return (f >> p >> h >> z) && z == 1;
}

static void remove_state(const string& path) {
    pid_t p;
    int h;
    if (!path.empty() && read_state(path, p, h) && p == getpid()) unlink(path.c_str());
}

static void stop_previous(const string& path) {
    pid_t p;
    int h;
    if (path.empty() || !read_state(path, p, h) || p == getpid() || !is_ours(p)) return;
    kill(p, SIGTERM);
    for (int i = 0; i < 40 && kill(p, 0) == 0; i++) usleep(25000);
}

static bool choose_layout(const Stats& s, const StaticInfo& si, const Scene& sc, int cols, int rows,
                          Color accent, int need, Layout& L, int& frameH) {
    int avail_w = cols - 1 - sc.CW - GAP;
    L.bw = std::max(8, std::min(16, avail_w - (2 + LBL + 1 + VALW)));
    L.panelw = 2 + LBL + L.bw + 1 + VALW;
    if (sc.CW + GAP + L.panelw > cols - 1) return false;
    for (int level = 0; level <= 3; level++) {
        L.level = level;
        auto info = build_info(s, si, L, accent);
        frameH = std::max(sc.CH, (int)info.size());
        if (frameH + need <= rows) return true;
    }
    return false;
}


static string proc_comm(pid_t p) {
    string c = read_file("/proc/" + std::to_string(p) + "/comm");
    while (!c.empty() && (c.back() == '\n' || c.back() == ' ')) c.pop_back();
    return c;
}

static pid_t proc_ppid(pid_t p) {
    string st = read_file("/proc/" + std::to_string(p) + "/stat");
    size_t r = st.rfind(')');
    if (r == string::npos) return -1;
    char state;
    long pp = -1;
    if (sscanf(st.c_str() + r + 1, " %c %ld", &state, &pp) != 2) return -1;
    return (pid_t)pp;
}

static bool proc_command_mode(pid_t p) {
    string cl = read_file("/proc/" + std::to_string(p) + "/cmdline");
    size_t pos = 0;
    bool first = true;
    while (pos < cl.size()) {
        size_t e = cl.find('\0', pos);
        if (e == string::npos) e = cl.size();
        string a = cl.substr(pos, e - pos);
        pos = e + 1;
        if (first) { first = false; continue; }
        if (a.size() >= 2 && a[0] == '-' && a[1] != '-' && a.find('c') != string::npos) return true;
        if (!a.empty() && a[0] != '-') break;
    }
    return false;
}

static pid_t find_shell() {
    static const char* names[] = {"fish", "bash", "zsh", "sh", "dash", "ash", "ksh", "mksh", "oksh",
                                  "yash", "tcsh", "csh", "nu", "xonsh", "elvish", "pwsh"};
    pid_t sid = tcgetsid(1);
    pid_t p = getppid();
    for (int i = 0; i < 12 && p > 1; i++) {
        string c = proc_comm(p);
        bool sh = false;
        for (const char* n : names) if (c == n) sh = true;
        if (sh && !proc_command_mode(p)) return p;
        if (p == sid) break;
        pid_t pp = proc_ppid(p);
        if (pp <= 1) break;
        p = pp;
    }
    return getppid();
}

static string home_dir() {
    string h = env_or("HOME");
    if (h.empty()) {
        struct passwd* pw = getpwuid(getuid());
        if (pw && pw->pw_dir) h = pw->pw_dir;
    }
    return h;
}

static string config_dir() {
    string x = env_or("XDG_CONFIG_HOME");
    return x.empty() ? home_dir() + "/.config" : x;
}

static void make_dirs(const string& path) {
    for (size_t i = 1; i <= path.size(); i++)
        if (i == path.size() || path[i] == '/') mkdir(path.substr(0, i).c_str(), 0755);
}

static string detect_shell_exe() {
    pid_t p = find_shell();
    char buf[4096];
    ssize_t n = readlink(("/proc/" + std::to_string(p) + "/exe").c_str(), buf, sizeof buf - 1);
    if (n > 0) {
        buf[n] = 0;
        string e = buf;
        size_t d = e.find(" (deleted)");
        if (d != string::npos) e.erase(d);
        return e;
    }
    return env_or("SHELL");
}

static string shell_name() {
    string sh = g_shell_exe.empty() ? detect_shell_exe() : g_shell_exe;
    size_t sl = sh.rfind('/');
    return sl == string::npos ? sh : sh.substr(sl + 1);
}

static const char* RC_MARK = "# deepfetch";

static string rc_path(const string& sh) {
    if (sh == "fish") return config_dir() + "/fish/conf.d/deepfetch.fish";
    if (sh == "bash") return home_dir() + "/.bashrc";
    if (sh == "zsh") {
        string z = env_or("ZDOTDIR");
        return (z.empty() ? home_dir() : z) + "/.zshrc";
    }
    return "";
}

static bool setup_installed(const string& sh) {
    string p = rc_path(sh);
    return !p.empty() && read_file(p).find("deepfetch --init") != string::npos;
}

static bool setup_integration(const string& sh, bool remove) {
    string p = rc_path(sh);
    if (p.empty()) return false;
    if (sh == "fish") {
        if (remove) return unlink(p.c_str()) == 0 || errno == ENOENT;
        make_dirs(config_dir() + "/fish/conf.d");
        std::ofstream f(p, std::ios::trunc);
        f << "if command -q deepfetch\n    deepfetch --init fish | source\nend\n";
        return (bool)f;
    }
    string cur = read_file(p);
    string line = "command -v deepfetch >/dev/null 2>&1 && eval \"$(deepfetch --init " + sh + ")\"";
    string block = string(RC_MARK) + "\n" + line + "\n";
    size_t at = cur.find(block);
    if (remove) {
        if (at == string::npos) return true;
        cur.erase(at, block.size());
        std::ofstream f(p, std::ios::trunc);
        f << cur;
        return (bool)f;
    }
    if (at != string::npos) return true;
    std::ofstream f(p, std::ios::app);
    if (!cur.empty() && cur.back() != '\n') f << "\n";
    f << block;
    return (bool)f;
}

static string source_hint(const string& sh) {
    if (sh == "fish") return "source " + rc_path(sh);
    return "source " + rc_path(sh);
}

static string asked_path() { return config_dir() + "/deepfetch/asked"; }

static void first_run_setup() {
    if (!env_or("DEEPFETCH_HOOK").empty() || !env_or("DEEPFETCH_NO_SETUP").empty()) return;
    string sh = shell_name();
    if (rc_path(sh).empty()) return;
    if (access(asked_path().c_str(), F_OK) == 0) return;
    if (setup_installed(sh)) return;
    printf("deepfetch: чтобы clear и Ctrl+L корректно убирали блок, нужна интеграция с %s.\n", sh.c_str());
    printf("Добавить её в %s? [Y/n] ", rc_path(sh).c_str());
    fflush(stdout);
    char buf[64] = {0};
    bool yes = true;
    if (fgets(buf, sizeof buf, stdin)) yes = !(buf[0] == 'n' || buf[0] == 'N');
    make_dirs(config_dir() + "/deepfetch");
    {
        std::ofstream f(asked_path());
        f << "1\n";
    }
    if (yes && setup_integration(sh, false)) {
        printf("Готово. Откройте новый терминал или выполните: %s\n", source_hint(sh).c_str());
        printf("Убрать интеграцию: deepfetch --unsetup. Enter для продолжения...");
        fflush(stdout);
        if (!fgets(buf, sizeof buf, stdin)) {}
    }
}

static void usage() {
    puts("deepfetch - fastfetch с живыми данными и вращающимся 3D-логотипом\n"
         "\n"
         "  deepfetch                 запустить (работает в фоне, терминал свободен)\n"
         "  deepfetch --stop          остановить анимацию, блок остаётся на экране\n"
         "  deepfetch --static        один раз вывести блок без анимации\n"
         "  deepfetch --clear         остановить и полностью очистить экран\n"
         "  deepfetch --release       остановить анимацию (для хуков оболочки)\n"
         "  deepfetch --prompt        сообщить позицию курсора (для хуков оболочки)\n"
         "  deepfetch --init SHELL    код интеграции для fish, bash или zsh\n"
         "  deepfetch --help          справка\n"
         "\n"
         "Переменные: DEEPFETCH_FPS (4-30), FFSPIN_ASCII, FFSPIN_COLORS,\n"
         "            DEEPFETCH_GRAPHICS (0 выключить, 1 включить графику kitty), DEEPFETCH_FRAMES (24-240)");
}

static int print_init(const string& sh) {
    if (sh == "fish") {
        puts("set -gx DEEPFETCH_HOOK 1\n"
             "function __deepfetch_release --on-event fish_preexec\n"
             "    deepfetch --release\n"
             "end\n"
             "function __deepfetch_prompt --on-event fish_prompt\n"
             "    deepfetch --prompt\n"
             "end\n"
             "function clear\n"
             "    deepfetch --clear\n"
             "end\n"
             "bind \\cl 'deepfetch --clear; commandline -f repaint'");
    } else if (sh == "bash") {
        puts("export DEEPFETCH_HOOK=1\n"
             "PS0=${PS0}'$(deepfetch --release >/dev/null 2>&1)'\n"
             "__deepfetch_prompt() { local s=$?; deepfetch --prompt 2>/dev/null; return $s; }\n"
             "case \"$(declare -p PROMPT_COMMAND 2>/dev/null)\" in\n"
             "  \"declare -a\"*) [[ \" ${PROMPT_COMMAND[*]} \" == *\" __deepfetch_prompt \"* ]] || PROMPT_COMMAND=(__deepfetch_prompt \"${PROMPT_COMMAND[@]}\") ;;\n"
             "  *) [[ \"$PROMPT_COMMAND\" == *__deepfetch_prompt* ]] || PROMPT_COMMAND=\"__deepfetch_prompt${PROMPT_COMMAND:+;$PROMPT_COMMAND}\" ;;\n"
             "esac\n"
             "clear() { deepfetch --clear; }\n"
             "bind -x '\"\\C-l\": deepfetch --clear'");
    } else if (sh == "zsh") {
        puts("export DEEPFETCH_HOOK=1\n"
             "autoload -Uz add-zsh-hook\n"
             "deepfetch-release() { deepfetch --release }\n"
             "add-zsh-hook preexec deepfetch-release\n"
             "deepfetch-prompt() { deepfetch --prompt }\n"
             "add-zsh-hook precmd deepfetch-prompt\n"
             "clear() { deepfetch --clear }\n"
             "deepfetch-clear-widget() { deepfetch --clear; zle reset-prompt }\n"
             "zle -N deepfetch-clear-widget\n"
             "bindkey '^L' deepfetch-clear-widget");
    } else {
        fprintf(stderr, "deepfetch: --init принимает fish, bash или zsh\n");
        return 2;
    }
    return 0;
}


static int query_cursor_row() {
    if (!isatty(0) || !isatty(1)) return -1;
    termios old{};
    if (tcgetattr(0, &old) != 0) return -1;
    termios raw = old;
    raw.c_lflag &= ~(tcflag_t)(ICANON | ECHO);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(0, TCSANOW, &raw) != 0) return -1;
    const char* q = "\x1b[6n";
    ssize_t w = write(1, q, 4);
    (void)w;
    string buf;
    double end = now_sec() + 0.4;
    int row = -1;
    while (now_sec() < end) {
        pollfd p{0, POLLIN, 0};
        if (poll(&p, 1, 50) <= 0) continue;
        char tmp[64];
        ssize_t n = read(0, tmp, sizeof tmp);
        if (n <= 0) break;
        buf.append(tmp, (size_t)n);
        size_t e = buf.find('R');
        size_t b = buf.rfind("\x1b[", e);
        int r = 0, c = 0;
        if (e != string::npos && b != string::npos && sscanf(buf.c_str() + b + 2, "%d;%d", &r, &c) == 2) {
            row = r;
            break;
        }
    }
    tcsetattr(0, TCSANOW, &old);
    return row;
}

static bool wait_exit(pid_t p) {
    for (int i = 0; i < 60; i++) {
        if (kill(p, 0) != 0) return true;
        string st = read_file("/proc/" + std::to_string(p) + "/stat");
        size_t r = st.rfind(')');
        if (r != string::npos && r + 2 < st.size() && st[r + 2] == 'Z') return true;
        usleep(25000);
    }
    return false;
}

int main(int argc, char** argv) {
    string arg = argc > 1 ? argv[1] : "";
    if (arg == "--help" || arg == "-h") { usage(); return 0; }
    if (arg == "--init") return print_init(argc > 2 ? argv[2] : "");
    if (arg == "--setup" || arg == "--unsetup") {
        string sh = argc > 2 ? argv[2] : shell_name();
        bool rm = arg == "--unsetup";
        if (rc_path(sh).empty()) {
            fprintf(stderr, "deepfetch: оболочка %s не поддерживается, используйте fish, bash или zsh\n", sh.c_str());
            return 2;
        }
        if (!setup_integration(sh, rm)) {
            fprintf(stderr, "deepfetch: не удалось изменить %s\n", rc_path(sh).c_str());
            return 1;
        }
        if (rm) printf("Интеграция убрана из %s\n", rc_path(sh).c_str());
        else printf("Интеграция добавлена в %s. Откройте новый терминал или выполните: %s\n", rc_path(sh).c_str(), source_hint(sh).c_str());
        return 0;
    }
    if (arg == "--prompt") {
        string sp = state_path();
        pid_t p = 0;
        int h = 0;
        if (sp.empty() || !read_state(sp, p, h) || !is_ours(p)) return 0;
        int r = query_cursor_row();
        if (r > 0) {
            std::ofstream f(sp + ".row", std::ios::trunc);
            f << r << " " << (long long)(now_sec() * 1e6) << "\n";
        }
        return 0;
    }
    if (arg != "" && arg != "--stop" && arg != "--static" && arg != "--once" && arg != "--clear" && arg != "--release") {
        fprintf(stderr, "deepfetch: неизвестный параметр %s (см. --help)\n", arg.c_str());
        return 2;
    }
    if (arg == "--stop" || arg == "--clear" || arg == "--release") {
        string sp = state_path();
        pid_t p = 0;
        int h = 0;
        bool alive = !sp.empty() && read_state(sp, p, h) && is_ours(p);
        if (alive) {
            if (arg == "--release") {
                kill(p, SIGUSR1);
                for (int i = 0; i < 100 && is_ours(p) && !state_paused(sp); i++) usleep(5000);
                return 0;
            }
            kill(p, SIGTERM);
            wait_exit(p);
        }
        if (arg == "--release") return 0;
        if (arg == "--clear") {
            if (kg::hinted() || env_or("DEEPFETCH_GRAPHICS") == "1") printf("\x1b_Ga=d,d=A,q=2\x1b\\");
            printf("\x1b[r\x1b[H\x1b[2J\x1b[3J");
        }
        fflush(stdout);
        return 0;
    }

    setpriority(PRIO_PROCESS, 0, 10);
    signal(SIGPIPE, SIG_IGN);
    detect_caps();

    bool interactive = !g_caps.plain && isatty(0) && isatty(1);
    bool hooked = !env_or("DEEPFETCH_HOOK").empty();
    bool background = interactive && arg == "" && hooked;
    kg::Probe kp;
    if (interactive && (arg == "" || arg == "--static" || arg == "--once")) kp = kg::probe();
    bool gfx = kp.ok;
    pid_t shell_pid = find_shell();
    g_shell_exe = detect_shell_exe();
    int ready_fd = -1;
    if (interactive && arg == "" && !hooked) first_run_setup();
    if (background) {
        int r = query_cursor_row();
        if (r > 0) g_top = r - 1;
        else background = false;
    }
    string spath = state_path();

    if (background) {
        int pfd[2];
        if (pipe(pfd) == 0) {
            fflush(stdout);
            pid_t pid = fork();
            if (pid > 0) {
               close(pfd[1]);
                pollfd p{pfd[0], POLLIN, 0};
                if (poll(&p, 1, 15000) > 0) {
                    char b;
                    ssize_t n = read(pfd[0], &b, 1);
                    (void)n;
                }
                return 0;
            }
            if (pid == 0) {
                close(pfd[0]);
                ready_fd = pfd[1];
                setpgid(0, 0);
                signal(SIGTTOU, SIG_IGN);
                signal(SIGTTIN, SIG_IGN);
                int dn = open("/dev/null", O_RDWR);
                if (dn >= 0) {
                    dup2(dn, 0);
                    dup2(dn, 2);
                    if (dn > 2) close(dn);
                }
            } else {
                background = false;
                close(pfd[0]);
                close(pfd[1]);
            }
        } else {
            background = false;
        }
    }
    auto signal_ready = [&]() {
        if (ready_fd >= 0) {
            ssize_t n = write(ready_fd, "1", 1);
            (void)n;
            close(ready_fd);
            ready_fd = -1;
        }
    };

    if (background) {
        stop_previous(spath);
        struct sigaction sa{};
        sigemptyset(&sa.sa_mask);
        sa.sa_handler = on_stop;
        sigaction(SIGTERM, &sa, nullptr);
        sigaction(SIGINT, &sa, nullptr);
        sigaction(SIGHUP, &sa, nullptr);
        struct sigaction sr{};
        sigemptyset(&sr.sa_mask);
        sr.sa_handler = on_release;
        sigaction(SIGUSR1, &sr, nullptr);
    }

    Logo logo = load_logo();
    Scene scene;
    scene.init(logo);

    Sampler smp;
    smp.init();
    std::thread(async_worker).detach();
    smp.sample();
    smp.wait_gpu(1.5);
    smp.finalize_layout();
    usleep(250000);
    smp.sample();
    Stats& st = smp.s;
    StaticInfo si = collect_static(smp.threads);
    vector<Gauge*> gs = st.gauges();

    int cols, rows;
    term_size(cols, rows);
    if (g_caps.plain) {
        cols = 200;
        rows = 100;
    }
    Layout L;
    int H = 0;
    int need_used = SHELL_ROWS;
    bool fits = false;
    if (background) {
        for (int need : {SHELL_ROWS, FREEZE_ROWS + 1}) {
            need_used = need;
            fits = choose_layout(st, si, scene, cols, rows, logo.accent, need, L, H);
            if (fits) break;
        }
    } else {
        fits = choose_layout(st, si, scene, cols, rows, logo.accent, 1, L, H);
    }
    Grid grid, canvas;

    vector<string> gframes;
    int gfw = 0, gfh = 0, gap_ms = 62;
    unsigned gid = 0;
    if (!fits) gfx = false;
    if (gfx) {
        int nf = kg::DEFAULT_FRAMES;
        string ef = env_or("DEEPFETCH_FRAMES");
        if (!ef.empty()) nf = std::max(24, std::min(240, atoi(ef.c_str())));
        string ep = env_or("DEEPFETCH_FPS", env_or("FFSPIN_FPS"));
        if (!ep.empty()) gap_ms = (int)std::lround(1000.0 / std::max(4.0, std::min(30.0, atof(ep.c_str()))));
        gframes = kg::build(scene, kp.cw, kp.ch, nf, gfw, gfh);
        if (gframes.empty()) gfx = false;
        else gid = kg::new_id();
    }

    auto static_print = [&]() {
        for (Gauge* g : gs) g->snap();
        auto info = build_info(st, si, L, logo.accent);
        compose(scene, identity(), info, std::max(scene.CH, (int)info.size()), L, grid, canvas, gfx);
        if (gfx) kg::upload(gframes, gfw, gfh, gid, gap_ms);
        write_all(grid_text(grid));
        if (gfx) write_all(kg::place_rel(gid, scene.CW, scene.CH, grid.h, (grid.h - scene.CH) / 2));
    };

    if (!background || !fits) {
        if (!fits) {
            L.level = 3;
            L.bw = 10;
            L.panelw = 2 + LBL + L.bw + 1 + VALW;
        }
        static_print();
        if (background && !fits) {
            char msg[200];
            snprintf(msg, sizeof msg, "deepfetch: окно мало для анимации (строк: %d, нужно около %d), показан статичный вывод\n",
                     rows, H + 3);
            write_all(msg);
        }
        if (interactive && arg == "" && !hooked)
            write_all(gfx ? "deepfetch: чтобы значения обновлялись вживую, установите интеграцию (deepfetch --setup) и откройте новый терминал\n"
                          : "deepfetch: анимация включится после установки интеграции (deepfetch --setup) и нового терминала\n");
        signal_ready();
        smp.shutdown();
        return 0;
    }

    double fps = SPIN_FPS;
    {
        string e = env_or("DEEPFETCH_FPS", env_or("FFSPIN_FPS"));
        if (!e.empty()) fps = std::max(4.0, std::min(30.0, atof(e.c_str())));
    }
    const string SYNC_ON = g_caps.modern ? "\x1b[?2026h" : "\x1b[?25l";
    const string SYNC_OFF = g_caps.modern ? "\x1b[?2026l" : "\x1b[?25h";
    pid_t shell_pgrp = getpgid(shell_pid);

    auto foreground_ok = [&]() {
        pid_t fg = tcgetpgrp(1);
        return fg < 0 || shell_pgrp < 0 || fg == shell_pgrp;
    };

    Pinned pres;
    Spin spin;
    vector<Line> info = build_info(st, si, L, logo.accent);
    for (Gauge* g : gs) g->snap();
    spin.step(0);
    compose(scene, spin.R, info, H, L, grid, canvas, gfx);
    if (gfx) kg::upload(gframes, gfw, gfh, gid, gap_ms);
    {

        string o = SYNC_ON;
        int over = std::max(0, g_top + H + need_used - rows);
        if (over > g_top) over = g_top;
        if (over > 0) {
            o += "\x1b[" + std::to_string(rows) + ";1H" + string((size_t)over, '\n');
            g_top -= over;
        }
        o += "\x1b[" + std::to_string(g_top + 1) + ";1H";
        o += pres.update(grid);
        if (gfx) o += kg::place_at(gid, scene.CW, scene.CH, g_top + 1 + (H - scene.CH) / 2, 1);
        o += "\x1b[" + std::to_string(g_top + H + 1) + ";1H";
        o += SYNC_OFF;
        write_all(o);
    }
    write_state(spath, H);
    signal_ready();

    string rowpath = spath.empty() ? "" : spath + ".row";
    if (!rowpath.empty()) unlink(rowpath.c_str());
    const int block_end = g_top + H;
    auto read_row = [&](int& row, long long& stamp) {
        row = 0;
        stamp = 0;
        if (rowpath.empty()) return;
        std::ifstream rf(rowpath);
        if (!(rf >> row >> stamp)) row = 0;
    };
    bool info_dirty = true, aborted = false, paused = false, skip_final = false;
    long long pause_stamp = 0;
    double t_last = now_sec(), next_sample = t_last + SAMPLE_SEC;
    double ema_work = 0.002, fps_cap = fps;
    int async_seen = g_async.version.load();

    while (!g_stop && !g_write_failed) {
        if (kill(shell_pid, 0) != 0 && errno == ESRCH) break;
        int c2, r2;
        term_size(c2, r2);
        if (c2 != cols || r2 != rows) {
            aborted = true;
            break;
        }
        if (g_pause && !paused) {
            g_pause = 0;
            paused = true;
            int pr;
            read_row(pr, pause_stamp);
            write_state(spath, H, 1);
        }
        if (paused) {
            int cur;
            long long stamp;
            read_row(cur, stamp);
            if (cur > 0 && stamp != pause_stamp) {
                if (cur <= block_end) {
                    aborted = true;
                    break;
                }
                if (rows - cur < FREEZE_ROWS) {
                    skip_final = true;
                    break;
                }
                paused = false;
                g_pause = 0;
                pres.invalidate();
                info_dirty = true;
                write_state(spath, H, 0);
                t_last = now_sec();
            } else {
                usleep(30000);
                continue;
            }
        }
        if (!foreground_ok()) {
            usleep(30000);
            t_last = now_sec();
            continue;
        }

        double t = now_sec();
        double dt = std::min(0.25, t - t_last);
        t_last = t;

        {
            int cur;
            long long stamp;
            read_row(cur, stamp);
            if (cur > 0) {
                if (cur <= block_end) {
                    aborted = true;
                    break;
                }
                if (rows - cur < FREEZE_ROWS) break;
            }
        }

        spin.step(dt);
        bool anim_g = false;
        for (Gauge* g : gs) {
            g->step(dt);
            if (!g->settled()) anim_g = true;
        }
        if (t >= next_sample) {
            smp.sample();
            st.pulse = !st.pulse;
            next_sample = t + SAMPLE_SEC;
            info_dirty = true;
        }
        int av = g_async.version.load();
        if (av != async_seen) {
            async_seen = av;
            info_dirty = true;
        }
        if (anim_g) info_dirty = true;
        if (gfx && !info_dirty) {
            usleep(40000);
            continue;
        }

        double w0 = now_sec();
        if (info_dirty) {
            info = build_info(st, si, L, logo.accent);
            info_dirty = false;
        }
        compose(scene, spin.R, info, H, L, grid, canvas, gfx);
        write_all(SYNC_ON + pres.update(grid) + SYNC_OFF);
        double w = now_sec() - w0;
        ema_work = ema_work * 0.8 + w * 0.2;
        fps_cap = std::max(FPS_MIN, std::min(fps, 1.0 / (ema_work * 3.0)));

        double spent = now_sec() - t;
        double wait = 1.0 / fps_cap - spent;
        if (wait > 0) usleep((useconds_t)(wait * 1e6));
    }

    if (!aborted && !skip_final && !paused && !g_write_failed) {
        for (Gauge* g : gs) g->snap();
        info = build_info(st, si, L, logo.accent);
        compose(scene, identity(), info, H, L, grid, canvas, gfx);
        write_all(SYNC_ON + pres.update(grid) + SYNC_OFF);
    }
    if (!rowpath.empty()) unlink(rowpath.c_str());
    remove_state(spath);
    smp.shutdown();
    return 0;
}
