#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "AnalysisTypes.hpp"

/**
 * @file ProjectParser.hpp
 * @brief Declares the lightweight parser used for per-file analysis.
 */

/**
 * @brief Parses C/C++ source files into the internal analysis DTOs.
 */
class ProjectParser {
public:
    ProjectParser() = delete;

    /**
     * @brief Parses one source file and extracts headers, classes, and public methods.
     * @param path Path to the source file to analyze.
     * @return Parsed representation of the file.
     */
    [[nodiscard]] static analysis::FileAnalysis analyzeFile(const std::filesystem::path& path);

private:
    /**
     * @brief Reads the entire file into memory.
     * @param path Path to the file being loaded.
     * @return Raw file contents.
     */
    [[nodiscard]] static std::string readFile(const std::filesystem::path& path);

    /**
     * @brief Extracts all `#include` targets from sanitized source code.
     * @param content Source code with comments already removed.
     * @return List of parsed header names.
     */
    static std::vector<std::string> parseHeaders(const std::string& content);

    /**
     * @brief Extracts top-level classes and structs from sanitized source code.
     * @param content Source code with comments already removed.
     * @return Parsed class descriptions.
     */
    static std::vector<analysis::ClassInfo> parseClasses(const std::string& content);

    /**
     * @brief Removes line and block comments while preserving string literals.
     * @param content Raw source code.
     * @return Source code without comments.
     */
    static std::string stripComments(const std::string& content);

    /**
     * @brief Finds the matching closing brace for a class or struct body.
     * @param content Source code containing the brace sequence.
     * @param openBracePos Position of the opening brace.
     * @return Index of the matching closing brace or `std::string::npos`.
     */
    static std::size_t findMatchingBrace(const std::string& content, std::size_t openBracePos);

    /**
     * @brief Extracts public method declarations from a class body.
     * @param classBody View of the class body without the outer braces.
     * @param publicByDefault `true` when parsing a `struct`, `false` for a `class`.
     * @return Collected public method declarations.
     */
    static std::vector<std::string> parsePublicMethods(
        std::string_view classBody,
        bool publicByDefault
    );

    /**
     * @brief Trims leading and trailing ASCII whitespace from a string view.
     * @param value Input text view.
     * @return Trimmed view into the original text.
     */
    static std::string_view trim(std::string_view value);
};
