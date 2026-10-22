#!/usr/bin/env python3

import argparse
import csv
import html
import re
import shutil
import subprocess
from pathlib import Path


def normalize_accession(value: str) -> str:
    value = (value or "").strip()
    if not value:
        return ""
    if "/" in value:
        return value
    parts = value.split("_", 1)
    if len(parts) == 2 and parts[0].startswith("10."):
        return parts[0] + "/" + parts[1]
    return value


def load_csv_metadata(metadata_dir: Path, accession: str):
    if not metadata_dir.is_dir():
        return {}
    normalized = normalize_accession(accession)
    for csv_path in sorted(metadata_dir.glob("*.csv")):
        with csv_path.open(newline="") as handle:
            reader = csv.DictReader(handle)
            for row in reader:
                doi = normalize_accession(row.get("doi", ""))
                if doi == normalized:
                    return {
                        "author": html.unescape((row.get("authors") or "").strip()),
                        "title": html.unescape((row.get("title") or "").strip()),
                        "journal": html.unescape((row.get("journal") or "").strip()),
                        "year": html.unescape((row.get("year") or "").strip()),
                        "abstract": html.unescape((row.get("abstract") or "").strip()),
                        "pubmed_id": html.unescape((row.get("pubmed_id") or "").strip()),
                    }
    return {}


def run_text_command(command):
    if not shutil.which(command[0]):
        return ""
    proc = subprocess.run(
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        universal_newlines=True,
        # pdfinfo can emit non-UTF-8 bytes (e.g. a garbage CreationDate);
        # don't let that crash the whole .bib.
        encoding="utf-8",
        errors="replace",
        check=False,
    )
    if proc.returncode != 0:
        return ""
    return proc.stdout


def pdf_first_page_text(pdf_path: Path) -> str:
    return run_text_command(["pdftotext", "-f", "1", "-l", "1", str(pdf_path), "-"])


def pdf_info(pdf_path: Path) -> str:
    return run_text_command(["pdfinfo", str(pdf_path)])


def parse_pdf_info(info_text: str):
    metadata = {}
    for line in info_text.splitlines():
        if ":" not in line:
            continue
        key, value = line.split(":", 1)
        metadata[key.strip().lower()] = value.strip()
    return metadata


def parse_lines(text: str):
    return [line.strip() for line in text.splitlines() if line.strip()]


def is_noise_line(line: str) -> bool:
    lower = line.lower().strip()
    prefixes = (
        "abstract",
        "chapter ",
        "keywords",
        "introduction",
        "citation:",
        "editor:",
        "edited by:",
        "reviewed by:",
        "received:",
        "accepted:",
        "published:",
        "copyright",
        "open access",
        "specialty section:",
        "funding:",
        "data availability",
        "supplementary information",
        "doi:",
    )
    if lower.startswith(prefixes):
        return True
    if lower in {"1 introduction", "introduction"}:
        return True
    if "creativecommons" in lower or "front. plant sci." in lower or "plos one |" in lower:
        return True
    return False


def looks_like_affiliation_line(line: str) -> bool:
    lower = line.lower()
    if not line:
        return False
    if re.match(r"^\d+\s", line):
        return True
    affiliation_terms = (
        "department",
        "university",
        "institute",
        "school",
        "college",
        "laboratory",
        "centre",
        "center",
        "academy",
        "research service",
        "addis ababa",
        "united states",
        "ethiopia",
    )
    return any(term in lower for term in affiliation_terms)


def clean_author_line(line: str) -> str:
    if not line:
        return ""
    cleaned = html.unescape(line)
    cleaned = re.sub(r"\s*\*.*$", "", cleaned)
    cleaned = re.sub(r"\b(?:orcid|https?://\S+)\b", "", cleaned, flags=re.I)
    cleaned = re.sub(r"(?<=\D)\d+(?=[,*†‡§]?(?:\s|$))", "", cleaned)
    cleaned = cleaned.replace("·", ", ")
    cleaned = re.sub(r"\s+", " ", cleaned)
    cleaned = re.sub(r"\s+,", ",", cleaned)
    cleaned = cleaned.strip(" ,.;")
    if is_noise_line(cleaned) or looks_like_affiliation_line(cleaned):
        return ""
    if len(re.findall(r"\b[A-Z][A-Za-z'`.-]+(?:\s+[A-Z][A-Za-z'`.-]+)+\b", cleaned)) < 1:
        return ""
    return cleaned


def looks_like_author_line(line: str) -> bool:
    if not line or len(line) > 200:
        return False
    if is_noise_line(line):
        return False
    if re.search(r"\bhttps?://", line, re.I):
        return False
    if "@" in line:
        return False
    if looks_like_affiliation_line(line):
        return False
    if not re.search(r"[A-Za-z]", line):
        return False
    if len(re.findall(r"\b[A-Z][a-z]+(?:[-'][A-Z][a-z]+)?\b", line)) < 2:
        return False
    if any(token in line for token in [",", " and ", " et al", "·"]):
        return True
    words = line.split()
    return 2 <= len(words) <= 12


def collect_author_block(lines, start_idx: int) -> str:
    parts = []
    for candidate in lines[start_idx:start_idx + 4]:
        if is_noise_line(candidate) or looks_like_affiliation_line(candidate):
            break
        if candidate.startswith("*") or "@" in candidate:
            break
        if looks_like_author_line(candidate):
            parts.append(candidate)
            if not candidate.endswith(","):
                break
        elif parts:
            break
    return clean_author_line(" ".join(parts))


def extract_author_from_pdf(text: str, known_title: str) -> str:
    lines = parse_lines(text)
    if not lines:
        return ""
    if known_title:
        normalized_title = re.sub(r"\s+", " ", known_title).strip().lower()
        for idx in range(len(lines)):
            combined = lines[idx]
            for end in range(idx, min(idx + 4, len(lines))):
                combined = " ".join(lines[idx:end + 1])
                normalized_combined = re.sub(r"\s+", " ", combined).strip().lower()
                if normalized_combined == normalized_title or normalized_title in normalized_combined:
                    author_block = collect_author_block(lines, end + 1)
                    if author_block:
                        return author_block
    for idx, line in enumerate(lines[:12]):
        if line.lower() == "abstract":
            for candidate in reversed(lines[max(0, idx - 3):idx]):
                cleaned = clean_author_line(candidate)
                if cleaned and looks_like_author_line(candidate):
                    return cleaned
    for idx, line in enumerate(lines[:20]):
        if looks_like_author_line(line):
            cleaned = collect_author_block(lines, idx)
            if cleaned:
                return cleaned
    return ""


def extract_year_from_pdf(text: str, info_text: str) -> str:
    year_patterns = [
        r"©[^0-9]*((?:19|20)\d{2})",
        r"copyright[^0-9]*((?:19|20)\d{2})",
        r"springer nature\s+((?:19|20)\d{2})",
        r"\b((?:19|20)\d{2})\b",
    ]
    for pattern in year_patterns:
        match = re.search(pattern, text, re.I)
        if match:
            return match.group(1)
    info = parse_pdf_info(info_text)
    for key in ("creationdate", "moddate"):
        value = info.get(key, "")
        match = re.search(r"\b((?:19|20)\d{2})\b", value)
        if match:
            return match.group(1)
    match = re.search(r"(?:CreationDate|ModDate):.*\b((?:19|20)\d{2})\b", info_text)
    if match:
        return match.group(1)
    return ""


def extract_title_from_pdf(text: str) -> str:
    lines = parse_lines(text)
    title_lines = []
    for line in lines:
        lower = line.lower()
        if lower.startswith("chapter "):
            continue
        if lower in {"abstract", "keywords"} or lower.startswith("1 introduction"):
            break
        if looks_like_author_line(line) and title_lines:
            break
        title_lines.append(line)
        if len(title_lines) >= 4:
            break
    return " ".join(title_lines).strip()


def extract_journal_from_pdf(text: str) -> str:
    for line in parse_lines(text):
        if "methods in molecular biology" in line.lower():
            return line.split(",", 1)[0].strip()
    return ""


def extract_author_from_pdfinfo(info_text: str) -> str:
    author = parse_pdf_info(info_text).get("author", "")
    return clean_author_line(author)


def extract_title_from_pdfinfo(info_text: str) -> str:
    return parse_pdf_info(info_text).get("title", "").strip()


def write_bib(path: Path, fields):
    path.write_text(
        "\n".join(
            [
                f"author|{fields['author']}",
                f"accession|{fields['accession']}",
                f"type|{fields['type']}",
                f"title|{fields['title']}",
                f"journal|{fields['journal']}",
                f"citation|{fields['citation']}",
                f"year|{fields['year']}",
                f"abstract|{fields['abstract']}",
            ]
        )
        + "\n"
    )


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--pdf", required=True)
    parser.add_argument("--bib", required=True)
    parser.add_argument("--accession", required=True)
    parser.add_argument("--metadata-dir", required=True)
    args = parser.parse_args()

    pdf_path = Path(args.pdf)
    bib_path = Path(args.bib)
    metadata_dir = Path(args.metadata_dir)
    accession = args.accession

    metadata = load_csv_metadata(metadata_dir, accession)
    text = pdf_first_page_text(pdf_path) if pdf_path.is_file() else ""
    info_text = pdf_info(pdf_path) if pdf_path.is_file() else ""

    author = (
        metadata.get("author")
        or extract_author_from_pdfinfo(info_text)
        or extract_author_from_pdf(text, metadata.get("title", ""))
    )
    title = metadata.get("title") or extract_title_from_pdfinfo(info_text) or extract_title_from_pdf(text) or accession
    journal = metadata.get("journal") or extract_journal_from_pdf(text) or "<not uploaded>"
    year = metadata.get("year") or extract_year_from_pdf(text, info_text) or "<not uploaded>"
    abstract = metadata.get("abstract") or "<not uploaded>"

    citation = "<not uploaded>"
    if metadata.get("pubmed_id"):
        citation = f"PMID: {metadata['pubmed_id']}"

    fields = {
        "author": author or "<not uploaded>",
        "accession": accession,
        "type": "Journal_article",
        "title": title,
        "journal": journal,
        "citation": citation,
        "year": year,
        "abstract": abstract,
    }
    bib_path.parent.mkdir(parents=True, exist_ok=True)
    write_bib(bib_path, fields)


if __name__ == "__main__":
    main()
