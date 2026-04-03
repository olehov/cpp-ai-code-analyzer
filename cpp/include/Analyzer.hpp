#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "AnalysisTypes.hpp"

/**
 * @file Analyzer.hpp
 * @brief Declares the high-level project analyzer entry point.
 */

/**
 * @brief Coordinates project traversal, parsing, and JSON serialization.
 */
class Analyzer {
public:
    Analyzer() = delete;

    /**
     * @brief Creates an analyzer for the given project root or input file.
     * @param path Directory or source file that should be analyzed.
     */
    explicit Analyzer(const std::filesystem::path& path);

    /**
     * @brief Runs the filesystem traversal and parsing stages.
     */
    void run();

    /**
     * @brief Returns the latest in-memory analysis results.
     * @return Parsed file analysis data produced by the last call to `run()`.
     */
    [[nodiscard]] const std::vector<analysis::FileAnalysis>& getAnalysis() const noexcept;

    /**
     * @brief Serializes the current analysis results to JSON.
     * @return JSON representation of the project analysis.
     */
    [[nodiscard]] std::string toJson() const;

private:
    std::vector<std::filesystem::path> paths_;
    std::filesystem::path rootPath_;
    std::vector<analysis::FileAnalysis> analysis_;

    void collectProjectFiles();
    void analyzeFiles();
    static bool isSupportedSourceFile(const std::filesystem::path& path);
    static bool shouldSkipDirectory(const std::filesystem::path& path);
};
