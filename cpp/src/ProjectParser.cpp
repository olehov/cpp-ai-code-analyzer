#include "ProjectParser.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <regex>
#include <stdexcept>
#include <string_view>

namespace {
enum class ParserState {
    code,
    singleLineComment,
    multiLineComment,
    stringLiteral,
    charLiteral,
    rawStringLiteral,
};

bool startsWith(std::string_view value, std::string_view prefix) {
    return value.rfind(prefix, 0) == 0;
}

std::string_view trimView(std::string_view value) {
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string_view::npos) {
        return {};
    }

    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

std::string_view accessSpecifierKeyword(std::string_view value) {
    value = trimView(value);
    for (const std::string_view keyword : {"public", "private", "protected"}) {
        if (startsWith(value, keyword)) {
            const std::string_view suffix = trimView(value.substr(keyword.size()));
            if (suffix == ":") {
                return keyword;
            }
        }
    }
    return {};
}

bool isAccessSpecifier(std::string_view value) {
    return !accessSpecifierKeyword(value).empty();
}

bool isFilteredStatement(std::string_view statement) {
    return startsWith(statement, "using ") || startsWith(statement, "typedef ") ||
           startsWith(statement, "friend ") || startsWith(statement, "enum ") ||
           startsWith(statement, "class ") || startsWith(statement, "struct ") ||
           startsWith(statement, "static_assert") || startsWith(statement, "if ") ||
           startsWith(statement, "if(") || startsWith(statement, "for ") ||
           startsWith(statement, "for(") || startsWith(statement, "while ") ||
           startsWith(statement, "while(") || startsWith(statement, "switch ") ||
           startsWith(statement, "switch(") || startsWith(statement, "return ") ||
           startsWith(statement, "catch ") || startsWith(statement, "else") ||
           startsWith(statement, "#");
}

bool looksLikeMethodDeclaration(std::string_view statement) {
    const auto openParen = statement.find('(');
    const auto closeParen = statement.rfind(')');
    if (openParen == std::string_view::npos || closeParen == std::string_view::npos ||
        closeParen < openParen) {
        return false;
    }

    if (statement.find("(*") != std::string_view::npos ||
        statement.find("(&") != std::string_view::npos) {
        return false;
    }

    std::string_view suffix = trimView(statement.substr(closeParen + 1));
    if (!suffix.empty() && suffix != ";" && suffix != "{" && suffix != "= default;" &&
        suffix != "= delete;" && suffix != "= 0;" && !startsWith(suffix, "const") &&
        !startsWith(suffix, "noexcept") && !startsWith(suffix, "override") &&
        !startsWith(suffix, "final") && !startsWith(suffix, "requires") &&
        !startsWith(suffix, "->") && !startsWith(suffix, "&") && !startsWith(suffix, "&&")) {
        return false;
    }

    const std::string_view prefix = trimView(statement.substr(0, openParen));
    if (prefix.empty()) {
        return false;
    }

    const std::size_t separator = prefix.find_last_of(" \t*&:");
    const std::string_view name =
        separator == std::string_view::npos ? prefix : prefix.substr(separator + 1);
    if (name.empty()) {
        return false;
    }

    constexpr std::array<std::string_view, 10> kControlKeywords = {
        "if",
        "for",
        "while",
        "switch",
        "catch",
        "return",
        "sizeof",
        "alignof",
        "decltype",
        "requires",
    };
    if (std::find(kControlKeywords.begin(), kControlKeywords.end(), name) !=
        kControlKeywords.end()) {
        return false;
    }

    const unsigned char firstCharacter = static_cast<unsigned char>(name.front());
    return std::isalpha(firstCharacter) != 0 || name.front() == '_' || name.front() == '~';
}

bool isEnumClassMatch(std::string_view content, std::size_t matchStart) {
    const std::string_view prefix = trimView(content.substr(0, matchStart));
    return prefix.size() >= 4 && prefix.substr(prefix.size() - 4) == "enum";
}

bool tryParseRawStringStart(
    std::string_view content,
    std::size_t position,
    std::string& terminator
) {
    if (position + 2 >= content.size() || content[position] != 'R' || content[position + 1] != '"') {
        return false;
    }

    const std::size_t delimiterStart = position + 2;
    const std::size_t openParen = content.find('(', delimiterStart);
    if (openParen == std::string_view::npos) {
        return false;
    }

    const std::string_view delimiter = content.substr(delimiterStart, openParen - delimiterStart);
    if (delimiter.size() > 16 ||
        delimiter.find_first_of(" \t\r\n()\\") != std::string_view::npos) {
        return false;
    }

    terminator.clear();
    terminator.reserve(delimiter.size() + 2);
    terminator.push_back(')');
    terminator.append(delimiter);
    terminator.push_back('"');
    return true;
}
}

namespace {
std::size_t findMatchingBrace(std::string_view content, std::size_t openBracePos) noexcept;
std::vector<std::string> parsePublicMethods(std::string_view classBody, bool publicByDefault);

std::string readFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error("Unable to open file: " + path.string());
    }

    std::error_code errorCode;
    const auto fileSize = std::filesystem::file_size(path, errorCode);
    if (!errorCode) {
        std::string content(static_cast<std::size_t>(fileSize), '\0');
        if (!content.empty()) {
            const auto bytesToRead = static_cast<std::streamsize>(content.size());
            file.read(content.data(), bytesToRead);
            if (file.gcount() != bytesToRead) {
                throw std::runtime_error("Unable to read file contents: " + path.string());
            }
        }
        if (!file && !file.eof()) {
            throw std::runtime_error("Unable to read file contents: " + path.string());
        }
        return content;
    }

    std::string content{
        std::istreambuf_iterator<char>(file),
        std::istreambuf_iterator<char>()
    };
    if (file.bad()) {
        throw std::runtime_error("Unable to read file contents: " + path.string());
    }

    return content;
}

std::vector<std::string> parseHeaders(std::string_view content) {
    std::vector<std::string> headers;
    std::size_t lineStart = 0;

    while (lineStart <= content.size()) {
        const std::size_t lineEnd = content.find('\n', lineStart);
        const std::size_t lineLength =
            (lineEnd == std::string_view::npos ? content.size() : lineEnd) - lineStart;
        std::string_view line = trimView(content.substr(lineStart, lineLength));

        if (startsWith(line, "#")) {
            line.remove_prefix(1);
            line = trimView(line);
            if (startsWith(line, "include")) {
                line.remove_prefix(std::string_view("include").size());
                line = trimView(line);

                if (line.size() >= 2) {
                    const char opening = line.front();
                    const char closing = opening == '<' ? '>' : (opening == '"' ? '"' : '\0');
                    if (closing != '\0') {
                        const std::size_t closePosition = line.find(closing, 1);
                        if (closePosition != std::string_view::npos) {
                            std::string header(line.substr(1, closePosition - 1));
                            if (std::find(headers.begin(), headers.end(), header) == headers.end()) {
                                headers.push_back(std::move(header));
                            }
                        }
                    }
                }
            }
        }

        if (lineEnd == std::string_view::npos) {
            break;
        }
        lineStart = lineEnd + 1;
    }

    return headers;
}

std::vector<analysis::ClassInfo> parseClasses(std::string_view content) {
    std::vector<analysis::ClassInfo> classes;
    const std::string source(content);
    const std::regex classRegex(
        R"((?:template\s*<[^;{]*>\s*)*(?:\[\[[^\]]+\]\]\s*)*\b(class|struct)\s+([A-Za-z_]\w*)[^;{]*\{)"
    );
    auto searchBegin = source.cbegin();
    std::match_results<std::string::const_iterator> match;

    while (std::regex_search(searchBegin, source.cend(), match, classRegex)) {
        const std::size_t matchStart =
            static_cast<std::size_t>(std::distance(source.cbegin(), match[0].first));
        if (isEnumClassMatch(content, matchStart)) {
            searchBegin = match[0].second;
            continue;
        }

        const std::string keyword = match[1].str();
        const std::string className = match[2].str();
        const std::size_t openBracePos =
            static_cast<std::size_t>(
                matchStart + match.length(0) - 1
            );
        const std::size_t closeBracePos = findMatchingBrace(content, openBracePos);

        if (closeBracePos == std::string::npos || closeBracePos <= openBracePos) {
            searchBegin = match[0].second;
            continue;
        }

        const std::string_view body = content.substr(openBracePos + 1, closeBracePos - openBracePos - 1);

        analysis::ClassInfo classInfo;
        classInfo.name = className;
        classInfo.publicMethods = parsePublicMethods(body, keyword == "struct");
        classes.push_back(std::move(classInfo));

        searchBegin = match[0].second;
    }

    return classes;
}

std::string stripComments(const std::string& content) {
    std::string result;
    result.reserve(content.size());

    ParserState state = ParserState::code;
    bool escaped = false;
    std::string rawStringTerminator;
    std::size_t rawStringMatchIndex = 0;

    for (std::size_t i = 0; i < content.size(); ++i) {
        const char current = content[i];
        const char next = i + 1 < content.size() ? content[i + 1] : '\0';

        switch (state) {
            case ParserState::code:
                if (current == '/' && next == '/') {
                    state = ParserState::singleLineComment;
                    ++i;
                } else if (current == '/' && next == '*') {
                    state = ParserState::multiLineComment;
                    ++i;
                } else if (tryParseRawStringStart(content, i, rawStringTerminator)) {
                    state = ParserState::rawStringLiteral;
                    rawStringMatchIndex = 0;
                    result.push_back(current);
                } else {
                    result.push_back(current);
                    if (current == '"') {
                        state = ParserState::stringLiteral;
                    } else if (current == '\'') {
                        state = ParserState::charLiteral;
                    }
                }
                break;

            case ParserState::singleLineComment:
                if (current == '\n') {
                    state = ParserState::code;
                    result.push_back(current);
                }
                break;

            case ParserState::multiLineComment:
                if (current == '*' && next == '/') {
                    state = ParserState::code;
                    ++i;
                } else if (current == '\n') {
                    result.push_back('\n');
                }
                break;

            case ParserState::stringLiteral:
                result.push_back(current);
                if (escaped) {
                    escaped = false;
                } else if (current == '\\') {
                    escaped = true;
                } else if (current == '"') {
                    state = ParserState::code;
                }
                break;

            case ParserState::charLiteral:
                result.push_back(current);
                if (escaped) {
                    escaped = false;
                } else if (current == '\\') {
                    escaped = true;
                } else if (current == '\'') {
                    state = ParserState::code;
                }
                break;

            case ParserState::rawStringLiteral:
                result.push_back(current);
                if (current == rawStringTerminator[rawStringMatchIndex]) {
                    ++rawStringMatchIndex;
                    if (rawStringMatchIndex == rawStringTerminator.size()) {
                        rawStringMatchIndex = 0;
                        rawStringTerminator.clear();
                        state = ParserState::code;
                    }
                } else if (current == rawStringTerminator[0]) {
                    rawStringMatchIndex = 1;
                } else {
                    rawStringMatchIndex = 0;
                }
                break;
        }

        if (state == ParserState::code && current == '\\' && next == '\n') {
            result.push_back(next);
            ++i;
        }
    }

    return result;
}

std::size_t findMatchingBrace(
    std::string_view content,
    std::size_t openBracePos
) noexcept {
    int depth = 0;
    bool inString = false;
    bool inChar = false;
    bool escaped = false;
    bool inRawString = false;
    std::string rawStringTerminator;
    std::size_t rawStringMatchIndex = 0;

    for (std::size_t i = openBracePos; i < content.size(); ++i) {
        const char current = content[i];

        if (!inString && !inChar && !inRawString && tryParseRawStringStart(content, i, rawStringTerminator)) {
            inRawString = true;
            rawStringMatchIndex = 0;
        }

        if (inRawString) {
            if (current == rawStringTerminator[rawStringMatchIndex]) {
                ++rawStringMatchIndex;
                if (rawStringMatchIndex == rawStringTerminator.size()) {
                    inRawString = false;
                    rawStringMatchIndex = 0;
                    rawStringTerminator.clear();
                }
            } else if (current == rawStringTerminator[0]) {
                rawStringMatchIndex = 1;
            } else {
                rawStringMatchIndex = 0;
            }
            continue;
        }

        if (escaped) {
            escaped = false;
            continue;
        }

        if ((inString || inChar) && current == '\\') {
            escaped = true;
            continue;
        }

        if (!inChar && current == '"') {
            inString = !inString;
            continue;
        }

        if (!inString && current == '\'') {
            inChar = !inChar;
            continue;
        }

        if (inString || inChar) {
            continue;
        }

        if (current == '{') {
            ++depth;
        } else if (current == '}') {
            --depth;
            if (depth == 0) {
                return i;
            }
        }
    }

    return std::string::npos;
}

std::vector<std::string> parsePublicMethods(
    std::string_view classBody,
    bool publicByDefault
) {
    std::vector<std::string> methods;
    std::string currentStatement;
    std::string accessLevel = publicByDefault ? "public" : "private";
    int nestedBraceDepth = 0;
    int nestedParenDepth = 0;

    auto flushStatement = [&]() {
        const std::string_view statement = trimView(currentStatement);
        currentStatement.clear();

        if (accessLevel != "public" || statement.empty() || isAccessSpecifier(statement) ||
            isFilteredStatement(statement) || !looksLikeMethodDeclaration(statement)) {
            return;
        }

        methods.emplace_back(statement);
    };

    for (char character : classBody) {
        currentStatement.push_back(character);

        if (character == '(') {
            ++nestedParenDepth;
        } else if (character == ')') {
            nestedParenDepth = std::max(0, nestedParenDepth - 1);
        } else if (character == '{') {
            ++nestedBraceDepth;
            if (nestedBraceDepth == 1 && nestedParenDepth == 0) {
                flushStatement();
            }
        } else if (character == '}') {
            nestedBraceDepth = std::max(0, nestedBraceDepth - 1);
            if (nestedBraceDepth == 0) {
                currentStatement.clear();
            }
        } else if (character == ';' && nestedBraceDepth == 0) {
            flushStatement();
        }

        if (nestedBraceDepth == 0) {
            const std::string_view trimmed = trimView(currentStatement);
            if (const std::string_view keyword = accessSpecifierKeyword(trimmed);
                !keyword.empty()) {
                accessLevel = std::string(keyword);
                currentStatement.clear();
            }
        }
    }

    return methods;
}
}

namespace analysis::project_parser {
FileAnalysis analyzeFile(const std::filesystem::path& path) {
    const std::string content = readFile(path);
    const std::string sanitized = stripComments(content);

    FileAnalysis result;
    result.filePath = path;
    result.headers = parseHeaders(sanitized);
    result.classes = parseClasses(sanitized);
    return result;
}
}
