#!/usr/bin/env python3
"""Patch EXACT/RELATED synonym typing in a maize gene OBO file.

The OBO is built by an R script (not tracked in this repo) whose Rule 2 is
meant to demote a locus_synonym to RELATED whenever the same synonym string
is curated onto more than one gene -- on the theory that a string shared by
multiple genes can't be a precise, unique identifier for any single one of
them. That rule groups on the literal synonym string, which is case-sensitive
in R: "Glu" (curated on one gene) and "GLU" (curated on a different gene) are
treated as two distinct, non-colliding strings, so neither gets demoted, even
though the downstream annotator matches case-insensitively and treats them as
the same term. See /tmp .../obo_r_fix.md for the corresponding upstream fix
to the R script itself (not applied here -- this script patches the already-
generated OBO file directly).

This script:
  1. Re-runs the collision check case-insensitively across every EXACT
     locus_synonym entry in the file and demotes any that collide with
     another gene's entry once case is normalized.
  2. Applies a small manually-curated FORCE_RELATED list for terms that are
     semantically generic (family/domain-name abbreviations, e.g. "bZIP")
     but happen to be curated onto only one gene in the current source data,
     so the frequency-based rule can't catch them on its own.

Only the EXACT -> RELATED token on affected `synonym:` lines is changed;
every other line (including all other synonym types/sources) is copied
through byte-for-byte.
"""
import argparse
import re
from collections import defaultdict

TERM_RE = re.compile(r'^id: (\S+)')
SYN_LINE_RE = re.compile(r'^(synonym: ")([^"]*)(" )(\S+)( locus_synonym \[\])$')

# Manually confirmed generic family/domain-name abbreviations: force RELATED
# regardless of how many genes currently curate them, since being curated
# onto only one gene today doesn't mean the term uniquely identifies it.
FORCE_RELATED = {"bzip"}


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input_obo")
    ap.add_argument("output_obo")
    args = ap.parse_args()

    with open(args.input_obo) as f:
        lines = f.readlines()

    # Pass 1: find every EXACT locus_synonym line and which gene it belongs to.
    current_id = None
    entries = []  # (line_index, gene_id, syn_text)
    for i, line in enumerate(lines):
        stripped = line.rstrip("\n")
        if stripped == "[Term]":
            current_id = None
            continue
        m = TERM_RE.match(stripped)
        if m:
            current_id = m.group(1)
            continue
        m = SYN_LINE_RE.match(stripped)
        if m and m.group(4) == "EXACT" and current_id:
            entries.append((i, current_id, m.group(2)))

    # Case-insensitive collision map across ALL locus_synonym entries (EXACT
    # and RELATED both count toward "how many genes claim this string"),
    # matching the R rule's original scope.
    ci_genes = defaultdict(set)
    current_id = None
    for line in lines:
        stripped = line.rstrip("\n")
        if stripped == "[Term]":
            current_id = None
            continue
        m = TERM_RE.match(stripped)
        if m:
            current_id = m.group(1)
            continue
        m = SYN_LINE_RE.match(stripped)
        if m and current_id:
            ci_genes[m.group(2).lower()].add(current_id)

    n_case_collision = 0
    n_force_related = 0
    for i, gene_id, syn_text in entries:
        text_ci = syn_text.lower()
        demote = False
        reason = None
        if text_ci in FORCE_RELATED:
            demote = True
            reason = "force"
        elif len(ci_genes[text_ci]) > 1:
            demote = True
            reason = "case_collision"
        if demote:
            m = SYN_LINE_RE.match(lines[i].rstrip("\n"))
            lines[i] = f"{m.group(1)}{m.group(2)}{m.group(3)}RELATED{m.group(5)}\n"
            if reason == "case_collision":
                n_case_collision += 1
            else:
                n_force_related += 1

    with open(args.output_obo, "w") as f:
        f.writelines(lines)

    print(f"Demoted {n_case_collision} lines via case-insensitive collision")
    print(f"Demoted {n_force_related} lines via FORCE_RELATED ({sorted(FORCE_RELATED)})")
    print(f"Wrote {args.output_obo}")


if __name__ == "__main__":
    main()
