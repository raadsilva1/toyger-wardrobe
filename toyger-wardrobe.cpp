#include <Xm/Form.h>
#include <Xm/Label.h>
#include <Xm/MessageB.h>
#include <Xm/Protocols.h>
#include <Xm/PushB.h>
#include <Xm/RowColumn.h>
#include <Xm/ScrolledW.h>
#include <Xm/Separator.h>
#include <Xm/VendorS.h>
#include <Xm/Xm.h>

#include <X11/Shell.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>

#include <cerrno>
#include <cctype>
#include <csignal>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include <pwd.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

constexpr int kWindowWidth = 600;
constexpr int kWindowHeight = 500;
constexpr int kRowHeight = 58;
constexpr int kIconCellWidth = 64;
constexpr int kNameCellWidth = 300;
constexpr std::size_t kTexturePatternChars = 2048;

void log_info(const std::string& message) {
  std::cerr << "[toyger-wardrobe] INFO: " << message << '\n';
}

void log_warn(const std::string& message) {
  std::cerr << "[toyger-wardrobe] WARN: " << message << '\n';
}

void log_error(const std::string& message) {
  std::cerr << "[toyger-wardrobe] ERROR: " << message << '\n';
}

const std::string& texture_fill_pattern() {
  static const std::string pattern = [] {
    std::string value;
    value.reserve(kTexturePatternChars);
    while (value.size() < kTexturePatternChars) {
      value += ".:";
    }
    return value;
  }();
  return pattern;
}

std::string trim(std::string value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) {
    return "";
  }
  const auto last = value.find_last_not_of(" \t\r\n");
  return value.substr(first, last - first + 1);
}

std::string expand_tilde(const std::string& path) {
  if (path.empty() || path[0] != '~') {
    return path;
  }

  const char* home = std::getenv("HOME");
  if (!home || std::strlen(home) == 0) {
    return path;
  }

  if (path.size() == 1) {
    return std::string(home);
  }

  if (path[1] == '/') {
    return std::string(home) + path.substr(1);
  }

  return path;
}

namespace mini_json {

struct ParseError : public std::runtime_error {
  ParseError(std::string message, std::size_t at)
      : std::runtime_error(std::move(message)), position(at) {}
  std::size_t position;
};

struct Value {
  using Array = std::vector<Value>;
  using Object = std::map<std::string, Value>;

  enum class Type {
    kNull,
    kBool,
    kNumber,
    kString,
    kArray,
    kObject,
  };

  Type type = Type::kNull;
  bool bool_value = false;
  double number_value = 0.0;
  std::string string_value;
  Array array_value;
  Object object_value;

  static Value make_null() {
    return Value{};
  }

  static Value make_bool(bool value) {
    Value v;
    v.type = Type::kBool;
    v.bool_value = value;
    return v;
  }

  static Value make_number(double value) {
    Value v;
    v.type = Type::kNumber;
    v.number_value = value;
    return v;
  }

  static Value make_string(std::string value) {
    Value v;
    v.type = Type::kString;
    v.string_value = std::move(value);
    return v;
  }

  static Value make_array(Array value) {
    Value v;
    v.type = Type::kArray;
    v.array_value = std::move(value);
    return v;
  }

  static Value make_object(Object value) {
    Value v;
    v.type = Type::kObject;
    v.object_value = std::move(value);
    return v;
  }

  bool is_null() const { return type == Type::kNull; }
  bool is_bool() const { return type == Type::kBool; }
  bool is_number() const { return type == Type::kNumber; }
  bool is_string() const { return type == Type::kString; }
  bool is_array() const { return type == Type::kArray; }
  bool is_object() const { return type == Type::kObject; }

  const std::string* string_or_null() const {
    if (!is_string()) {
      return nullptr;
    }
    return &string_value;
  }

  const Array* array_or_null() const {
    if (!is_array()) {
      return nullptr;
    }
    return &array_value;
  }

  const Object* object_or_null() const {
    if (!is_object()) {
      return nullptr;
    }
    return &object_value;
  }

  const Value* get(const std::string& key) const {
    if (!is_object()) {
      return nullptr;
    }
    const auto it = object_value.find(key);
    if (it == object_value.end()) {
      return nullptr;
    }
    return &it->second;
  }
};

class Parser {
 public:
  explicit Parser(std::string_view text) : text_(text) {}

  Value parse() {
    skip_whitespace();
    Value root = parse_value();
    skip_whitespace();
    if (!eof()) {
      throw ParseError("unexpected trailing content", pos_);
    }
    return root;
  }

 private:
  static bool is_hex_digit(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
  }

  static int hex_digit_to_int(char c) {
    if (c >= '0' && c <= '9') {
      return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
      return 10 + (c - 'a');
    }
    if (c >= 'A' && c <= 'F') {
      return 10 + (c - 'A');
    }
    return -1;
  }

  static void append_utf8(std::string& out, unsigned int codepoint) {
    if (codepoint <= 0x7F) {
      out.push_back(static_cast<char>(codepoint));
      return;
    }

    if (codepoint <= 0x7FF) {
      out.push_back(static_cast<char>(0xC0 | ((codepoint >> 6) & 0x1F)));
      out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
      return;
    }

    if (codepoint <= 0xFFFF) {
      out.push_back(static_cast<char>(0xE0 | ((codepoint >> 12) & 0x0F)));
      out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
      return;
    }

    out.push_back(static_cast<char>(0xF0 | ((codepoint >> 18) & 0x07)));
    out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  }

  bool eof() const { return pos_ >= text_.size(); }

  char peek() const {
    if (eof()) {
      return '\0';
    }
    return text_[pos_];
  }

  char take() {
    if (eof()) {
      throw ParseError("unexpected end of input", pos_);
    }
    return text_[pos_++];
  }

  void expect(char c) {
    const char got = take();
    if (got != c) {
      std::ostringstream oss;
      oss << "expected '" << c << "'";
      throw ParseError(oss.str(), pos_);
    }
  }

  void skip_whitespace() {
    while (!eof()) {
      const char c = peek();
      if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
        ++pos_;
        continue;
      }
      break;
    }
  }

  Value parse_value() {
    if (eof()) {
      throw ParseError("unexpected end of input", pos_);
    }

    const char c = peek();
    if (c == '{') {
      return parse_object();
    }
    if (c == '[') {
      return parse_array();
    }
    if (c == '"') {
      return Value::make_string(parse_string());
    }
    if (c == 't') {
      parse_literal("true");
      return Value::make_bool(true);
    }
    if (c == 'f') {
      parse_literal("false");
      return Value::make_bool(false);
    }
    if (c == 'n') {
      parse_literal("null");
      return Value::make_null();
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
      return Value::make_number(parse_number());
    }

    throw ParseError("unexpected token", pos_);
  }

  void parse_literal(const char* literal) {
    for (std::size_t i = 0; literal[i] != '\0'; ++i) {
      if (take() != literal[i]) {
        throw ParseError("invalid literal", pos_);
      }
    }
  }

  std::string parse_string() {
    expect('"');

    std::string output;
    while (!eof()) {
      const char c = take();
      if (c == '"') {
        return output;
      }

      if (c == '\\') {
        const char esc = take();
        switch (esc) {
          case '"':
            output.push_back('"');
            break;
          case '\\':
            output.push_back('\\');
            break;
          case '/':
            output.push_back('/');
            break;
          case 'b':
            output.push_back('\b');
            break;
          case 'f':
            output.push_back('\f');
            break;
          case 'n':
            output.push_back('\n');
            break;
          case 'r':
            output.push_back('\r');
            break;
          case 't':
            output.push_back('\t');
            break;
          case 'u': {
            unsigned int codepoint = 0;
            for (int i = 0; i < 4; ++i) {
              if (eof() || !is_hex_digit(peek())) {
                throw ParseError("invalid unicode escape", pos_);
              }
              codepoint = (codepoint << 4) |
                          static_cast<unsigned int>(hex_digit_to_int(take()));
            }
            append_utf8(output, codepoint);
            break;
          }
          default:
            throw ParseError("invalid escape sequence", pos_);
        }
        continue;
      }

      if (static_cast<unsigned char>(c) < 0x20) {
        throw ParseError("invalid control character in string", pos_);
      }

      output.push_back(c);
    }

    throw ParseError("unterminated string", pos_);
  }

  double parse_number() {
    const std::size_t start = pos_;

    if (peek() == '-') {
      ++pos_;
    }

    if (eof()) {
      throw ParseError("invalid number", pos_);
    }

    if (peek() == '0') {
      ++pos_;
    } else {
      if (peek() < '1' || peek() > '9') {
        throw ParseError("invalid number", pos_);
      }
      while (!eof() && peek() >= '0' && peek() <= '9') {
        ++pos_;
      }
    }

    if (!eof() && peek() == '.') {
      ++pos_;
      if (eof() || peek() < '0' || peek() > '9') {
        throw ParseError("invalid number", pos_);
      }
      while (!eof() && peek() >= '0' && peek() <= '9') {
        ++pos_;
      }
    }

    if (!eof() && (peek() == 'e' || peek() == 'E')) {
      ++pos_;
      if (!eof() && (peek() == '+' || peek() == '-')) {
        ++pos_;
      }
      if (eof() || peek() < '0' || peek() > '9') {
        throw ParseError("invalid number", pos_);
      }
      while (!eof() && peek() >= '0' && peek() <= '9') {
        ++pos_;
      }
    }

    const std::string number_text(text_.substr(start, pos_ - start));
    char* endptr = nullptr;
    errno = 0;
    const double value = std::strtod(number_text.c_str(), &endptr);
    if (errno != 0 || !endptr || *endptr != '\0') {
      throw ParseError("invalid number", start);
    }
    return value;
  }

  Value parse_array() {
    expect('[');
    skip_whitespace();

    Value::Array values;
    if (!eof() && peek() == ']') {
      ++pos_;
      return Value::make_array(std::move(values));
    }

    while (true) {
      skip_whitespace();
      values.push_back(parse_value());
      skip_whitespace();

      if (!eof() && peek() == ',') {
        ++pos_;
        continue;
      }

      if (!eof() && peek() == ']') {
        ++pos_;
        break;
      }

      throw ParseError("expected ',' or ']' in array", pos_);
    }

    return Value::make_array(std::move(values));
  }

  Value parse_object() {
    expect('{');
    skip_whitespace();

    Value::Object fields;
    if (!eof() && peek() == '}') {
      ++pos_;
      return Value::make_object(std::move(fields));
    }

    while (true) {
      skip_whitespace();
      if (eof() || peek() != '"') {
        throw ParseError("expected string key in object", pos_);
      }

      std::string key = parse_string();
      skip_whitespace();
      expect(':');
      skip_whitespace();

      fields[std::move(key)] = parse_value();
      skip_whitespace();

      if (!eof() && peek() == ',') {
        ++pos_;
        continue;
      }

      if (!eof() && peek() == '}') {
        ++pos_;
        break;
      }

      throw ParseError("expected ',' or '}' in object", pos_);
    }

    return Value::make_object(std::move(fields));
  }

  std::string_view text_;
  std::size_t pos_ = 0;
};

Value parse(std::string_view text) {
  Parser parser(text);
  return parser.parse();
}

}  // namespace mini_json

enum class IconKind {
  kInformation,
  kWarning,
  kError,
  kQuestion,
  kWorking,
};

struct ApplicationEntry {
  std::string name;
  IconKind icon = IconKind::kInformation;
  std::vector<std::string> argv;
};

struct ParsedConfigFile {
  bool valid = false;
  std::vector<ApplicationEntry> applications;
  std::string reason;
};

struct LoadConfigResult {
  bool ok = false;
  std::vector<ApplicationEntry> applications;
  std::string selected_path;
  std::string error_message;
};

struct CliOptions {
  std::optional<std::string> config_override;
};

struct RuntimeState;

struct LaunchContext {
  RuntimeState* runtime = nullptr;
  std::size_t app_index = 0;
};

struct RuntimeState {
  XtAppContext app_context = nullptr;
  Widget toplevel = nullptr;
  Widget status_primary_label = nullptr;
  Widget status_secondary_label = nullptr;
  XmFontList ui_fontlist = nullptr;
  XFontStruct* ui_font = nullptr;
  std::vector<ApplicationEntry> applications;
  std::vector<std::unique_ptr<LaunchContext>> launch_contexts;
  std::string hostname = "unknown";
  std::string username = "unknown";
  std::optional<std::time_t> boot_time;
  std::string last_opened = "none";
  XtTranslations quit_translations = nullptr;
  bool should_exit = false;
};

RuntimeState* g_runtime_state = nullptr;

void initialize_ui_font(RuntimeState& state) {
  Display* display = XtDisplay(state.toplevel);
  if (display == nullptr) {
    return;
  }

  const char* candidates[] = {
      "-misc-dejavu sans-medium-r-normal--20-*-*-*-*-*-*-*",
      "-misc-liberation sans-medium-r-normal--20-*-*-*-*-*-*-*",
      "-adobe-helvetica-medium-r-normal--20-*-*-*-*-*-*-*",
      "-adobe-helvetica-bold-r-normal--20-*-*-*-*-*-*-*",
      "-misc-fixed-medium-r-normal--20-*-*-*-*-*-*-*",
  };

  for (const char* font_name : candidates) {
    XFontStruct* loaded = XLoadQueryFont(display, font_name);
    if (loaded == nullptr) {
      continue;
    }

    XmFontList fontlist =
        XmFontListCreate(loaded, const_cast<char*>(XmSTRING_DEFAULT_CHARSET));
    if (fontlist == nullptr) {
      XFreeFont(display, loaded);
      continue;
    }

    state.ui_font = loaded;
    state.ui_fontlist = fontlist;
    log_info(std::string("using UI font: ") + font_name);
    return;
  }

  log_warn("no preferred large X11 font was found; using Motif defaults");
}

void free_ui_font(RuntimeState& state) {
  if (state.ui_fontlist != nullptr) {
    XmFontListFree(state.ui_fontlist);
    state.ui_fontlist = nullptr;
  }

  if (state.ui_font != nullptr) {
    XFreeFont(XtDisplay(state.toplevel), state.ui_font);
    state.ui_font = nullptr;
  }
}

void apply_ui_font_if_available(Widget widget, const RuntimeState& state) {
  if (state.ui_fontlist == nullptr) {
    return;
  }
  XtVaSetValues(widget, XmNfontList, state.ui_fontlist, nullptr);
}

std::optional<IconKind> icon_from_string(const std::string& icon_name) {
  if (icon_name == "information") {
    return IconKind::kInformation;
  }
  if (icon_name == "warning") {
    return IconKind::kWarning;
  }
  if (icon_name == "error") {
    return IconKind::kError;
  }
  if (icon_name == "question") {
    return IconKind::kQuestion;
  }
  if (icon_name == "working") {
    return IconKind::kWorking;
  }
  return std::nullopt;
}

const char* icon_to_pixmap_name(IconKind icon) {
  switch (icon) {
    case IconKind::kInformation:
      return "xm_information";
    case IconKind::kWarning:
      return "xm_warning";
    case IconKind::kError:
      return "xm_error";
    case IconKind::kQuestion:
      return "xm_question";
    case IconKind::kWorking:
      return "xm_working";
  }
  return "xm_information";
}

bool tokenize_command_safely(const std::string& command,
                             std::vector<std::string>& tokens,
                             std::string& reason) {
  const std::string trimmed = trim(command);
  if (trimmed.empty()) {
    reason = "command is empty";
    return false;
  }

  static const std::string kForbiddenShellChars = "|&;<>`$(){}[]*?!~\\\"'";
  if (trimmed.find_first_of(kForbiddenShellChars) != std::string::npos) {
    reason = "command contains shell-style metacharacters";
    return false;
  }

  for (const unsigned char c : trimmed) {
    if (std::iscntrl(c) != 0) {
      reason = "command contains control characters";
      return false;
    }
  }

  std::istringstream iss(trimmed);
  std::string token;
  while (iss >> token) {
    tokens.push_back(token);
  }

  if (tokens.empty()) {
    reason = "command produced no tokens";
    return false;
  }

  return true;
}

bool read_file_to_string(const std::string& path, std::string& output,
                         std::string& reason) {
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) {
    reason = "unable to open file";
    return false;
  }

  std::ostringstream oss;
  oss << in.rdbuf();
  if (!in.good() && !in.eof()) {
    reason = "unable to read file";
    return false;
  }

  output = oss.str();
  return true;
}

std::optional<std::vector<std::string>> parse_argv_array(
    const mini_json::Value& value, std::string& reason) {
  const auto* argv_array = value.array_or_null();
  if (!argv_array) {
    reason = "argv must be an array of strings";
    return std::nullopt;
  }

  if (argv_array->empty()) {
    reason = "argv must contain at least one element";
    return std::nullopt;
  }

  std::vector<std::string> argv;
  argv.reserve(argv_array->size());
  for (std::size_t i = 0; i < argv_array->size(); ++i) {
    const auto* item = (*argv_array)[i].string_or_null();
    if (!item) {
      std::ostringstream oss;
      oss << "argv[" << i << "] must be a string";
      reason = oss.str();
      return std::nullopt;
    }

    if (item->empty()) {
      std::ostringstream oss;
      oss << "argv[" << i << "] must be non-empty";
      reason = oss.str();
      return std::nullopt;
    }

    argv.push_back(*item);
  }

  return argv;
}

ParsedConfigFile parse_config_text(const std::string& content,
                                   const std::string& path) {
  ParsedConfigFile parsed;

  mini_json::Value root;
  try {
    root = mini_json::parse(content);
  } catch (const mini_json::ParseError& e) {
    std::ostringstream oss;
    oss << "invalid JSON at byte " << e.position << ": " << e.what();
    parsed.reason = oss.str();
    return parsed;
  }

  const auto* root_object = root.object_or_null();
  if (!root_object) {
    parsed.reason = "top-level JSON must be an object";
    return parsed;
  }

  const mini_json::Value* applications_value = root.get("applications");
  if (!applications_value) {
    parsed.reason = "missing required top-level key: applications";
    return parsed;
  }

  const auto* applications_array = applications_value->array_or_null();
  if (!applications_array) {
    parsed.reason = "applications must be an array";
    return parsed;
  }

  for (std::size_t i = 0; i < applications_array->size(); ++i) {
    const mini_json::Value& entry = (*applications_array)[i];
    const auto* obj = entry.object_or_null();
    if (!obj) {
      std::ostringstream oss;
      oss << path << ": applications[" << i << "] skipped: entry must be an object";
      log_warn(oss.str());
      continue;
    }

    const mini_json::Value* name_value = entry.get("name");
    if (!name_value || !name_value->is_string()) {
      std::ostringstream oss;
      oss << path << ": applications[" << i
          << "] skipped: name is required and must be a string";
      log_warn(oss.str());
      continue;
    }

    const std::string name = trim(name_value->string_value);
    if (name.empty()) {
      std::ostringstream oss;
      oss << path << ": applications[" << i << "] skipped: name must be non-empty";
      log_warn(oss.str());
      continue;
    }

    const mini_json::Value* icon_value = entry.get("icon");
    if (!icon_value || !icon_value->is_string()) {
      std::ostringstream oss;
      oss << path << ": applications[" << i
          << "] skipped: icon is required and must be a string";
      log_warn(oss.str());
      continue;
    }

    const auto icon = icon_from_string(icon_value->string_value);
    if (!icon.has_value()) {
      std::ostringstream oss;
      oss << path << ": applications[" << i
          << "] skipped: icon must be one of {information, warning, error, question, working}";
      log_warn(oss.str());
      continue;
    }

    std::optional<std::vector<std::string>> argv;
    const mini_json::Value* argv_value = entry.get("argv");
    if (argv_value) {
      std::string reason;
      argv = parse_argv_array(*argv_value, reason);
      if (!argv.has_value()) {
        std::ostringstream oss;
        oss << path << ": applications[" << i << "] skipped: invalid argv: " << reason;
        log_warn(oss.str());
        continue;
      }

      if (entry.get("command") != nullptr) {
        std::ostringstream oss;
        oss << path << ": applications[" << i
            << "] warning: both argv and command present; command ignored";
        log_warn(oss.str());
      }
    } else {
      const mini_json::Value* command_value = entry.get("command");
      if (!command_value || !command_value->is_string()) {
        std::ostringstream oss;
        oss << path << ": applications[" << i
            << "] skipped: requires argv (array) or command (string)";
        log_warn(oss.str());
        continue;
      }

      std::vector<std::string> tokens;
      std::string reason;
      if (!tokenize_command_safely(command_value->string_value, tokens, reason)) {
        std::ostringstream oss;
        oss << path << ": applications[" << i
            << "] skipped: command rejected: " << reason;
        log_warn(oss.str());
        continue;
      }

      argv = std::move(tokens);
    }

    ApplicationEntry app;
    app.name = name;
    app.icon = *icon;
    app.argv = std::move(*argv);

    parsed.applications.push_back(std::move(app));
  }

  if (parsed.applications.empty()) {
    parsed.reason = "configuration has zero valid applications";
    return parsed;
  }

  parsed.valid = true;
  return parsed;
}

std::vector<std::string> build_config_candidates(
    const std::optional<std::string>& config_override) {
  std::vector<std::string> paths;

  if (config_override.has_value()) {
    paths.push_back(expand_tilde(*config_override));
  }

  const char* xdg_config_home = std::getenv("XDG_CONFIG_HOME");
  if (xdg_config_home && std::strlen(xdg_config_home) > 0) {
    paths.emplace_back(std::string(xdg_config_home) + "/toyger-wardrobe/apps.json");
  }

  const char* home = std::getenv("HOME");
  if (home && std::strlen(home) > 0) {
    paths.emplace_back(std::string(home) + "/.config/toyger-wardrobe/apps.json");
  }

  paths.emplace_back("/etc/toyger-wardrobe/apps.json");

  std::vector<std::string> unique_paths;
  unique_paths.reserve(paths.size());
  std::unordered_set<std::string> seen;
  for (const auto& path : paths) {
    if (seen.insert(path).second) {
      unique_paths.push_back(path);
    }
  }

  return unique_paths;
}

LoadConfigResult load_configuration(
    const std::optional<std::string>& config_override) {
  const std::vector<std::string> candidates = build_config_candidates(config_override);

  std::vector<std::string> failures;
  for (const auto& path : candidates) {
    std::string content;
    std::string read_reason;
    if (!read_file_to_string(path, content, read_reason)) {
      std::ostringstream oss;
      oss << path << ": " << read_reason;
      failures.push_back(oss.str());
      log_warn(oss.str());
      continue;
    }

    ParsedConfigFile parsed = parse_config_text(content, path);
    if (!parsed.valid) {
      std::ostringstream oss;
      oss << path << ": " << parsed.reason;
      failures.push_back(oss.str());
      log_warn(oss.str());
      continue;
    }

    log_info("selected config path: " + path);
    log_info("config parse success with " + std::to_string(parsed.applications.size()) +
             " application(s)");

    LoadConfigResult result;
    result.ok = true;
    result.applications = std::move(parsed.applications);
    result.selected_path = path;
    return result;
  }

  LoadConfigResult result;
  result.ok = false;

  std::ostringstream message;
  message << "No readable valid configuration file found.\n";
  message << "Search order:\n";
  for (std::size_t i = 0; i < candidates.size(); ++i) {
    message << "  " << (i + 1) << ". " << candidates[i] << '\n';
  }
  message << "\nDiagnostics:\n";
  for (const auto& failure : failures) {
    message << "- " << failure << '\n';
  }

  result.error_message = message.str();
  return result;
}

void set_dialog_title(Widget dialog, const std::string& title) {
  Widget shell = XtParent(dialog);
  if (shell) {
    XtVaSetValues(shell, XmNtitle, title.c_str(), nullptr);
  }
}

void show_runtime_error_dialog(Widget parent, const std::string& title,
                               const std::string& message) {
  XmString xm_message = XmStringCreateLocalized(const_cast<char*>(message.c_str()));
  Arg args[1];
  int n = 0;
  XtSetArg(args[n], XmNmessageString, xm_message);
  ++n;

  Widget dialog = XmCreateErrorDialog(parent, const_cast<char*>("runtimeErrorDialog"),
                                      args, n);
  XmStringFree(xm_message);

  XtUnmanageChild(XmMessageBoxGetChild(dialog, XmDIALOG_HELP_BUTTON));
  XtUnmanageChild(XmMessageBoxGetChild(dialog, XmDIALOG_CANCEL_BUTTON));
  set_dialog_title(dialog, title);

  XtManageChild(dialog);
}

struct BlockingDialogState {
  bool done = false;
};

void blocking_dialog_close_cb(Widget, XtPointer client_data, XtPointer) {
  auto* state = static_cast<BlockingDialogState*>(client_data);
  state->done = true;
}

void show_startup_error_dialog(const std::string& message) {
  int argc = 1;
  char app_name[] = "toyger-wardrobe";
  char* argv[] = {app_name, nullptr};

  XtAppContext app_context = nullptr;
  Widget shell = XtVaAppInitialize(
      &app_context, const_cast<char*>("ToygerWardrobe"), nullptr, 0, &argc, argv,
      nullptr, XmNtitle, "toyger-wardrobe: startup error", nullptr);

  XmString xm_message = XmStringCreateLocalized(const_cast<char*>(message.c_str()));
  Arg args[1];
  int n = 0;
  XtSetArg(args[n], XmNmessageString, xm_message);
  ++n;

  Widget dialog =
      XmCreateErrorDialog(shell, const_cast<char*>("startupErrorDialog"), args, n);
  XmStringFree(xm_message);

  XtUnmanageChild(XmMessageBoxGetChild(dialog, XmDIALOG_HELP_BUTTON));
  XtUnmanageChild(XmMessageBoxGetChild(dialog, XmDIALOG_CANCEL_BUTTON));
  set_dialog_title(dialog, "toyger-wardrobe: configuration error");

  BlockingDialogState state;
  XtAddCallback(dialog, XmNokCallback, blocking_dialog_close_cb, &state);
  XtAddCallback(dialog, XmNdestroyCallback, blocking_dialog_close_cb, &state);

  XtManageChild(dialog);
  XtRealizeWidget(shell);

  while (!state.done) {
    XtAppProcessEvent(app_context, XtIMAll);
  }

  XtDestroyWidget(shell);
  XtDestroyApplicationContext(app_context);
}

std::string local_time_hhmmss() {
  std::time_t now = std::time(nullptr);
  std::tm local_tm;
  localtime_r(&now, &local_tm);

  char buf[9] = {0};
  if (std::strftime(buf, sizeof(buf), "%H:%M:%S", &local_tm) == 0) {
    return "00:00:00";
  }

  return std::string(buf);
}

std::string get_hostname() {
  char hostname[256] = {0};
  if (gethostname(hostname, sizeof(hostname) - 1) != 0) {
    return "unknown";
  }
  hostname[sizeof(hostname) - 1] = '\0';
  return std::string(hostname);
}

std::string get_logged_in_username() {
  const char* user_env = std::getenv("USER");
  if (user_env != nullptr && user_env[0] != '\0') {
    return std::string(user_env);
  }

  const passwd* pw = getpwuid(getuid());
  if (pw != nullptr && pw->pw_name != nullptr && pw->pw_name[0] != '\0') {
    return std::string(pw->pw_name);
  }

  return "unknown";
}

std::optional<std::time_t> get_boot_time() {
  int mib[2] = {CTL_KERN, KERN_BOOTTIME};
  timeval boottime;
  std::memset(&boottime, 0, sizeof(boottime));

  std::size_t len = sizeof(boottime);
  if (sysctl(mib, 2, &boottime, &len, nullptr, 0) != 0) {
    return std::nullopt;
  }

  if (len != sizeof(boottime) || boottime.tv_sec <= 0) {
    return std::nullopt;
  }

  return static_cast<std::time_t>(boottime.tv_sec);
}

std::string format_uptime(std::optional<std::time_t> boot_time) {
  if (!boot_time.has_value()) {
    return "unknown";
  }

  const std::time_t now = std::time(nullptr);
  if (now <= *boot_time) {
    return "00:00:00";
  }

  long long elapsed = static_cast<long long>(now - *boot_time);
  const long long days = elapsed / 86400;
  elapsed %= 86400;

  const long long hours = elapsed / 3600;
  elapsed %= 3600;
  const long long minutes = elapsed / 60;
  const long long seconds = elapsed % 60;

  char hhmmss[16] = {0};
  std::snprintf(hhmmss, sizeof(hhmmss), "%02lld:%02lld:%02lld", hours, minutes,
                seconds);

  if (days > 0) {
    return std::to_string(days) + "d " + hhmmss;
  }
  return std::string(hhmmss);
}

void refresh_status(RuntimeState& state) {
  const std::string primary = "Time: " + local_time_hhmmss() + "    Host: " +
                              state.hostname + "    User: " + state.username +
                              "    Uptime: " + format_uptime(state.boot_time);
  const std::string secondary = "Last opened: " + state.last_opened;

  XmString xm_primary = XmStringCreateLocalized(const_cast<char*>(primary.c_str()));
  XtVaSetValues(state.status_primary_label, XmNlabelString, xm_primary, nullptr);
  XmStringFree(xm_primary);

  XmString xm_secondary =
      XmStringCreateLocalized(const_cast<char*>(secondary.c_str()));
  XtVaSetValues(state.status_secondary_label, XmNlabelString, xm_secondary, nullptr);
  XmStringFree(xm_secondary);
}

void clock_timeout_cb(XtPointer client_data, XtIntervalId*) {
  auto* state = static_cast<RuntimeState*>(client_data);
  refresh_status(*state);
  XtAppAddTimeOut(state->app_context, 1000, clock_timeout_cb, client_data);
}

bool set_fd_cloexec(int fd, std::string& error_reason) {
  int flags = fcntl(fd, F_GETFD);
  if (flags < 0) {
    error_reason = std::string("fcntl(F_GETFD) failed: ") + std::strerror(errno);
    return false;
  }

  if (fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0) {
    error_reason = std::string("fcntl(F_SETFD) failed: ") + std::strerror(errno);
    return false;
  }

  return true;
}

bool launch_application_direct(const ApplicationEntry& app, std::string& reason) {
  int pipe_fds[2] = {-1, -1};
  if (pipe(pipe_fds) != 0) {
    reason = std::string("pipe failed: ") + std::strerror(errno);
    return false;
  }

  std::string cloexec_reason;
  if (!set_fd_cloexec(pipe_fds[0], cloexec_reason) ||
      !set_fd_cloexec(pipe_fds[1], cloexec_reason)) {
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    reason = cloexec_reason;
    return false;
  }

  const pid_t pid = fork();
  if (pid < 0) {
    reason = std::string("fork failed: ") + std::strerror(errno);
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    return false;
  }

  if (pid == 0) {
    close(pipe_fds[0]);

    auto report_errno_and_exit = [&](int err) {
      (void)write(pipe_fds[1], &err, sizeof(err));
      _exit(127);
    };

    if (setsid() < 0) {
      report_errno_and_exit(errno);
    }

    const int dev_null_fd = open("/dev/null", O_RDWR);
    if (dev_null_fd >= 0) {
      (void)dup2(dev_null_fd, STDIN_FILENO);
      (void)dup2(dev_null_fd, STDOUT_FILENO);
      (void)dup2(dev_null_fd, STDERR_FILENO);
      if (dev_null_fd > STDERR_FILENO) {
        close(dev_null_fd);
      }
    }

    std::vector<char*> argv;
    argv.reserve(app.argv.size() + 1);
    for (const auto& arg : app.argv) {
      argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);

    execvp(argv[0], argv.data());
    report_errno_and_exit(errno);
  }

  close(pipe_fds[1]);

  int child_errno = 0;
  const ssize_t read_size = read(pipe_fds[0], &child_errno, sizeof(child_errno));
  close(pipe_fds[0]);

  if (read_size == 0) {
    return true;
  }

  if (read_size < 0) {
    reason = std::string("failed to read child status: ") + std::strerror(errno);
    return false;
  }

  if (read_size == static_cast<ssize_t>(sizeof(child_errno))) {
    reason = std::strerror(child_errno);
    return false;
  }

  reason = "child launch status stream was truncated";
  return false;
}

void launch_activate_cb(Widget, XtPointer client_data, XtPointer) {
  auto* context = static_cast<LaunchContext*>(client_data);
  RuntimeState& state = *context->runtime;
  const ApplicationEntry& app = state.applications[context->app_index];

  log_info("launch attempt: " + app.name);

  std::string reason;
  if (!launch_application_direct(app, reason)) {
    log_error("launch failure for '" + app.name + "': " + reason);

    show_runtime_error_dialog(
        state.toplevel, "Launch failed",
        "Failed to launch '" + app.name + "'.\nReason: " + reason);
    return;
  }

  state.last_opened = app.name;
  refresh_status(state);
}

void icon_click_cb(Widget widget, XtPointer client_data, XEvent* event, Boolean*) {
  if (event->type != ButtonRelease) {
    return;
  }

  const auto* button_event = reinterpret_cast<XButtonEvent*>(event);
  if (button_event->button != Button1) {
    return;
  }

  launch_activate_cb(widget, client_data, nullptr);
}

void quit_action(Widget, XEvent*, String*, Cardinal*) {
  if (g_runtime_state != nullptr) {
    g_runtime_state->should_exit = true;
  }
}

void wm_delete_cb(Widget, XtPointer client_data, XtPointer) {
  auto* state = static_cast<RuntimeState*>(client_data);
  state->should_exit = true;
}

void install_quit_translation(Widget widget, XtTranslations translations) {
  XtOverrideTranslations(widget, translations);
}

void apply_fixed_size_hints(Display* display, Window window, int width, int height) {
  XSizeHints hints;
  std::memset(&hints, 0, sizeof(hints));
  hints.flags = PMinSize | PMaxSize;
  hints.min_width = width;
  hints.max_width = width;
  hints.min_height = height;
  hints.max_height = height;
  XSetWMNormalHints(display, window, &hints);
}

void center_window(Display* display, Window window, int width, int height) {
  const int screen = DefaultScreen(display);
  int x = (DisplayWidth(display, screen) - width) / 2;
  int y = (DisplayHeight(display, screen) - height) / 2;
  if (x < 0) {
    x = 0;
  }
  if (y < 0) {
    y = 0;
  }
  XMoveResizeWindow(display, window, x, y, static_cast<unsigned int>(width),
                    static_cast<unsigned int>(height));
}

void apply_borderless_hints(Display* display, Window window) {
  struct MotifWmHints {
    unsigned long flags;
    unsigned long functions;
    unsigned long decorations;
    long input_mode;
    unsigned long status;
  };

  constexpr unsigned long kMwmHintsFunctions = 1UL << 0;
  constexpr unsigned long kMwmHintsDecorations = 1UL << 1;

  MotifWmHints hints;
  hints.flags = kMwmHintsFunctions | kMwmHintsDecorations;
  hints.functions = 0;
  hints.decorations = 0;
  hints.input_mode = 0;
  hints.status = 0;

  Atom property = XInternAtom(display, "_MOTIF_WM_HINTS", False);
  XChangeProperty(display, window, property, property, 32, PropModeReplace,
                  reinterpret_cast<unsigned char*>(&hints), 5);
}

void apply_always_on_top_hints(Display* display, Window window) {
  Atom wm_state = XInternAtom(display, "_NET_WM_STATE", False);
  Atom wm_state_above = XInternAtom(display, "_NET_WM_STATE_ABOVE", False);
  Atom wm_state_stays_on_top =
      XInternAtom(display, "_NET_WM_STATE_STAYS_ON_TOP", True);

  if (wm_state == None || wm_state_above == None) {
    log_warn("EWMH _NET_WM_STATE/_ABOVE not available; always-on-top is best effort");
    XRaiseWindow(display, window);
    return;
  }

  std::vector<Atom> state_list = {wm_state_above};
  if (wm_state_stays_on_top != None) {
    state_list.push_back(wm_state_stays_on_top);
  }

  XChangeProperty(display, window, wm_state, XA_ATOM, 32, PropModeReplace,
                  reinterpret_cast<unsigned char*>(state_list.data()),
                  static_cast<int>(state_list.size()));

  XEvent event;
  std::memset(&event, 0, sizeof(event));
  event.xclient.type = ClientMessage;
  event.xclient.window = window;
  event.xclient.message_type = wm_state;
  event.xclient.format = 32;
  event.xclient.data.l[0] = 1;
  event.xclient.data.l[1] = static_cast<long>(wm_state_above);
  event.xclient.data.l[2] =
      wm_state_stays_on_top == None ? 0 : static_cast<long>(wm_state_stays_on_top);
  event.xclient.data.l[3] = 1;
  event.xclient.data.l[4] = 0;

  XSendEvent(display, DefaultRootWindow(display), False,
             SubstructureRedirectMask | SubstructureNotifyMask, &event);
  XRaiseWindow(display, window);
}

Pixmap load_stock_icon_pixmap(Widget reference_widget, IconKind icon) {
  Pixel foreground = 0;
  Pixel background = 0;
  XtVaGetValues(reference_widget, XmNforeground, &foreground, XmNbackground, &background,
                nullptr);

  const char* name = icon_to_pixmap_name(icon);
  Pixmap pixmap =
      XmGetPixmap(XtScreen(reference_widget), const_cast<char*>(name), foreground,
                  background);

  if (pixmap == XmUNSPECIFIED_PIXMAP) {
    log_warn(std::string("Motif stock pixmap unavailable: ") + name +
             "; falling back to xm_question");
    pixmap = XmGetPixmap(XtScreen(reference_widget), const_cast<char*>("xm_question"),
                         foreground, background);
  }

  return pixmap;
}

void install_sigchld_ignoring() {
  struct sigaction sa;
  std::memset(&sa, 0, sizeof(sa));
  sa.sa_handler = SIG_IGN;
  sa.sa_flags = SA_NOCLDWAIT;
  sigemptyset(&sa.sa_mask);

  if (sigaction(SIGCHLD, &sa, nullptr) != 0) {
    log_warn(std::string("sigaction(SIGCHLD) failed: ") + std::strerror(errno));
  }
}

bool parse_cli_options(int argc, char** argv, CliOptions& options,
                       std::string& error) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];

    if (arg == "--config") {
      if (i + 1 >= argc) {
        error = "--config requires a path argument";
        return false;
      }
      options.config_override = argv[++i];
      continue;
    }

    if (arg == "--help") {
      std::cout << "Usage: toyger-wardrobe [--config /path/to/apps.json]\n";
      std::cout << "Config search order:\n";
      std::cout << "  1. --config /path/to/file.json\n";
      std::cout << "  2. $XDG_CONFIG_HOME/toyger-wardrobe/apps.json\n";
      std::cout << "  3. ~/.config/toyger-wardrobe/apps.json\n";
      std::cout << "  4. /etc/toyger-wardrobe/apps.json\n";
      std::exit(0);
    }

    error = "unknown argument: " + arg;
    return false;
  }

  return true;
}

Widget build_main_ui(RuntimeState& state) {
  Widget form = XmCreateForm(state.toplevel, const_cast<char*>("mainForm"), nullptr, 0);
  install_quit_translation(form, state.quit_translations);
  XtManageChild(form);

  Widget header =
      XmCreateForm(form, const_cast<char*>("applicationsHeader"), nullptr, 0);
  install_quit_translation(header, state.quit_translations);
  XtVaSetValues(header, XmNfractionBase, 100, XmNshadowThickness, 0,
                XmNmarginHeight, 0, XmNmarginWidth, 0, XmNtopAttachment,
                XmATTACH_FORM, XmNleftAttachment, XmATTACH_FORM,
                XmNrightAttachment, XmATTACH_FORM, nullptr);
  XtManageChild(header);

  Widget header_icon =
      XmCreateLabel(header, const_cast<char*>("headerIconLabel"), nullptr, 0);
  install_quit_translation(header_icon, state.quit_translations);
  XmString header_icon_text = XmStringCreateLocalized(const_cast<char*>("Icon"));
  XtVaSetValues(header_icon, XmNlabelString, header_icon_text, XmNrecomputeSize,
                False, XmNwidth, kIconCellWidth, XmNalignment,
                XmALIGNMENT_CENTER, XmNmarginHeight, 8, XmNleftAttachment,
                XmATTACH_FORM, XmNtopAttachment, XmATTACH_FORM,
                XmNbottomAttachment, XmATTACH_FORM, nullptr);
  XmStringFree(header_icon_text);
  XtManageChild(header_icon);

  Widget header_column_separator =
      XmCreateSeparator(header, const_cast<char*>("headerColumnSeparator"), nullptr, 0);
  install_quit_translation(header_column_separator, state.quit_translations);
  XtVaSetValues(header_column_separator, XmNorientation, XmVERTICAL,
                XmNleftAttachment, XmATTACH_WIDGET, XmNleftWidget, header_icon,
                XmNtopAttachment, XmATTACH_FORM, XmNbottomAttachment, XmATTACH_FORM,
                nullptr);
  XtManageChild(header_column_separator);

  Widget header_name =
      XmCreateLabel(header, const_cast<char*>("headerNameLabel"), nullptr, 0);
  install_quit_translation(header_name, state.quit_translations);

  XmString header_name_text =
      XmStringCreateLocalized(const_cast<char*>("Application"));
  XtVaSetValues(header_name, XmNlabelString, header_name_text, XmNalignment,
                XmALIGNMENT_BEGINNING, XmNmarginHeight, 8, XmNmarginLeft, 10,
                XmNleftAttachment, XmATTACH_WIDGET, XmNleftWidget,
                header_column_separator, XmNrecomputeSize, False, XmNwidth,
                kNameCellWidth, XmNrightAttachment, XmATTACH_NONE,
                XmNtopAttachment, XmATTACH_FORM, XmNbottomAttachment, XmATTACH_FORM,
                nullptr);
  XmStringFree(header_name_text);
  XtManageChild(header_name);

  Widget header_fill_separator =
      XmCreateSeparator(header, const_cast<char*>("headerFillSeparator"), nullptr, 0);
  install_quit_translation(header_fill_separator, state.quit_translations);
  XtVaSetValues(header_fill_separator, XmNorientation, XmVERTICAL,
                XmNleftAttachment, XmATTACH_WIDGET, XmNleftWidget, header_name,
                XmNtopAttachment, XmATTACH_FORM, XmNbottomAttachment, XmATTACH_FORM,
                nullptr);
  XtManageChild(header_fill_separator);

  Widget header_fill_pattern =
      XmCreateLabel(header, const_cast<char*>("headerFillPattern"), nullptr, 0);
  install_quit_translation(header_fill_pattern, state.quit_translations);
  XmString header_texture_text = XmStringCreateLocalized(
      const_cast<char*>(texture_fill_pattern().c_str()));
  XtVaSetValues(header_fill_pattern, XmNlabelString, header_texture_text,
                XmNrecomputeSize, False, XmNalignment, XmALIGNMENT_BEGINNING,
                XmNmarginHeight, 8, XmNmarginLeft, 0, XmNmarginRight, 0,
                XmNleftAttachment, XmATTACH_WIDGET, XmNleftWidget,
                header_fill_separator, XmNrightAttachment, XmATTACH_FORM,
                XmNtopAttachment, XmATTACH_FORM, XmNbottomAttachment, XmATTACH_FORM,
                nullptr);
  XmStringFree(header_texture_text);
  XtManageChild(header_fill_pattern);

  Widget header_separator =
      XmCreateSeparator(form, const_cast<char*>("headerSeparator"), nullptr, 0);
  install_quit_translation(header_separator, state.quit_translations);
  XtVaSetValues(header_separator, XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget,
                header, XmNleftAttachment, XmATTACH_FORM, XmNrightAttachment,
                XmATTACH_FORM, nullptr);
  XtManageChild(header_separator);

  Widget footer = XmCreateForm(form, const_cast<char*>("statusFooter"), nullptr, 0);
  install_quit_translation(footer, state.quit_translations);
  XtVaSetValues(footer, XmNfractionBase, 100, XmNmarginHeight, 0, XmNmarginWidth, 0,
                XmNbottomAttachment, XmATTACH_FORM, XmNleftAttachment, XmATTACH_FORM,
                XmNrightAttachment, XmATTACH_FORM, nullptr);
  XtManageChild(footer);

  Widget footer_primary =
      XmCreateLabel(footer, const_cast<char*>("statusPrimary"), nullptr, 0);
  install_quit_translation(footer_primary, state.quit_translations);
  XtVaSetValues(footer_primary, XmNalignment, XmALIGNMENT_BEGINNING,
                XmNmarginHeight, 4, XmNmarginWidth, 8, XmNtopAttachment,
                XmATTACH_FORM, XmNleftAttachment, XmATTACH_FORM,
                XmNrightAttachment, XmATTACH_FORM, nullptr);
  XtManageChild(footer_primary);

  Widget footer_secondary =
      XmCreateLabel(footer, const_cast<char*>("statusSecondary"), nullptr, 0);
  install_quit_translation(footer_secondary, state.quit_translations);
  XtVaSetValues(footer_secondary, XmNalignment, XmALIGNMENT_BEGINNING,
                XmNmarginHeight, 3, XmNmarginWidth, 8, XmNtopAttachment,
                XmATTACH_WIDGET, XmNtopWidget, footer_primary, XmNbottomAttachment,
                XmATTACH_FORM, XmNleftAttachment, XmATTACH_FORM,
                XmNrightAttachment, XmATTACH_FORM, nullptr);
  XtManageChild(footer_secondary);

  state.status_primary_label = footer_primary;
  state.status_secondary_label = footer_secondary;

  Widget separator =
      XmCreateSeparator(form, const_cast<char*>("footerSeparator"), nullptr, 0);
  install_quit_translation(separator, state.quit_translations);
  XtVaSetValues(separator, XmNbottomAttachment, XmATTACH_WIDGET,
                XmNbottomWidget, footer, XmNleftAttachment, XmATTACH_FORM,
                XmNrightAttachment, XmATTACH_FORM, nullptr);
  XtManageChild(separator);

  Arg scrolled_args[3];
  int scrolled_argc = 0;
  XtSetArg(scrolled_args[scrolled_argc], XmNscrollingPolicy, XmAUTOMATIC);
  ++scrolled_argc;
  XtSetArg(scrolled_args[scrolled_argc], XmNvisualPolicy, XmCONSTANT);
  ++scrolled_argc;
  XtSetArg(scrolled_args[scrolled_argc], XmNscrollBarDisplayPolicy, XmAS_NEEDED);
  ++scrolled_argc;

  Widget scrolled = XmCreateScrolledWindow(form, const_cast<char*>("appsScrolled"),
                                           scrolled_args, scrolled_argc);
  install_quit_translation(scrolled, state.quit_translations);
  XtVaSetValues(scrolled, XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget,
                header_separator,
                XmNleftAttachment, XmATTACH_FORM, XmNrightAttachment, XmATTACH_FORM,
                XmNbottomAttachment, XmATTACH_WIDGET, XmNbottomWidget,
                separator, nullptr);
  XtManageChild(scrolled);

  Widget vertical_scrollbar = nullptr;
  Widget horizontal_scrollbar = nullptr;
  XtVaGetValues(scrolled, XmNverticalScrollBar, &vertical_scrollbar,
                XmNhorizontalScrollBar, &horizontal_scrollbar, nullptr);
  if (vertical_scrollbar != nullptr) {
    install_quit_translation(vertical_scrollbar, state.quit_translations);
  }
  if (horizontal_scrollbar != nullptr) {
    install_quit_translation(horizontal_scrollbar, state.quit_translations);
  }

  Widget list_column =
      XmCreateRowColumn(scrolled, const_cast<char*>("applicationsColumn"), nullptr, 0);
  install_quit_translation(list_column, state.quit_translations);
  XtVaSetValues(list_column, XmNorientation, XmVERTICAL, XmNpacking, XmPACK_TIGHT,
                XmNnumColumns, 1, XmNspacing, 0, XmNmarginHeight, 0,
                XmNmarginWidth, 0, XmNentryBorder, 0, XmNadjustLast, False,
                XmNisAligned, False, nullptr);
  XtManageChild(list_column);

  for (std::size_t i = 0; i < state.applications.size(); ++i) {
    const ApplicationEntry& app = state.applications[i];

    auto launch_context = std::make_unique<LaunchContext>();
    launch_context->runtime = &state;
    launch_context->app_index = i;
    LaunchContext* launch_context_ptr = launch_context.get();
    state.launch_contexts.push_back(std::move(launch_context));

    Widget row =
        XmCreateForm(list_column, const_cast<char*>("applicationRow"), nullptr, 0);
    install_quit_translation(row, state.quit_translations);
    XtVaSetValues(row, XmNheight, kRowHeight, XmNfractionBase, 100,
                  XmNshadowThickness, 0, XmNmarginHeight, 0, XmNmarginWidth, 0,
                  nullptr);

    Widget icon_label =
        XmCreateLabel(row, const_cast<char*>("applicationIcon"), nullptr, 0);
    install_quit_translation(icon_label, state.quit_translations);

    const Pixmap icon_pixmap = load_stock_icon_pixmap(state.toplevel, app.icon);
    XtVaSetValues(icon_label, XmNlabelType, XmPIXMAP, XmNlabelPixmap, icon_pixmap,
                  XmNrecomputeSize, False, XmNwidth, kIconCellWidth,
                  XmNleftAttachment, XmATTACH_FORM, XmNtopAttachment,
                  XmATTACH_FORM, XmNbottomAttachment, XmATTACH_FORM,
                  XmNmarginLeft, 10, XmNmarginRight, 10, XmNalignment,
                  XmALIGNMENT_CENTER, nullptr);
    XtAddEventHandler(icon_label, ButtonReleaseMask, False, icon_click_cb,
                      launch_context_ptr);
    XtManageChild(icon_label);

    Widget row_column_separator =
        XmCreateSeparator(row, const_cast<char*>("rowColumnSeparator"), nullptr, 0);
    install_quit_translation(row_column_separator, state.quit_translations);
    XtVaSetValues(row_column_separator, XmNorientation, XmVERTICAL,
                  XmNleftAttachment, XmATTACH_WIDGET, XmNleftWidget, icon_label,
                  XmNtopAttachment, XmATTACH_FORM, XmNbottomAttachment, XmATTACH_FORM,
                  nullptr);
    XtManageChild(row_column_separator);

    Widget button =
        XmCreatePushButton(row, const_cast<char*>("applicationButton"), nullptr, 0);
    install_quit_translation(button, state.quit_translations);

    XmString label = XmStringCreateLocalized(const_cast<char*>(app.name.c_str()));
    XtVaSetValues(button, XmNlabelString, label, XmNalignment,
                  XmALIGNMENT_BEGINNING, XmNleftAttachment, XmATTACH_WIDGET,
                  XmNleftWidget, row_column_separator, XmNrightAttachment,
                  XmATTACH_NONE, XmNrecomputeSize, False, XmNwidth,
                  kNameCellWidth, XmNtopAttachment,
                  XmATTACH_FORM, XmNbottomAttachment, XmATTACH_FORM,
                  XmNhighlightThickness, 0, XmNshadowThickness, 0,
                  XmNmarginLeft, 12, XmNmarginRight, 12, XmNmarginTop, 8,
                  XmNmarginBottom, 8,
                  nullptr);
    XmStringFree(label);

    apply_ui_font_if_available(button, state);
    XtAddCallback(button, XmNactivateCallback, launch_activate_cb, launch_context_ptr);
    XtManageChild(button);

    Widget row_fill_separator =
        XmCreateSeparator(row, const_cast<char*>("rowFillSeparator"), nullptr, 0);
    install_quit_translation(row_fill_separator, state.quit_translations);
    XtVaSetValues(row_fill_separator, XmNorientation, XmVERTICAL,
                  XmNleftAttachment, XmATTACH_WIDGET, XmNleftWidget, button,
                  XmNtopAttachment, XmATTACH_FORM, XmNbottomAttachment, XmATTACH_FORM,
                  nullptr);
    XtManageChild(row_fill_separator);

    Widget row_fill_pattern =
        XmCreateLabel(row, const_cast<char*>("rowFillPattern"), nullptr, 0);
    install_quit_translation(row_fill_pattern, state.quit_translations);
    XmString middle_texture_text = XmStringCreateLocalized(
        const_cast<char*>(texture_fill_pattern().c_str()));
    XtVaSetValues(row_fill_pattern, XmNlabelString, middle_texture_text,
                  XmNrecomputeSize, False, XmNalignment, XmALIGNMENT_BEGINNING,
                  XmNmarginHeight, 5, XmNmarginLeft, 0, XmNmarginRight, 0,
                  XmNleftAttachment, XmATTACH_WIDGET, XmNleftWidget,
                  row_fill_separator,
                  XmNrightAttachment, XmATTACH_FORM, XmNtopAttachment, XmATTACH_FORM,
                  XmNbottomAttachment, XmATTACH_FORM, nullptr);
    XmStringFree(middle_texture_text);
    apply_ui_font_if_available(row_fill_pattern, state);
    XtAddEventHandler(row_fill_pattern, ButtonReleaseMask, False, icon_click_cb,
                      launch_context_ptr);
    XtManageChild(row_fill_pattern);

    XtManageChild(row);

    if (i + 1 < state.applications.size()) {
      Widget row_separator =
          XmCreateSeparator(list_column, const_cast<char*>("rowSeparator"), nullptr, 0);
      install_quit_translation(row_separator, state.quit_translations);
      XtVaSetValues(row_separator, XmNleftAttachment, XmATTACH_FORM,
                    XmNrightAttachment, XmATTACH_FORM, nullptr);
      XtManageChild(row_separator);
    }
  }

  refresh_status(state);
  return form;
}

int run_launcher(std::vector<ApplicationEntry> applications,
                 const std::string& selected_config_path) {
  install_sigchld_ignoring();

  RuntimeState state;
  state.applications = std::move(applications);
  state.hostname = get_hostname();
  state.username = get_logged_in_username();
  state.boot_time = get_boot_time();

  XtSetLanguageProc(nullptr, nullptr, nullptr);

  int argc = 1;
  char app_name[] = "toyger-wardrobe";
  char* argv[] = {app_name, nullptr};

  state.toplevel = XtVaAppInitialize(
      &state.app_context, const_cast<char*>("ToygerWardrobe"), nullptr, 0,
      &argc, argv, nullptr, XmNtitle, "toyger-wardrobe", XmNwidth,
      kWindowWidth, XmNheight, kWindowHeight, nullptr);

  if (!state.toplevel) {
    log_error("failed to initialize Xt/Motif top-level widget");
    return 1;
  }

  initialize_ui_font(state);

  g_runtime_state = &state;

  XtActionsRec actions[] = {{const_cast<char*>("tw-quit"), quit_action}};
  XtAppAddActions(state.app_context, actions,
                  static_cast<Cardinal>(sizeof(actions) / sizeof(actions[0])));

  state.quit_translations = XtParseTranslationTable(
      "<Key>Escape: tw-quit()\n<Key>osfCancel: tw-quit()\nCtrl<Key>q: tw-quit()\nCtrl<Key>Q: tw-quit()");
  install_quit_translation(state.toplevel, state.quit_translations);

  Display* display = XtDisplay(state.toplevel);
  const int screen = DefaultScreen(display);
  int x = (DisplayWidth(display, screen) - kWindowWidth) / 2;
  int y = (DisplayHeight(display, screen) - kWindowHeight) / 2;
  if (x < 0) {
    x = 0;
  }
  if (y < 0) {
    y = 0;
  }

  XtVaSetValues(state.toplevel, XmNallowShellResize, False,
                XmNdeleteResponse, XmDO_NOTHING, XtNx, x, XtNy, y,
                XmNwidth, kWindowWidth, XmNheight, kWindowHeight, nullptr);

  (void)build_main_ui(state);

  Atom wm_delete = XmInternAtom(display, const_cast<char*>("WM_DELETE_WINDOW"), False);
  XmAddWMProtocolCallback(state.toplevel, wm_delete, wm_delete_cb, &state);

  log_info("using config: " + selected_config_path);

  XtRealizeWidget(state.toplevel);

  const Window window = XtWindow(state.toplevel);
  apply_borderless_hints(display, window);
  apply_always_on_top_hints(display, window);
  apply_fixed_size_hints(display, window, kWindowWidth, kWindowHeight);
  center_window(display, window, kWindowWidth, kWindowHeight);

  XFlush(display);

  XtAppAddTimeOut(state.app_context, 1000, clock_timeout_cb, &state);

  while (!state.should_exit) {
    XEvent event;
    XtAppNextEvent(state.app_context, &event);
    XtDispatchEvent(&event);
  }

  free_ui_font(state);
  XtDestroyWidget(state.toplevel);
  g_runtime_state = nullptr;
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  CliOptions options;
  std::string cli_error;
  if (!parse_cli_options(argc, argv, options, cli_error)) {
    log_error(cli_error);
    std::cerr << "Usage: toyger-wardrobe [--config /path/to/apps.json]\n";
    return 2;
  }

  const LoadConfigResult config = load_configuration(options.config_override);
  if (!config.ok) {
    log_error(config.error_message);
    show_startup_error_dialog(config.error_message);
    return 1;
  }

  return run_launcher(config.applications, config.selected_path);
}
