#include "Analyzer.hpp"
#include "JsonWriter.hpp"
#include "ProjectParser.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>

namespace {
const std::array<std::filesystem::path, 8> kSupportedExtensions = {
    ".h", ".hh", ".hpp", ".hxx", ".c", ".cc", ".cpp", ".cxx"
};

const std::array<std::filesystem::path, 8> kSkippedDirectories = {
    ".git", ".idea", ".vscode", "build", "cmake-build-debug", "cmake-build-release", "venv",
    "__pycache__"
};
}

Analyzer::Analyzer(const std::filesystem::path& path) : rootPath_(std::filesystem::absolute(path)) {
}

void Analyzer::run() {
    paths_.clear();
    analysis_.clear();
    collectProjectFiles();
    analyzeFiles();
}

const std::vector<analysis::FileAnalysis>& Analyzer::getAnalysis() const noexcept {
    return analysis_;
}

void Analyzer::collectProjectFiles() {
    const std::filesystem::path root(rootPath_);

    if (!std::filesystem::exists(root)) {
        throw std::runtime_error("Path does not exist: " + rootPath_.string());
    }

    if (std::filesystem::is_regular_file(root)) {
        if (isSupportedSourceFile(root)) {
            paths_.push_back(root);
        }
        return;
    }

    std::error_code errorCode;
    const auto options = std::filesystem::directory_options::skip_permission_denied;
    std::filesystem::recursive_directory_iterator iterator(root, options, errorCode);
    const std::filesystem::recursive_directory_iterator end;

    if (errorCode) {
        throw std::runtime_error(
            "Unable to traverse project directory: " + errorCode.message()
        );
    }

    while (iterator != end) {
        const auto entryPath = iterator->path();

        if (iterator->is_directory(errorCode)) {
            if (!errorCode && shouldSkipDirectory(entryPath)) {
                iterator.disable_recursion_pending();
            }
        } else if (!errorCode && iterator->is_regular_file(errorCode) &&
                   isSupportedSourceFile(entryPath)) {
            paths_.push_back(entryPath);
        }

        errorCode.clear();
        iterator.increment(errorCode);
        if (errorCode) {
            errorCode.clear();
        }
    }

    std::sort(paths_.begin(), paths_.end());
}

void Analyzer::analyzeFiles() {
    analysis_.clear();
    analysis_.reserve(paths_.size());
    std::transform(
        paths_.begin(),
        paths_.end(),
        std::back_inserter(analysis_),
        [](const auto& path) { return ProjectParser::analyzeFile(path); }
    );
    paths_.clear();
    paths_.shrink_to_fit();
}

bool Analyzer::isSupportedSourceFile(const std::filesystem::path& path) {
    const auto extension = path.extension();
    return std::find(kSupportedExtensions.begin(), kSupportedExtensions.end(), extension) !=
           kSupportedExtensions.end();
}

bool Analyzer::shouldSkipDirectory(const std::filesystem::path& path) {
    const auto directoryName = path.filename();
    return std::find(kSkippedDirectories.begin(), kSkippedDirectories.end(), directoryName) !=
           kSkippedDirectories.end();
}

std::string Analyzer::toJson() const {
    return JsonWriter::write(rootPath_, getAnalysis());
}
