#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#ifdef __unix__
#include <spawn.h>
#include <sys/wait.h>
#endif
#include <system_error>
#include <stdexcept>
#include <string>
#include <vector>

#include "AiReportReader.hpp"
#include "Analyzer.hpp"

#ifdef __unix__
extern "C" char** environ;
#endif

namespace {
std::filesystem::path getExecutableDirectory(const char* argv0) {
    try {
        const std::filesystem::path procSelfExe("/proc/self/exe");
        if (std::filesystem::exists(procSelfExe)) {
            return std::filesystem::read_symlink(procSelfExe).parent_path();
        }
    } catch (const std::exception&) {
    }

    if (argv0 != nullptr) {
        return std::filesystem::absolute(argv0).parent_path();
    }

    return std::filesystem::current_path();
}

std::filesystem::path getProjectRoot(const char* argv0) {
    return getExecutableDirectory(argv0);
}

std::filesystem::path resolvePythonExecutable(const std::filesystem::path& projectRoot) {
    const std::filesystem::path venvPython = projectRoot / "venv" / "bin" / "python";
    if (std::filesystem::exists(venvPython)) {
        return venvPython;
    }

    if (const char* pythonFromEnv = std::getenv("PYTHON_EXE")) {
        return std::filesystem::path(pythonFromEnv);
    }

    return std::filesystem::path("python3");
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
        [](const std::string& argument) {
            return const_cast<char*>(argument.c_str());
        }
    );
    argv.push_back(nullptr);

    pid_t childPid = 0;
    const int spawnResult =
        posix_spawnp(&childPid, argv[0], nullptr, nullptr, argv.data(), environ);
    if (spawnResult != 0) {
        throw std::system_error(spawnResult, std::generic_category(), "posix_spawnp failed");
    }

    int processStatus = 0;
    if (waitpid(childPid, &processStatus, 0) == -1) {
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

        if (pythonExecutable.is_absolute() && !std::filesystem::exists(pythonExecutable)) {
            throw std::runtime_error(
                "Python executable does not exist: " + pythonExecutable.string()
            );
        }

        Analyzer analyzer{inputPath};
        analyzer.run();
        const std::string json = analyzer.toJson();

        std::filesystem::create_directories(outputDirectory);

        {
            std::ofstream outputFile;
            outputFile.exceptions(std::ofstream::failbit | std::ofstream::badbit);
            outputFile.open(analysisPath);
            outputFile << json;
        }

        std::cout << "Project JSON saved to: " << analysisPath.string() << '\n';
        std::cout << "Waiting for AI response..." << std::endl;
        runPythonAnalyzer(
            pythonExecutable,
            scriptPath,
            analysisPath,
            geminiOutputPath,
            configPath
        );
        std::cout << "AI analysis saved to: " << geminiOutputPath.string() << '\n';
        if (!AiReportReader::printConsoleReport(geminiOutputPath, std::cout)) {
            return 1;
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }

    return 0;
}
