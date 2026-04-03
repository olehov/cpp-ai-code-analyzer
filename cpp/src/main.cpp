#include <cerrno>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#ifdef __unix__
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
#include <sstream>
#include <system_error>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "AiReportReader.hpp"
#include "Analyzer.hpp"
#include "JsonWriter.hpp"

#ifdef __unix__
extern "C" char** environ;
#endif

namespace {
constexpr char kPathListSeparator =
#ifdef _WIN32
    ';';
#else
    ':';
#endif

std::filesystem::path findExecutableOnPath(const char* argv0) {
    if (argv0 == nullptr) {
        return {};
    }

    const std::string executableName(argv0);
    if (executableName.find('/') != std::string::npos) {
        return {};
    }

    const char* pathEnvironment = std::getenv("PATH");
    if (pathEnvironment == nullptr) {
        return {};
    }

    std::stringstream pathStream(pathEnvironment);
    std::string candidateDirectory;
    while (std::getline(pathStream, candidateDirectory, kPathListSeparator)) {
        const std::filesystem::path candidatePath =
            std::filesystem::path(candidateDirectory) / executableName;
        if (std::filesystem::exists(candidatePath) && std::filesystem::is_regular_file(candidatePath)) {
            return std::filesystem::absolute(candidatePath);
        }
    }

    return {};
}

std::filesystem::path resolveExecutablePath(std::string_view executable) {
    if (executable.empty()) {
        return {};
    }

    const std::filesystem::path executablePath(executable);
    if (executablePath.is_absolute()) {
        return executablePath;
    }

    if (executablePath.has_parent_path()) {
        return std::filesystem::absolute(executablePath);
    }

    return findExecutableOnPath(executable.data());
}

std::filesystem::path getExecutableDirectory(const char* argv0) {
    try {
        const std::filesystem::path procSelfExe("/proc/self/exe");
        if (std::filesystem::exists(procSelfExe)) {
            return std::filesystem::read_symlink(procSelfExe).parent_path();
        }
    } catch (const std::exception&) {
    }

    if (argv0 != nullptr) {
        if (const std::filesystem::path pathFromEnvironment = findExecutableOnPath(argv0);
            !pathFromEnvironment.empty()) {
            return pathFromEnvironment.parent_path();
        }
        return std::filesystem::absolute(argv0).parent_path();
    }

    return std::filesystem::current_path();
}

bool looksLikeProjectRoot(const std::filesystem::path& path) {
    return std::filesystem::exists(path / "cpp") && std::filesystem::exists(path / "python") &&
           std::filesystem::exists(path / "config");
}

std::filesystem::path getProjectRoot(const char* argv0) {
    if (const char* rootFromEnv = std::getenv("ANALYZER_ROOT")) {
        const std::filesystem::path configuredRoot(rootFromEnv);
        if (!configuredRoot.empty()) {
            return std::filesystem::weakly_canonical(configuredRoot);
        }
    }

    std::filesystem::path currentPath = getExecutableDirectory(argv0);

    while (!currentPath.empty()) {
        if (looksLikeProjectRoot(currentPath)) {
            return currentPath;
        }

        const std::filesystem::path parentPath = currentPath.parent_path();
        if (parentPath == currentPath) {
            break;
        }
        currentPath = parentPath;
    }

    return getExecutableDirectory(argv0);
}

std::filesystem::path resolvePythonExecutable(const std::filesystem::path& projectRoot) {
    const std::filesystem::path venvPython =
#ifdef _WIN32
        projectRoot / "venv" / "Scripts" / "python.exe";
#else
        projectRoot / "venv" / "bin" / "python";
#endif
    if (std::filesystem::exists(venvPython)) {
        return std::filesystem::absolute(venvPython);
    }

    if (const char* pythonFromEnv = std::getenv("PYTHON_EXE")) {
        if (const auto resolved = resolveExecutablePath(pythonFromEnv); !resolved.empty()) {
            return resolved;
        }
    }

    if (const auto resolved = resolveExecutablePath("python3"); !resolved.empty()) {
        return resolved;
    }

    return std::filesystem::path("python3");
}

bool isExecutableFile(const std::filesystem::path& path) {
    std::error_code errorCode;
    if (!std::filesystem::exists(path, errorCode) ||
        !std::filesystem::is_regular_file(path, errorCode) || errorCode) {
        return false;
    }

#ifdef __unix__
    const auto permissions = std::filesystem::status(path, errorCode).permissions();
    if (errorCode) {
        return false;
    }
    using std::filesystem::perms;
    return (permissions & perms::owner_exec) != perms::none ||
           (permissions & perms::group_exec) != perms::none ||
           (permissions & perms::others_exec) != perms::none;
#else
    return true;
#endif
}

void runPythonAnalyzer(
    const std::filesystem::path& pythonExecutable,
    const std::filesystem::path& scriptPath,
    const std::filesystem::path& analysisPath,
    const std::filesystem::path& outputPath,
    const std::filesystem::path& configPath
) {
#ifdef __unix__
    std::vector<std::string> arguments = {
        pythonExecutable.string(),
        scriptPath.string(),
        analysisPath.string(),
        outputPath.string(),
        configPath.string(),
    };

    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1);
    std::transform(
        arguments.begin(),
        arguments.end(),
        std::back_inserter(argv),
        [](auto& argument) -> char* {
            return argument.data();
        }
    );
    argv.push_back(nullptr);

    pid_t childPid = 0;
    const int spawnResult = pythonExecutable.is_absolute()
                                ? posix_spawn(&childPid, argv[0], nullptr, nullptr, argv.data(), environ)
                                : posix_spawnp(&childPid, argv[0], nullptr, nullptr, argv.data(), environ);
    if (spawnResult != 0) {
        throw std::system_error(spawnResult, std::generic_category(), "posix_spawnp failed");
    }

    int processStatus = 0;
    while (waitpid(childPid, &processStatus, 0) == -1) {
        if (errno == EINTR) {
            continue;
        }
        throw std::runtime_error("Failed to wait for Python analyzer process.");
    }

    if (WIFEXITED(processStatus)) {
        const int exitCode = WEXITSTATUS(processStatus);
        if (exitCode != 0) {
            throw std::runtime_error("Python analyzer failed with exit code: " +
                                     std::to_string(exitCode));
        }
    } else if (WIFSIGNALED(processStatus)) {
        throw std::runtime_error("Python analyzer terminated by signal: " +
                                 std::to_string(WTERMSIG(processStatus)));
    } else {
        throw std::runtime_error("Python analyzer terminated abnormally.");
    }
#else
    (void)pythonExecutable;
    (void)scriptPath;
    (void)analysisPath;
    (void)outputPath;
    (void)configPath;
    throw std::runtime_error("Safe Python process launching is only implemented for POSIX.");
#endif
}
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: ./analyzer <path-to-project-or-file>\n";
        return 1;
    }

    try {
        const std::filesystem::path projectRoot = getProjectRoot(argv[0]);
        const std::filesystem::path inputPath(argv[1]);
        const std::filesystem::path outputDirectory = projectRoot / "tmp";
        const std::filesystem::path analysisPath = outputDirectory / "analysis.json";
        const std::filesystem::path geminiOutputPath = outputDirectory / "gemini_analysis.json";
        const std::filesystem::path configPath = projectRoot / "config" / "ai_config.json";
        const std::filesystem::path scriptPath = projectRoot / "python" / "analyzer.py";
        const std::filesystem::path pythonExecutable = resolvePythonExecutable(projectRoot);

        if (!std::filesystem::exists(inputPath)) {
            throw std::runtime_error("Input path does not exist: " + inputPath.string());
        }

        if (!std::filesystem::exists(configPath)) {
            throw std::runtime_error("AI config file does not exist: " + configPath.string());
        }

        if (!std::filesystem::exists(scriptPath)) {
            throw std::runtime_error("Python analyzer script does not exist: " + scriptPath.string());
        }

        if (pythonExecutable.is_absolute() && !isExecutableFile(pythonExecutable)) {
            throw std::runtime_error("Python executable is not runnable: " +
                                     pythonExecutable.string());
        }

        analysis::Analyzer analyzer{inputPath};
        analyzer.run();

        for (const auto& diagnostic : analyzer.getDiagnostics()) {
            std::cerr << "Warning: " << diagnostic << '\n';
        }

        std::filesystem::create_directories(outputDirectory);
        analysis::json::write(
            analysisPath,
            std::filesystem::absolute(inputPath),
            analyzer.getAnalysis()
        );

        std::cout << "Project JSON saved to: " << analysisPath.string() << '\n';
        std::cout << "Waiting for AI responses (live progress below)..." << std::endl;
        runPythonAnalyzer(
            pythonExecutable,
            scriptPath,
            analysisPath,
            geminiOutputPath,
            configPath
        );
        std::cout << "AI analysis saved to: " << geminiOutputPath.string() << '\n';
        if (!analysis::ai_report::printConsoleReport(geminiOutputPath, std::cout)) {
            std::cerr << "Failed to print AI console report.\n";
            return 1;
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }

    return 0;
}
