/*

  Copyright (c) 2012 by Vinnie Falco (ORIGINAL)

  NOTE (07-14-2026; Dominic Arruda): Implemented several changes.
  Recursively navigates file structure.
  Maps known C++ libraries to prevent inclusion of standard library.

*/
#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <regex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;

//==============================================================================
// Whitespace helper
//==============================================================================

static bool isSpace(char c) {
    switch (c) {
        case ' ': case '\t': case '\r': case '\n': return true;
        default: return false;
    }
}

//==============================================================================
// String utility helpers
//==============================================================================

static std::string trimmed(const std::string& s) {
    size_t start = 0;
    while (start < s.size() && isSpace(s[start])) ++start;
    size_t end = s.size();
    while (end > start && isSpace(s[end - 1])) --end;
    return s.substr(start, end - start);
}

static std::string trimmedStart(const std::string& s) {
    size_t start = 0;
    while (start < s.size() && isSpace(s[start])) ++start;
    return s.substr(start);
}

static std::string trimmedEnd(const std::string& s) {
    if (s.empty()) return s;
    size_t end = s.size();
    while (end > 0 && isSpace(s[end - 1])) --end;
    return s.substr(0, end);
}

static std::string toLower(const std::string& s) {
    std::string r = s;
    std::transform(r.begin(), r.end(), r.begin(), [](unsigned char c) { return std::tolower(c); });
    return r;
}

static bool startsWith(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

static bool startsWithIgnoreCase(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() && toLower(s.substr(0, prefix.size())) == toLower(prefix);
}

static std::string removeCharacters(const std::string& s, const char* chars) {
    std::string r;
    r.reserve(s.size());
    for (char c : s) {
        bool remove = false;
        for (const char* p = chars; *p; ++p) {
            if (c == *p) { remove = true; break; }
        }
        if (!remove) r += c;
    }
    return r;
}

static bool containsIgnoreCase(const std::string& s, const std::string& sub) {
    auto it = std::search(s.begin(), s.end(), sub.begin(), sub.end(),
        [](char a, char b) { return std::tolower(a) == std::tolower(b); });
    return it != s.end();
}

static std::string repeatedString(const std::string& s, size_t n) {
    std::string r;
    r.reserve(s.size() * n);
    for (size_t i = 0; i < n; ++i) r += s;
    return r;
}

static bool matchesWildcard(const std::string& name, const std::string& pattern) {
    size_t n = 0, p = 0, star = std::string::npos, match = 0;
    while (n < name.size()) {
        if (p < pattern.size() && (pattern[p] == name[n] || pattern[p] == '?')) {
            ++n; ++p;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            match = n;
        } else if (star != std::string::npos) {
            p = star + 1;
            n = ++match;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

static std::vector<std::string> splitTokens(const std::string& s, const char* delims) {
    std::vector<std::string> tokens;
    size_t start = 0;
    while (start < s.size()) {
        size_t pos = s.find_first_of(delims, start);
        if (pos == std::string::npos) {
            if (start < s.size()) tokens.push_back(s.substr(start));
            break;
        }
        if (pos > start) tokens.push_back(s.substr(start, pos - start));
        start = pos + 1;
    }
    return tokens;
}

static std::vector<std::string> splitLines(const std::string& s) {
    std::vector<std::string> lines;
    std::istringstream iss(s);
    std::string line;
    while (std::getline(iss, line)) {
        lines.push_back(line);
    }
    return lines;
}

static std::string readFileToString(const fs::path& path) {
    std::ifstream f(path, std::ios::in | std::ios::binary);
    if (!f) return {};
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static int64_t calculateStringHashCode(const std::string& s) {
    int64_t t = 0;
    for (unsigned char c : s) {
        t = t * 65599 + c;
    }
    return t;
}

static int64_t calculateStreamHashCode(std::istream& in) {
    int64_t t = 0;
    const size_t bufferSize = 4096;
    std::vector<uint8_t> buffer(bufferSize);
    for (;;) {
        in.read(reinterpret_cast<char*>(buffer.data()), bufferSize);
        std::streamsize num = in.gcount();
        if (num <= 0) break;
        for (std::streamsize i = 0; i < num; ++i)
            t = t * 65599 + buffer[static_cast<size_t>(i)];
    }
    return t;
}

static int64_t calculateFileHashCode(const fs::path& path) {
    std::ifstream f(path, std::ios::in | std::ios::binary);
    return f ? calculateStreamHashCode(f) : 0;
}

//==============================================================================
// Strip #pragma once and include guards from inlined files
//==============================================================================

static std::string prepareFileForInlining(const std::string& content) {
    auto lines = splitLines(content);
    std::vector<std::string> result;
    result.reserve(lines.size());

    size_t idx = 0;

    while (idx < lines.size()) {
        std::string t = removeCharacters(trimmed(lines[idx]), " \t");
        if (t.empty() || startsWith(lines[idx], "//") || startsWith(lines[idx], "/*")) {
            result.push_back(lines[idx]);
            ++idx;
        } else if (t == "#pragmaonce") {
            ++idx;
        } else {
            break;
        }
    }

    bool hasGuard = false;
    if (idx + 1 < lines.size()) {
        std::string l1 = removeCharacters(trimmed(lines[idx]), " \t");
        std::string l2 = removeCharacters(trimmed(lines[idx + 1]), " \t");
        if (startsWith(l1, "#ifndef") && startsWith(l2, "#define")) {
            std::string guard = trimmed(l1.substr(7));
            std::string def   = trimmed(l2.substr(7));
            if (guard == def) {
                hasGuard = true;
                idx += 2;
            }
        }
    }

    for (; idx < lines.size(); ++idx) {
        std::string t = removeCharacters(trimmed(lines[idx]), " \t");
        if (t == "#pragmaonce") continue;
        result.push_back(lines[idx]);
    }

    if (hasGuard) {
        size_t last = result.size();
        while (last > 0 && trimmed(result[last - 1]).empty()) --last;
        if (last > 0) {
            std::string lastLine = removeCharacters(trimmed(result[last - 1]), " \t");
            if (startsWith(lastLine, "#endif")) {
                result.resize(last - 1);
            }
        }
    }

    std::string out;
    for (auto& l : result) out += l + "\n";
    return out;
}

//==============================================================================
// TemporaryFile RAII helper
//==============================================================================

class TemporaryFile {
public:
    explicit TemporaryFile(const fs::path& target) : target_(target) {
        temp_ = target.parent_path() / (target.stem().string() + "_temp" + target.extension().string());
        int counter = 0;
        while (fs::exists(temp_)) {
            temp_ = target.parent_path() / (target.stem().string() + "_temp" + std::to_string(counter++) + target.extension().string());
        }
    }

    ~TemporaryFile() {
        if (!done_ && fs::exists(temp_)) {
            std::error_code ec;
            fs::remove(temp_, ec);
        }
    }

    const fs::path& getFile() const { return temp_; }

    bool overwriteTargetFileWithTemporary() {
        std::error_code ec;
        fs::rename(temp_, target_, ec);
        if (ec) {
            fs::copy_file(temp_, target_, fs::copy_options::overwrite_existing, ec);
            if (!ec) {
                fs::remove(temp_, ec);
                done_ = true;
                return true;
            }
            return false;
        }
        done_ = true;
        return true;
    }

private:
    fs::path target_;
    fs::path temp_;
    bool done_ = false;
};

//==============================================================================
// Standard Library Header Detection
//==============================================================================

class StandardLibraryDetector {
public:
    StandardLibraryDetector() {
        const char* cpp_stl[] = {
            "algorithm", "bitset", "complex", "deque", "exception", "fstream",
            "functional", "iomanip", "ios", "iosfwd", "iostream", "istream",
            "iterator", "limits", "list", "locale", "map", "memory", "new",
            "numeric", "ostream", "queue", "set", "sstream", "stack",
            "stdexcept", "streambuf", "string", "strstream", "typeinfo",
            "utility", "valarray", "vector",
            "array", "atomic", "chrono", "codecvt", "condition_variable",
            "forward_list", "future", "initializer_list", "mutex", "random",
            "ratio", "regex", "scoped_allocator", "system_error", "thread",
            "tuple", "type_traits", "typeindex", "unordered_map", "unordered_set",
            "shared_mutex",
            "any", "charconv", "execution", "filesystem", "memory_resource",
            "optional", "string_view", "variant",
            "barrier", "bit", "compare", "concepts", "coroutine", "format",
            "latch", "numbers", "ranges", "semaphore", "source_location",
            "span", "stop_token", "syncstream", "version",
            "expected", "flat_map", "flat_set", "generator", "mdspan",
            "print", "spanstream", "stacktrace", "stdfloat", "text_encoding",
            "cassert", "ccomplex", "cctype", "cerrno", "cfenv", "cfloat",
            "cinttypes", "ciso646", "climits", "clocale", "cmath", "csetjmp",
            "csignal", "cstdalign", "cstdarg", "cstdbool", "cstddef", "cstdint",
            "cstdio", "cstdlib", "cstring", "ctgmath", "ctime", "cuchar",
            "cwchar", "cwctype",
            "assert.h", "complex.h", "ctype.h", "errno.h", "fenv.h", "float.h",
            "inttypes.h", "iso646.h", "limits.h", "locale.h", "math.h",
            "setjmp.h", "signal.h", "stdalign.h", "stdarg.h", "stdbool.h",
            "stddef.h", "stdint.h", "stdio.h", "stdlib.h", "string.h",
            "tgmath.h", "time.h", "uchar.h", "wchar.h", "wctype.h",
            "tr1/functional", "tr1/memory", "tr1/random", "tr1/regex",
            "tr1/tuple", "tr1/type_traits", "tr1/unordered_map", "tr1/unordered_set",
            "tr1/utility",
            nullptr
        };

        for (size_t i = 0; cpp_stl[i] != nullptr; ++i) {
            stl_headers_.insert(cpp_stl[i]);
        }
    }

    bool isStandardHeader(const std::string& filename) const {
        return stl_headers_.count(filename) > 0;
    }

private:
    std::unordered_set<std::string> stl_headers_;
};

//==============================================================================
// @remap directive handling
//==============================================================================

class RemapTable {
public:
    RemapTable() : pattern_(
        "[ \\t]*/\\*[ \\t]*@remap[ \\t]+\"([^\"]+)\"[ \\t]+\"([^\"]+)\"[ \\t]*\\*/[ \\t]*")
    {}

    bool processLine(const std::string& line) {
        std::smatch result;
        if (std::regex_match(line, result, pattern_)) {
            std::string from = result[1].str();
            std::string to = result[2].str();
            if (table_.find(from) == table_.end()) {
                table_[from] = to;
            } else {
                std::cout << "Warning: duplicate @remap directive" << std::endl;
            }
            return true;
        }
        return false;
    }

    const std::string* find(const std::string& key) const {
        auto it = table_.find(key);
        return (it != table_.end()) ? &it->second : nullptr;
    }

private:
    std::regex pattern_;
    std::unordered_map<std::string, std::string> table_;
};

//==============================================================================
// Amalgamator
//==============================================================================

class Amalgamator {
public:
    RemapTable remapTable;

    explicit Amalgamator(std::string toolName)
        : name_(std::move(toolName))
    {
        setWildcards("*.cpp;*.c;*.hpp;*.h");
    }

    const std::string& name() const { return name_; }

    void setCheckSystemIncludes(bool check) { checkSystemIncludes_ = check; }

    void setWildcards(const std::string& wildcards) {
        wildcards_.clear();
        auto tokens = splitTokens(wildcards, ";,\\'\\\"");
        for (auto& t : tokens) {
            auto tr = trimmed(t);
            if (!tr.empty()) wildcards_.push_back(tr);
        }
    }

    void setTemplate(const std::string& fileName) {
        templateFile_ = fs::absolute(fileName);
    }

    void setTarget(const std::string& fileName) {
        targetFile_ = fs::absolute(fileName);
    }

    void setVerbose() { verbose_ = true; }
    void setConvertSpacesToTabs() { tabs_ = true; }

    void addDirectoryToSearch(const std::string& dir) {
        fs::path p(dir);
        if (p.is_relative()) p = fs::absolute(p);
        directoriesToSearch_.push_back(p.string());
    }

    void addPreventReinclude(const std::string& identifier) {
        preventReincludes_.insert(identifier);
    }

    void addForceReinclude(const std::string& identifier) {
        forceReincludes_.insert(identifier);
    }

    void addDefine(const std::string& name, const std::string& value) {
        macrosDefined_[name] = value;
    }

    bool process() {
        if (!fs::exists(templateFile_) || !fs::is_regular_file(templateFile_)) {
            std::cout << name() << " The template file doesn't exist!\n\n";
            return true;
        }

        std::cout << "Building: " << targetFile_.string() << "...\n";

        TemporaryFile temp(targetFile_);
        {
            std::ofstream out(temp.getFile(), std::ios::out | std::ios::binary | std::ios::trunc);
            if (!out) {
                std::cout << "  \n!! ERROR - couldn't write to the target file: "
                          << temp.getFile().string() << "\n\n";
                return true;
            }

            std::unordered_set<std::string> alreadyIncluded;
            std::unordered_set<int64_t> alreadyIncludedHashes;
            std::vector<std::string> includesToIgnore;

            if (!parseFile(targetFile_, out, templateFile_,
                           alreadyIncluded, alreadyIncludedHashes,
                           includesToIgnore, 0, false)) {
                return false;
            }
        }

        if (calculateFileHashCode(targetFile_) == calculateFileHashCode(temp.getFile())) {
            std::cout << "No need to write - new file is identical\n";
            return false;
        }

        if (!temp.overwriteTargetFileWithTemporary()) {
            std::cout << "ERROR - couldn't write to the target file: "
                      << targetFile_.string() << "\n\n";
            return true;
        }

        return false;
    }

private:
    static constexpr int maxRecursionDepth = 100;

    struct ParsedInclude {
        bool isIncludeLine = false;
        bool isAngleBracket = false;
        bool preventReinclude = false;
        bool forceReinclude = false;
        size_t endOfInclude = 0;
        std::string lineUpToEndOfInclude;
        std::string lineAfterInclude;
        std::string filename;
    };

    static bool isStdLibHeader(const std::string& filename) {
        static const StandardLibraryDetector detector;
        return detector.isStandardHeader(filename);
    }

    bool matchesAnyWildcard(const std::string& filename) const {
        std::string normalized = filename;
        std::replace(normalized.begin(), normalized.end(), '\\', '/');
        for (const auto& wc : wildcards_) {
            if (matchesWildcard(normalized, wc)) return true;
        }
        return false;
    }

    static bool canFileBeReincluded(const std::string& content) {
        if (content.empty()) return true;

        std::string stripped = content;
        for (;;) {
            stripped = trimmedStart(stripped);
            if (startsWith(stripped, "//")) {
                size_t nl = stripped.find('\n');
                stripped = (nl == std::string::npos) ? "" : stripped.substr(nl + 1);
            } else if (startsWith(stripped, "/*")) {
                size_t end = stripped.find("*/");
                stripped = (end == std::string::npos) ? "" : stripped.substr(end + 2);
            } else {
                break;
            }
        }

        auto lines = splitLines(stripped);
        for (auto& l : lines) l = trimmed(l);
        lines.erase(std::remove_if(lines.begin(), lines.end(),
                                   [](const std::string& s) { return s.empty(); }), lines.end());

        if (lines.size() < 2) return true;

        std::string l1 = removeCharacters(lines[0], " \t");
        std::string l2 = removeCharacters(lines[1], " \t");

        if (startsWith(l1, "#ifndef") && startsWith(l2, "#define")) {
            std::string guard = l1.substr(7);
            std::string def = l2.substr(7);
            if (trimmed(guard) == trimmed(def)) return false;
        }
        if (l1 == "#pragmaonce") return false;

        return true;
    }

    fs::path findInclude(const fs::path& siblingFile, const std::string& filename) const {
        fs::path sibling = siblingFile;
        if (sibling.has_filename()) sibling = sibling.parent_path();

        fs::path candidate = sibling / filename;
        if (fs::exists(candidate) && fs::is_regular_file(candidate))
            return fs::weakly_canonical(candidate);

        for (const auto& dir : directoriesToSearch_) {
            candidate = fs::path(dir) / filename;
            if (fs::exists(candidate) && fs::is_regular_file(candidate))
                return fs::weakly_canonical(candidate);
        }

        return {};
    }

    ParsedInclude parseInclude(const std::string& line, const std::string& trimmedLine) const {
        ParsedInclude parsed;

        if (!startsWith(trimmedLine, "#")) return parsed;

        std::string removed = removeCharacters(trimmedLine, " \t");

        if (startsWithIgnoreCase(removed, "#include\"")) {
            size_t first = line.find('"');
            size_t second = line.find('"', first + 1);
            if (first != std::string::npos && second != std::string::npos) {
                parsed.endOfInclude = second + 1;
                parsed.filename = line.substr(first + 1, second - first - 1);
                parsed.isIncludeLine = true;
                parsed.lineUpToEndOfInclude = line.substr(0, second + 1);
                parsed.lineAfterInclude = line.substr(second + 1);
                parsed.preventReinclude = preventReincludes_.count(parsed.filename) > 0;
                parsed.forceReinclude = forceReincludes_.count(parsed.filename) > 0;
            }
        }
        else if (startsWithIgnoreCase(removed, "#include<")) {
            size_t first = line.find('<');
            size_t second = line.find('>', first + 1);
            if (first != std::string::npos && second != std::string::npos) {
                parsed.endOfInclude = second + 1;
                parsed.filename = line.substr(first + 1, second - first - 1);
                parsed.isIncludeLine = true;
                parsed.isAngleBracket = true;
                parsed.lineUpToEndOfInclude = line.substr(0, second + 1);
                parsed.lineAfterInclude = line.substr(second + 1);
                parsed.preventReinclude = preventReincludes_.count(parsed.filename) > 0;
                parsed.forceReinclude = forceReincludes_.count(parsed.filename) > 0;
            }
        }
        else if (startsWithIgnoreCase(removed, "#include")) {
            std::string name;
            size_t incPos = toLower(line).find("#include");
            size_t afterInc = incPos + 8;

            if (line.find("/*") != std::string::npos) {
                name = trimmed(line.substr(afterInc, line.find("/*") - afterInc));
            } else {
                name = trimmed(line.substr(afterInc));
            }

            parsed.endOfInclude = line.find(name) + name.length();

            auto it = macrosDefined_.find(name);
            if (it != macrosDefined_.end()) {
                const std::string& value = it->second;
                if (startsWith(value, "\"") || startsWith(value, "\'")) {
                    size_t first = value.find('"');
                    size_t second = value.find('"', first + 1);
                    if (first != std::string::npos && second != std::string::npos) {
                        parsed.endOfInclude = line.length();
                        parsed.filename = value.substr(first + 1, second - first - 1);
                        parsed.isIncludeLine = true;
                    }
                } else if (startsWith(value, "<")) {
                    size_t first = value.find('<');
                    size_t second = value.find('>', first + 1);
                    if (first != std::string::npos && second != std::string::npos) {
                        parsed.endOfInclude = line.length();
                        parsed.filename = value.substr(first + 1, second - first - 1);
                        parsed.isIncludeLine = true;
                        parsed.isAngleBracket = true;
                    }
                }

                if (parsed.isIncludeLine) {
                    parsed.preventReinclude = preventReincludes_.count(name) > 0;
                    parsed.forceReinclude = forceReincludes_.count(name) > 0;
                    parsed.lineUpToEndOfInclude = line.substr(0, parsed.endOfInclude);
                    parsed.lineAfterInclude = line.substr(parsed.endOfInclude);
                }
            }
        }

        return parsed;
    }

    void findAllFilesIncludedIn(const fs::path& hppTemplate,
                                std::unordered_set<std::string>& alreadyIncludedFiles) const {
        std::string content = readFileToString(hppTemplate);
        auto lines = splitLines(content);

        for (const auto& line : lines) {
            ParsedInclude inc = parseInclude(line, trimmed(line));
            if (!inc.isIncludeLine || inc.isAngleBracket) continue;

            fs::path targetFile = findInclude(hppTemplate, inc.filename);
            if (!targetFile.empty()) {
                std::string canon = fs::weakly_canonical(targetFile).string();
                if (!alreadyIncludedFiles.count(canon)) {
                    alreadyIncludedFiles.insert(canon);
                    findAllFilesIncludedIn(targetFile, alreadyIncludedFiles);
                }
            }
        }
    }

    bool parseFile(const fs::path& newTargetFile,
                   std::ostream& dest,
                   const fs::path& file,
                   std::unordered_set<std::string>& alreadyIncludedFiles,
                   std::unordered_set<int64_t>& alreadyIncludedHashes,
                   const std::vector<std::string>& includesToIgnore,
                   int level,
                   bool stripCommentBlocks) {
        fs::path canonFile = fs::weakly_canonical(file);

        if (!fs::exists(canonFile) || !fs::is_regular_file(canonFile)) {
            std::cout << "  !! ERROR - file doesn't exist!";
            return false;
        }

        std::string rawContent = readFileToString(canonFile);
        if (rawContent.empty()) {
            std::cout << "  !! ERROR - input file was empty: " << canonFile.string();
            return false;
        }

        std::string fileContent = prepareFileForInlining(rawContent);

        if (verbose_) {
            if (level == 0)
                std::cout << "  Processing \"" << canonFile.filename().string() << "\"\n";
            else
                std::cout << "  Inlining " << repeatedString(" ", static_cast<size_t>(level) - 1)
                          << "\"" << canonFile.filename().string() << "\"\n";
        }

        auto lines = splitLines(fileContent);
        bool lastLineWasBlank = true;

        for (size_t i = 0; i < lines.size(); ++i) {
            std::string line = lines[i];
            std::string trimmedLine = trimmedStart(line);

            if (level != 0 && startsWith(trimmedLine, "//================================================================")) {
                line.clear();
            }

            if (remapTable.processLine(line)) {
                line.clear();
            } else {
                ParsedInclude parsedInclude = parseInclude(line, trimmedLine);

                if (parsedInclude.isIncludeLine) {
                    if (isStdLibHeader(parsedInclude.filename)) {
                        // Keep system headers as-is
                    } else if (parsedInclude.isAngleBracket && !checkSystemIncludes_) {
                        // Angle brackets only with -s
                    } else {
                        fs::path targetFile = findInclude(canonFile, parsedInclude.filename);

                        if (!targetFile.empty() && fs::exists(targetFile)) {
                            std::string targetCanon = fs::weakly_canonical(targetFile).string();

                            auto rewriteToRelative = [&](const fs::path& target) {
                                fs::path rel;
                                try { rel = fs::relative(target, newTargetFile.parent_path()); }
                                catch (...) { rel = target; }
                                std::string relStr = rel.string();
                                std::replace(relStr.begin(), relStr.end(), '\\', '/');
                                size_t quotePos = parsedInclude.lineUpToEndOfInclude.rfind('"');
                                if (quotePos == std::string::npos)
                                    quotePos = parsedInclude.lineUpToEndOfInclude.length();
                                return parsedInclude.lineUpToEndOfInclude.substr(0, quotePos + 1)
                                       + relStr + "\"" + parsedInclude.lineAfterInclude;
                            };

                            if (matchesAnyWildcard(parsedInclude.filename)) {
                                bool shouldIgnore = false;
                                std::string targetFilename = targetFile.filename().string();
                                for (const auto& ign : includesToIgnore) {
                                    if (targetFilename == ign) { shouldIgnore = true; break; }
                                }

                                if (!shouldIgnore) {
                                    std::string fileNamePart = fs::path(parsedInclude.filename).filename().string();
                                    auto remapTo = remapTable.find(fileNamePart);

                                    if (level != 0 && remapTo != nullptr) {
                                        line = std::string("#include \"") + *remapTo + "\"\n";
                                        findAllFilesIncludedIn(targetFile, alreadyIncludedFiles);
                                    } else if (containsIgnoreCase(line, "FORCE_AMALGAMATOR_INCLUDE")
                                               || parsedInclude.forceReinclude
                                               || (!alreadyIncludedFiles.count(targetCanon)
                                                   && !alreadyIncludedHashes.count(calculateFileHashCode(targetFile)))) {
                                        bool canRecurse = (level + 1 <= maxRecursionDepth);

                                        if (!canRecurse) {
                                            std::cout << "  !! WARNING - max recursion depth ("
                                                      << maxRecursionDepth << ") reached for "
                                                      << targetFile.filename().string()
                                                      << ", leaving #include as-is\n";
                                        } else {
                                            std::string targetRaw = readFileToString(targetFile);
                                            if (targetRaw.empty()) {
                                                std::cout << "  !! WARNING - empty file: "
                                                          << targetFile.string() << "\n";
                                                line.clear();
                                            } else {
                                                int64_t targetHash = calculateStringHashCode(targetRaw);
                                                bool hasGuard = !canFileBeReincluded(targetRaw);

                                                if (parsedInclude.preventReinclude) {
                                                    alreadyIncludedFiles.insert(targetCanon);
                                                    alreadyIncludedHashes.insert(targetHash);
                                                } else if (parsedInclude.forceReinclude) {
                                                    // Force: don't add to already-included
                                                } else if (hasGuard) {
                                                    alreadyIncludedFiles.insert(targetCanon);
                                                    alreadyIncludedHashes.insert(targetHash);
                                                }

                                                std::string indent(level * 2, ' ');
                                                dest << indent << "// === " << targetFile.filename().string() << " ===\n";

                                                if (!parseFile(newTargetFile, dest, targetFile,
                                                               alreadyIncludedFiles, alreadyIncludedHashes,
                                                               includesToIgnore, level + 1, stripCommentBlocks)) {
                                                    return false;
                                                }

                                                dest << indent << "// === end " << targetFile.filename().string() << " ===\n";
                                                line = parsedInclude.lineAfterInclude;
                                            }
                                        }
                                    } else {
                                        line.clear();
                                    }
                                } else {
                                    line = rewriteToRelative(targetFile);
                                }
                            } else {
                                line = rewriteToRelative(targetFile);
                            }
                        }
                        // else: file not found, leave #include as-is
                    }
                }
            }

            if ((stripCommentBlocks || i == 0) && startsWith(trimmedLine, "/*") && (i > 10 || level != 0)) {
                size_t originalI = i;
                std::string originalLine = line;

                for (;;) {
                    size_t endPos = line.find("*/");
                    if (endPos != std::string::npos) {
                        line = line.substr(endPos + 2);

                        if (i + 1 < lines.size() && containsIgnoreCase(lines[i + 1], "assert")) {
                            i = originalI;
                            line = originalLine;
                        } else if (i + 2 < lines.size() && containsIgnoreCase(lines[i + 2], "assert")) {
                            i = originalI;
                            line = originalLine;
                        }
                        break;
                    }

                    if (++i >= lines.size()) break;
                    line = lines[i];
                }

                line = trimmedEnd(line);
                if (line.empty()) continue;
            }

            line = trimmedEnd(line);

            if (tabs_ && !line.empty()) {
                size_t numInitialSpaces = 0;
                while (numInitialSpaces < line.size() && line[numInitialSpaces] == ' ')
                    ++numInitialSpaces;
                if (numInitialSpaces > 0) {
                    const int tabSize = 4;
                    size_t numTabs = numInitialSpaces / tabSize;
                    line = repeatedString("\t", numTabs) + line.substr(numTabs * tabSize);
                }
            }

            if (!line.empty() || !lastLineWasBlank)
                dest << line << "\n";

            lastLineWasBlank = line.empty();
        }

        return true;
    }

    std::string name_;
    bool verbose_ = false;
    bool checkSystemIncludes_ = false;
    bool tabs_ = false;
    std::unordered_map<std::string, std::string> macrosDefined_;
    std::vector<std::string> wildcards_;
    std::unordered_set<std::string> forceReincludes_;
    std::unordered_set<std::string> preventReincludes_;
    std::vector<std::string> directoriesToSearch_;
    fs::path templateFile_;
    fs::path targetFile_;
};

//==============================================================================
// Usage & Argument Parsing
//==============================================================================

static void print_usage(const char* argv0) {
    std::string name = fs::path(argv0).filename().string();

    std::cout << "\n"
              << "  NAME\n"
              << "  \n"
              << "   " << name << " - produce an amalgamation of C/C++ source files.\n"
              << "  \n"
              << "  SYNOPSIS\n"
              << "  \n"
              << "   " << name << " [-s] [-t]\n"
              << "     [-w {wildcards}]\n"
              << "     [-f {file|macro}]...\n"
              << "     [-p {file|macro}]...\n"
              << "     [-d {name}={file}]...\n"
              << "     [-i {dir}]...\n"
              << "     [-h]\n"
              << "     [-v]\n"
              << "     {inputFile} {outputFile}\n"
              << "  \n"
              << "  DESCRIPTION\n"
              << "  \n"
              << "   Produces an amalgamation of {inputFile} by replacing #include statements with\n"
              << "   the contents of the file they refer to. This replacement will only occur if\n"
              << "   the file was located in the same directory, or one of the additional include\n"
              << "   paths added with the -i option.\n"
              << "   \n"
              << "   Files included in angle brackets (system includes) are only inlined if the\n"
              << "   -s option is specified.\n"
              << "  \n"
              << "   If an #include line contains a macro instead of a string literal, the list\n"
              << "   of definitions provided through the -d option is consulted to convert the\n"
              << "   macro into a string.\n"
              << "  \n"
              << "   A file will only be inlined once, with subsequent #include lines for the same\n"
              << "   file silently ignored, unless the -f option is specified for the file.\n"
              << "  \n"
              << "  OPTIONS\n"
              << "  \n"
              << "    -s                Process #include lines containing angle brackets (i.e.\n"
              << "                      system includes). Normally these are not inlined.\n"
              << "  \n"
              << "    -t                Convert spaces into tabs.\n"
              << "  \n"
              << "    -w {wildcards}    Specify a comma separated list of file name patterns to\n"
              << "                      match when deciding to inline (assuming the file can be\n"
              << "                      located). The default setting is \"*.cpp;*.c;*.hpp;*.h\".\n"
              << "  \n"
              << "    -f {file|macro}   Force reinclusion of the specified file or macro on\n"
              << "                      all appearances in #include lines.\n"
              << "  \n"
              << "    -p {file|macro}   Prevent reinclusion of the specified file or macro on\n"
              << "                      subsequent appearances in #include lines.\n"
              << "  \n"
              << "    -d {name}={file}  Use {file} for macro {name} if it appears in an #include\n"
              << "                      line.\n"
              << "  \n"
              << "    -i {dir}          Additionally look in the specified directory for files when\n"
              << "                      processing #include lines.\n"
              << "  \n"
              << "    -h                Print help and exit\n"
              << "  \n"
              << "    -v                Verbose output mode\n"
              << "\n";
}

int main(int argc, char* argv[]) {
    bool error = false;
    bool usage = true;

    std::string name = fs::path(argv[0]).filename().string();

    Amalgamator amalgamator(name);
    bool gotCheckSystem = false;
    bool gotWildcards = false;
    bool gotTemplate = false;
    bool gotTarget = false;
    auto start = std::chrono::steady_clock::now();


    for (int i = 1; i < argc; ++i) {
        std::string option(argv[i]);

        if (option == "-i" || option == "-I") {
            if (++i < argc) {
                amalgamator.addDirectoryToSearch(argv[i]);
            } else {
                std::cout << name << ": Missing parameter for -i\n";
                error = true; break;
            }
        }
        else if (toLower(option) == "-f") {
            if (++i < argc) {
                std::string value = argv[i];
                if (value.size() >= 2 && ((value.front() == '\"' && value.back() == '\"') || (value.front() == '\'' && value.back() == '\'')))
                    value = value.substr(1, value.size() - 2);
                amalgamator.addForceReinclude(value);
            } else {
                std::cout << name << ": Missing parameter for -f\n";
                error = true; break;
            }
        }
        else if (toLower(option) == "-p") {
            if (++i < argc) {
                std::string value = argv[i];
                if (value.size() >= 2 && ((value.front() == '\"' && value.back() == '\"') || (value.front() == '\'' && value.back() == '\'')))
                    value = value.substr(1, value.size() - 2);
                amalgamator.addPreventReinclude(value);
            } else {
                std::cout << name << ": Missing parameter for -p\n";
                error = true; break;
            }
        }
        else if (toLower(option) == "-d") {
            if (++i < argc) {
                std::string value = argv[i];
                size_t eq = value.find('=');
                if (eq != std::string::npos) {
                    std::string dname = value.substr(0, eq);
                    std::string dval = value.substr(eq + 1);
                    amalgamator.addDefine(dname, dval);
                } else {
                    std::cout << name << ": Incorrect syntax for -d\n";
                }
            } else {
                std::cout << name << ": Missing parameter for -d\n";
                error = true; break;
            }
        }
        else if (toLower(option) == "-w") {
            if (++i < argc) {
                if (!gotWildcards) {
                    amalgamator.setWildcards(argv[i]);
                    gotWildcards = true;
                } else {
                    std::cout << name << ": Duplicate option -w\n";
                    error = true; break;
                }
            } else {
                std::cout << name << ": Missing parameter for -w\n";
                error = true; break;
            }
        }
        else if (toLower(option) == "-s") {
            if (!gotCheckSystem) {
                amalgamator.setCheckSystemIncludes(true);
                gotCheckSystem = true;
            } else {
                std::cout << name << ": Duplicate option -s\n";
                error = true; break;
            }
        }
        else if (toLower(option) == "-t") {
            amalgamator.setConvertSpacesToTabs();
        }
        else if (toLower(option) == "-v") {
            amalgamator.setVerbose();
        }
        else if (toLower(option) == "-h" || toLower(option) == "--help") {
            print_usage(argv[0]);
            return 0;
        }
        else if (startsWith(option, "-")) {
            std::cout << name << ": Unknown option \"" << option << "\"\n";
            error = true; break;
        }
        else {
            if (!gotTemplate) {
                amalgamator.setTemplate(option);
                gotTemplate = true;
            } else if (!gotTarget) {
                amalgamator.setTarget(option);
                gotTarget = true;
            } else {
                std::cout << name << ": Too many arguments\n";
                error = true; break;
            }
        }
    }

    if (gotTemplate && gotTarget) {
        usage = false;
        error = amalgamator.process();
    } else {
        if (argc > 1)
            std::cout << name << " Too few arguments\n";
        error = true;
    }

    if (error && usage)
        print_usage(argv[0]);

    auto end = std::chrono::steady_clock::now();
    auto diff = end - start;
    auto diff_ms = std::chrono::duration_cast<std::chrono::milliseconds>(diff);


    std::cout << "Build finished in " << (diff_ms.count()) << "ms.\n";


    return error ? 1 : 0;
}