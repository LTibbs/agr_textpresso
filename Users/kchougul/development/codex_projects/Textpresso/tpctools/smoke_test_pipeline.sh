#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
DATA_DIR="${ROOT_DIR}/.data"
CONTAINER_NAME="${CONTAINER_NAME:-agr-textpresso-textpresso-1}"
CORPUS_NAME="${CORPUS_NAME:-PipelineSmoke}"
SOURCE_PDF="${SOURCE_PDF:-${ROOT_DIR}/libtpc/tests/data/pdf/C. elegans/WBPaper00000011/WBPaper00000011.pdf}"
DOC_ID="${DOC_ID:-$(basename "${SOURCE_PDF}" .pdf)}"
API_URL="${API_URL:-http://localhost:18080}"

if [[ ! -f "${SOURCE_PDF}" ]]
then
    echo "Missing source PDF: ${SOURCE_PDF}" >&2
    exit 1
fi

mkdir -p "${DATA_DIR}/raw_files/pdf/${CORPUS_NAME}/${DOC_ID}"
cp "${SOURCE_PDF}" "${DATA_DIR}/raw_files/pdf/${CORPUS_NAME}/${DOC_ID}/${DOC_ID}.pdf"
rm -rf "${DATA_DIR}/tpcas-1/${CORPUS_NAME}" "${DATA_DIR}/tpcas-2/${CORPUS_NAME}"

PIPELINE_LOG=$(mktemp)
trap 'rm -f "${PIPELINE_LOG}"' EXIT

docker cp "${ROOT_DIR}/tpctools/run_tpc_pipeline_incremental.sh" \
    "${CONTAINER_NAME}:/tmp/run_tpc_pipeline_incremental.sh"
docker exec "${CONTAINER_NAME}" sh -lc \
    "chmod +x /tmp/run_tpc_pipeline_incremental.sh && /tmp/run_tpc_pipeline_incremental.sh -P 1 -e download_pdf,download_xml,bib,invert_img,remove_invalidated" \
    | tee "${PIPELINE_LOG}"

if grep -q "Segmentation fault" "${PIPELINE_LOG}"
then
    echo "Smoke test failed: pipeline log contains a segmentation fault." >&2
    exit 1
fi

RESULT_COUNT=$(
docker exec -i "${CONTAINER_NAME}" python3 - <<PY
import json
import urllib.request

payload = {
    "query": {
        "accession": "${DOC_ID}",
        "type": "document",
        "case_sensitive": False,
        "sort_by_year": False,
        "count": 10,
        "corpora": ["${CORPUS_NAME}"],
    }
}
request = urllib.request.Request(
    "${API_URL}/v1/textpresso/api/get_documents_count",
    data=json.dumps(payload).encode(),
    headers={"Content-Type": "application/json"},
)
with urllib.request.urlopen(request, timeout=20) as response:
    print(response.read().decode().strip())
PY
)
RESULT_COUNT=$(printf '%s\n' "${RESULT_COUNT}" | tr -d '\r' | tail -n 1)

if [[ ! "${RESULT_COUNT}" =~ ^[0-9]+$ ]]
then
    echo "Smoke test failed: API returned a non-numeric document count '${RESULT_COUNT}'." >&2
    exit 1
fi

if [[ "${RESULT_COUNT}" == "0" ]]
then
    echo "Smoke test failed: accession '${DOC_ID}' returned zero results for corpus '${CORPUS_NAME}'." >&2
    exit 1
fi

echo "Smoke test passed."
echo "corpus=${CORPUS_NAME}"
echo "doc=${DOC_ID}"
echo "accession=${DOC_ID}"
echo "results=${RESULT_COUNT}"
