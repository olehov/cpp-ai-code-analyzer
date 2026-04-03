#pragma once

#include <filesystem>
#include <iosfwd>
#include <span>
#include <string>

#include "AnalysisTypes.hpp"

/**
 * @file JsonWriter.hpp
 * @brief Declares JSON serialization helpers for analysis results.
 */

/**
 * @namespace analysis::json
 * @brief Serializes analyzer output into JSON.
 */
namespace analysis::json {
/**
 * @brief Serializes the analysis results into a JSON string.
 * @param rootPath Root path that was analyzed.
 * @param analysis Parsed file analysis results.
 * @return JSON string containing the serialized analysis.
 */
[[nodiscard]] std::string write(
    const std::filesystem::path& rootPath,
    std::span<const FileAnalysis> analysis
);

/**
 * @brief Writes the analysis results directly to an output stream as JSON.
 * @param output Destination stream.
 * @param rootPath Root path that was analyzed.
 * @param analysis Parsed file analysis results.
 */
void write(
    std::ostream& output,
    const std::filesystem::path& rootPath,
    std::span<const FileAnalysis> analysis
);

/**
 * @brief Writes the analysis results directly to a JSON file.
 * @param destination Destination JSON file path.
 * @param rootPath Root path that was analyzed.
 * @param analysis Parsed file analysis results.
 * @throws std::ios_base::failure When the destination file cannot be written.
 */
void write(
    const std::filesystem::path& destination,
    const std::filesystem::path& rootPath,
    std::span<const FileAnalysis> analysis
);
}
