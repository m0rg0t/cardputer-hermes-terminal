#!/usr/bin/env python3
"""Set Hermes profiles to Edge and the installed local Whisper command.

Run with the Dashboard's Python (requires PyYAML). Defaults to a dry run;
--apply saves private backups before replacing configurations atomically.
Only the two speech sections are rendered again. Other configuration values
are verified unchanged, and their original source text is preserved.
"""

import argparse
from copy import deepcopy
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import re
import tempfile


def prepare(text, home, yaml):
    original = yaml.safe_load(text)
    if not isinstance(original, dict):
        raise ValueError("Expected a YAML configuration mapping")
    updated = deepcopy(original)
    for name in ("tts", "stt"):
        if updated.get(name) is None:
            updated[name] = {}
        if not isinstance(updated[name], dict):
            raise ValueError(f"Expected {name} to be a mapping")
    tts, stt = updated["tts"], updated["stt"]
    tts["provider"] = "edge"
    if tts.get("edge") is None:
        tts["edge"] = {}
    if not isinstance(tts["edge"], dict):
        raise ValueError("Expected tts.edge to be a mapping")
    tts["edge"]["voice"] = "en-US-AvaMultilingualNeural"
    stt.update(enabled=True, provider="cardputer_whisper")
    if stt.get("providers") is None:
        stt["providers"] = {}
    if not isinstance(stt["providers"], dict):
        raise ValueError("Expected stt.providers to be a mapping")
    python = home / "tools/cardputer-speech-venv/bin/python"
    wrapper = home / "tools/cardputer-speech/hermes_local_stt.py"
    # Hermes handles command placeholder quoting; installation paths must be
    # single tokens in the configured command template.
    if any(re.search(r"\s", str(path)) for path in (python, wrapper)):
        raise ValueError("Speech installation paths must not contain whitespace")
    stt["providers"]["cardputer_whisper"] = {
        "type": "command",
        "command": (f"{python} {wrapper} --input {{input_path}} --output {{output_path}} "
                    "--model {model} --language {language}"),
        "model": "base", "language": "auto", "output_format": "txt",
        "timeout_seconds": 120,
    }
    node = yaml.compose(text)
    replacements = []
    found = set()
    for key, value in node.value:
        if key.value in ("tts", "stt"):
            if key.value in found:
                raise ValueError(f"Duplicate {key.value} configuration section")
            found.add(key.value)
            # Reject flow-style roots rather than risk rewriting other fields.
            if key.start_mark.column != 0 or key.start_mark.index != text.rfind(
                    "\n", 0, key.start_mark.index) + 1:
                raise ValueError("Speech sections must be top-level block mappings")
            start = key.start_mark.index
            end = value.end_mark.index
            end_of_line = text.find("\n", end)
            if value.end_mark.column:
                end = len(text) if end_of_line < 0 else end_of_line + 1
            rendered = yaml.safe_dump({key.value: updated[key.value]},
                                      sort_keys=False, allow_unicode=True)
            replacements.append((start, end, rendered))
    result = text
    for start, end, rendered in sorted(replacements, reverse=True):
        result = result[:start] + rendered + result[end:]
    for name in ("tts", "stt"):
        if name not in found:
            result = result.rstrip() + "\n\n" + yaml.safe_dump(
                {name: updated[name]}, sort_keys=False, allow_unicode=True)
    if yaml.safe_load(result) != updated:
        raise ValueError("Refusing an ambiguous YAML rewrite")
    return result, original != updated


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--hermes-home", type=Path, default=Path("~/.hermes"))
    parser.add_argument("--profile", action="append", required=True)
    parser.add_argument("--apply", action="store_true")
    args = parser.parse_args()
    import yaml

    home = args.hermes_home.expanduser().resolve()
    python = home / "tools/cardputer-speech-venv/bin/python"
    wrapper = home / "tools/cardputer-speech/hermes_local_stt.py"
    if not python.is_file() or not os.access(python, os.X_OK) or not wrapper.is_file():
        parser.error("Install the isolated Whisper runtime and wrapper first")
    plans = []
    for profile in dict.fromkeys(args.profile):
        if not re.fullmatch(r"[A-Za-z0-9_-]+", profile):
            parser.error("Profile names must contain only letters, digits, underscores or hyphens")
        path = (home / "config.yaml" if profile == "default"
                else home / "profiles" / profile / "config.yaml")
        if path.is_symlink():
            parser.error("Refusing to replace a symlink configuration")
        text = path.read_text(encoding="utf-8")
        result, changed = prepare(text, home, yaml)
        plans.append((profile, path, text, result, changed))
    backup = None
    if args.apply and any(plan[4] for plan in plans):
        maintenance = home / "maintenance"
        maintenance.mkdir(mode=0o700, exist_ok=True)
        stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
        backup = Path(tempfile.mkdtemp(prefix=f"cardputer-speech-{stamp}-", dir=maintenance))
        for profile, path, text, _, changed in plans:
            if changed:
                with (backup / f"{profile}.config.yaml").open("x", encoding="utf-8") as file:
                    os.chmod(file.name, 0o600)
                    file.write(text)
        for _, path, text, result, changed in plans:
            if not changed:
                continue
            if path.read_text(encoding="utf-8") != text:
                raise RuntimeError("Configuration changed since preparation; backups are saved")
            fd, name = tempfile.mkstemp(prefix=".speech-config-", dir=path.parent)
            try:
                with os.fdopen(fd, "w", encoding="utf-8") as file:
                    file.write(result)
                    file.flush()
                    os.fsync(file.fileno())
                os.replace(name, path)
            finally:
                if os.path.exists(name):
                    os.unlink(name)
    for profile, _, _, _, changed in plans:
        print(json.dumps({"profile": profile, "changed": changed, "applied": args.apply,
                          "tts": "edge", "voice": "en-US-AvaMultilingualNeural",
                          "stt": "cardputer_whisper", "model": "base",
                          "backup": str(backup) if backup else None}))


if __name__ == "__main__":
    main()
