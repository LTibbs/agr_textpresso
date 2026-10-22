#!/usr/bin/env bash
# make script Fail fast, fail loudly, fail safely
set -euo pipefail

: "${MOD:?MOD is required}"

send_fail() {
    local subject="$1"
    local message="$2"
    conda run -n agr_textpresso python3 /data/textpresso/tpctools/send_report.py "$subject" "$message"
    exit 1
}

send_ok() {
    local subject="$1"
    local message="$2"
    conda run -n agr_textpresso python3 /data/textpresso/tpctools/send_report.py "$subject" "$message"
    exit 0
}

ONTO_DIR="/data/textpresso/tpctools/getOntologies"
OBO_PROD="/data/textpresso/obofiles4production"

# MaizeGDB (2026-10-22): when the annotation build's *_annot.obo files are in
# production, the category files come from build_match_dictionary.py
# (maizegdb_textpresso_implementation), not from the AGR category generator.
# Moving AGR's go/po/to files in beside them would match those ontologies
# twice and restore every string the build drops, so skip the whole update.
shopt -s nullglob
annot=( "${OBO_PROD}"/*_annot.obo )
shopt -u nullglob
if (( ${#annot[@]} > 0 )); then
    echo "Annotation build in ${OBO_PROD} (${#annot[@]} *_annot.obo); skipping AGR ontology update."
    exit 0
fi

echo "Changing to ontology directory..."
cd "${ONTO_DIR}"

echo "Removing old .obo files..."
rm -f ./*.obo

echo "Starting category files generation..."
if ! conda run -n agr_textpresso python3 get_categories.py -m "${MOD}" -a; then
    send_fail "${MOD} Textpresso Category Files Generation" "Failed to generate category files."
fi

echo "Moving category files to production..."
# Treat unmatched globs as empty
shopt -s nullglob
obos=( ./*.obo )
if (( ${#obos[@]} == 0 )); then
    send_fail "${MOD} Textpresso Getting Category Files Ready" "No .obo files generated."
fi

if ! mv "${obos[@]}" "${OBO_PROD}/"; then
    send_fail "${MOD} Textpresso Getting Category Files Ready" "Failed to move category files."
fi

echo "Generating CAS-2 files..."
if ! annotate -P 2; then
    send_fail "${MOD} Textpresso CAS-2 Files Generation" "Failed to generate CAS-2 files."
fi

echo "Indexing papers..."
if ! index; then
    send_fail "${MOD} Textpresso Indexing" "Failed to index papers."
fi

send_ok "${MOD} Textpresso Ontology Update Report" \
        "Ontology/category terms updated successfully and full-text papers have been re-annotated."
