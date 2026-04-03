#pragma once

#include <filesystem>
#include <string>
#include <vector>

/**
 * @file AnalysisTypes.hpp
 * @brief Shared data structures used across the analysis pipeline.
 */

/**
 * @namespace analysis
 * @brief Contains the lightweight DTOs produced by the analyzer.
 */
namespace analysis {
/**
 * @brief Describes a parsed C++ class or struct.
 */
struct ClassInfo {
    std::string name; ///< Parsed class or struct name.
    std::vector<std::string> publicMethods; ///< Public method declarations found in the type.

    /**
     * @brief Compares two parsed class descriptions.
     * @param other Class description to compare against.
     * @return `true` when both objects contain the same parsed data.
     */
    bool operator==(const ClassInfo& other) const = default;
};

/**
 * @brief Stores the analysis result for a single source file.
 */
struct FileAnalysis {
    std::filesystem::path filePath; ///< Absolute or project-relative path to the analyzed file.
    std::vector<std::string> headers; ///< Header names extracted from `#include` directives.
    std::vector<ClassInfo> classes; ///< Parsed classes and structs discovered in the file.
    std::string parseError; ///< Non-empty when the file could not be parsed completely.

    /**
     * @brief Compares two file-level analysis results.
     * @param other File analysis to compare against.
     * @return `true` when both objects contain the same parsed data.
     */
    bool operator==(const FileAnalysis& other) const = default;
};
}
