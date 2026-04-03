#pragma once

#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "AnalysisTypes.hpp"

/**
 * @file Analyzer.hpp
 * @brief Declares the high-level project analyzer entry point.
 */

/**
 * @brief Coordinates project traversal and parsing.
 */
namespace analysis {
class Analyzer {
public:
    Analyzer() = delete;
    Analyzer(const Analyzer&) = delete;
    Analyzer& operator=(const Analyzer&) = delete;
    Analyzer(Analyzer&&) noexcept = default;
    Analyzer& operator=(Analyzer&&) noexcept = default;
    ~Analyzer() = default;

    /**
     * @brief Creates an analyzer for the given project root or input file.
     * @param path Directory or source file that should be analyzed.
     * @throws std::runtime_error When the input path is empty or does not exist.
     */
    explicit Analyzer(const std::filesystem::path& path);

    /**
     * @brief Runs the filesystem traversal and parsing stages.
     * @details Clears previous analysis results before starting a new run.
     * @throws std::runtime_error When filesystem traversal or parsing fails.
     */
    void run();

    /**
     * @brief Returns the latest in-memory analysis results.
     * @return Parsed file analysis data produced by the last call to `run()`.
     */
    [[nodiscard]] std::span<const FileAnalysis> getAnalysis() const noexcept;

    /**
     * @brief Returns non-fatal diagnostics collected during the last run.
     * @return Warning messages gathered while traversing or parsing the project.
     */
    [[nodiscard]] std::span<const std::string> getDiagnostics() const noexcept;

private:
    std::filesystem::path rootPath_;
    std::vector<FileAnalysis> analysis_;
    std::vector<std::string> diagnostics_;

    [[nodiscard]] std::vector<std::filesystem::path> collectProjectFiles();
    void analyzeFiles(const std::vector<std::filesystem::path>& paths);
};
}
