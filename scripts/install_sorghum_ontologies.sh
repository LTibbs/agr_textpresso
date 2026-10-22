#!/usr/bin/env bash
set -euo pipefail

TPC_HOME="${TPC_HOME:-/home/ec2-user/agr_textpresso}"
TPC_DATA="${TPC_DATA:-${TPC_HOME}/.data}"
SOURCE_DIR="${SOURCE_DIR:-/home/ec2-user/sorghum_obo_files}"
CONTAINER="${CONTAINER:-agr-textpresso-textpresso-1}"
CORPUS="${CORPUS:-SorghumBase}"
N_PROC="${N_PROC:-2}"
REANNOTATE="${REANNOTATE:-1}"
REINDEX="${REINDEX:-1}"
RESTART_CONTAINER="${RESTART_CONTAINER:-0}"
ALLOW_CONCURRENT_PIPELINE="${ALLOW_CONCURRENT_PIPELINE:-0}"

OBO_PROD="${TPC_DATA}/obofiles4production"
OBO_HEADERS="${TPC_DATA}/oboheaderfiles"
HOST_ONTOLOGY_CONF="${TPC_HOME}/textpressocentral/etc/ontology.conf"
BACKUP_DIR="${TPC_DATA}/backups/ontology-$(date -u +%Y%m%dT%H%M%SZ)"

require_file() {
    local path="$1"
    if [[ ! -f "${path}" ]]; then
        echo "Required file not found: ${path}" >&2
        exit 1
    fi
}

run_container() {
    docker exec "${CONTAINER}" bash -lc "$1"
}

write_ontology_conf() {
    local target="$1"
    cat > "${target}" <<'EOF'
/data/textpresso/obofiles4production/go.obo 6 goslim_generic goslim_agr goslim_plant
/data/textpresso/obofiles4production/po.obo 8
/data/textpresso/obofiles4production/to.obo 8
EOF
}

materialize_base_tables() {
    run_container "psql -v ON_ERROR_STOP=1 -d www-data <<'SQL'
do \$\$
declare
  t text;
  first_table text;
begin
  select tablename into first_table
    from pg_tables
   where schemaname = 'public'
     and tablename like 'pcrelations_%'
   order by tablename
   limit 1;
  if first_table is not null then
    execute 'drop table if exists pcrelations';
    execute format('create table pcrelations as select * from %I with no data', first_table);
    for t in
      select tablename from pg_tables
       where schemaname = 'public'
         and tablename like 'pcrelations_%'
       order by tablename
    loop
      execute format('insert into pcrelations select * from %I', t);
    end loop;
  end if;

  select tablename into first_table
    from pg_tables
   where schemaname = 'public'
     and tablename like 'tpontology_%'
   order by tablename
   limit 1;
  if first_table is not null then
    execute 'drop table if exists tpontology';
    execute format('create table tpontology as select * from %I with no data', first_table);
    for t in
      select tablename from pg_tables
       where schemaname = 'public'
         and tablename like 'tpontology_%'
       order by tablename
    loop
      execute format('insert into tpontology select * from %I', t);
    end loop;
  end if;
end \$\$;
create index if not exists pcrelations_parent_idx on pcrelations(parent);
create index if not exists pcrelations_child_idx on pcrelations(child);
create index if not exists tpontology_category_idx on tpontology(category);
create index if not exists tpontology_term_idx on tpontology(term);
SQL"
}

verify_nonzero_tables() {
    run_container "psql -v ON_ERROR_STOP=1 -At -d www-data <<'SQL'
select 'tpontology=' || count(*) from tpontology;
select 'pcrelations=' || count(*) from pcrelations;
select 'ontologymembers=' || count(*) from ontologymembers;
select 'members=' || string_agg(list, ',' order by list) from ontologymembers;
SQL"
}

ensure_no_concurrent_pipeline() {
    if [[ "${ALLOW_CONCURRENT_PIPELINE}" == "1" ]]; then
        return
    fi
    local pids
    pids="$(docker exec "${CONTAINER}" bash -lc "ps -eo pid,args | awk '/run_tpc_pipeline_incremental.sh/ && !/awk/ {print \$1}'")"
    if [[ -n "${pids}" ]]; then
        echo "Refusing to continue while run_tpc_pipeline_incremental.sh is active in ${CONTAINER}: ${pids}" >&2
        echo "Stop the existing pipeline or rerun with ALLOW_CONCURRENT_PIPELINE=1 if this is intentional." >&2
        exit 1
    fi
}

backup_current_ontology_tables() {
    mkdir -p "${BACKUP_DIR}"
    if docker exec "${CONTAINER}" pg_dump -F c -t tpontology -t pcrelations -t ontologymembers www-data > "${BACKUP_DIR}/ontology-base-tables.dump"; then
        echo "Backed up ontology base tables to ${BACKUP_DIR}/ontology-base-tables.dump"
    else
        echo "Base table backup failed or tables were absent; continuing with rebuild." >&2
        rm -f "${BACKUP_DIR}/ontology-base-tables.dump"
    fi
}

install_obo_files() {
    require_file "${SOURCE_DIR}/GO.obo"
    require_file "${SOURCE_DIR}/PO.obo"
    require_file "${SOURCE_DIR}/TO.obo"

    mkdir -p "${OBO_PROD}" "${OBO_HEADERS}"
    install -m 0644 "${SOURCE_DIR}/GO.obo" "${OBO_PROD}/go.obo"
    install -m 0644 "${SOURCE_DIR}/PO.obo" "${OBO_PROD}/po.obo"
    install -m 0644 "${SOURCE_DIR}/TO.obo" "${OBO_PROD}/to.obo"

    write_ontology_conf "${HOST_ONTOLOGY_CONF}"
    local runtime_conf="${TPC_DATA}/tmp/sorghum_ontology.conf"
    mkdir -p "$(dirname "${runtime_conf}")"
    write_ontology_conf "${runtime_conf}"
    docker cp "${runtime_conf}" "${CONTAINER}:/usr/local/etc/ontology.conf"
}

reset_and_rebuild_lexica() {
    run_container "psql -v ON_ERROR_STOP=1 -d www-data <<'SQL'
do \$\$
declare
  t text;
begin
  for t in
    select tablename from pg_tables
     where schemaname = 'public'
       and (tablename = 'ontologymembers'
        or tablename like 'tpontology%'
        or tablename like 'pcrelations%')
  loop
    execute format('drop table if exists %I cascade', t);
  end loop;
end \$\$;
SQL"
    run_container "CreateLexica.bash"
    materialize_base_tables
    verify_nonzero_tables
}

reannotate_sorghum_corpus() {
    run_container "set -euo pipefail
log=/data/textpresso/logs/sorghum-reannotate-\$(date -u +%Y%m%dT%H%M%SZ).log
tmp_root=/data/textpresso/tmp/sorghum-reannotate
rm -rf \"\${tmp_root}\"
mkdir -p \"\${tmp_root}/cas1/${CORPUS}\"
find /data/textpresso/tpcas-1/${CORPUS} -mindepth 1 -maxdepth 1 -type d -print0 |
while IFS= read -r -d '' src; do
  acc=\$(basename \"\${src}\")
  dst=\"\${tmp_root}/cas1/${CORPUS}/\${acc}\"
  mkdir -p \"\${dst}\"
  find \"\${src}\" -maxdepth 1 -name '*.tpcas.gz' -print -quit |
  while IFS= read -r cas; do
    ln -sf \"\${cas}\" \"\${dst}/\$(basename \"\${cas}\")\"
  done
  if [[ -d \"\${src}/images\" ]]; then
    ln -sfn \"\${src}/images\" \"\${dst}/images\"
  fi
done
rm -f /data/textpresso/tmp/07cas1tocas2.lock
annotate -c \"\${tmp_root}/cas1\" -C /data/textpresso/tpcas-2 -t /data/textpresso/tmp -P ${N_PROC} 2>&1 | tee \"\${log}\"
if grep -E 'No space left|Error opening output xmi|std::exception' \"\${log}\" >/dev/null; then
  echo \"Sorghum reannotation reported errors; see \${log}\" >&2
  exit 1
fi
rm -rf \"\${tmp_root}\""
    materialize_base_tables
    verify_nonzero_tables
}

reindex_corpora() {
    run_container "set -euo pipefail
log=/data/textpresso/logs/sorghum-reindex-\$(date -u +%Y%m%dT%H%M%SZ).log
rm -rf /data/textpresso/luceneindex_new
rm -f /data/textpresso/tmp/12index.lock
set +e
index -C /data/textpresso/tpcas-2 -i /data/textpresso/luceneindex 2>&1 | tee \"\${log}\"
status=\${PIPESTATUS[0]}
set -e
if grep -E 'No space left|cannot create|error writing|std::exception' \"\${log}\" >/dev/null || [[ \"\${status}\" -ne 0 ]]; then
  echo \"Sorghum reindex reported errors; see \${log}\" >&2
  if [[ -d /data/textpresso/luceneindex.bk ]]; then
    rm -rf /data/textpresso/luceneindex
    mv /data/textpresso/luceneindex.bk /data/textpresso/luceneindex
  fi
  rm -rf /data/textpresso/luceneindex_new
  exit 1
fi
rm -rf /data/textpresso/luceneindex_new"
}

main() {
    docker inspect "${CONTAINER}" >/dev/null
    ensure_no_concurrent_pipeline
    backup_current_ontology_tables
    install_obo_files
    reset_and_rebuild_lexica

    if [[ "${REANNOTATE}" == "1" ]]; then
        reannotate_sorghum_corpus
    fi

    if [[ "${REINDEX}" == "1" ]]; then
        reindex_corpora
    fi

    if [[ "${RESTART_CONTAINER}" == "1" ]]; then
        docker restart "${CONTAINER}" >/dev/null
    fi

    echo "Sorghum GO/PO/TO ontologies are installed and available to Textpresso."
}

main "$@"
