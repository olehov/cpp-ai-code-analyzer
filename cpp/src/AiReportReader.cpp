#include "AiReportReader.hpp"

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <algorithm>
#include <cctype>
#include <exception>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#ifdef __unix__
#include <unistd.h>
#endif

namespace {
constexpr const char* kRed = "\033[31m";
constexpr const char* kYellow = "\033[33m";
constexpr const char* kGreen = "\033[32m";
constexpr const char* kCyan = "\033[36m";
constexpr const char* kReset = "\033[0m";
constexpr std::string_view kErrorPrefix = "[ERROR]";
constexpr std::string_view kSummaryMarker = "[SUMMARY]";
constexpr std::string_view kIssuesMarker = "[ISSUES]";
constexpr std::string_view kSuggestionsMarker = "[SUGGESTIONS]";
const std::vector<std::string_view> kNoIssueMarkers = {
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

std::string toLower(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return result;
}

std::string normalizeNewlines(std::string_view value) {
    std::string normalized;
    normalized.reserve(value.size());

    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '\r') {
            normalized.push_back('\n');
            if (i + 1 < value.size() && value[i + 1] == '\n') {
                ++i;
            }
            continue;
        }
        normalized.push_back(value[i]);
    }

    return normalized;
}

std::string_view extractSection(std::string_view normalizedAnalysis, std::string_view marker) {
    const std::size_t start = normalizedAnalysis.find(marker);
    if (start == std::string::npos) {
        return {};
    }

    const std::size_t contentStart = start + marker.size();
    std::size_t nextHeader = std::string::npos;
    for (std::size_t i = contentStart; i < normalizedAnalysis.size(); ++i) {
        if (normalizedAnalysis[i] == '[' &&
            (i == 0 || normalizedAnalysis[i - 1] == '\n')) {
            nextHeader = i;
            break;
        }
    }
    if (nextHeader == std::string::npos) {
        nextHeader = normalizedAnalysis.size();
    }

    return trim(normalizedAnalysis.substr(contentStart, nextHeader - contentStart));
}

bool containsMeaningfulIssues(std::string_view issues) {
    if (issues.empty()) {
        return false;
    }

    const std::string normalized = toLower(trim(issues));

    return std::none_of(
        kNoIssueMarkers.begin(),
        kNoIssueMarkers.end(),
        [&](std::string_view marker) { return normalized == marker; }
    );
}

void printColor(std::ostream& output, std::string_view text, const char* color) {
#ifdef __unix__
    if ((&output == &std::cout && ::isatty(STDOUT_FILENO) != 0) ||
        (&output == &std::cerr && ::isatty(STDERR_FILENO) != 0)) {
        output << color << text << kReset;
        return;
    }
#endif
    output << text;
}
}

bool AiReportReader::printConsoleReport(
    const std::filesystem::path& reportPath,
    std::ostream& output
) {
    boost::property_tree::ptree reportTree;
    if (!std::filesystem::exists(reportPath)) {
        printColor(output, "AI report file does not exist: ", kRed);
        output << reportPath.string() << '\n';
        return false;
    }

    try {
        std::ifstream input(reportPath);
        input.exceptions(std::ifstream::failbit | std::ifstream::badbit);
        boost::property_tree::read_json(input, reportTree);
    } catch (const std::exception& error) {
        printColor(output, "Failed to read AI report: ", kRed);
        output << error.what() << '\n';
        return false;
    }

    const std::string model = reportTree.get<std::string>("model", "unknown");
    std::size_t totalFiles = 0;
    std::size_t redFiles = 0;
    std::size_t yellowFiles = 0;
    std::size_t greenFiles = 0;

    output << "\n";
    printColor(output, "=== AI Report Summary ===", kCyan);
    output << '\n';
    output << "Model: " << model << '\n';

    const auto results = reportTree.get_child_optional("results");
    if (!results) {
        printColor(output, "AI report is missing the 'results' section.", kRed);
        output << '\n';
        return false;
    }

    for (const auto& resultNode : *results) {
        ++totalFiles;

        const auto& result = resultNode.second;
        const std::string path = result.get<std::string>("path", "<unknown>");
        const std::string aiAnalysis = result.get<std::string>("ai_analysis", "");
        const std::string normalizedAiAnalysis = normalizeNewlines(aiAnalysis);
        const auto ruleIssuesChild = result.get_child_optional("rule_based_issues");

        std::vector<std::string> ruleIssues;
        if (ruleIssuesChild) {
            ruleIssues.reserve(ruleIssuesChild->size());
            std::transform(
                ruleIssuesChild->begin(),
                ruleIssuesChild->end(),
                std::back_inserter(ruleIssues),
                [](const auto& issueNode) {
                    return issueNode.second.template get_value<std::string>();
                }
            );
        }

        const bool hasAiError = normalizedAiAnalysis.starts_with(kErrorPrefix);
        const std::string_view summary =
            hasAiError ? std::string_view{} : extractSection(normalizedAiAnalysis, kSummaryMarker);
        const std::string_view issues =
            hasAiError ? std::string_view{} : extractSection(normalizedAiAnalysis, kIssuesMarker);
        const std::string_view suggestions =
            hasAiError ? std::string_view{}
                       : extractSection(normalizedAiAnalysis, kSuggestionsMarker);

        const bool hasRuleIssues = !ruleIssues.empty();
        const bool hasAiIssues = containsMeaningfulIssues(issues);
        const bool hasSuggestions = !trim(suggestions).empty();

        std::string status;
        const char* statusColor = nullptr;

        if (hasAiError || hasRuleIssues || hasAiIssues) {
            status = "PROBLEM";
            statusColor = kRed;
            ++redFiles;
        } else if (hasSuggestions) {
            status = "IMPROVE";
            statusColor = kYellow;
            ++yellowFiles;
        } else {
            status = "GOOD";
            statusColor = kGreen;
            ++greenFiles;
        }

        output << "\n";
        printColor(output, "[" + status + "]", statusColor);
        output << " " << path << '\n';

        if (hasRuleIssues) {
            printColor(output, "Rule-based issues:", kRed);
            output << '\n';
            for (const auto& issue : ruleIssues) {
                output << "  - " << issue << '\n';
            }
        } else {
            printColor(output, "Rule-based issues: none", kGreen);
            output << '\n';
        }

        if (hasAiError) {
            printColor(output, "AI analysis:", kRed);
            output << '\n' << aiAnalysis << '\n';
            continue;
        }

        if (!summary.empty()) {
            printColor(output, "Summary:", kCyan);
            output << '\n' << summary << '\n';
        }
        if (hasAiIssues) {
            printColor(output, "Issues:", kRed);
            output << '\n' << issues << '\n';
        }
        if (hasSuggestions) {
            printColor(output, "Suggestions:", kYellow);
            output << '\n' << suggestions << '\n';
        }
    }

    output << "\n";
    printColor(output, "=== Status Totals ===", kCyan);
    output << '\n';
    printColor(output, "Problems: " + std::to_string(redFiles), kRed);
    output << '\n';
    printColor(output, "Needs improvement: " + std::to_string(yellowFiles), kYellow);
    output << '\n';
    printColor(output, "Good: " + std::to_string(greenFiles), kGreen);
    output << '\n';
    output << "Total files: " << totalFiles << '\n';
    return true;
}
