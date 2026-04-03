#include "AiReportReader.hpp"

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <exception>
#include <fstream>
#include <iostream>
#include <ostream>
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
    boost::property_tree::ptree reportTree;
    if (!std::filesystem::exists(reportPath)) {
        return std::unexpected("AI report file does not exist: " + reportPath.string());
    }

    try {
        std::ifstream input(reportPath);
        input.exceptions(std::ifstream::failbit | std::ifstream::badbit);
        boost::property_tree::read_json(input, reportTree);
    } catch (const std::exception& error) {
        return std::unexpected("Failed to read AI report: " + std::string(error.what()));
    }

    const auto results = reportTree.get_child_optional("results");
    if (!results) {
        return std::unexpected("AI report is missing the 'results' section.");
    }

    Report report;
    report.model = reportTree.get<std::string>("model", "unknown");
    report.results.reserve(results->size());

    for (const auto& resultNode : *results) {
        const auto& result = resultNode.second;

        FileReport fileReport;
        fileReport.path = result.get_optional<std::string>("path").value_or("<unknown>");
        fileReport.aiAnalysis = result.get_optional<std::string>("ai_analysis").value_or("");
        fileReport.aiStatus =
            result.get_optional<std::string>("ai_status").value_or(
                fileReport.aiAnalysis.starts_with(kErrorPrefix) ? "error" : "generated"
            );

        if (const auto ruleIssues = result.get_child_optional("rule_based_issues")) {
            fileReport.ruleBasedIssues.reserve(ruleIssues->size());
            std::transform(
                ruleIssues->begin(),
                ruleIssues->end(),
                std::back_inserter(fileReport.ruleBasedIssues),
                [](const auto& issueNode) {
                    return issueNode.second.template get_value<std::string>();
                }
            );
        }

        report.results.push_back(std::move(fileReport));
    }

    return report;
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
