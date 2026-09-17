"""Fallout dialogue indexing shared by the voice-cast and renderer tools."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
import re


@dataclass(frozen=True)
class ScriptEntry:
    list_id: int
    name: str
    comment: str


@dataclass(frozen=True)
class DialogRecord:
    list_id: int
    message_list_id: int
    message_id: int
    script: str
    comment: str
    audio: str
    text: str
    roles: frozenset[str]


@dataclass(frozen=True)
class RoleReference:
    speaker_list_id: int
    message_list_id: int
    message_id: int
    roles: frozenset[str]
    text: str | None = None


def load_fields(path: Path) -> list[tuple[int, str, str]]:
    data = path.read_text(encoding="cp1252")
    cursor = 0
    fields: list[str] = []
    while True:
        start = data.find("{", cursor)
        if start == -1:
            break
        end = data.find("}", start + 1)
        if end == -1:
            break
        fields.append(data[start + 1 : end].replace("\n", ""))
        cursor = end + 1

    entries = []
    for offset in range(0, len(fields) - 2, 3):
        try:
            number = int(fields[offset].strip())
        except ValueError:
            continue
        entries.append((number, fields[offset + 1].strip(), fields[offset + 2]))
    return entries


def load_scripts(path: Path) -> list[ScriptEntry]:
    result = []
    for list_id, raw in enumerate(path.read_text(encoding="cp1252").splitlines(), 1):
        code, _, comment = raw.partition(";")
        token = code.strip().split(maxsplit=1)
        if not token:
            result.append(ScriptEntry(list_id, "", comment.strip()))
            continue
        result.append(ScriptEntry(list_id, Path(token[0]).stem.upper(), comment.strip()))
    return result


def _call_arguments(source: str, name: str):
    pattern = re.compile(rf"\b{re.escape(name)}\s*\(", re.IGNORECASE)
    for match in pattern.finditer(source):
        start = match.end()
        depth = 1
        quote = None
        escaped = False
        for cursor in range(start, len(source)):
            character = source[cursor]
            if quote is not None:
                if escaped:
                    escaped = False
                elif character == "\\":
                    escaped = True
                elif character == quote:
                    quote = None
                continue
            if character in {'"', "'"}:
                quote = character
            elif character == "(":
                depth += 1
            elif character == ")":
                depth -= 1
                if depth == 0:
                    yield source[start:cursor]
                    break


def _split_arguments(arguments: str) -> list[str]:
    result = []
    start = 0
    depth = 0
    quote = None
    escaped = False
    for cursor, character in enumerate(arguments):
        if quote is not None:
            if escaped:
                escaped = False
            elif character == "\\":
                escaped = True
            elif character == quote:
                quote = None
            continue
        if character in {'"', "'"}:
            quote = character
        elif character == "(":
            depth += 1
        elif character == ")":
            depth -= 1
        elif character == "," and depth == 0:
            result.append(arguments[start:cursor].strip())
            start = cursor + 1
    result.append(arguments[start:].strip())
    return result


def _split_concatenation(expression: str) -> list[str]:
    result = []
    start = 0
    depth = 0
    quote = None
    escaped = False
    for cursor, character in enumerate(expression):
        if quote is not None:
            if escaped:
                escaped = False
            elif character == "\\":
                escaped = True
            elif character == quote:
                quote = None
            continue
        if character in {'"', "'"}:
            quote = character
        elif character == "(":
            depth += 1
        elif character == ")":
            depth -= 1
        elif character == "+" and depth == 0:
            result.append(expression[start:cursor].strip())
            start = cursor + 1
    result.append(expression[start:].strip())
    return result


def _player_name_text(
    expression: str,
    script_list_id: int,
    message_texts: dict[tuple[int, int], str],
) -> str | None:
    name_pattern = re.compile(
        r"proto_data\s*\(\s*obj_pid\s*\(\s*dude_obj\s*\)\s*,\s*1\s*\)",
        re.IGNORECASE,
    )
    if name_pattern.search(expression) is None:
        return None

    pieces = []
    for term in _split_concatenation(expression):
        if name_pattern.fullmatch(term):
            pieces.append("Vault Dweller")
            continue
        if len(term) >= 2 and term[0] == term[-1] and term[0] in {'"', "'"}:
            pieces.append(term[1:-1].replace(r"\"", '"').replace(r"\\", "\\"))
            continue
        message_calls = list(_call_arguments(term, "message_str"))
        if len(message_calls) == 1:
            fields = _split_arguments(message_calls[0])
            if (
                len(fields) >= 2
                and re.fullmatch(r"\d+", fields[0])
                and re.fullmatch(r"\d+", fields[1])
            ):
                message_list_id = int(fields[0]) or script_list_id
                text = message_texts.get((message_list_id, int(fields[1])))
                if text is not None:
                    pieces.append(text)
                    continue
        return None
    return "".join(pieces)


def _player_name_roles(
    source: str,
    script_list_id: int,
    message_texts: dict[tuple[int, int], str],
) -> list[RoleReference]:
    references = []
    calls = {
        "gsay_reply": ("npc", 1),
        "gsay_message": ("npc", 1),
        "gdialog_reply": ("npc", 2),
        "sayReply": ("npc", 1),
        "giq_option": ("player", 2),
        "gsay_option": ("player", 1),
        "gdialog_option": ("player", 1),
        "sayOption": ("player", 0),
    }
    for function, (role, expression_index) in calls.items():
        for arguments in _call_arguments(source, function):
            fields = _split_arguments(arguments)
            if expression_index >= len(fields):
                continue
            text = _player_name_text(fields[expression_index], script_list_id, message_texts)
            if text:
                references.append(
                    RoleReference(
                        script_list_id,
                        script_list_id,
                        -1,
                        frozenset({role, "player_name"}),
                        text,
                    )
                )

    for arguments in _call_arguments(source, "float_msg"):
        fields = _split_arguments(arguments)
        if len(fields) < 2:
            continue
        text = _player_name_text(fields[1], script_list_id, message_texts)
        if text:
            role = "player" if fields[0].strip().lower() == "dude_obj" else "npc"
            references.append(
                RoleReference(
                    script_list_id,
                    script_list_id,
                    -1,
                    frozenset({role, "floating", "player_name"}),
                    text,
                )
            )

    assignment_pattern = re.compile(
        r"\b[A-Za-z_]\w*\s*:=\s*([^;]*proto_data\s*\(\s*obj_pid\s*\(\s*dude_obj\s*\)\s*,\s*1\s*\)[^;]*);",
        re.IGNORECASE,
    )
    for expression in assignment_pattern.findall(source):
        text = _player_name_text(expression, script_list_id, message_texts)
        if text:
            references.append(
                RoleReference(
                    script_list_id,
                    script_list_id,
                    -1,
                    frozenset({"npc", "player_name"}),
                    text,
                )
            )

    chained_assignment_pattern = re.compile(
        r"\b(?P<variable>[A-Za-z_]\w*)\s*:=\s*(?P<prefix>[^;]+);\s*"
        r"(?P=variable)\s*:=\s*(?P=variable)\s*\+\s*"
        r"(?P<name>proto_data\s*\(\s*obj_pid\s*\(\s*dude_obj\s*\)\s*,\s*1\s*\))\s*;\s*"
        r"(?P=variable)\s*:=\s*(?P=variable)\s*\+\s*(?P<suffix>[^;]+);",
        re.IGNORECASE,
    )
    for match in chained_assignment_pattern.finditer(source):
        expression = " + ".join(
            (match.group("prefix"), match.group("name"), match.group("suffix"))
        )
        text = _player_name_text(expression, script_list_id, message_texts)
        if text:
            references.append(
                RoleReference(
                    script_list_id,
                    script_list_id,
                    -1,
                    frozenset({"npc", "player_name"}),
                    text,
                )
            )
    return references


def _static_message_ids(expression: str) -> set[int] | None:
    expression = expression.strip()
    if re.fullmatch(r"\d+", expression):
        return {int(expression)}

    random_match = re.fullmatch(r"random\s*\(\s*(\d+)\s*,\s*(\d+)\s*\)", expression, re.IGNORECASE)
    if random_match:
        first, last = (int(value) for value in random_match.groups())
        return set(range(min(first, last), max(first, last) + 1))

    offset_random_match = re.fullmatch(
        r"(\d+)\s*\+\s*random\s*\(\s*(\d+)\s*,\s*(\d+)\s*\)",
        expression,
        re.IGNORECASE,
    )
    if offset_random_match:
        offset, first, last = (int(value) for value in offset_random_match.groups())
        return set(range(offset + min(first, last), offset + max(first, last) + 1))

    return None


def _message_references(
    expression: str,
    available_messages: dict[int, set[int]],
) -> list[tuple[int, int]]:
    references = []
    for message_arguments in _call_arguments(expression, "message_str"):
        fields = _split_arguments(message_arguments)
        if len(fields) < 2 or not re.fullmatch(r"\d+", fields[0]):
            continue
        message_list_id = int(fields[0])
        message_ids = _static_message_ids(fields[1])
        if message_ids is None:
            message_ids = available_messages.get(message_list_id, set())
        references.extend((message_list_id, message_id) for message_id in message_ids)
    return references


def _floating_roles(
    source: str,
    script_list_id: int,
    player_script: bool,
    available_messages: dict[int, set[int]],
) -> list[RoleReference]:
    references = []
    for arguments in _call_arguments(source, "float_msg"):
        float_arguments = _split_arguments(arguments)
        if len(float_arguments) < 2:
            continue
        owner = float_arguments[0].strip().lower()
        role = "player" if owner == "dude_obj" or (owner == "self_obj" and player_script) else "npc"
        message_references = _message_references(float_arguments[1], available_messages)
        if not message_references and re.fullmatch(r"[A-Za-z_]\w*", float_arguments[1]):
            variable = re.escape(float_arguments[1])
            for assignment in re.findall(rf"\b{variable}\s*:=\s*([^;]+);", source, re.IGNORECASE):
                message_references.extend(_message_references(assignment, available_messages))

        for message_list_id, message_id in message_references:
            if role == "player" or owner == "self_obj":
                speaker_list_id = script_list_id
            else:
                # Named objects normally use their own message list. The runtime
                # still resolves the object's actual script before playback.
                speaker_list_id = message_list_id

            references.append(
                RoleReference(
                    speaker_list_id,
                    message_list_id,
                    message_id,
                    frozenset({role, "floating"}),
                )
            )
    return references


def load_roles(
    ssl_dir: Path,
    scripts: dict[int, ScriptEntry],
    available_messages: dict[int, set[int]],
    message_texts: dict[tuple[int, int], str],
) -> list[RoleReference]:
    roles: dict[tuple[int, int], set[str]] = {}
    patterns = {
        "npc": (
            r"gsay_(?:reply|message)\(\s*(\d+)\s*,\s*(\d+)",
            r"gdialog_reply\([^,]*,\s*(\d+)\s*,\s*(\d+)",
            r"gsay_(?:reply|message)\(\s*\d+\s*,\s*message_str\(\s*(\d+)\s*,\s*(\d+)\s*\)\s*(?:,|\))",
        ),
        "player": (
            r"giq_option\([^,]*,\s*(\d+)\s*,\s*(\d+)",
            r"gdialog_option\(\s*(\d+)\s*,\s*(\d+)",
            r"giq_option\([^,]*,\s*\d+\s*,\s*message_str\(\s*(\d+)\s*,\s*(\d+)\s*\)\s*,",
        ),
    }
    floating = []
    scripts_by_name = {entry.name: entry for entry in scripts.values()}
    for path in ssl_dir.glob("*.ssl"):
        source = path.read_text(encoding="cp1252", errors="replace")
        for role, expressions in patterns.items():
            for expression in expressions:
                for list_id, message_id in re.findall(expression, source, re.IGNORECASE):
                    key = (int(list_id), int(message_id))
                    if key[0] > 0:
                        roles.setdefault(key, set()).add(role)

        for role, expression in (
            ("npc", r"gsay_(?:reply|message)\(\s*(\d+)\s*,\s*random\(\s*(\d+)\s*,\s*(\d+)\s*\)"),
            ("npc", r"gsay_(?:reply|message)\(\s*\d+\s*,\s*message_str\(\s*(\d+)\s*,\s*random\(\s*(\d+)\s*,\s*(\d+)\s*\)"),
        ):
            for list_id, first, last in re.findall(expression, source, re.IGNORECASE):
                if int(list_id) > 0:
                    for message_id in range(int(first), int(last) + 1):
                        roles.setdefault((int(list_id), message_id), set()).add(role)

        script = scripts_by_name.get(path.stem.upper())
        if script is not None:
            if not source.strip():
                # A few original scripts cannot be decompiled. Their message
                # files still contain both sides of the conversation, so render
                # each line for both roles. Runtime lookup selects the correct
                # speaker path, and recorded NPC lines remain preferred.
                for message_id in available_messages.get(script.list_id, set()):
                    floating.append(
                        RoleReference(
                            script.list_id,
                            script.list_id,
                            message_id,
                            frozenset({"npc", "player", "unclassified"}),
                        )
                    )
                continue
            floating.extend(_player_name_roles(source, script.list_id, message_texts))
            floating.extend(
                _floating_roles(
                    source,
                    script.list_id,
                    script.name == "OBJ_DUDE",
                    available_messages,
                )
            )

    references = [
        RoleReference(list_id, list_id, message_id, frozenset(entry_roles))
        for (list_id, message_id), entry_roles in roles.items()
    ]
    references.extend(floating)
    return references


def load_dialogue(
    source: Path,
    patch_source: Path | None,
    scripts_list: Path,
    ssl_dir: Path,
) -> list[DialogRecord]:
    scripts = {entry.list_id: entry for entry in load_scripts(scripts_list) if entry.name}
    messages_by_name: dict[tuple[str, int], tuple[str, str]] = {}
    for directory in (source, patch_source):
        if directory is None or not directory.exists():
            continue
        for path in sorted(directory.glob("*.MSG")):
            for message_id, audio, text in load_fields(path):
                messages_by_name[(path.stem.upper(), message_id)] = (audio, text)

    messages: dict[tuple[int, int], tuple[str, str]] = {}
    available_messages: dict[int, set[int]] = {}
    list_ids_by_name: dict[str, list[int]] = {}
    for list_id, script in scripts.items():
        list_ids_by_name.setdefault(script.name, []).append(list_id)
    for (name, message_id), message in messages_by_name.items():
        for list_id in list_ids_by_name.get(name, []):
            messages[(list_id, message_id)] = message
            available_messages.setdefault(list_id, set()).add(message_id)

    message_texts = {key: message[1] for key, message in messages.items()}
    roles = load_roles(ssl_dir, scripts, available_messages, message_texts)

    records_by_key = {}
    for reference in roles:
        script = scripts.get(reference.speaker_list_id)
        if script is None:
            continue
        message = (
            ("", reference.text)
            if reference.text is not None
            else messages.get((reference.message_list_id, reference.message_id))
        )
        if message is None or not message[1].strip():
            continue
        key = (reference.speaker_list_id, reference.message_list_id, reference.message_id, message[1])
        existing = records_by_key.get(key)
        entry_roles = reference.roles if existing is None else existing.roles | reference.roles
        records_by_key[key] = DialogRecord(
            reference.speaker_list_id,
            reference.message_list_id,
            reference.message_id,
            script.name,
            script.comment,
            message[0],
            message[1],
            frozenset(entry_roles),
        )
    return sorted(
        records_by_key.values(),
        key=lambda record: (record.list_id, record.message_list_id, record.message_id, record.text),
    )
