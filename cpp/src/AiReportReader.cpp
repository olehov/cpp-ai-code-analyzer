#include "AiReportReader.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>

#ifdef __unix__
#include <unistd.h>
#elif defined(_WIN32)
#include <io.h>
#endif

namespace {
constexpr const char* kRed = "\033[31m";
constexpr const char* kYellow = "\033[33m";
constexpr const char* kGreen = "\033[32m";
constexpr const char* kCyan = "\033[36m";
constexpr const char* kReset = "\033[0m";
constexpr std::string_view kErrorPrefix = "[ERROR]";
constexpr std::string_view kSkippedPrefix = "[SKIPPED]";
constexpr std::string_view kSummaryMarker = "[SUMMARY]";
constexpr std::string_view kIssuesMarker = "[ISSUES]";
constexpr std::string_view kSuggestionsMarker = "[SUGGESTIONS]";
constexpr std::array<std::string_view, 6> kNoIssueMarkers = {
    "none",
    "no issues",
    "no major issues",
    "no critical issues",
    "no obvious issues",
    "no significant issues",
};

std::string_view trim(std::string_view value) {
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string_view::npos) {
        return {};
    }

    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

char toLowerAscii(char value) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
}

bool equalsIgnoreAsciiCase(std::string_view left, std::string_view right) {
    return left.size() == right.size() &&
           std::equal(left.begin(), left.end(), right.begin(), [](char lhs, char rhs) {
               return toLowerAscii(lhs) == toLowerAscii(rhs);
           });
}

std::size_t findLineEnd(std::string_view value, std::size_t lineStart) {
    const std::size_t lineEnd = value.find('\n', lineStart);
    return lineEnd == std::string_view::npos ? value.size() : lineEnd;
}

std::string_view readLine(std::string_view value, std::size_t lineStart) {
    const std::size_t lineEnd = findLineEnd(value, lineStart);
    std::string_view line = value.substr(lineStart, lineEnd - lineStart);
    if (!line.empty() && line.back() == '\r') {
        line.remove_suffix(1);
    }
    return line;
}

std::size_t advanceLine(std::string_view value, std::size_t lineStart) {
    const std::size_t lineEnd = findLineEnd(value, lineStart);
    return lineEnd == value.size() ? value.size() + 1 : lineEnd + 1;
}

std::string_view stripMarkdownPrefix(std::string_view line) {
    line = trim(line);
    while (!line.empty() &&
           (line.front() == '#' || line.front() == '*' || line.front() == '_' ||
            line.front() == '-' || line.front() == '>')) {
        line.remove_prefix(1);
        line = trim(line);
    }
    return line;
}

bool lineMatchesMarker(std::string_view line, std::string_view marker) {
    line = stripMarkdownPrefix(line);
    if (!line.starts_with(marker)) {
        return false;
    }

    std::string_view suffix = trim(line.substr(marker.size()));
    while (!suffix.empty() && (suffix.front() == '*' || suffix.front() == '_')) {
        suffix.remove_prefix(1);
        suffix = trim(suffix);
    }

    if (!suffix.empty() && suffix.front() == ':') {
        suffix.remove_prefix(1);
        suffix = trim(suffix);
    }

    while (!suffix.empty() && (suffix.back() == '*' || suffix.back() == '_')) {
        suffix.remove_suffix(1);
        suffix = trim(suffix);
    }

    return suffix.empty();
}

class JsonParseError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class MissingResultsSectionError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

void appendUtf8(std::string& output, std::uint32_t codePoint) {
    if (codePoint <= 0x7f) {
        output.push_back(static_cast<char>(codePoint));
        return;
    }

    if (codePoint <= 0x7ff) {
        output.push_back(static_cast<char>(0xc0 | (codePoint >> 6)));
        output.push_back(static_cast<char>(0x80 | (codePoint & 0x3f)));
        return;
    }

    if (codePoint <= 0xffff) {
        output.push_back(static_cast<char>(0xe0 | (codePoint >> 12)));
        output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | (codePoint & 0x3f)));
        return;
    }

    if (codePoint <= 0x10ffff) {
        output.push_back(static_cast<char>(0xf0 | (codePoint >> 18)));
        output.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | (codePoint & 0x3f)));
        return;
    }

    throw JsonParseError("Encountered invalid Unicode code point.");
}

class JsonParser {
public:
    explicit JsonParser(std::string_view input)
        : input_(input) {}

    [[nodiscard]] analysis::ai_report::Report parseReport() {
        skipWhitespace();
        expect('{', "Expected a JSON object at the top level.");

        analysis::ai_report::Report report;
        report.model = "unknown";
        bool hasResults = false;

        skipWhitespace();
        if (consume('}')) {
            throw MissingResultsSectionError("AI report is missing the 'results' section.");
        }

        while (true) {
            const std::string key = parseString();
            skipWhitespace();
            expect(':', "Expected ':' after an object key.");
            skipWhitespace();

            if (key == "model") {
                if (consumeLiteral("null")) {
                    report.model = "unknown";
                } else {
                    report.model = parseString();
                }
            } else if (key == "results") {
                report.results = parseResultsArray();
                hasResults = true;
            } else {
                skipValue();
            }

            skipWhitespace();
            if (consume('}')) {
                break;
            }

            expect(',', "Expected ',' between object members.");
            skipWhitespace();
        }

        skipWhitespace();
        if (!isAtEnd()) {
            throw error("Unexpected trailing content after the JSON object.");
        }

        if (!hasResults) {
            throw MissingResultsSectionError("AI report is missing the 'results' section.");
        }

        return report;
    }

private:
    [[nodiscard]] bool isAtEnd() const {
        return position_ >= input_.size();
    }

    [[nodiscard]] JsonParseError error(std::string_view message) const {
        return JsonParseError(std::string(message) + " At byte " +
                              std::to_string(position_) + ".");
    }

    void skipWhitespace() {
        while (!isAtEnd() &&
               std::isspace(static_cast<unsigned char>(input_[position_])) != 0) {
            ++position_;
        }
    }

    [[nodiscard]] bool consume(char expected) {
        if (!isAtEnd() && input_[position_] == expected) {
            ++position_;
            return true;
        }
        return false;
    }

    void expect(char expected, std::string_view message) {
        if (!consume(expected)) {
            throw error(message);
        }
    }

    [[nodiscard]] bool consumeLiteral(std::string_view literal) {
        if (input_.substr(position_, literal.size()) == literal) {
            position_ += literal.size();
            return true;
        }
        return false;
    }

    void expectLiteral(std::string_view literal, std::string_view message) {
        if (!consumeLiteral(literal)) {
            throw error(message);
        }
    }

    [[nodiscard]] std::uint32_t parseHexDigit(char value) const {
        if (value >= '0' && value <= '9') {
            return static_cast<std::uint32_t>(value - '0');
        }
        if (value >= 'a' && value <= 'f') {
            return static_cast<std::uint32_t>(10 + (value - 'a'));
        }
        if (value >= 'A' && value <= 'F') {
            return static_cast<std::uint32_t>(10 + (value - 'A'));
        }
        throw error("Expected a hexadecimal digit in a Unicode escape.");
    }

    [[nodiscard]] std::uint32_t parseCodeUnit() {
        if (position_ + 4 > input_.size()) {
            throw error("Incomplete Unicode escape sequence.");
        }

        std::uint32_t codeUnit = 0;
        for (int index = 0; index < 4; ++index) {
            codeUnit = (codeUnit << 4) | parseHexDigit(input_[position_]);
            ++position_;
        }
        return codeUnit;
    }

    [[nodiscard]] std::string parseString() {
        expect('"', "Expected a JSON string.");

        std::string result;
        while (true) {
            if (isAtEnd()) {
                throw error("Unterminated JSON string.");
            }

            const unsigned char current = static_cast<unsigned char>(input_[position_++]);
            if (current == '"') {
                return result;
            }

            if (current == '\\') {
                if (isAtEnd()) {
                    throw error("Incomplete escape sequence in JSON string.");
                }

                const char escaped = input_[position_++];
                switch (escaped) {
                    case '"':
                    case '\\':
                    case '/':
                        result.push_back(escaped);
                        break;
                    case 'b':
                        result.push_back('\b');
                        break;
                    case 'f':
                        result.push_back('\f');
                        break;
                    case 'n':
                        result.push_back('\n');
                        break;
                    case 'r':
                        result.push_back('\r');
                        break;
                    case 't':
                        result.push_back('\t');
                        break;
                    case 'u': {
                        std::uint32_t codePoint = parseCodeUnit();
                        if (codePoint >= 0xd800 && codePoint <= 0xdbff) {
                            expect('\\', "Expected a low surrogate after a high surrogate.");
                            expect('u', "Expected a Unicode escape after a high surrogate.");
                            const std::uint32_t lowSurrogate = parseCodeUnit();
                            if (lowSurrogate < 0xdc00 || lowSurrogate > 0xdfff) {
                                throw error("Invalid low surrogate in Unicode escape.");
                            }
                            codePoint =
                                0x10000 + (((codePoint - 0xd800) << 10) |
                                           (lowSurrogate - 0xdc00));
                        } else if (codePoint >= 0xdc00 && codePoint <= 0xdfff) {
                            throw error("Unexpected low surrogate in Unicode escape.");
                        }
                        appendUtf8(result, codePoint);
                        break;
                    }
                    default:
                        throw error("Unsupported escape sequence in JSON string.");
                }

                continue;
            }

            if (current < 0x20) {
                throw error("Control characters must be escaped in JSON strings.");
            }

            result.push_back(static_cast<char>(current));
        }
    }

    void skipNumber() {
        if (consume('-')) {
        }

        if (consume('0')) {
        } else {
            if (isAtEnd() ||
                std::isdigit(static_cast<unsigned char>(input_[position_])) == 0) {
                throw error("Invalid JSON number.");
            }

            while (!isAtEnd() &&
                   std::isdigit(static_cast<unsigned char>(input_[position_])) != 0) {
                ++position_;
            }
        }

        if (consume('.')) {
            if (isAtEnd() ||
                std::isdigit(static_cast<unsigned char>(input_[position_])) == 0) {
                throw error("Invalid JSON number fraction.");
            }

            while (!isAtEnd() &&
                   std::isdigit(static_cast<unsigned char>(input_[position_])) != 0) {
                ++position_;
            }
        }

        if (!isAtEnd() && (input_[position_] == 'e' || input_[position_] == 'E')) {
            ++position_;
            if (!isAtEnd() && (input_[position_] == '+' || input_[position_] == '-')) {
                ++position_;
            }

            if (isAtEnd() ||
                std::isdigit(static_cast<unsigned char>(input_[position_])) == 0) {
                throw error("Invalid JSON number exponent.");
            }

            while (!isAtEnd() &&
                   std::isdigit(static_cast<unsigned char>(input_[position_])) != 0) {
                ++position_;
            }
        }
    }

    void skipArray() {
        expect('[', "Expected a JSON array.");
        skipWhitespace();
        if (consume(']')) {
            return;
        }

        while (true) {
            skipWhitespace();
            skipValue();
            skipWhitespace();
            if (consume(']')) {
                return;
            }
            expect(',', "Expected ',' between array elements.");
            skipWhitespace();
        }
    }

    void skipObject() {
        expect('{', "Expected a JSON object.");
        skipWhitespace();
        if (consume('}')) {
            return;
        }

        while (true) {
            skipWhitespace();
            (void)parseString();
            skipWhitespace();
            expect(':', "Expected ':' after an object key.");
            skipWhitespace();
            skipValue();
            skipWhitespace();
            if (consume('}')) {
                return;
            }
            expect(',', "Expected ',' between object members.");
            skipWhitespace();
        }
    }

    void skipValue() {
        skipWhitespace();
        if (isAtEnd()) {
            throw error("Unexpected end of JSON input.");
        }

        const char current = input_[position_];
        if (current == '"') {
            (void)parseString();
            return;
        }
        if (current == '{') {
            skipObject();
            return;
        }
        if (current == '[') {
            skipArray();
            return;
        }
        if (current == 't') {
            expectLiteral("true", "Expected the literal 'true'.");
            return;
        }
        if (current == 'f') {
            expectLiteral("false", "Expected the literal 'false'.");
            return;
        }
        if (current == 'n') {
            expectLiteral("null", "Expected the literal 'null'.");
            return;
        }
        if (current == '-' ||
            std::isdigit(static_cast<unsigned char>(current)) != 0) {
            skipNumber();
            return;
        }

        throw error("Encountered an unsupported JSON value.");
    }

    [[nodiscard]] std::vector<std::string> parseStringArray() {
        expect('[', "Expected an array of strings.");

        std::vector<std::string> values;
        skipWhitespace();
        if (consume(']')) {
            return values;
        }

        while (true) {
            values.push_back(parseString());
            skipWhitespace();
            if (consume(']')) {
                return values;
            }
            expect(',', "Expected ',' between string array elements.");
            skipWhitespace();
        }
    }

    [[nodiscard]] analysis::ai_report::FileReport parseFileReport() {
        expect('{', "Expected each result entry to be a JSON object.");

        analysis::ai_report::FileReport fileReport;
        fileReport.path = "<unknown>";
        fileReport.aiAnalysis.clear();
        fileReport.aiStatus.clear();

        bool hasAiStatus = false;

        skipWhitespace();
        if (consume('}')) {
            fileReport.aiStatus =
                fileReport.aiAnalysis.starts_with(kErrorPrefix) ? "error" : "generated";
            return fileReport;
        }

        while (true) {
            const std::string key = parseString();
            skipWhitespace();
            expect(':', "Expected ':' after an object key.");
            skipWhitespace();

            if (key == "path") {
                if (consumeLiteral("null")) {
                    fileReport.path = "<unknown>";
                } else {
                    fileReport.path = parseString();
                }
            } else if (key == "ai_analysis") {
                if (consumeLiteral("null")) {
                    fileReport.aiAnalysis.clear();
                } else {
                    fileReport.aiAnalysis = parseString();
                }
            } else if (key == "ai_status") {
                if (consumeLiteral("null")) {
                    fileReport.aiStatus.clear();
                    hasAiStatus = false;
                } else {
                    fileReport.aiStatus = parseString();
                    hasAiStatus = true;
                }
            } else if (key == "rule_based_issues") {
                if (consumeLiteral("null")) {
                    fileReport.ruleBasedIssues.clear();
                } else {
                    fileReport.ruleBasedIssues = parseStringArray();
                }
            } else {
                skipValue();
            }

            skipWhitespace();
            if (consume('}')) {
                break;
            }

            expect(',', "Expected ',' between result object members.");
            skipWhitespace();
        }

        if (!hasAiStatus) {
            fileReport.aiStatus =
                fileReport.aiAnalysis.starts_with(kErrorPrefix) ? "error" : "generated";
        }

        return fileReport;
    }

    [[nodiscard]] std::vector<analysis::ai_report::FileReport> parseResultsArray() {
        expect('[', "Expected the 'results' field to be a JSON array.");

        std::vector<analysis::ai_report::FileReport> results;
        skipWhitespace();
        if (consume(']')) {
            return results;
        }

        while (true) {
            results.push_back(parseFileReport());
            skipWhitespace();
            if (consume(']')) {
                return results;
            }
            expect(',', "Expected ',' between result entries.");
            skipWhitespace();
        }
    }

    std::string_view input_;
    std::size_t position_ = 0;
};

std::string_view extractSection(std::string_view analysis, std::string_view marker) {
    std::size_t lineStart = 0;
    std::size_t contentStart = std::string_view::npos;
    bool inCodeFence = false;

    while (lineStart <= analysis.size()) {
        const std::string_view line = readLine(analysis, lineStart);
        const std::string_view trimmedLine = trim(line);

        if (trimmedLine.starts_with("```")) {
            inCodeFence = !inCodeFence;
        }

        if (!inCodeFence && lineMatchesMarker(line, marker)) {
            const std::size_t lineEnd = findLineEnd(analysis, lineStart);
            contentStart = lineEnd == analysis.size() ? analysis.size() : lineEnd + 1;
            break;
        }

        if (lineStart == analysis.size()) {
            break;
        }
        lineStart = advanceLine(analysis, lineStart);
    }

    if (contentStart == std::string_view::npos) {
        return {};
    }

    std::size_t nextHeader = std::string_view::npos;
    lineStart = contentStart;
    inCodeFence = false;
    while (lineStart <= analysis.size()) {
        const std::string_view line = readLine(analysis, lineStart);
        const std::string_view trimmedLine = trim(line);

        if (trimmedLine.starts_with("```")) {
            inCodeFence = !inCodeFence;
        }

        if (!inCodeFence &&
            (lineMatchesMarker(line, kSummaryMarker) || lineMatchesMarker(line, kIssuesMarker) ||
             lineMatchesMarker(line, kSuggestionsMarker) ||
             lineMatchesMarker(line, kErrorPrefix) || lineMatchesMarker(line, kSkippedPrefix))) {
            nextHeader = lineStart;
            break;
        }

        if (lineStart == analysis.size()) {
            break;
        }
        lineStart = advanceLine(analysis, lineStart);
    }

    if (nextHeader == std::string_view::npos) {
        nextHeader = analysis.size();
    }

    return trim(analysis.substr(contentStart, nextHeader - contentStart));
}

std::string_view trimIssueLine(std::string_view line) {
    line = trim(stripMarkdownPrefix(line));
    while (!line.empty() &&
           (line.front() == '.' || line.front() == ')' || line.front() == ':' ||
            line.front() == '`' || std::isdigit(static_cast<unsigned char>(line.front())) != 0)) {
        line.remove_prefix(1);
        line = trim(line);
    }

    while (!line.empty() &&
           (line.back() == '.' || line.back() == ':' || line.back() == ';' ||
            line.back() == '*' || line.back() == '`' || line.back() == '_')) {
        line.remove_suffix(1);
        line = trim(line);
    }

    return line;
}

bool containsMeaningfulIssues(std::string_view issues) {
    if (issues.empty()) {
        return false;
    }

    std::size_t lineStart = 0;
    while (lineStart <= issues.size()) {
        const std::string_view normalizedLine = trimIssueLine(readLine(issues, lineStart));

        if (!normalizedLine.empty()) {
            const bool matchesNoIssueMarker = std::any_of(
                kNoIssueMarkers.begin(),
                kNoIssueMarkers.end(),
                [&](std::string_view marker) {
                    return equalsIgnoreAsciiCase(normalizedLine, marker);
                }
            );
            if (!matchesNoIssueMarker) {
                return true;
            }
        }

        if (lineStart == issues.size()) {
            break;
        }
        lineStart = advanceLine(issues, lineStart);
    }

    return false;
}

bool streamSupportsColor(const std::ostream& output) {
#ifdef __unix__
    return (&output == &std::cout && ::isatty(STDOUT_FILENO) != 0) ||
           (&output == &std::cerr && ::isatty(STDERR_FILENO) != 0);
#elif defined(_WIN32)
    return (&output == &std::cout && ::_isatty(_fileno(stdout)) != 0) ||
           (&output == &std::cerr && ::_isatty(_fileno(stderr)) != 0);
#else
    (void)output;
    return false;
#endif
}

void printColor(
    std::ostream& output,
    std::string_view text,
    const char* color,
    bool useColors
) {
    if (useColors) {
        output << color << text << kReset;
        return;
    }
    output << text;
}

void printColoredCount(
    std::ostream& output,
    std::string_view label,
    std::size_t value,
    const char* color,
    bool useColors
) {
    if (useColors) {
        output << color;
    }
    output << label << value;
    if (useColors) {
        output << kReset;
    }
}

void printStatusLabel(
    std::ostream& output,
    std::string_view status,
    const char* color,
    bool useColors
) {
    if (useColors) {
        output << color;
    }
    output << "[" << status << "]";
    if (useColors) {
        output << kReset;
    }
}
}

namespace analysis::ai_report {
std::expected<Report, std::string> parseReport(
    const std::filesystem::path& reportPath
) {
    if (!std::filesystem::exists(reportPath)) {
        return std::unexpected("AI report file does not exist: " + reportPath.string());
    }

    std::string reportJson;
    try {
        std::ifstream input(reportPath, std::ios::binary);
        input.exceptions(std::ifstream::badbit);
        if (!input.is_open()) {
            return std::unexpected("Failed to read AI report: could not open file.");
        }

        reportJson.assign(
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()
        );
    } catch (const std::exception& error) {
        return std::unexpected("Failed to read AI report: " + std::string(error.what()));
    }

    try {
        return JsonParser(reportJson).parseReport();
    } catch (const MissingResultsSectionError& error) {
        return std::unexpected(error.what());
    } catch (const JsonParseError& error) {
        return std::unexpected("Failed to read AI report: " + std::string(error.what()));
    }
}

void printReport(const Report& report, std::ostream& output) {
    const bool useColors = streamSupportsColor(output);
    std::size_t totalFiles = 0;
    std::size_t redFiles = 0;
    std::size_t yellowFiles = 0;
    std::size_t greenFiles = 0;

    output << "\n";
    printColor(output, "=== AI Report Summary ===", kCyan, useColors);
    output << '\n';
    output << "Model: " << report.model << '\n';

    if (report.results.empty()) {
        printColor(output, "AI report does not contain file results.", kYellow, useColors);
        output << '\n';
        return;
    }

    for (const auto& result : report.results) {
        ++totalFiles;

        const std::string_view aiAnalysis = result.aiAnalysis;
        const std::string_view aiStatus = result.aiStatus;
        const bool hasAiError = aiStatus == "error" || aiAnalysis.starts_with(kErrorPrefix);
        const bool aiSkipped = aiStatus == "skipped" || aiAnalysis.starts_with(kSkippedPrefix);
        const bool aiCached = aiStatus == "cached";
        const std::string_view summary =
            (hasAiError || aiSkipped) ? std::string_view{} : extractSection(aiAnalysis, kSummaryMarker);
        const std::string_view issues =
            (hasAiError || aiSkipped) ? std::string_view{} : extractSection(aiAnalysis, kIssuesMarker);
        const std::string_view suggestions =
            (hasAiError || aiSkipped) ? std::string_view{}
                                      : extractSection(aiAnalysis, kSuggestionsMarker);

        const bool hasRuleIssues = !result.ruleBasedIssues.empty();
        const bool hasAiIssues = containsMeaningfulIssues(issues);
        const bool hasSuggestions = !trim(suggestions).empty();

        std::string status;
        const char* statusColor = nullptr;

        if (hasAiError || hasRuleIssues || hasAiIssues) {
            status = "PROBLEM";
            statusColor = kRed;
            ++redFiles;
        } else if (aiSkipped || hasSuggestions) {
            status = "IMPROVE";
            statusColor = kYellow;
            ++yellowFiles;
        } else {
            status = "GOOD";
            statusColor = kGreen;
            ++greenFiles;
        }

        output << "\n";
        printStatusLabel(output, status, statusColor, useColors);
        output << " " << result.path << '\n';

        if (hasRuleIssues) {
            printColor(output, "Rule-based issues:", kRed, useColors);
            output << '\n';
            for (const auto& issue : result.ruleBasedIssues) {
                output << "  - " << issue << '\n';
            }
        } else {
            printColor(output, "Rule-based issues: none", kGreen, useColors);
            output << '\n';
        }

        if (hasAiError) {
            printColor(output, "AI analysis:", kRed, useColors);
            output << '\n' << aiAnalysis << '\n';
            continue;
        }

        if (aiSkipped) {
            printColor(output, "AI analysis skipped:", kYellow, useColors);
            output << '\n' << aiAnalysis << '\n';
            continue;
        }

        if (aiCached) {
            printColor(output, "AI result reused from cache", kCyan, useColors);
            output << '\n';
        }

        if (!summary.empty()) {
            printColor(output, "Summary:", kCyan, useColors);
            output << '\n' << summary << '\n';
        }
        if (hasAiIssues) {
            printColor(output, "Issues:", kRed, useColors);
            output << '\n' << issues << '\n';
        }
        if (hasSuggestions) {
            printColor(output, "Suggestions:", kYellow, useColors);
            output << '\n' << suggestions << '\n';
        }
    }

    output << "\n";
    printColor(output, "=== Status Totals ===", kCyan, useColors);
    output << '\n';
    printColoredCount(output, "Problems: ", redFiles, kRed, useColors);
    output << '\n';
    printColoredCount(output, "Needs improvement: ", yellowFiles, kYellow, useColors);
    output << '\n';
    printColoredCount(output, "Good: ", greenFiles, kGreen, useColors);
    output << '\n';
    output << "Total files: " << totalFiles << '\n';
}

bool printConsoleReport(
    const std::filesystem::path& reportPath,
    std::ostream& output
) {
    const auto parsedReport = parseReport(reportPath);
    if (!parsedReport) {
        const bool useColors = streamSupportsColor(output);
        printColor(output, parsedReport.error(), kRed, useColors);
        output << '\n';
        return false;
    }

    printReport(*parsedReport, output);
    return !output.fail();
}
}
