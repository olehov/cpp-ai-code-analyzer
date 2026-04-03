import hashlib
import json
import os
import sys
import threading
import time
from collections import deque
from concurrent.futures import ThreadPoolExecutor, as_completed
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
DEFAULT_STATE_PATH = Path("tmp/ai_run_state.json")
DEFAULT_MAX_REQUESTS_PER_MINUTE = 15
DEFAULT_MIN_RUN_INTERVAL_SECONDS = 120.0
DEFAULT_MAX_CONCURRENT_REQUESTS = 3
DEFAULT_MAX_RETRIES = 3
DEFAULT_BACKOFF_SECONDS = 2.0
DEFAULT_BACKOFF_MULTIPLIER = 2.0
DEFAULT_MAX_BACKOFF_SECONDS = 15.0


class RequestLimiter:
    def __init__(self, max_requests_per_minute: int) -> None:
        self.max_requests_per_minute = max_requests_per_minute
        self.request_timestamps: deque[float] = deque()
        self.lock = threading.Lock()

    def wait_for_slot(self) -> None:
        if self.max_requests_per_minute <= 0:
            return

        while True:
            sleep_seconds = 0.0
            with self.lock:
                now = time.monotonic()
                while self.request_timestamps and now - self.request_timestamps[0] >= 60.0:
                    self.request_timestamps.popleft()

                if len(self.request_timestamps) < self.max_requests_per_minute:
                    self.request_timestamps.append(now)
                    return

                sleep_seconds = 60.0 - (now - self.request_timestamps[0])

            if sleep_seconds > 0:
                print(
                    "[rate-limit] "
                    f"Reached {self.max_requests_per_minute} AI requests per minute. "
                    f"Sleeping {sleep_seconds:.1f}s before the next request.",
                    flush=True,
                )
                time.sleep(sleep_seconds)


class ThreadLocalAiClients:
    def __init__(self, api_key: str) -> None:
        self.api_key = api_key
        self.local = threading.local()

    def get_client(self) -> Any:
        client = getattr(self.local, "client", None)
        if client is None:
            client = create_ai_client(self.api_key)
            self.local.client = client
        return client


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


def compute_file_hash(content: str) -> str:
    return hashlib.sha256(content.encode("utf-8")).hexdigest()


def load_previous_report(output_path: Path) -> tuple[dict[str, dict], float | None]:
    if not output_path.exists():
        return {}, None

    try:
        payload = json.loads(output_path.read_text(encoding="utf-8"))
        report_mtime = output_path.stat().st_mtime
    except (OSError, json.JSONDecodeError):
        return {}, None

    results = payload.get("results", [])
    if not isinstance(results, list):
        return {}, report_mtime

    previous_results: dict[str, dict] = {}
    for result in results:
        if not isinstance(result, dict):
            continue

        path = result.get("path")
        if isinstance(path, str):
            previous_results[path] = result

    return previous_results, report_mtime


def load_run_state(state_path: Path) -> dict:
    if not state_path.exists():
        return {}

    try:
        return json.loads(state_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return {}


def save_run_state(state_path: Path, last_ai_run_started_at: float) -> None:
    state_path.parent.mkdir(parents=True, exist_ok=True)
    payload = {
        "last_ai_run_started_at": last_ai_run_started_at,
    }
    state_path.write_text(json.dumps(payload, indent=2), encoding="utf-8")


def get_cooldown_remaining_seconds(
    state_path: Path,
    min_run_interval_seconds: float,
) -> float:
    if min_run_interval_seconds <= 0:
        return 0.0

    state = load_run_state(state_path)
    last_ai_run_started_at = state.get("last_ai_run_started_at")
    if not isinstance(last_ai_run_started_at, int | float):
        return 0.0

    elapsed = time.time() - float(last_ai_run_started_at)
    return max(0.0, min_run_interval_seconds - elapsed)


def is_error_like_analysis(previous_result: dict) -> bool:
    ai_status = previous_result.get("ai_status", "")
    ai_analysis = previous_result.get("ai_analysis", "")
    return ai_status in {"error", "skipped"} or (
        isinstance(ai_analysis, str) and ai_analysis.startswith("[ERROR]")
    )


def can_reuse_cached_result(
    previous_result: dict | None,
    file_path: Path,
    file_hash: str,
    report_mtime: float | None,
) -> bool:
    if previous_result is None or is_error_like_analysis(previous_result):
        return False

    cached_file_hash = previous_result.get("file_hash")
    if isinstance(cached_file_hash, str):
        return cached_file_hash == file_hash

    if report_mtime is None:
        return False

    try:
        return file_path.stat().st_mtime <= report_mtime
    except OSError:
        return False


def build_cooldown_message(cooldown_remaining_seconds: float) -> str:
    remaining_seconds = max(1, int(round(cooldown_remaining_seconds)))
    return (
        "[SKIPPED]\n"
        "AI analysis was skipped because the cooldown window is still active.\n\n"
        f"Next AI run is available in about {remaining_seconds} seconds."
    )


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


def create_ai_client(api_key: str) -> Any:
    if genai is None:
        raise RuntimeError("google-genai is not installed")

    if not api_key:
        raise RuntimeError("Gemini API key is not set")

    return genai.Client(api_key=api_key)


def create_result_record(
    path: str,
    rule_based_issues: list[str],
    ai_status: str,
    ai_analysis: str,
    file_hash: str,
) -> dict:
    return {
        "path": path,
        "rule_based_issues": rule_based_issues,
        "ai_status": ai_status,
        "ai_analysis": ai_analysis,
        "file_hash": file_hash,
    }


def order_results(ordered_paths: list[str], results_by_path: dict[str, dict]) -> list[dict]:
    return [results_by_path[path] for path in ordered_paths if path in results_by_path]


def print_progress(
    completed_files: int,
    total_files: int,
    path: str,
    ai_status: str,
) -> None:
    print(
        f"[progress {completed_files}/{total_files}] {ai_status.upper()} {path}",
        flush=True,
    )


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

Current parser status:
{file_info.get("parse_error") or "Parser completed without file-level errors."}

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
    client_provider: ThreadLocalAiClients,
    model_name: str,
    request_limiter: RequestLimiter,
    max_retries: int,
    initial_backoff_seconds: float,
    backoff_multiplier: float,
    max_backoff_seconds: float,
) -> str:
    backoff_seconds = max(0.0, initial_backoff_seconds)
    attempts = max(1, max_retries)
    last_error: Exception | None = None

    for attempt in range(1, attempts + 1):
        try:
            request_limiter.wait_for_slot()
            client = client_provider.get_client()
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
                f"Sleeping {backoff_seconds:.1f}s before retry.",
                flush=True,
            )
            time.sleep(backoff_seconds)
            backoff_seconds = min(
                max_backoff_seconds,
                max(backoff_seconds * backoff_multiplier, backoff_seconds),
            )

    if last_error is not None:
        raise last_error

    raise RuntimeError("AI analysis failed without an error object")


def write_output(
    output_path: Path,
    analysis_path: Path,
    project_analysis: dict,
    results: list[dict],
    model_name: str,
    stats: dict[str, int],
) -> None:
    output_path.parent.mkdir(parents=True, exist_ok=True)
    payload = {
        "root": project_analysis["root"],
        "analysis_file": str(analysis_path),
        "model": model_name,
        "generated_at_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "stats": stats,
        "results": results,
    }
    output_path.write_text(
        json.dumps(payload, indent=2, ensure_ascii=False),
        encoding="utf-8",
    )


def analyze_project_files(
    project_analysis: dict,
    analysis_path: Path,
    api_key: str,
    model_name: str,
    output_path: Path,
    state_path: Path,
    max_requests_per_minute: int,
    min_run_interval_seconds: float,
    max_concurrent_requests: int,
    max_retries: int,
    initial_backoff_seconds: float,
    backoff_multiplier: float,
    max_backoff_seconds: float,
) -> tuple[list[dict], dict[str, int], float]:
    project_context = build_project_context(project_analysis)
    previous_results, report_mtime = load_previous_report(output_path)
    cooldown_remaining_seconds = get_cooldown_remaining_seconds(
        state_path,
        min_run_interval_seconds,
    )
    allow_new_ai_requests = cooldown_remaining_seconds <= 0.0

    results_by_path: dict[str, dict] = {}
    ordered_paths: list[str] = []
    pending_ai_jobs: list[dict[str, Any]] = []

    stats = {
        "generated": 0,
        "cached": 0,
        "skipped": 0,
        "errors": 0,
    }

    total_files = len(project_analysis.get("files", []))
    completed_files = 0

    for file_info in project_analysis.get("files", []):
        path = file_info["path"]
        ordered_paths.append(path)
        file_path = Path(path)

        if not file_path.exists():
            stats["errors"] += 1
            completed_files += 1
            results_by_path[path] = create_result_record(
                path,
                [],
                "error",
                "[ERROR]\nFile not found on disk.",
                "",
            )
            print_progress(completed_files, total_files, path, "error")
            write_output(
                output_path,
                analysis_path,
                project_analysis,
                order_results(ordered_paths, results_by_path),
                model_name,
                stats,
            )
            continue

        file_code = file_path.read_text(encoding="utf-8")
        file_hash = compute_file_hash(file_code)
        rule_based_issues = simple_checks(file_code)
        previous_result = previous_results.get(path)

        if can_reuse_cached_result(previous_result, file_path, file_hash, report_mtime):
            stats["cached"] += 1
            completed_files += 1
            results_by_path[path] = create_result_record(
                path,
                rule_based_issues,
                "cached",
                str(previous_result.get("ai_analysis", "")),
                file_hash,
            )
            print_progress(completed_files, total_files, path, "cached")
            write_output(
                output_path,
                analysis_path,
                project_analysis,
                order_results(ordered_paths, results_by_path),
                model_name,
                stats,
            )
            continue

        if not allow_new_ai_requests:
            stats["skipped"] += 1
            completed_files += 1
            results_by_path[path] = create_result_record(
                path,
                rule_based_issues,
                "skipped",
                build_cooldown_message(cooldown_remaining_seconds),
                file_hash,
            )
            print_progress(completed_files, total_files, path, "skipped")
            write_output(
                output_path,
                analysis_path,
                project_analysis,
                order_results(ordered_paths, results_by_path),
                model_name,
                stats,
            )
            continue

        pending_ai_jobs.append(
            {
                "path": path,
                "file_info": file_info,
                "file_code": file_code,
                "file_hash": file_hash,
                "rule_based_issues": rule_based_issues,
            }
        )

    if pending_ai_jobs:
        try:
            create_ai_client(api_key)
            client_provider = ThreadLocalAiClients(api_key)
        except Exception as error:
            for job in pending_ai_jobs:
                stats["errors"] += 1
                completed_files += 1
                results_by_path[job["path"]] = create_result_record(
                    job["path"],
                    job["rule_based_issues"],
                    "error",
                    "[ERROR]\n"
                    "AI analysis unavailable.\n\n"
                    f"Reason: {error}",
                    job["file_hash"],
                )
                print_progress(completed_files, total_files, job["path"], "error")
                write_output(
                    output_path,
                    analysis_path,
                    project_analysis,
                    order_results(ordered_paths, results_by_path),
                    model_name,
                    stats,
                )
        else:
            request_limiter = RequestLimiter(max_requests_per_minute)
            save_run_state(state_path, time.time())

            max_workers = max(1, min(max_concurrent_requests, len(pending_ai_jobs)))
            print(
                f"[parallel] Starting {len(pending_ai_jobs)} AI jobs with up to {max_workers} worker threads.",
                flush=True,
            )

            with ThreadPoolExecutor(max_workers=max_workers) as executor:
                future_to_job = {
                    executor.submit(
                        analyze_file_with_ai,
                        project_context,
                        job["file_info"],
                        job["file_code"],
                        client_provider,
                        model_name,
                        request_limiter,
                        max_retries,
                        initial_backoff_seconds,
                        backoff_multiplier,
                        max_backoff_seconds,
                    ): job
                    for job in pending_ai_jobs
                }

                for future in as_completed(future_to_job):
                    job = future_to_job[future]
                    try:
                        ai_analysis = future.result()
                        ai_status = "generated"
                        stats["generated"] += 1
                    except Exception as error:
                        ai_status = "error"
                        ai_analysis = (
                            "[ERROR]\n"
                            "AI analysis unavailable.\n\n"
                            f"Reason: {error}"
                        )
                        stats["errors"] += 1

                    completed_files += 1
                    results_by_path[job["path"]] = create_result_record(
                        job["path"],
                        job["rule_based_issues"],
                        ai_status,
                        ai_analysis,
                        job["file_hash"],
                    )
                    print_progress(completed_files, total_files, job["path"], ai_status)
                    write_output(
                        output_path,
                        analysis_path,
                        project_analysis,
                        order_results(ordered_paths, results_by_path),
                        model_name,
                        stats,
                    )

    return order_results(ordered_paths, results_by_path), stats, cooldown_remaining_seconds


def main() -> None:
    analysis_path = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_ANALYSIS_PATH
    output_path = Path(sys.argv[2]) if len(sys.argv) > 2 else DEFAULT_OUTPUT_PATH
    config_path = Path(sys.argv[3]) if len(sys.argv) > 3 else DEFAULT_CONFIG_PATH
    state_path = DEFAULT_STATE_PATH

    project_analysis = load_project_analysis(analysis_path)
    config = load_config(config_path)
    api_key = config.get("gemini_api_key") or os.getenv("GEMINI_API_KEY", "")
    model_name = config.get("model") or MODEL_NAME
    max_requests_per_minute = int(
        config.get("max_requests_per_minute", DEFAULT_MAX_REQUESTS_PER_MINUTE)
    )
    min_run_interval_seconds = float(
        config.get("min_run_interval_seconds", DEFAULT_MIN_RUN_INTERVAL_SECONDS)
    )
    max_concurrent_requests = int(
        config.get("max_concurrent_requests", DEFAULT_MAX_CONCURRENT_REQUESTS)
    )
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

    results, stats, cooldown_remaining_seconds = analyze_project_files(
        project_analysis,
        analysis_path,
        api_key,
        model_name,
        output_path,
        state_path,
        max_requests_per_minute,
        min_run_interval_seconds,
        max_concurrent_requests,
        max_retries,
        initial_backoff_seconds,
        backoff_multiplier,
        max_backoff_seconds,
    )
    write_output(
        output_path,
        analysis_path,
        project_analysis,
        results,
        model_name,
        stats,
    )

    print(f"Project analysis loaded from: {analysis_path}")
    print(f"AI config loaded from: {config_path}")
    print(f"Gemini report saved to: {output_path}")
    print(f"Files analyzed: {len(results)}")
    print(f"Generated AI results: {stats['generated']}")
    print(f"Reused cached results: {stats['cached']}")
    print(f"Skipped by cooldown: {stats['skipped']}")
    print(f"AI errors: {stats['errors']}")
    print(f"Max AI requests per minute: {max_requests_per_minute}")
    print(f"Max concurrent AI requests: {max_concurrent_requests}")
    print(f"Minimum time between AI runs: {min_run_interval_seconds:.0f}s")
    if cooldown_remaining_seconds > 0:
        print(
            "Cooldown status: active "
            f"({int(round(cooldown_remaining_seconds))}s remaining at start of run)"
        )
    print(
        "Retry policy: "
        f"attempts={max_retries}, "
        f"initial_backoff={initial_backoff_seconds}s, "
        f"multiplier={backoff_multiplier}, "
        f"max_backoff={max_backoff_seconds}s"
    )


if __name__ == "__main__":
    main()
