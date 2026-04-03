# C++ AI Code Analyzer

A small C++23 + Python project that scans a C/C++ codebase, extracts structural information, serializes it to JSON, and sends each file to an AI model for review.

The project is split into two stages:

1. A C++ analyzer that walks the project tree and extracts:
   - `#include` headers
   - class / struct names
   - public method declarations
2. A Python AI runner that reads the generated JSON, sends each file to Gemini, and stores the AI report as JSON.

## Features

- Recursive project traversal with directory skipping
- Lightweight C++ parser for headers, classes, and public methods
- JSON export of project structure
- Safe Python process launch from C++ with `posix_spawnp`
- Retry / backoff for temporary AI API failures
- Colorized console report for AI results

## Project Structure

```text
cpp/
  include/
  src/
python/
  analyzer.py
config/
  ai_config.example.json
tmp/
  analysis.json
  gemini_analysis.json
```

## Requirements

- C++23-compatible compiler
- Python 3.11+
- `make`
- Gemini API key

## Setup

Create and activate a virtual environment if you want an isolated Python setup:

```bash
python3 -m venv venv
source venv/bin/activate
pip install -r requirements.txt
```

Create the local config file:

```bash
cp config/ai_config.example.json config/ai_config.json
```

Then put your real Gemini API key into `config/ai_config.json`.

## Build

```bash
make
```

## Run

Analyze the current project:

```bash
make run
```

Useful Make targets:

- `make` or `make all` - build the analyzer
- `make run` - build and run the full pipeline
- `make ai` - alias for the full pipeline
- `make clean` - remove object files
- `make fclean` - remove object files and the binary
- `make re` - rebuild from scratch

## Output

Generated files are written to `tmp/`:

- `tmp/analysis.json` - structural project analysis from the C++ stage
- `tmp/gemini_analysis.json` - AI review results from the Python stage

## Notes

- The parser is heuristic-based, not a full C++ AST parser.
- Complex templates, macros, and advanced syntax may still produce imperfect results.
- AI analysis requires working network access and a valid Gemini API key.
