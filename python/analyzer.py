import json
import os
import sys
import time
from pathlib import Path
from typing import Any

try:
    from google import genai
except ImportError:
    genai = None


MODEL_NAME = "gemini-2.5-flash"
DEFAULT_ANALYSIS_PATH = Path("tmp/analysis.json")
DEFAULT_OUTPUT_PATH = Path("tmp/gemini_analysis.json")
DEFAULT_CONFIG_PATH = Path("config/ai_config.json")
DEFAULT_MAX_RETRIES = 3
DEFAULT_BACKOFF_SECONDS = 2.0
DEFAULT_BACKOFF_MULTIPLIER = 2.0
DEFAULT_MAX_BACKOFF_SECONDS = 15.0


def simple_checks(code: str) -> list[str]:
    issues: list[str] = []

    if "new " in code and "delete" not in code:
        issues.append("Possible memory leak: 'new' used without 'delete'")

    if "malloc" in code and "free" not in code:
        issues.append("Possible memory leak: 'malloc' used without 'free'")

    if "while(true)" in code or "for(;;)" in code:
        issues.append("Potential infinite loop detected")

    return issues


def load_project_analysis(analysis_path: Path) -> dict:
    if not analysis_path.exists():
        raise FileNotFoundError(f"Analysis file not found: {analysis_path}")

    return json.loads(analysis_path.read_text(encoding="utf-8"))


def load_config(config_path: Path) -> dict:
    if not config_path.exists():
        return {}

    return json.loads(config_path.read_text(encoding="utf-8"))


def build_project_context(project_analysis: dict) -> str:
    project_snapshot = {
        "root": project_analysis["root"],
        "files": project_analysis["files"],
    }
    return json.dumps(project_snapshot, indent=2, ensure_ascii=False)


def is_retryable_error(error: Exception) -> bool:
    retry_markers = (
        "429",
        "500",
        "502",
        "503",
        "504",
        "resource_exhausted",
        "unavailable",
        "deadline_exceeded",
        "internal",
        "timeout",
        "temporarily unavailable",
        "try again later",
        "high demand",
        "temporary failure in name resolution",
        "name resolution",
        "connection reset",
        "connection aborted",
        "network is unreachable",
    )
    message = str(error).lower()
    return any(marker in message for marker in retry_markers)


def analyze_file_with_ai_once(
    project_context: str,
    file_info: dict,
    file_code: str,
    client: Any,
    model_name: str,
) -> str:
    prompt = f"""
You are a senior C/C++ engineer reviewing one file inside a larger project.

You are given:
1. A JSON snapshot of the project structure, headers, classes, and public methods.
2. The current file path.
3. The current file content.

Use the project JSON as context, but focus your analysis on the current file.

Return the answer in this exact structure:

[SUMMARY]
...

[ISSUES]
...

[SUGGESTIONS]
...

Be concise, technical, and specific.

Project JSON:
{project_context}

Current file path:
{file_info["path"]}

Current file headers:
{json.dumps(file_info.get("headers", []), ensure_ascii=False)}

Current file classes:
{json.dumps(file_info.get("classes", []), indent=2, ensure_ascii=False)}

Current file code:
```cpp
{file_code}
```
"""

    response = client.models.generate_content(model=model_name, contents=prompt)
    return response.text


def analyze_file_with_ai(
    project_context: str,
    file_info: dict,
    file_code: str,
    api_key: str,
    model_name: str,
    max_retries: int,
    initial_backoff_seconds: float,
    backoff_multiplier: float,
    max_backoff_seconds: float,
) -> str:
    if genai is None:
        raise RuntimeError("google-genai is not installed")

    if not api_key:
        raise RuntimeError("Gemini API key is not set")

    client = genai.Client(api_key=api_key)
    backoff_seconds = max(0.0, initial_backoff_seconds)
    attempts = max(1, max_retries)
    last_error: Exception | None = None

    for attempt in range(1, attempts + 1):
        try:
            return analyze_file_with_ai_once(
                project_context,
                file_info,
                file_code,
                client,
                model_name,
            )
        except Exception as error:
            last_error = error
            if attempt >= attempts or not is_retryable_error(error):
                raise

            print(
                f"[retry {attempt}/{attempts - 1}] "
                f"Temporary AI error for {file_info['path']}: {error}. "
                f"Sleeping {backoff_seconds:.1f}s before retry."
            )
            time.sleep(backoff_seconds)
            backoff_seconds = min(
                max_backoff_seconds,
                max(backoff_seconds * backoff_multiplier, backoff_seconds),
            )

    if last_error is not None:
        raise last_error

    raise RuntimeError("AI analysis failed without an error object")


def analyze_project_files(
    project_analysis: dict,
    api_key: str,
    model_name: str,
    max_retries: int,
    initial_backoff_seconds: float,
    backoff_multiplier: float,
    max_backoff_seconds: float,
) -> list[dict]:
    project_context = build_project_context(project_analysis)
    results: list[dict] = []

    for file_info in project_analysis.get("files", []):
        file_path = Path(file_info["path"])
        if not file_path.exists():
            results.append(
                {
                    "path": file_info["path"],
                    "rule_based_issues": [],
                    "ai_analysis": "[ERROR]\nFile not found on disk.",
                }
            )
            continue

        file_code = file_path.read_text(encoding="utf-8")
        rule_based_issues = simple_checks(file_code)

        try:
            ai_analysis = analyze_file_with_ai(
                project_context,
                file_info,
                file_code,
                api_key,
                model_name,
                max_retries,
                initial_backoff_seconds,
                backoff_multiplier,
                max_backoff_seconds,
            )
        except Exception as error:
            ai_analysis = (
                "[ERROR]\n"
                "AI analysis unavailable.\n\n"
                f"Reason: {error}"
            )

        results.append(
            {
                "path": file_info["path"],
                "rule_based_issues": rule_based_issues,
                "ai_analysis": ai_analysis,
            }
        )

    return results


def write_output(
    output_path: Path,
    analysis_path: Path,
    project_analysis: dict,
    results: list[dict],
    model_name: str,
) -> None:
    output_path.parent.mkdir(parents=True, exist_ok=True)
    payload = {
        "root": project_analysis["root"],
        "analysis_file": str(analysis_path),
        "model": model_name,
        "results": results,
    }
    output_path.write_text(
        json.dumps(payload, indent=2, ensure_ascii=False),
        encoding="utf-8",
    )


def main() -> None:
    analysis_path = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_ANALYSIS_PATH
    output_path = Path(sys.argv[2]) if len(sys.argv) > 2 else DEFAULT_OUTPUT_PATH
    config_path = Path(sys.argv[3]) if len(sys.argv) > 3 else DEFAULT_CONFIG_PATH

    project_analysis = load_project_analysis(analysis_path)
    config = load_config(config_path)
    api_key = config.get("gemini_api_key") or os.getenv("GEMINI_API_KEY", "")
    model_name = config.get("model") or MODEL_NAME
    max_retries = int(config.get("max_retries", DEFAULT_MAX_RETRIES))
    initial_backoff_seconds = float(
        config.get("initial_backoff_seconds", DEFAULT_BACKOFF_SECONDS)
    )
    backoff_multiplier = float(
        config.get("backoff_multiplier", DEFAULT_BACKOFF_MULTIPLIER)
    )
    max_backoff_seconds = float(
        config.get("max_backoff_seconds", DEFAULT_MAX_BACKOFF_SECONDS)
    )

    results = analyze_project_files(
        project_analysis,
        api_key,
        model_name,
        max_retries,
        initial_backoff_seconds,
        backoff_multiplier,
        max_backoff_seconds,
    )
    write_output(output_path, analysis_path, project_analysis, results, model_name)

    print(f"Project analysis loaded from: {analysis_path}")
    print(f"AI config loaded from: {config_path}")
    print(f"Gemini report saved to: {output_path}")
    print(f"Files analyzed: {len(results)}")
    print(
        "Retry policy: "
        f"attempts={max_retries}, "
        f"initial_backoff={initial_backoff_seconds}s, "
        f"multiplier={backoff_multiplier}, "
        f"max_backoff={max_backoff_seconds}s"
    )


if __name__ == "__main__":
    main()
