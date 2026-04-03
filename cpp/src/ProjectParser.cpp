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

bool isAccessSpecifier(std::string_view value) {
    return value == "public:" || value == "private:" || value == "protected:";
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
           statement.find('#') != std::string_view::npos;
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
}

analysis::FileAnalysis ProjectParser::analyzeFile(const std::filesystem::path& path) {
    const std::string content = readFile(path);
    const std::string sanitized = stripComments(content);

    analysis::FileAnalysis result;
    result.filePath = path;
    result.headers = parseHeaders(sanitized);
    result.classes = parseClasses(sanitized);
    return result;
}

std::string ProjectParser::readFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error("Unable to open file: " + path.string());
    }

    file.seekg(0, std::ios::end);
    const auto endPosition = file.tellg();
    if (endPosition < 0) {
        throw std::runtime_error("Unable to determine file size: " + path.string());
    }

    std::string content(static_cast<std::size_t>(endPosition), '\0');
    file.seekg(0, std::ios::beg);

    if (!content.empty()) {
        file.read(content.data(), static_cast<std::streamsize>(content.size()));
    }

    return content;
}

std::vector<std::string> ProjectParser::parseHeaders(const std::string& content) {
    std::vector<std::string> headers;
    const std::regex includeRegex(
        R"(^\s*#\s*include\s*[<"]([^>"]+)[>"])",
        std::regex::ECMAScript | std::regex::multiline
    );

    auto begin = std::sregex_iterator(content.begin(), content.end(), includeRegex);
    auto end = std::sregex_iterator();

    for (auto it = begin; it != end; ++it) {
        headers.push_back((*it)[1].str());
    }

    return headers;
}

std::vector<analysis::ClassInfo> ProjectParser::parseClasses(const std::string& content) {
    std::vector<analysis::ClassInfo> classes;
    const std::regex classRegex(
        R"((?:\[\[[^\]]+\]\]\s*)*\b(class|struct)\s+([A-Za-z_]\w*)[^;{]*\{)"
    );
    auto searchBegin = content.cbegin();
    std::smatch match;

    while (std::regex_search(searchBegin, content.cend(), match, classRegex)) {
        const std::string keyword = match[1].str();
        const std::string className = match[2].str();
        const std::size_t openBracePos =
            static_cast<std::size_t>(
                std::distance(content.cbegin(), match[0].first) + match.length(0) - 1
            );
        const std::size_t closeBracePos = findMatchingBrace(content, openBracePos);

        if (closeBracePos == std::string::npos || closeBracePos <= openBracePos) {
            searchBegin = match[0].second;
            continue;
        }

        const std::string_view contentView(content);
        const std::string_view body =
            contentView.substr(openBracePos + 1, closeBracePos - openBracePos - 1);

        analysis::ClassInfo classInfo;
        classInfo.name = className;
        classInfo.publicMethods = parsePublicMethods(body, keyword == "struct");
        classes.push_back(std::move(classInfo));

        searchBegin = content.cbegin() + static_cast<std::ptrdiff_t>(closeBracePos + 1);
    }

    return classes;
}

std::string ProjectParser::stripComments(const std::string& content) {
    std::string result;
    result.reserve(content.size());

    ParserState state = ParserState::code;
    bool escaped = false;

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
        }
    }

    return result;
}

std::size_t ProjectParser::findMatchingBrace(
    const std::string& content,
    std::size_t openBracePos
) {
    int depth = 0;
    bool inString = false;
    bool inChar = false;
    bool escaped = false;

    for (std::size_t i = openBracePos; i < content.size(); ++i) {
        const char current = content[i];

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

std::vector<std::string> ProjectParser::parsePublicMethods(
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
            if (isAccessSpecifier(trimmed)) {
                accessLevel = std::string(trimmed.substr(0, trimmed.find(':')));
                currentStatement.clear();
            }
        }
    }

    return methods;
}

std::string_view ProjectParser::trim(std::string_view value) {
    return trimView(value);
}
