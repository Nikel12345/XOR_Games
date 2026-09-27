# -*- coding: utf-8 -*-
"""
Проверка векторизации горячих циклов.

Зачем. Потеря векторизации не видна ни в коде, ни в тестах, ни на глаз в профиле:
замена тернарника на std::max стоила 3 мс из 26 и обнаружилась случайно. Но у неё
есть детерминированный признак — вердикт компилятора, и его можно проверять как тест,
без замеров и без шума.

Как. Горячий цикл помечается в исходнике макросом VEC_HOT("имя") (см. Utils.h; в
компиляцию он не попадает). Скрипт находит метки, берёт следующий за каждой цикл,
собирает проект с /Qvec-report:2 и требует, чтобы для этой строки компилятор выдал
C5001 ("векторизирован") и ни одного C5002.

Важно про "ни одного": отчёт печатается по разу на каждый контекст инлайна, и они
расходятся. У замера V14 из sandbox/GravityVecProbe.cpp было четыре отказа и один
успех — и по времени он оказался скалярным. Поэтому правило именно такое.

Запуск:
    python scripts/vec_check.py                 # собрать и проверить
    python scripts/vec_check.py --log build.log # проверить готовый лог сборки
"""
import argparse
import io
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ENGINE = os.path.dirname(ROOT)
SRC_DIRS = [os.path.join(ENGINE, "src")] + [os.path.join(ROOT, d) for d in ("game", "mygame", "sandbox")]
CMAKE = os.environ.get("CMAKE", r"G:\Cmake\bin\cmake.exe")

MARKER = re.compile(r'VEC_HOT\("([^"]+)"\)')
LOOP = re.compile(r'\b(for|while)\s*\(')
# Локаль в сообщениях любая, поэтому цепляемся за коды: C5001 - векторизирован,
# C5002 - нет (в кавычках номер причины).
VERDICT = re.compile(r'([\w.]+)\((\d+)\)\s*:\s*info\s+(C5001|C5002)(?:.*?"(\d+)")?')

SOURCE_EXT = (".cpp", ".h", ".inl")


def find_markers():
    """[(файл, строка цикла, имя метки)] по всем исходникам."""
    out = []
    for src in SRC_DIRS:
        for base, _dirs, files in os.walk(src):
            for f in files:
                if not f.endswith(SOURCE_EXT):
                    continue
                path = os.path.join(base, f)
                try:
                    lines = io.open(path, encoding="utf-8", errors="replace").read().split("\n")
                except OSError:
                    continue
                for i, line in enumerate(lines):
                    m = MARKER.search(line)
                    if not m or line.lstrip().startswith(("//", "*", "#define")):
                        continue
                    # Цикл ищем в ближайших строках после метки: между ними может стоять
                    # подъём указателей или комментарий.
                    loop_line = None
                    for k in range(i + 1, min(i + 8, len(lines))):
                        if LOOP.search(lines[k]):
                            loop_line = k + 1
                            break
                    out.append((path, loop_line, m.group(1), i + 1))
    return out


def build(log_path):
    cmd = [CMAKE, "--build", os.path.join(ROOT, "build"), "--config", "Release"]
    print("сборка:", " ".join(cmd))
    with io.open(log_path, "wb") as f:
        p = subprocess.run(cmd, stdout=f, stderr=subprocess.STDOUT)
    return p.returncode


def read_verdicts(log_path):
    """{(имя файла, строка): [коды]} из отчёта /Qvec-report:2."""
    data = io.open(log_path, "rb").read()
    text = data.decode("cp1251", errors="replace")
    res = {}
    for m in VERDICT.finditer(text):
        key = (m.group(1).lower(), int(m.group(2)))
        res.setdefault(key, []).append((m.group(3), m.group(4)))
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--log", help="готовый лог сборки; без него скрипт соберёт сам")
    args = ap.parse_args()

    log_path = args.log or os.path.join(ROOT, "build", "vec_check.log")
    if not args.log:
        rc = build(log_path)
        if rc != 0:
            print("СБОРКА УПАЛА, см.", log_path)
            return rc

    markers = find_markers()
    if not markers:
        print("меток VEC_HOT не найдено")
        return 0

    verdicts = read_verdicts(log_path)
    failed = 0
    print("\n%-28s %-34s %s" % ("метка", "файл:строка", "вердикт"))
    for path, loop_line, name, marker_line in sorted(markers, key=lambda t: (t[0], t[3])):
        rel = os.path.relpath(path, ENGINE).replace("\\", "/")
        where = "%s:%s" % (rel, loop_line if loop_line else "?")

        if loop_line is None:
            print("%-28s %-34s ЦИКЛ НЕ НАЙДЕН после метки" % (name, where))
            failed += 1
            continue

        got = verdicts.get((os.path.basename(path).lower(), loop_line))
        if not got:
            print("%-28s %-34s НЕТ ОТЧЁТА (TU собран без /Qvec-report:2?)" % (name, where))
            failed += 1
            continue

        bad = [why for code, why in got if code == "C5002"]
        if bad:
            print("%-28s %-34s НЕ ВЕКТОРИЗОВАН, причина %s" % (name, where, ", ".join(bad)))
            failed += 1
        else:
            print("%-28s %-34s ok (%d)" % (name, where, len(got)))

    print()
    if failed:
        print("ПРОВАЛ: %d из %d горячих циклов потеряли векторизацию" % (failed, len(markers)))
        return 1
    print("все %d горячих циклов векторизованы" % len(markers))
    return 0


if __name__ == "__main__":
    sys.exit(main())
