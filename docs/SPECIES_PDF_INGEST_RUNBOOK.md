# Textpresso Species PDF Ingest Runbook (Reproducible)

This runbook documents the exact workflow we used and generalizes it so any species corpus can be ingested in a repeatable way.

It covers:
- Bringing up Textpresso with Docker
- Staging PDFs for a new corpus
- Running CAS1 -> CAS2 -> index pipeline in safe batches
- Populating `.bib` sidecar metadata (title, journal, year, author, abstract)
- Backfilling missing years from DOI services
- Reindexing and API validation

## 1. Conventions and Variables

Set these once per session:

```bash
export TPC_HOME=/home/ec2-user/agr_textpresso
export TPC_DATA=/home/ec2-user/agr_textpresso/.data
export CONTAINER=agr-textpresso-textpresso-1

# Example corpus name. Change this for your species.
export CORPUS=SorghumBase

# Metadata CSV for your species papers (must include doi/title/journal/year/etc when possible)
export METADATA_CSV=/home/ec2-user/sorghumbase_textpresso_implementation/sorghumbase_textpresso_implementation/metadata/sorghumbase_papers.csv
```

Expected accession format: DOI with `/` replaced by `_`
Example: `10.1371/journal.pone.0151271` -> `10.1371_journal.pone.0151271`

## 2. One-Time Host Setup

```bash
sudo yum update -y
sudo yum install -y git docker
sudo service docker start
sudo usermod -a -G docker ec2-user

# re-login required after group change

sudo curl -L "https://github.com/docker/compose/releases/latest/download/docker-compose-$(uname -s)-$(uname -m)" -o /usr/local/bin/docker-compose
sudo chmod +x /usr/local/bin/docker-compose
```

If host nginx is occupying port 80/8080, stop or reconfigure it.

## 3. Build and Start Textpresso

```bash
cd "$TPC_HOME"
cp -n .env_example .env
mkdir -p "$TPC_DATA"

# Set data root and ports
sed -i 's|^TEXTPRESSO_DATA_DIR=.*|TEXTPRESSO_DATA_DIR=/home/ec2-user/agr_textpresso/.data|' .env
sed -i 's|^TPC_UI_PORT=.*|TPC_UI_PORT=8080|' .env
sed -i 's|^TPC_API_PORT=.*|TPC_API_PORT=18080|' .env

# Optional if sasl_passwd is required by your compose env
touch sasl_passwd

docker-compose build
docker-compose up -d
docker-compose ps
```

## 4. Stage PDFs into Required Layout

Textpresso expects:
`$TPC_DATA/raw_files/pdf/<CORPUS>/<accession>/<accession>.pdf`

```bash
mkdir -p "$TPC_DATA/raw_files/pdf/$CORPUS"

# If your source PDFs are flat in one directory:
SRC_PDF_DIR=/path/to/species_pdfs
for pdf in "$SRC_PDF_DIR"/*.pdf; do
  accession=$(basename "$pdf" .pdf)
  mkdir -p "$TPC_DATA/raw_files/pdf/$CORPUS/$accession"
  cp "$pdf" "$TPC_DATA/raw_files/pdf/$CORPUS/$accession/$accession.pdf"
done
```

## 5. Run CAS Pipeline in Smaller Batches (Recommended)

For large corpora, use low parallelism first (for memory stability):

```bash
docker exec "$CONTAINER" bash -lc "mkdir -p \
    /data/textpresso/tpcas-1/$CORPUS \
    /data/textpresso/tpcas-2/$CORPUS \
    /data/textpresso/luceneindex/$CORPUS"

docker exec "$CONTAINER" bash -lc "\
run_tpc_pipeline_incremental.sh \
    -p /data/textpresso/raw_files/pdf \
    -x /data/textpresso/raw_files/xml \
    -c /data/textpresso/tpcas-1 \
    -C /data/textpresso/tpcas-2 \
    -t /data/textpresso/tmp \
    -i /data/textpresso/luceneindex \
  -P 1 \
  -e download_pdf,download_xml,bib\
"
```

Notes:
- Important: `run_tpc_pipeline_incremental.sh` expects root dirs for `-p/-x/-c/-C/-t/-i`, not per-corpus dirs.
- Scope is controlled by what exists under `/data/textpresso/raw_files/pdf` and by excluding download steps.
- To process only one corpus, ensure only that corpus has new/changed PDFs, or run in a clean data area.
- Do not create `/data/textpresso/raw_files/xml/$CORPUS` for PDF-only runs. An empty XML corpus folder can trigger noisy PMCOA warnings.
- Excluding `download_pdf,download_xml` uses already staged files.
- Excluding `bib` avoids external bib download dependencies.
- You can increase `-P` later if resources allow.

### 5.1 Verify corpus scoping before CAS2

```bash
docker exec "$CONTAINER" bash -lc "find /data/textpresso/raw_files/pdf -mindepth 1 -maxdepth 1 -type d -printf '%f\n'"
docker exec "$CONTAINER" bash -lc "find /data/textpresso/raw_files/pdf/$CORPUS -mindepth 2 -maxdepth 2 -name '*.pdf' | wc -l"
```

If multiple corpus folders are present and you only want one run, move non-target corpus folders out of `/data/textpresso/raw_files/pdf` temporarily.

### 5.2 Check pcrelations/tpontology prerequisites (CAS2)

```bash
docker exec "$CONTAINER" bash -lc "psql -At -d www-data -c \"select tablename from pg_tables where schemaname='public' and (tablename like 'pcrelations%' or tablename like 'tmppcrelations%' or tablename like 'tpontology%' or tablename like 'tmptpontology%') order by tablename\""
```

If this returns nothing, initialize lexica/tables first before CAS2.

Initialize lexica from ontology sources:

```bash
docker exec "$CONTAINER" bash -lc "CreateLexica.bash"
```

If ontology source folders are empty and CAS2 still reports missing `pcrelations/tpontology`, create minimal fallback tables (allows CAS2 processing, but without ontology-driven lexical expansion):

```bash
docker exec "$CONTAINER" bash -lc "psql -d www-data -c \"create table if not exists pcrelations (parent text, child text); create table if not exists tpontology (term text, lexicalvariations text, category text);\""
```

## 6. Populate `.bib` Sidecars from Species Metadata CSV

Why this matters: UI result columns (title, journal, year, author, abstract) come from indexed fields and/or `.bib` sidecars.

Important: do not write the literal string `<not uploaded>` in curated bib files. The pipeline helper `ensure_pdf_bib_files` treats that marker as incomplete metadata and can regenerate/overwrite sidecars during later runs.

### 6.1 Copy metadata CSV into container

```bash
docker cp "$METADATA_CSV" "$CONTAINER":/tmp/species_papers.csv
```

### 6.2 Backfill `.bib` files

This script maps CSV `doi` -> accession (`/` -> `_`) and writes sidecars.

```bash
docker exec "$CONTAINER" bash -lc "python3 - <<'PY'
import csv, glob, html, os
from collections import defaultdict

csv_path = '/tmp/species_papers.csv'
cas_root = '/data/textpresso/tpcas-2/' + os.environ.get('CORPUS', 'SorghumBase')

records = defaultdict(lambda: {'title':'','abstract':'','authors':'','journal':'','year':''})
with open(csv_path, newline='', encoding='utf-8') as f:
    for row in csv.DictReader(f):
        doi = (row.get('doi') or '').strip()
        if not doi:
            continue
        key = doi.replace('/', '_')
        rec = records[key]
        for src,dst in [('title','title'),('abstract','abstract'),('authors','authors'),('journal','journal'),('year','year')]:
            v = (row.get(src) or '').strip()
            if v and not rec[dst]:
                rec[dst] = html.unescape(v)

updated = matched = unmatched = 0
for bib in glob.glob(os.path.join(cas_root, '*', '*.bib')):
    acc = os.path.basename(bib)[:-4]
    rec = records.get(acc)
    if not rec:
        unmatched += 1
        continue
    matched += 1

    author = rec['authors'] or 'unknown'
    title = rec['title'] or acc
    journal = rec['journal'] or 'unknown'
    year = rec['year'] or ''
    abstract = rec['abstract'] or ''
    citation = ' '.join([x for x in [author, f'({year})' if year else '', title, journal] if x]).strip() or title

    content = (
        f'author|{author}\n'
        f'accession|{acc}\n'
        'type|Journal_article\n'
        f'title|{title}\n'
        f'journal|{journal}\n'
        f'citation|{citation}\n'
        f'year|{year}\n'
        f'abstract|{abstract}\n'
    )
    old = ''
    if os.path.exists(bib):
        with open(bib,'r',encoding='utf-8',errors='ignore') as fh:
            old = fh.read()
    if old != content:
        with open(bib,'w',encoding='utf-8') as fh:
            fh.write(content)
        updated += 1

print('matched', matched)
print('unmatched', unmatched)
print('updated', updated)
PY"
```

Run with corpus env passed in:

```bash
docker exec -e CORPUS="$CORPUS" "$CONTAINER" bash -lc 'echo "CORPUS=$CORPUS"'
```

## 7. Backfill Missing Year Values from DOI Metadata

If many `.bib` rows still have empty year (`year|`), enrich from Crossref:

```bash
docker exec "$CONTAINER" bash -lc "python3 - <<'PY'
import glob, json, os, urllib.parse, urllib.request

corpus = os.environ.get('CORPUS', 'SorghumBase')
bib_glob = f'/data/textpresso/tpcas-2/{corpus}/*/*.bib'

opener = urllib.request.build_opener()
opener.addheaders = [('User-Agent', 'textpresso-year-fix/1.0')]
cache = {}
updated = 0


def year_from_crossref(doi):
    if doi in cache:
        return cache[doi]
    url = 'https://api.crossref.org/works/' + urllib.parse.quote(doi)
    try:
        with opener.open(url, timeout=20) as r:
            msg = json.load(r).get('message', {})
        y = None
        for key in ('published-print', 'published-online', 'issued', 'created'):
            dp = msg.get(key, {}).get('date-parts')
            if dp and dp[0] and isinstance(dp[0][0], int):
                y = str(dp[0][0])
                break
        cache[doi] = y
        return y
    except Exception:
        cache[doi] = None
        return None

for bib in glob.glob(bib_glob):
    lines = open(bib, encoding='utf-8', errors='ignore').read().splitlines()
    fields = {}
    for i, line in enumerate(lines):
        if '|' in line:
            k, v = line.split('|', 1)
            fields[k] = (v, i)

    year = fields.get('year', ('', -1))[0]
    if year:
        continue

    accession = fields.get('accession', ('', -1))[0] or os.path.basename(bib)[:-4]
    candidates = []
    if '_' in accession:
        a, b = accession.split('_', 1)
        candidates.append(a + '/' + b)
    candidates.append(accession.replace('_', '/'))

    found = None
    for doi in dict.fromkeys(candidates):
        found = year_from_crossref(doi)
        if found:
            break

    if not found:
        continue

    idx = fields.get('year', ('', -1))[1]
    if idx >= 0:
        lines[idx] = f'year|{found}'
    else:
        lines.append(f'year|{found}')

    with open(bib, 'w', encoding='utf-8') as out:
        out.write('\n'.join(lines) + '\n')
    updated += 1

print('updated_year', updated)
PY"
```

## 8. Reindex After Metadata Changes

Any `.bib` updates require index refresh:

```bash
docker exec "$CONTAINER" bash -lc "\
/usr/local/bin/run_tpc_pipeline_incremental.sh \
    -c /data/textpresso/tpcas-1 \
    -C /data/textpresso/tpcas-2 \
    -i /data/textpresso/luceneindex \
  -e download_pdf,download_xml,cas1,cas2,bib,invert_img,remove_invalidated,remove_temp\
"
```

## 9. Add OBO Ontologies or Species Gene Names

Copying an OBO file into `obofiles4production` is not sufficient by itself.
Textpresso must also load the OBO into PostgreSQL, generate lexical
variations, re-annotate the target corpus, and rebuild the Lucene index.

Set the ontology filename and target corpus:

```bash
export OBO_FILE=maize_genes.obo
export CORPUS=MaizeTest
```

### 9.1 Prepare the OBO file

Each term needs a unique ID and should include the names that Textpresso is
expected to recognize. Connect terms to a common root with `is_a`.

Example:

```text
format-version: 1.2
ontology: maize_genes

[Term]
id: MAIZE_GENE:0000000
name: maize classical gene

[Term]
id: MAIZE_GENE:0000001
name: bronze1
synonym: "bz1" EXACT []
synonym: "bronze-1" EXACT []
is_a: MAIZE_GENE:0000000 ! maize classical gene
```

Notes:
- Use a lowercase, filesystem-safe filename such as `maize_genes.obo`.
- The filename stem becomes the PostgreSQL table suffix. For example,
  `maize_genes.obo` creates `tpontology_maize_genes` and
  `pcrelations_maize_genes`.
- Include abbreviations and alternate gene names as OBO `synonym` entries.
- Textpresso omits terms shorter than two characters or longer than five
  words from its lexical ontology table.

### 9.2 Install and configure the OBO file

```bash
mkdir -p "$TPC_DATA/obofiles4production" "$TPC_DATA/oboheaderfiles"
cp "/path/to/$OBO_FILE" "$TPC_DATA/obofiles4production/$OBO_FILE"
```

Add the full container path and desired tree depth to
`$TPC_HOME/textpressocentral/etc/ontology.conf`. Preserve all existing lines:

```text
/data/textpresso/obofiles4production/maize_genes.obo 3
```

The repository configuration is not bind-mounted into `/usr/local/etc`, so
copy the updated runtime configuration into the container:

```bash
docker cp "$TPC_HOME/textpressocentral/etc/ontology.conf" \
  "$CONTAINER":/usr/local/etc/ontology.conf
```

### 9.3 Rebuild ontology and lexical-variation tables

Do not run this while another ingest, annotation, or ontology process is
active:

```bash
docker exec "$CONTAINER" bash -lc "\
ps -eo pid,args | grep -E \
'run_tpc_pipeline_incremental|annotate|CreateLexica|generatelexicalvariations' \
| grep -v grep || true"
```

Back up the current ontology tables, then rebuild:

```bash
mkdir -p "$TPC_DATA/backups"
docker exec "$CONTAINER" pg_dump -F c \
  -t 'tpontology*' -t 'pcrelations*' -t ontologymembers www-data \
  > "$TPC_DATA/backups/ontology-$(date -u +%Y%m%dT%H%M%SZ).dump"

docker exec "$CONTAINER" bash -lc "CreateLexica.bash"
```

Verify that the new ontology was loaded:

```bash
OBO_STEM="${OBO_FILE%.obo}"
docker exec "$CONTAINER" bash -lc "psql -At -d www-data -c \"
select list from ontologymembers order by list;
select count(*) from tpontology_${OBO_STEM};
select count(*) from pcrelations_${OBO_STEM};
\""
```

The OBO terms are now available in the ontology-specific PostgreSQL tables.
For UI autocomplete and tools that query the unsuffixed base tables, combine
the ontology member tables:

```bash
docker exec "$CONTAINER" bash -lc "psql -v ON_ERROR_STOP=1 -d www-data <<'SQL'
do \$\$
declare
  t text;
  first_table text;
begin
  select tablename into first_table
  from pg_tables
  where schemaname = 'public' and tablename like 'pcrelations_%'
  order by tablename limit 1;

  if first_table is not null then
    execute 'drop table if exists pcrelations';
    execute format(
      'create table pcrelations as select * from %I with no data',
      first_table
    );
    for t in
      select tablename from pg_tables
      where schemaname = 'public' and tablename like 'pcrelations_%'
      order by tablename
    loop
      execute format('insert into pcrelations select * from %I', t);
    end loop;
  end if;

  select tablename into first_table
  from pg_tables
  where schemaname = 'public' and tablename like 'tpontology_%'
  order by tablename limit 1;

  if first_table is not null then
    execute 'drop table if exists tpontology';
    execute format(
      'create table tpontology as select * from %I with no data',
      first_table
    );
    for t in
      select tablename from pg_tables
      where schemaname = 'public' and tablename like 'tpontology_%'
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
```

### 9.4 Re-annotate only the target corpus

The standard `annotate` command processes every corpus beneath its CAS1 input
directory. Create a temporary input tree containing symlinks only for the
target corpus:

```bash
docker exec -e CORPUS="$CORPUS" "$CONTAINER" bash -lc '
set -euo pipefail
tmp_root=/data/textpresso/tmp/ontology-reannotate
rm -rf "$tmp_root"
mkdir -p "$tmp_root/cas1/$CORPUS"

find "/data/textpresso/tpcas-1/$CORPUS" \
  -mindepth 1 -maxdepth 1 -type d -print0 |
while IFS= read -r -d "" src; do
  accession=$(basename "$src")
  dst="$tmp_root/cas1/$CORPUS/$accession"
  mkdir -p "$dst"

  find "$src" -maxdepth 1 -name "*.tpcas.gz" -print -quit |
  while IFS= read -r cas; do
    ln -sf "$cas" "$dst/$(basename "$cas")"
  done

  if [[ -d "$src/images" ]]; then
    ln -sfn "$src/images" "$dst/images"
  fi
done

rm -f /data/textpresso/tmp/07cas1tocas2.lock
annotate \
  -c "$tmp_root/cas1" \
  -C /data/textpresso/tpcas-2 \
  -t /data/textpresso/tmp \
  -P 2 2>&1 | tee "/data/textpresso/logs/${CORPUS}-ontology-annotate.log"

rm -rf "$tmp_root"
'
```

Inspect the annotation log before indexing:

```bash
docker exec "$CONTAINER" bash -lc "\
grep -E 'No space left|Error opening output xmi|std::exception' \
  /data/textpresso/logs/${CORPUS}-ontology-annotate.log || true"
```

If any of those errors are present, do not assume every CAS2 file was
refreshed. Resolve the error and rerun annotation.

The legacy `annotate` helper temporarily creates and then drops the
unsuffixed `tpontology` and `pcrelations` tables. Repeat the base-table
materialization command from section 9.3 after annotation so curation
autocomplete remains available.

### 9.5 Rebuild the Lucene index

Check available disk space first. The indexer creates a second index before
swapping it into place, so allow room for both the current and new indexes:

```bash
df -h "$TPC_DATA"
docker exec "$CONTAINER" bash -lc "\
rm -rf /data/textpresso/luceneindex_new
rm -f /data/textpresso/tmp/12index.lock
index \
  -C /data/textpresso/tpcas-2 \
  -i /data/textpresso/luceneindex \
  2>&1 | tee /data/textpresso/logs/ontology-reindex.log"
```

Check for a failed or partial index:

```bash
docker exec "$CONTAINER" bash -lc "\
grep -E 'No space left|cannot create|error writing|std::exception' \
  /data/textpresso/logs/ontology-reindex.log || true"
```

Finally, verify the OBO file is served and the term can be found in
PostgreSQL:

```bash
curl -I "http://localhost:8080/obofiles/$OBO_FILE"

docker exec "$CONTAINER" bash -lc "psql -At -d www-data -c \"
select term, category
from tpontology
where term ilike '%bronze1%'
limit 10;
\""
```

## 10. Validate API and UI

Important: API routes are POST endpoints with JSON body.

### 10.1 Count query

```bash
docker exec "$CONTAINER" bash -lc "python3 - <<'PY'
import json, urllib.request
u='http://127.0.0.1:18080/v1/textpresso/api/get_documents_count'
payload={'query':{'keywords':'dw1','type':'document','case_sensitive':False,'corpora':['SorghumBase']}}
req=urllib.request.Request(u, data=json.dumps(payload).encode('utf-8'), headers={'Content-Type':'application/json'}, method='POST')
print(urllib.request.urlopen(req, timeout=30).read().decode('utf-8'))
PY"
```

### 10.2 Result row metadata

```bash
docker exec "$CONTAINER" bash -lc "python3 - <<'PY'
import json, urllib.request
u='http://127.0.0.1:18080/v1/textpresso/api/search_documents'
payload={
  'query': {'keywords':'dw1','type':'document','case_sensitive':False,'corpora':['SorghumBase']},
  'count': 3,
  'include_fulltext': False
}
req=urllib.request.Request(u, data=json.dumps(payload).encode('utf-8'), headers={'Content-Type':'application/json'}, method='POST')
for row in json.load(urllib.request.urlopen(req, timeout=30)):
    print(row.get('accession'), row.get('year'), row.get('journal'), row.get('title'))
PY"
```

If year/journal/title are missing in API response, inspect the corresponding `.bib` file under:
`/data/textpresso/tpcas-2/<CORPUS>/<accession>/<accession>.bib`

## 11. Common Failure Modes

1. Search returns hits but metadata columns are blank:
- Cause: `.bib` sidecars missing or placeholders.
- Fix: run sections 6, 7, then 8.

2. API returns 404:
- Cause: wrong method/path (using GET instead of POST for search/count routes) or proxy misroute.
- Fix: use POST JSON requests exactly as in section 9.

3. CAS2 OOM on large corpus:
- Cause: high parallelism.
- Fix: lower `-P` and ingest per corpus/batch.

4. Missing years after enrichment:
- Cause: DOI not resolvable via Crossref or nonstandard accession.
- Fix: fill those few manually in `.bib`, then reindex.

5. New OBO file does not appear in curation or search:
- Cause: the file was copied but `ontology.conf`, `CreateLexica.bash`,
  reannotation, or reindexing was skipped.
- Fix: run all of section 9 and verify the generated PostgreSQL tables.

6. Ontology annotation or reindex stops partway:
- Cause: concurrent pipeline processes, insufficient disk space, or an XMI
  writer error.
- Fix: stop conflicting processes, clean only stale temporary workspaces,
  confirm free disk space, inspect logs, and rerun before treating the update
  as complete.

## 12. Minimal Repro Checklist

- [ ] Docker stack is up
- [ ] PDFs staged in required folder structure
- [ ] CAS1/CAS2 pipeline completed for corpus
- [ ] `.bib` sidecars populated from metadata CSV
- [ ] Missing years enriched (optional but recommended)
- [ ] Custom OBO files configured and loaded (if applicable)
- [ ] Target corpus re-annotated after ontology changes
- [ ] Reindex completed
- [ ] API POST validation returns metadata fields
- [ ] UI table shows title/journal/year as expected
