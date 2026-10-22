#!/usr/bin/env bash
set -euo pipefail

# -----------------------------
# Config
# -----------------------------
MOD="${MOD:-}"
if [[ -z "${MOD}" ]]; then
  echo "ERROR: MOD env var is not set (e.g. MOD=WB)"
  exit 1
fi

BASE="/data/textpresso"
RAW_MAIN="${BASE}/raw_files"
CAS1_MAIN="${BASE}/tpcas-1"
CAS2_MAIN="${BASE}/tpcas-2"
PAST_WEEK="${BASE}/raw_files_past_week"

# Staging (timestamped so we never clobber another run)
RUN_ID="$(date +%Y%m%d_%H%M%S)"
STAGE_RAW="${BASE}/raw_files_new_${RUN_ID}"
STAGE_CAS1="${BASE}/tpcas-1_new_${RUN_ID}"
STAGE_CAS2="${BASE}/tpcas-2_new_${RUN_ID}"

LOCKFILE="${BASE}/tmp/incremental_build.lock"
LOG_PREFIX="[incremental_build ${RUN_ID}]"

cleanup_on_success() {
  rm -rf "${STAGE_RAW}" "${STAGE_CAS1}" "${STAGE_CAS2}"
  rm -f "${LOCKFILE}"
}

fail_handler() {
  local ec=$?
  echo "${LOG_PREFIX} ERROR: build failed (exit code ${ec})."
  echo "${LOG_PREFIX} Staging dirs preserved for debugging:"
  echo "  ${STAGE_RAW}"
  echo "  ${STAGE_CAS1}"
  echo "  ${STAGE_CAS2}"
  rm -f "${LOCKFILE}"
  exit "${ec}"
}

trap fail_handler ERR
trap 'rm -f "${LOCKFILE}"' INT TERM

# Lock
if [[ -e "${LOCKFILE}" ]]; then
  echo "${LOG_PREFIX} ERROR: lockfile exists (${LOCKFILE}). Another run may be in progress."
  exit 1
fi
mkdir -p "$(dirname "${LOCKFILE}")"
touch "${LOCKFILE}"

echo "${LOG_PREFIX} Starting for MOD=${MOD}"

# -----------------------------
# Stage: download PDFs + bibs
# -----------------------------
rm -rf "${STAGE_RAW}" "${STAGE_CAS1}" "${STAGE_CAS2}"
mkdir -p "${STAGE_RAW}"

conda run -n agr_textpresso python3 \
  "${BASE}/tpctools/getPdfBiblio/download_pdfs_bib_files.py" \
  -m "${MOD}" -d 14 -p "${STAGE_RAW}"

echo "${LOG_PREFIX} DONE downloading PDFs and generating bib files!"

# Bring forward last week leftovers into staging
mkdir -p "${PAST_WEEK}"
rsync -a "${PAST_WEEK}/" "${STAGE_RAW}/" || true

# Count PDFs
total_pdfs="$(find "${STAGE_RAW}/pdf" -maxdepth 3 -name "*.pdf" 2>/dev/null | wc -l | tr -d ' ')"
echo "${LOG_PREFIX} Total staged PDF file(s): ${total_pdfs}"

if [[ "${total_pdfs}" -lt 4 ]]; then
  echo "${LOG_PREFIX} <4 PDFs; carrying forward to next week and exiting."
  rsync -a "${STAGE_RAW}/" "${PAST_WEEK}/"
  rm -rf "${STAGE_RAW}"
  rm -f "${LOCKFILE}"
  exit 0
fi

# Clear past week only once we know we're proceeding
rm -rf "${PAST_WEEK:?}/"* || true
echo "${LOG_PREFIX} Proceeding; cleared ${PAST_WEEK}"

# -----------------------------
# Stage: pdf2txt + CAS generation
# -----------------------------
convert_text "${STAGE_RAW}"

# Papers with staged JATS (raw_files/xml/<corpus>/<acc>/<acc>.jats.json, from
# maizegdb_textpresso_implementation's bin/fetch_jats.py) get their CAS-1 from
# JATS; tokenize then skips them. Nothing stages JATS for the AGR download, so
# for AGR MODs both JATS steps are no-ops.
jats2cas -P 4 -j "${STAGE_RAW}/xml" -c "${STAGE_CAS1}"
tokenize -P 4 -p "${STAGE_RAW}/pdf" -c "${STAGE_CAS1}" -j "${STAGE_RAW}/xml"
echo "${LOG_PREFIX} DONE generating CAS-1 files!"

annotate -P 2 -c "${STAGE_CAS1}" -C "${STAGE_CAS2}"
echo "${LOG_PREFIX} DONE generating CAS-2 files!"

# Copy bibs into CAS2 staging
rsync -a "${STAGE_RAW}/bib/" "${STAGE_CAS2}/"
echo "${LOG_PREFIX} DONE transferring bib files into staged CAS-2!"

# Optional sanity checks (fail fast)
empty_cas1="$(find "${STAGE_CAS1}" -maxdepth 3 -type f -name "*.tpcas.gz" -exec ls -l {} \; | awk '$5==0{c++} END{print c+0}')"
empty_cas2="$(find "${STAGE_CAS2}" -maxdepth 3 -type f -name "*.tpcas.gz" -exec ls -l {} \; | awk '$5==48{c++} END{print c+0}')"
echo "${LOG_PREFIX} Empty CAS-1 count: ${empty_cas1}"
echo "${LOG_PREFIX} Empty CAS-2 count: ${empty_cas2}"

# If you want to hard-fail on empties, uncomment:
# if [[ "${empty_cas2}" -gt 0 ]]; then
#   echo "${LOG_PREFIX} ERROR: Found empty CAS-2 files (${empty_cas2}). Aborting before syncing to main dirs."
#   exit 1
# fi

# -----------------------------
# Commit: sync staging -> main (only after success so far)
# -----------------------------
echo "${LOG_PREFIX} Sync staging to main dirs..."

rsync -av "${STAGE_RAW}/"  "${RAW_MAIN}/"
rsync -av "${STAGE_CAS1}/" "${CAS1_MAIN}/"
rsync -av "${STAGE_CAS2}/" "${CAS2_MAIN}/"

echo "${LOG_PREFIX} DONE syncing staged data to main dirs."

# -----------------------------
# Index: same logic for all MODs (full index)
# -----------------------------
echo "${LOG_PREFIX} Starting index (full) ..."
index
echo "${LOG_PREFIX} DONE indexing!"

# Report
conda run -n agr_textpresso python3 "${BASE}/tpctools/send_report.py"
echo "${LOG_PREFIX} DONE report."

# Cleanup only on full success
cleanup_on_success
echo "${LOG_PREFIX} All done."
