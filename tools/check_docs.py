#!/usr/bin/env python3
"""Проверка, что документация не отстала от кода.

1. Каждый публичный класс, метод и поле из src/*.h упомянут в docs/API.md
   (внутри `кода` или блока ```...```). Исключения — tools/docs_ignore.txt.
2. Версия из library.json есть в CHANGELOG.md (заголовок "## [x.y.z]").

Запуск: python3 tools/check_docs.py   (код возврата 1 — есть расхождения)
"""
import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SRC = ROOT / "src"
API = ROOT / "docs" / "API.md"
CHANGELOG = ROOT / "CHANGELOG.md"
IGNORE = ROOT / "tools" / "docs_ignore.txt"

CLASS_RE = re.compile(r"^(?:template\s*<.*>\s*)?(?:enum\s+)?class\s+(\w+)(?:\s*<[^>]*>)?\s*(?::[^{]*)?\{")
TEMPLATE_LINE_RE = re.compile(r"^template\s*<.*>\s*$")
SECTION_RE = re.compile(r"^\s*(public|private|protected)\s*:")
# объявление метода: ... name(   — без ключевых слов управления
METHOD_RE = re.compile(r"^\s*(?:(?:virtual|static|explicit|inline|constexpr)\s+)*[\w:<>,\s\*&\(\)]*?\b(~?\w+)\s*\(")
FIELD_RE = re.compile(r"^\s*(?:(?:static|mutable|volatile)\s+)*[\w:<>,\s\*&\(\)]+?\b(\w+)\s*(?:=[^;]*|\{[^;]*\})?;\s*(?://.*)?$")
SKIP_WORDS = {"if", "for", "while", "switch", "return", "sizeof", "decltype", "operator", "static_assert",
              "using", "typedef", "template", "friend"}


def strip_angles(line):
    """Убрать аргументы шаблонов: std::function<void(int)> f; -> std::function f;"""
    prev = None
    while prev != line:
        prev = line
        line = re.sub(r"<[^<>]*>", "", line)
    return line


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def public_api(path):
    """Возвращает {(класс, имя)} публичных методов и полей верхнеуровневых классов."""
    lines = strip_comments(path.read_text(encoding="utf-8")).splitlines()
    found = set()
    depth = 0
    cls = None          # (имя, глубина тела класса, текущая секция)
    pending_template = False
    for raw in lines:
        line = raw.rstrip()
        stripped = line.strip()
        if depth == 0 and TEMPLATE_LINE_RE.match(stripped):
            pending_template = True
        if depth == 0:
            m = CLASS_RE.match(stripped)
            if m and not stripped.endswith(";"):
                name = m.group(1)
                section = "public" if stripped.startswith("enum") else "private"
                cls = [name, 1, section]
                found.add((name, name))
                is_enum = "enum class" in stripped
                if is_enum:
                    cls[2] = "enum"
            elif re.match(r"^(?:template\s*<.*>\s*)?class\s+\w+\s*;", stripped):
                pass
        if cls and depth == cls[1]:
            sm = SECTION_RE.match(line)
            if sm:
                cls[2] = sm.group(1)
            elif cls[2] == "enum":
                for item in re.findall(r"\b([A-Z]\w*)\b", stripped):
                    found.add((cls[0], item))
            elif cls[2] == "public" and stripped and not stripped.startswith(("}", "{")):
                first = stripped.split()[0] if stripped.split() else ""
                if first not in SKIP_WORDS and not stripped.startswith(("using ", "typedef ", "friend ")):
                    decl = strip_angles(line.split("{")[0] if "(" not in line.split("{")[0] else line)
                    mm = METHOD_RE.match(decl)
                    if mm and "=" not in decl.split("(")[0]:
                        name = mm.group(1)
                        if name not in SKIP_WORDS and not name.startswith("~") and not name.startswith("_"):
                            found.add((cls[0], name))
                    else:
                        fm = FIELD_RE.match(decl)
                        if fm:
                            name = fm.group(1)
                            if not name.startswith("_") and name not in SKIP_WORDS:
                                found.add((cls[0], name))
        depth += line.count("{") - line.count("}")
        if cls and depth < cls[1]:
            cls = None
        if depth == 0 and stripped.endswith(";"):
            pending_template = False
    return found


def doc_code_tokens(text):
    code = "\n".join(re.findall(r"```.*?```", text, flags=re.S))
    code += "\n" + "\n".join(re.findall(r"`([^`\n]+)`", re.sub(r"```.*?```", "", text, flags=re.S)))
    code = code.replace("&lt;", "<").replace("&gt;", ">")
    return set(re.findall(r"\w+", code))


def load_ignore():
    if not IGNORE.exists():
        return set()
    items = set()
    for line in IGNORE.read_text(encoding="utf-8").splitlines():
        line = line.split("#", 1)[0].strip()
        if line:
            items.add(line)
    return items


def main():
    errors = []

    api_text = API.read_text(encoding="utf-8")
    tokens = doc_code_tokens(api_text)
    ignore = load_ignore()
    symbols = set()
    for header in sorted(SRC.glob("*.h")):
        symbols |= public_api(header)
    missing = sorted(f"{c}::{n}" if c != n else c
                     for c, n in symbols
                     if n not in tokens and f"{c}::{n}" not in ignore and c not in ignore)
    for name in missing:
        errors.append(f"docs/API.md: не описан {name}")

    version = json.loads((ROOT / "library.json").read_text(encoding="utf-8"))["version"]
    changelog = CHANGELOG.read_text(encoding="utf-8") if CHANGELOG.exists() else ""
    if not re.search(r"^## \[" + re.escape(version) + r"\]", changelog, flags=re.M):
        errors.append(f"CHANGELOG.md: нет раздела для версии {version} из library.json (## [{version}])")

    print(f"Публичных имён в src/*.h: {len(symbols)}, в исключениях: {len(ignore)}")
    if errors:
        print("Документация отстала от кода:")
        for e in errors:
            print("  - " + e)
        print("Опишите недостающее в docs/API.md (или, если это внутренняя деталь, в tools/docs_ignore.txt с причиной).")
        return 1
    print("OK: документация соответствует коду")
    return 0


if __name__ == "__main__":
    sys.exit(main())
