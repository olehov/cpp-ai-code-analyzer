#pragma once

#include <filesystem>
#include <iosfwd>
#include <string>
#include <vector>

#include "AnalysisTypes.hpp"

/**
 * @file JsonWriter.hpp
 * @brief Declares JSON serialization helpers for analysis results.
 */

/**
 * @brief Serializes analyzer output into JSON.
 */
class JsonWriter final {
public:
    JsonWriter() = delete;

    /**
     * @brief Serializes the analysis results into a JSON string.
     * @param rootPath Root path that was analyzed.
     * @param analysis Parsed file analysis results.
     * @return JSON string containing the serialized analysis.
     */
    [[nodiscard]] static std::string write(
        const std::filesystem::path& rootPath,
        const std::vector<analysis::FileAnalysis>& analysis
    );

    /**
     * @brief Writes the analysis results directly to an output stream as JSON.
     * @param output Destination stream.
     * @param rootPath Root path that was analyzed.
     * @param analysis Parsed file analysis results.
     */
    static void write(
        std::ostream& output,
        const std::filesystem::path& rootPath,
        const std::vector<analysis::FileAnalysis>& analysis
    );
};
