#!/usr/bin/env bash
#
# CAS1 for papers whose main text comes from JATS (the "jats" layout).
#
# Input is what maizegdb_textpresso_implementation's bin/fetch_jats.py stages:
#   ${JATS_DIR}/<corpus>/<accession>/<accession>.jats.json   (read here)
#   ${JATS_DIR}/<corpus>/<accession>/<accession>.nxml.gz     (provenance)
#   ${JATS_DIR}/<corpus>/<accession>/<accession>.suppl.txt   (already in the .json)
# Output: ${CAS1_DIR}/<corpus>/<accession>/<accession>.tpcas.gz -- the same
# path the PDF (tai) step uses, so a paper is indexed once, from JATS.
#
# Run this before 03pdf2cas4tai.sh, which skips papers staged here.

function usage {
    echo "this script converts staged JATS (.jats.json) into cas files."
    echo
    echo "usage: $(basename $0) [-jcPh]"
    echo "  -j --jats-dir     directory with staged JATS, one subdirectory per corpus"
    echo "  -c --cas1-dir     directory where generated cas1 files will be stored"
    echo "  -P --num-proc     maximum number of parallel processes"
    echo "  -h --help         display help"
    rm ${LOCKFILE}
    exit 1
}

JATS_DIR="/data/textpresso/raw_files/xml"
CAS1_DIR="/data/textpresso/tpcas-1"
N_PROC=8
LOCKFILE="/data/textpresso/tmp/03jats2cas.lock"
if [[ -f "${LOCKFILE}" ]]
then
    echo $(basename $0) "is already running."
    exit 1
fi
mkdir -p "$(dirname "${LOCKFILE}")"
touch "${LOCKFILE}"
while [[ $# -gt 0 ]]
do
    case $1 in
        -j|--jats-dir) shift; JATS_DIR="$1"; shift ;;
        -c|--cas1-dir) shift; CAS1_DIR="$1"; shift ;;
        -P|--num-proc) shift; N_PROC=$1; shift ;;
        -h|--help) usage ;;
        *) echo "wrong argument: $1"; echo; usage ;;
    esac
done

export LD_LIBRARY_PATH=${LD_LIBRARY_PATH}:/usr/local/lib
export PATH=$PATH:/usr/local/bin

if [[ ! -d "${JATS_DIR}" ]]
then
    echo "no staged JATS in ${JATS_DIR}; nothing to do."
    rm ${LOCKFILE}
    exit 0
fi

echo "Generating CAS1 files from JATS ..."
tmp=$(mktemp -d)
cd "${JATS_DIR}"
for folder in */
do
    folder=${folder%/}
    mkdir -p "${CAS1_DIR}/${folder}"
    # accessions whose staged JSON is newer than their CAS1
    counter=0
    for d in "${folder}"/*/
    do
        acc=$(basename "${d}")
        json="${folder}/${acc}/${acc}.jats.json"
        cas="${CAS1_DIR}/${folder}/${acc}/${acc}.tpcas.gz"
        if [[ -f "${json}" ]] && [[ "${json}" -nt "${cas}" ]]
        then
            echo "${acc}" >> "${tmp}/${folder}.$((counter % N_PROC)).list"
            counter=$((counter+1))
        fi
    done
    echo "${folder}: ${counter} papers"
    (
        cd "${CAS1_DIR}"
        for ((j=0; j<${N_PROC}; j++))
        do
            if [[ -f "${tmp}/${folder}.$j.list" ]]
            then
                articles2cas -i "${JATS_DIR}/${folder}" -l "${tmp}/${folder}.$j.list" -t 5 -o "${folder}" -p > /dev/null &
            fi
        done
        wait
        # an earlier .tpcas.gz of the same paper (e.g. from the PDF path) is replaced
        find "${folder}" -name "*.tpcas" -print0 | xargs -0 -r -n 8 -P 8 pigz -f 2>/dev/null
    )
done
rm -rf "${tmp}"
rm ${LOCKFILE}
