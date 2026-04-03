#pragma once

#include <filesystem>

#include "AnalysisTypes.hpp"

/**
 * @file ProjectParser.hpp
 * @brief Declares the lightweight parser used for per-file analysis.
 */

/**
 * @namespace analysis::project_parser
 * @brief Parses C/C++ source files into the internal analysis DTOs.
 */
namespace analysis::project_parser {
/**
 * @brief Parses one source file and extracts headers, classes, and public methods.
 * @param path Path to the source file to analyze.
 * @return Parsed representation of the file.
 * @throws std::runtime_error When the file cannot be read successfully.
 */
[[nodiscard]] FileAnalysis analyzeFile(const std::filesystem::path& path);
}
