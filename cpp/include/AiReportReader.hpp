#pragma once

#include <filesystem>
#include <iosfwd>

/**
 * @file AiReportReader.hpp
 * @brief Declares helpers for reading and printing AI-generated reports.
 */

/**
 * @brief Reads AI report JSON files and prints a human-friendly console summary.
 */
class AiReportReader {
public:
    AiReportReader() = delete;

    /**
     * @brief Loads an AI report from disk and prints a formatted summary.
     * @param reportPath Path to the JSON report produced by the Python AI stage.
     * @param output Stream that receives the formatted report.
     * @return `true` on success, `false` when the report cannot be read or is malformed.
     */
    [[nodiscard]] static bool printConsoleReport(
        const std::filesystem::path& reportPath,
        std::ostream& output
    );
};
