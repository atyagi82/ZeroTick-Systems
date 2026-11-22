import glob
import json
from datetime import datetime, timezone
from pathlib import Path


class DataError(ValueError):
    pass


def parse_time(value):
    if not value:
        raise DataError("event_time_utc is missing")
    normalized = value[:-1] + "+00:00" if value.endswith("Z") else value
    parsed = datetime.fromisoformat(normalized)
    if parsed.tzinfo is None:
        parsed = parsed.replace(tzinfo=timezone.utc)
    return parsed.astimezone(timezone.utc)


def resolve_inputs(patterns, root):
    paths = []
    for pattern in patterns:
        absolute = str((root / pattern).resolve())
        paths.extend(Path(item) for item in glob.glob(absolute))
    unique = sorted(set(paths))
    if not unique:
        raise DataError("No shadow JSONL files matched: " + ", ".join(patterns))
    return unique


def load_events(paths):
    events = []
    schema_versions = set()
    malformed = []
    for path in paths:
        with path.open("r", encoding="utf-8") as stream:
            for line_number, line in enumerate(stream, 1):
                if not line.strip():
                    continue
                try:
                    event = json.loads(line)
                    event["_time"] = parse_time(event.get("event_time_utc"))
                    event["_source"] = str(path)
                    event["_line"] = line_number
                    schema_versions.add(event.get("schema_version"))
                    events.append(event)
                except (json.JSONDecodeError, DataError) as error:
                    malformed.append("{}:{}: {}".format(path, line_number, error))
    if malformed:
        preview = "\n".join(malformed[:10])
        raise DataError("Malformed shadow events:\n" + preview)
    if schema_versions != {1}:
        raise DataError("Only shadow schema_version 1 is supported; found {}".format(
            sorted(str(value) for value in schema_versions)))
    events.sort(key=lambda event: (event["_time"], event.get("event_id", "")))
    return events


def session_order(events):
    first_seen = {}
    for event in events:
        session = event.get("session_id")
        if session and session not in first_seen:
            first_seen[session] = event["_time"]
    return [item[0] for item in sorted(first_seen.items(), key=lambda item: item[1])]
