#include "Analyzer.hpp"
#include "ProjectParser.hpp"

#include <algorithm>
#include <stdexcept>
#include <unordered_set>

namespace {
const std::unordered_set<std::filesystem::path> kSupportedExtensions = {
    ".h", ".hh", ".hpp", ".hxx", ".c", ".cc", ".cpp", ".cxx"
};

const std::unordered_set<std::filesystem::path> kSkippedDirectories = {
    ".git", ".idea", ".vscode", "build", "cmake-build-debug", "cmake-build-release", "venv",
    "__pycache__"
};

bool isSupportedSourceFile(const std::filesystem::path& path) {
    return kSupportedExtensions.contains(path.extension());
}

bool shouldSkipDirectory(const std::filesystem::path& path) {
    return kSkippedDirectories.contains(path.filename());
}
}

namespace analysis {
Analyzer::Analyzer(const std::filesystem::path& path) {
    if (path.empty()) {
        throw std::runtime_error("Analyzer input path must not be empty.");
    }

    std::error_code errorCode;
    rootPath_ = std::filesystem::weakly_canonical(path, errorCode);
    if (errorCode) {
        throw std::runtime_error("Failed to resolve analyzer path: " + errorCode.message());
    }
    if (!std::filesystem::exists(rootPath_, errorCode) || errorCode) {
        throw std::runtime_error("Path does not exist: " + rootPath_.string());
    }
}

void Analyzer::run() {
    analysis_.clear();
    diagnostics_.clear();
    analyzeFiles(collectProjectFiles());
}

std::span<const FileAnalysis> Analyzer::getAnalysis() const noexcept {
    return analysis_;
}

std::span<const std::string> Analyzer::getDiagnostics() const noexcept {
    return diagnostics_;
}

std::vector<std::filesystem::path> Analyzer::collectProjectFiles() {
    std::vector<std::filesystem::path> paths;
    const std::filesystem::path& root = rootPath_;
    std::error_code errorCode;

    if (std::filesystem::is_regular_file(root, errorCode)) {
        if (isSupportedSourceFile(root)) {
            paths.push_back(root);
        }
        return paths;
    }

    const auto options = std::filesystem::directory_options::skip_permission_denied;
    std::filesystem::recursive_directory_iterator iterator(root, options, errorCode);
    const std::filesystem::recursive_directory_iterator end;

    if (errorCode) {
        throw std::runtime_error(
            "Unable to traverse project directory: " + errorCode.message()
        );
    }

    while (iterator != end) {
        const auto& entry = *iterator;
        const auto entryPath = entry.path();

        if (entry.is_directory(errorCode)) {
            if (!errorCode && shouldSkipDirectory(entryPath)) {
                iterator.disable_recursion_pending();
            }
        } else if (!errorCode && entry.is_regular_file(errorCode) && isSupportedSourceFile(entryPath)) {
            paths.push_back(entryPath);
        }

        if (errorCode) {
            diagnostics_.push_back(
                "Filesystem metadata failed at " + entryPath.string() + ": " + errorCode.message()
            );
        }

        errorCode.clear();
        iterator.increment(errorCode);
        if (errorCode) {
            diagnostics_.push_back(
                "Unable to continue directory traversal after " + entryPath.string() + ": " +
                errorCode.message()
            );
            break;
        }
    }

    std::sort(paths.begin(), paths.end());
    return paths;
}

void Analyzer::analyzeFiles(const std::vector<std::filesystem::path>& paths) {
    analysis_.reserve(paths.size());
    for (const auto& path : paths) {
        try {
            analysis_.push_back(project_parser::analyzeFile(path));
        } catch (const std::exception& error) {
            diagnostics_.push_back("Failed to parse " + path.string() + ": " + error.what());

            FileAnalysis failedAnalysis;
            failedAnalysis.filePath = path;
            failedAnalysis.parseError = error.what();
            analysis_.push_back(std::move(failedAnalysis));
        }
    }
}
}
