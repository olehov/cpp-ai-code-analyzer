#pragma once

#include <expected>
#include <filesystem>
#include <iosfwd>
#include <string>
#include <vector>

/**
 * @file AiReportReader.hpp
 * @brief Declares helpers for reading and printing AI-generated reports.
 */

/**
 * @namespace analysis::ai_report
 * @brief Reads AI report JSON files and prints a human-friendly console summary.
 */
namespace analysis::ai_report {
struct FileReport {
    std::string path;
    std::string aiStatus;
    std::string aiAnalysis;
    std::vector<std::string> ruleBasedIssues;
};

struct Report {
    std::string model;
    std::vector<FileReport> results;
};

/**
 * @brief Parses a JSON AI report into a structured representation.
 * @param reportPath Path to the JSON report produced by the Python AI stage.
 * @return Parsed report data or a textual error description.
 */
[[nodiscard]] std::expected<Report, std::string> parseReport(
    const std::filesystem::path& reportPath
);

/**
 * @brief Prints a parsed AI report to an output stream.
 * @param report Parsed report data.
 * @param output Stream that receives the formatted report.
 */
void printReport(const Report& report, std::ostream& output);

/**
 * @brief Loads an AI report from disk and prints a formatted summary.
 * @param reportPath Path to the JSON report produced by the Python AI stage.
 * @param output Stream that receives the formatted report.
 * @return `true` on success, `false` when the report cannot be read or is malformed.
 */
[[nodiscard]] bool printConsoleReport(
    const std::filesystem::path& reportPath,
    std::ostream& output
);
}
