// See pad_bindings.h.
#include "pad_bindings.h"
#include "platform.h"
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

const char* const kControlNames[kPadControls] = {
    "A", "B", "X", "Y", "Z", "start", "dpad_up", "dpad_down", "dpad_left", "dpad_right", "L", "R",
    "stick_x", "stick_y", "cstick_x", "cstick_y"};

const char* const kButtonNames[kPadButtons] = {
    "south", "east", "west", "north", "back", "guide", "start", "leftstick", "rightstick",
    "leftshoulder", "rightshoulder", "up", "down", "left", "right", "misc1", "rightpaddle1",
    "leftpaddle1", "rightpaddle2", "leftpaddle2", "touchpad", "misc2", "misc3", "misc4", "misc5", "misc6"};

const char* const kAxisNames[kPadAxes] = {"leftx", "lefty", "rightx", "righty", "lefttrigger", "righttrigger"};

bool is_trigger(int axis) { return axis == kPadLeftTrigger || axis == kPadRightTrigger; }
bool trigger(PadControl c) { return c == PadControl::L || c == PadControl::R; }

// What a source gives a one-way control, 0 to 32767: a button all or nothing, one way of an
// axis from its centre, all of a trigger, or all of a stick axis from one end to the other.
int one_way(const PadSource& s, const PadReading& r) {
    if (!s.axis) return r.button[s.index] ? 32767 : 0;
    int v = r.axis[s.index];
    if (s.invert) v = -v;
    if (s.half > 0) return std::clamp(v, 0, 32767);
    if (s.half < 0) return std::clamp(-v, 0, 32767);
    if (is_trigger(s.index)) return std::clamp(v, 0, 32767);
    return std::clamp((v + 32768) / 2, 0, 32767);
}

// A stick axis's value as SDL reads it. Only an axis is ever bound to one.
int whole(const PadSource& s, const PadReading& r) { return s.axis ? r.axis[s.index] : 0; }

// What the map gamepads.h describes does with a stick axis, which the game's own handling of
// the pad is tuned to: no rescaling past the dead zone, and full travel at 100 from the centre.
uint8_t axis_to_u8(int v, bool invert) {
    float f = v / 32767.0f;
    if (invert) f = -f;
    int r = 128 + (int)(f * 100.0f);
    return (uint8_t)std::clamp(r, 0, 255);
}

const uint16_t kBits[] = {PAD_A, PAD_B, PAD_X, PAD_Y, PAD_Z, PAD_START, PAD_UP, PAD_DOWN, PAD_LEFT, PAD_RIGHT,
                          PAD_L, PAD_R};

// ---- The file's text

// A raw axis distance as the fraction of the travel a controls.txt line gives, and back.
std::string fraction(int raw) {
    char s[32];
    snprintf(s, sizeof(s), "%g", raw / 32767.0);
    return s;
}

bool parse_fraction(const std::string& word, int& raw) {
    float f;
    if (!controls_float(word, f) || !(f >= 0.0f && f < 1.0f)) return false;
    raw = (int)std::lround(f * 32767.0);
    return true;
}

// What a quoted name can't hold as it is: its quote, a control character, and the '%' that
// escapes them.
bool escaped_in_name(unsigned char c) { return c == '"' || c == '%' || c < 0x20 || c == 0x7F; }

std::string line_for(const SavedBinding& b, PadControl c) {
    const PadBinding& k = b.binding;
    const PadSource& s = k.source[(int)c];
    std::string line = std::string(kControlNames[(int)c]) + " = \"" + percent_encode(b.name, escaped_in_name) +
                       "\" gamepad " + pad_source_name(s);
    if (s.invert) line += " invert";
    if (c == PadControl::StickX || c == PadControl::StickY) line += " deadzone " + fraction(k.stick_deadzone);
    if (c == PadControl::CStickX || c == PadControl::CStickY) line += " deadzone " + fraction(k.cstick_deadzone);
    if (trigger(c)) line += " deadzone " + fraction(k.trigger_deadzone) + " threshold " + fraction(k.trigger_click);
    return line + "\n";
}

// Whether `s` can drive `c`: a stick axis takes all of a stick's axis, and the others a button,
// a trigger or one way of a stick axis.
bool fits(PadControl c, const PadSource& s) {
    if (pad_control_is_stick(c)) return s.axis && !is_trigger(s.index) && s.half == 0;
    return !s.invert && (!s.axis || is_trigger(s.index) || s.half != 0);
}

// A word from the file as an error says it, cut to its first 32 bytes.
std::string cut(const std::string& word) { return word.size() <= 32 ? word : word.substr(0, 32) + "..."; }

// One control line into `b`, or false with `err` set.
bool parse_control(const std::vector<std::string>& w, SavedBinding& b, std::string& err) {
    const auto name = std::find_if(std::begin(kControlNames), std::end(kControlNames),
                                   [&](const char* n) { return w[0] == n; });
    if (name == std::end(kControlNames)) {
        err = "unknown pad control \"" + cut(w[0]) + "\"";
        return false;
    }
    const PadControl c = (PadControl)(name - std::begin(kControlNames));
    PadSource s;
    if (w.size() < 5 || w[1] != "=" || w[3] != "gamepad" || !pad_source_parse(w[4], s)) {
        err = "expected <pad control> = \"<name>\" gamepad <button or axis>";
        return false;
    }
    int deadzone = -1, threshold = -1;
    for (size_t i = 5; i < w.size(); i++) {
        if (w[i] == "invert") {
            s.invert = true;
        } else if ((w[i] == "deadzone" || w[i] == "threshold") && i + 1 < w.size()) {
            if (!parse_fraction(w[i + 1], w[i] == "deadzone" ? deadzone : threshold)) {
                err = w[i] + " is from 0 to below 1";
                return false;
            }
            i++;
        } else {
            err = "unknown option \"" + cut(w[i]) + "\"";
            return false;
        }
    }
    if (!fits(c, s)) {
        err = pad_control_is_stick(c) ? std::string(w[0]) + " takes a stick's whole axis"
                                      : std::string(w[0]) + " takes a button, a trigger or one way of a stick axis";
        return false;
    }
    // The setting each kind of control has: a stick's dead zone, or a trigger's and its click.
    const bool main_stick = c == PadControl::StickX || c == PadControl::StickY;
    int* dz = main_stick ? &b.binding.stick_deadzone
              : pad_control_is_stick(c) ? &b.binding.cstick_deadzone
              : trigger(c) ? &b.binding.trigger_deadzone : nullptr;
    if ((deadzone >= 0 && !dz) || (threshold >= 0 && !trigger(c))) {
        err = w[0] + (deadzone >= 0 && !dz ? " has no dead zone" : " has no threshold, which only L and R have");
        return false;
    }
    b.binding.source[(int)c] = s;
    b.given[(int)c] = true;
    if (deadzone >= 0) *dz = deadzone;
    if (threshold >= 0) b.binding.trigger_click = threshold;
    return true;
}

bool same_gamepad(const SavedBinding& b, const PortDevice& id) { return b.id.guid == id.guid && b.id.serial == id.serial; }

}  // namespace

const char* pad_control_name(PadControl c) { return kControlNames[(int)c]; }
bool pad_control_is_stick(PadControl c) { return c >= PadControl::StickX; }
const char* pad_button_name(int button) { return button >= 0 && button < kPadButtons ? kButtonNames[button] : "?"; }

std::string pad_source_name(const PadSource& s) {
    if (!s.axis) return pad_button_name(s.index);
    return std::string(kAxisNames[s.index]) + (s.half > 0 ? "+" : s.half < 0 ? "-" : "");
}

bool pad_source_parse(const std::string& word, PadSource& s) {
    for (int k = 0; k < kPadButtons; k++)
        if (word == kButtonNames[k]) {
            s = {false, k, 0, false};
            return true;
        }
    for (int a = 0; a < kPadAxes; a++) {
        const size_t n = strlen(kAxisNames[a]);
        if (word.compare(0, n, kAxisNames[a]) != 0) continue;
        const std::string rest = word.substr(n);
        if (rest.empty()) {
            s = {true, a, 0, false};
            return true;
        }
        if ((rest == "+" || rest == "-") && !is_trigger(a)) {
            s = {true, a, rest == "+" ? 1 : -1, false};
            return true;
        }
    }
    return false;
}

PadBinding pad_binding_default(const std::array<int, 4>& faces) {
    PadBinding b;
    auto set = [&](PadControl c, PadSource s) { b.source[(int)c] = s; };
    for (int k = 0; k < 4; k++) set((PadControl)k, {false, faces[k], 0, false});
    set(PadControl::Z, {false, kPadRightShoulder, 0, false});
    set(PadControl::Start, {false, kPadStart, 0, false});
    set(PadControl::DUp, {false, kPadUp, 0, false});
    set(PadControl::DDown, {false, kPadDown, 0, false});
    set(PadControl::DLeft, {false, kPadLeft, 0, false});
    set(PadControl::DRight, {false, kPadRight, 0, false});
    set(PadControl::L, {true, kPadLeftTrigger, 0, false});
    set(PadControl::R, {true, kPadRightTrigger, 0, false});
    set(PadControl::StickX, {true, kPadLeftX, 0, false});
    set(PadControl::StickY, {true, kPadLeftY, 0, true});
    set(PadControl::CStickX, {true, kPadRightX, 0, false});
    set(PadControl::CStickY, {true, kPadRightY, 0, true});
    return b;
}

void pad_binding_merge(const PadBinding& b, const PadReading& r, PadState& p) {
    for (int c = 0; c < (int)PadControl::L; c++)
        if (one_way(b.source[c], r) > kBindHalf) p.buttons |= kBits[c];
    auto trig = [&](PadControl c, uint8_t& out) {
        const int v = one_way(b.source[(int)c], r);
        if (v <= b.trigger_deadzone) return;
        out = std::max(out, (uint8_t)(v * 255 / 32767));
        if (v > b.trigger_click) p.buttons |= kBits[(int)c];
    };
    trig(PadControl::L, p.trig_l);
    trig(PadControl::R, p.trig_r);
    // A stick is taken over while either of its axes is past the dead zone, both axes then.
    auto stick = [&](PadControl cx, PadControl cy, int deadzone, uint8_t& x, uint8_t& y) {
        const PadSource &sx = b.source[(int)cx], &sy = b.source[(int)cy];
        const int vx = whole(sx, r), vy = whole(sy, r);
        if (abs(vx) <= deadzone && abs(vy) <= deadzone) return;
        x = axis_to_u8(vx, sx.invert);
        y = axis_to_u8(vy, sy.invert);
    };
    stick(PadControl::StickX, PadControl::StickY, b.stick_deadzone, p.stick_x, p.stick_y);
    stick(PadControl::CStickX, PadControl::CStickY, b.cstick_deadzone, p.cstick_x, p.cstick_y);
}

void pad_binding_set(PadBinding& b, PadControl c, const PadSource& s) {
    const PadSource was = b.source[(int)c];
    // The same input, whichever way round. Only a stick control takes all of a stick's axis,
    // and only the others a button, a trigger or one way of an axis (fits), so a control that
    // matches takes the same kind of input `c` had.
    auto same_input = [&](const PadSource& t) { return t.axis == s.axis && t.index == s.index && t.half == s.half; };
    for (int d = 0; d < kPadControls; d++)
        if (d != (int)c && same_input(b.source[d])) b.source[d] = was;
    b.source[(int)c] = s;
}

// ---- Press-to-bind

BindListen bind_listen(PadControl c, const PadReading& r, uint64_t now) {
    return {c, now + kBindListenNs, r};
}

BindHeard bind_hear(BindListen& l, const PadReading& r, uint64_t now, PadSource& out) {
    // What was held as it began is passed over until it's let go.
    for (int k = 0; k < kPadButtons; k++) l.held.button[k] = l.held.button[k] && r.button[k];
    for (int a = 0; a < kPadAxes; a++)
        if (abs(r.axis[a]) <= kBindHalf) l.held.axis[a] = 0;
    if (now >= l.until) return BindHeard::timed_out;
    auto pressed = [&](int k) { return r.button[k] && !l.held.button[k]; };
    if (pressed(kPadBack) || pressed(kPadGuide)) return BindHeard::cancelled;
    const bool stick = pad_control_is_stick(l.control);
    // A full pull on a trigger with a click button (a GameCube adapter's misc3 and misc4) can show
    // both in one reading. L and R take the trigger, so that they keep its travel.
    if (trigger(l.control))
        for (const int a : {kPadLeftTrigger, kPadRightTrigger})
            if (!l.held.axis[a] && r.axis[a] > kBindHalf) {
                out = {true, a, 0, false};
                return BindHeard::bound;
            }
    for (int k = 0; k < kPadButtons && !stick; k++)
        if (pressed(k)) {
            out = {false, k, 0, false};
            return BindHeard::bound;
        }
    for (int a = 0; a < kPadAxes; a++) {
        const int v = r.axis[a];
        if (l.held.axis[a] || abs(v) <= kBindHalf) continue;
        if (stick && is_trigger(a)) continue;
        if (stick) out = {true, a, 0, v < 0};
        else if (is_trigger(a)) out = {true, a, 0, false};
        else out = {true, a, v > 0 ? 1 : -1, false};
        return BindHeard::bound;
    }
    return BindHeard::waiting;
}

// ---- saves/pads.txt

PadBinding bindings_find(const SavedBindings& saved, const PortDevice& id, const PadBinding& fallback) {
    const auto it = std::find_if(saved.begin(), saved.end(), [&](const SavedBinding& b) { return same_gamepad(b, id); });
    if (it == saved.end()) return fallback;
    PadBinding b = it->binding;
    for (int c = 0; c < kPadControls; c++)
        if (!it->given[c]) b.source[c] = fallback.source[c];
    return b;
}

void bindings_put(SavedBindings& saved, const PortDevice& id, const std::string& name, const PadBinding& b) {
    SavedBinding s{{id.guid, id.serial, 0, 0}, name, b, {}};
    s.given.fill(true);
    const auto it = std::find_if(saved.begin(), saved.end(), [&](const SavedBinding& x) { return same_gamepad(x, id); });
    if (it != saved.end()) *it = s;
    else saved.push_back(s);
}

bool bindings_forget(SavedBindings& saved, const PortDevice& id) {
    const auto it = std::find_if(saved.begin(), saved.end(), [&](const SavedBinding& x) { return same_gamepad(x, id); });
    if (it == saved.end()) return false;
    saved.erase(it);
    return true;
}

std::string bindings_format(const SavedBindings& saved) {
    std::string text =
        "# Each gamepad's controls, as a player set them. The runtime writes this file. A block\n"
        "# starts with the gamepad's GUID and serial, and its lines are controls.txt's.\n";
    for (const SavedBinding& b : saved) {
        text += "# gamepad " + b.id.guid + " " + ports_encode_serial(b.id.serial) + "\n";
        for (int c = 0; c < kPadControls; c++)
            if (b.given[c]) text += line_for(b, (PadControl)c);
    }
    return text;
}

SavedBindings bindings_parse(const std::string& text, const char* name) {
    SavedBindings saved;
    int block = -1;  // the block the lines are read into, none before the first
    int n = 0;
    for (size_t start = 0; start < text.size();) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        const std::string line = text.substr(start, end - start);
        start = end + 1;
        n++;
        std::vector<std::string> w;
        std::string err;
        // A block's first line, which controls.txt reads as a comment.
        // Split on whitespace alone, as ports.txt is, since a serial may hold '#', '=' or '"'.
        if (!strncmp(line.c_str(), "# gamepad ", 10)) {
            w = ports_words(line.substr(10));
            SavedBinding b;
            if (w.size() == 2 && ports_is_guid(w[0]) && ports_decode_serial(w[1], b.id.serial)) {
                b.id.guid = w[0];
                bindings_forget(saved, b.id);
                saved.push_back(b);
                block = (int)saved.size() - 1;
                continue;
            }
            block = -1;
            err = "not a gamepad's GUID and serial";
        } else if (!controls_words(line.c_str(), w, err) || w.empty()) {
            if (err.empty()) continue;  // a blank line or a comment
        } else if (block < 0) {
            err = "a control before any gamepad's line";
        } else if (parse_control(w, saved[block], err)) {
            if (saved[block].name.empty() && !percent_decode(w[2], saved[block].name)) saved[block].name = w[2];
            continue;
        }
        fprintf(stderr, "%s:%d: %s, so the line is skipped\n", name, n, err.c_str());
    }
    return saved;
}

SavedBindings bindings_load(const std::string& path, bool& readable) {
    std::string text;
    int err = 0;
    readable = plat_read_file(path, text, err) || err == ENOENT;
    if (!readable)
        fprintf(stderr, "%s: can't read it (%s), so no bindings are read and it isn't saved over\n", path.c_str(),
                strerror(err));
    return readable ? bindings_parse(text, path.c_str()) : SavedBindings{};
}

bool bindings_save(const std::string& path, const SavedBindings& saved) {
    return plat_save_text_making_dirs(path, bindings_format(saved));
}

// ---- The words of a controls.txt line

bool controls_words(const char* s, std::vector<std::string>& out, std::string& err) {
    while (*s) {
        if (isspace((unsigned char)*s)) { s++; continue; }
        if (*s == '#') break;
        if (*s == '=') { out.emplace_back("="); s++; continue; }
        if (*s == '"') {
            const char* e = strchr(s + 1, '"');
            if (!e) { err = "no closing quote"; return false; }
            out.emplace_back(s + 1, e);
            s = e + 1;
            continue;
        }
        const char* e = s;
        while (*e && !isspace((unsigned char)*e) && *e != '=' && *e != '#' && *e != '"') e++;
        out.emplace_back(s, e);
        s = e;
    }
    return true;
}

bool controls_float(const std::string& word, float& v) {
    char* end;
    v = strtof(word.c_str(), &end);
    return !word.empty() && *end == 0;
}

bool controls_int(const std::string& word, int& v) {
    if (word.empty() || word.size() > 9) return false;
    for (char c : word)
        if (!isdigit((unsigned char)c)) return false;
    v = atoi(word.c_str());
    return true;
}
