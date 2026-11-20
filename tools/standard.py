#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""Writes src/data/ from the DICOM standard's own tables.

    uv run tools/standard.py [--from <folder holding part06.xml and part15.xml>]

Dicolens names tags, reads implicit files and anonymizes by three tables of
the standard, which NEMA publishes as DocBook: the data elements and the UIDs
of PS3.6, and the confidentiality profiles of PS3.15 (Table E.1-1). This
downloads the current edition and writes them as tab-separated files, sorted,
one row a line, so a new edition shows in a diff as the rows that changed.
The files are committed and compiled into the app; nothing here runs in a
build. Run it when an edition comes out, then run the tests, which pin what
the app relies on.
"""

import argparse
import re
import sys
import urllib.request
import xml.etree.ElementTree as ET
from pathlib import Path

SOURCE = "https://dicom.nema.org/medical/dicom/current/source/docbook/{0}/{0}.xml"
DOCBOOK = "{http://docbook.org/ns/docbook}"
XML_ID = "{http://www.w3.org/XML/1998/namespace}id"
DATA = Path(__file__).resolve().parent.parent / "src" / "data"

# The standard puts zero-width spaces in long words so they wrap in print.
INVISIBLE = dict.fromkeys(map(ord, "​‌‍﻿­"))


def book(part, folder):
    """A part of the standard, parsed, from a folder or from NEMA."""
    if folder:
        return ET.parse(Path(folder) / f"{part}.xml").getroot()
    with urllib.request.urlopen(SOURCE.format(part), timeout=300) as response:
        return ET.fromstring(response.read())


def edition(root):
    """The edition a part says it is, such as 2026d."""
    subtitle = root.findtext(f"{DOCBOOK}subtitle") or ""
    found = re.search(r"PS3\.\d+ (\d{4}[a-z])\b", subtitle)
    if not found:
        sys.exit(f"no edition in the subtitle: {subtitle!r}")
    return found.group(1)


def text(cell):
    """A cell's text on one line."""
    return " ".join("".join(cell.itertext()).translate(INVISIBLE).split())


def rows(root, table):
    """Every body row of a table, as its cells' texts."""
    for element in root.iter(f"{DOCBOOK}table"):
        if element.get(XML_ID) == f"table_{table}":
            body = element.find(f"{DOCBOOK}tbody")
            return [[text(cell) for cell in row.findall(f"{DOCBOOK}td")] for row in body.findall(f"{DOCBOOK}tr")]
    sys.exit(f"table {table} wasn't found; has the standard moved it?")


def pattern(tag):
    """(60xx,3000) as 60xx3000: eight hex digits, x where any digit goes."""
    found = re.fullmatch(r"\(([0-9A-Fa-fxX]{4}),([0-9A-Fa-fxX]{4})\)", tag)
    if not found:
        return None
    return "".join(c.lower() if c in "xX" else c.upper() for c in found.group(1) + found.group(2))


def elements(root):
    """PS3.6's data elements: tag, VR, R when retired, name."""
    found = {}
    for table in ("6-1", "7-1", "8-1", "9-1"):
        for cells in rows(root, table):
            tag = pattern(cells[0])
            if not tag:
                sys.exit(f"table {table}: not a tag: {cells[0]!r}")
            name, vr, note = cells[1], cells[3], cells[5] if len(cells) > 5 else ""
            # A few retired rows say "See Note" where a VR would be.
            if not re.fullmatch(r"[A-Z]{2}( or [A-Z]{2})*", vr):
                vr = ""
            if tag in found:
                sys.exit(f"table {table}: {tag} is listed twice")
            found[tag] = (tag, vr, "R" if note.startswith("RET") else "", name)
    return sorted(found.values())


def uids(root):
    """PS3.6 Table A-1: UID, what kind it is, name."""
    found = {}
    for cells in rows(root, "A-1"):
        uid, name, kind = cells[0], cells[1], cells[3]
        if not re.fullmatch(r"[0-9]+(\.[0-9]+)+", uid):
            sys.exit(f"table A-1: not a UID: {uid!r}")
        found[uid] = (uid, kind, name)
    return sorted(found.values(), key=lambda row: [int(part) for part in row[0].split(".")])


def profile(root):
    """PS3.15 Table E.1-1 in full: tag, the basic profile's action, every
    option's, whether it's retired and in a standard IOD, and its name."""
    found = {}
    for cells in rows(root, "E.1-1"):
        name, tag = cells[0], pattern(cells[1])
        if not tag:
            # Private attributes have no tag of their own, only a rule.
            if "odd" not in cells[1]:
                sys.exit(f"table E.1-1: not a tag: {cells[1]!r}")
            tag = "private"
        if len(cells) != 15:
            sys.exit(f"table E.1-1: {tag} has {len(cells)} columns, not 15")
        for action in cells[4:]:
            if not re.fullmatch(r"([XZDKCU]\*?(/[XZDKCU]\*?)*)?", action):
                sys.exit(f"table E.1-1: {tag} has an action this doesn't know: {action!r}")
        if tag in found:
            sys.exit(f"table E.1-1: {tag} is listed twice")
        found[tag] = (tag, *cells[4:], cells[2], cells[3], name)
    return sorted(found.values())


def write(name, header, table):
    lines = [f"# {line}" for line in header] + ["\t".join(row) for row in table]
    for line in lines:
        if "\n" in line or (not line.startswith("#") and line.count("\t") != lines[-1].count("\t")):
            sys.exit(f"{name}: a row doesn't fit its columns: {line!r}")
    DATA.mkdir(parents=True, exist_ok=True)
    (DATA / name).write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"{name}: {len(table)} rows")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--from", dest="folder", help="read part06.xml and part15.xml from this folder instead of downloading")
    arguments = parser.parse_args()

    part6, part15 = book("part06", arguments.folder), book("part15", arguments.folder)
    write("elements.tsv",
          [f"DICOM PS3.6 {edition(part6)}, Tables 6-1, 7-1, 8-1 and 9-1: the data elements. Written by tools/standard.py.",
           "tag (x: any digit)\tVR\tR when retired\tname"],
          elements(part6))
    write("uids.tsv",
          [f"DICOM PS3.6 {edition(part6)}, Table A-1: the UIDs. Written by tools/standard.py.",
           "UID\tkind\tname"],
          uids(part6))
    write("confidentiality.tsv",
          [f"DICOM PS3.15 {edition(part15)}, Table E.1-1: what each confidentiality profile does to an attribute. Written by tools/standard.py.",
           "tag (x: any digit; private: every odd group)\tBasic Profile\tRetain Safe Private\tRetain UIDs\tRetain Device Identity"
           "\tRetain Institution Identity\tRetain Patient Characteristics\tRetain Longitudinal Full Dates"
           "\tRetain Longitudinal Modified Dates\tClean Descriptors\tClean Structured Content\tClean Graphics"
           "\tretired\tin a standard IOD\tname"],
          profile(part15))


if __name__ == "__main__":
    main()
