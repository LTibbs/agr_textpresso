# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this repository is

Textpresso is a full-text literature search and curation platform for model organism databases (MODs). This repo contains the full system: a C++ core library, a REST API, a web UI, and a Python/shell data pipeline that ingests PDFs and XML from the Alliance of Genome Resources (AGR) curation API and produces searchable, annotated corpora.

It is deployed as one Docker container per MOD (FlyBase, Zebrafish, Mouse, WormBase, SGD), fronted by a shared Nginx reverse proxy.

The companion repo (`maizegdb_textpresso_implementation`) uses the classifier library to decide which papers are relevant before they enter this ingest pipeline.

## Build and run

### Start the full system (Docker Compose)
```bash
cp .env_example .env   # fill in credentials before starting
docker compose up
```

The container runs `start_textpresso.sh` as its entrypoint, which:
1. Sets up cron
2. Creates application directories and symlink aliases (`convert_text`, `tokenize`, `annotate`, `index`)
3. Starts PostgreSQL and loads the stopwords database
4. Backgrounds lexica creation and webserver startup
5. Starts the `textpressoapi` REST server on port 18080

Services after startup:
- Web UI: `http://localhost:80`
- REST API: `http://localhost:18080`

### Build the C++ components inside the container
```bash
./initialize.sh -t    # compile textpressocentral, textpressoapi, tpctools
./initialize.sh -p    # start PostgreSQL + create users + restore www-data DB
./initialize.sh -l    # create lexica
./initialize.sh -w    # start webserver
./initialize.sh -a    # run all steps
```

### Run the incremental build manually
```bash
MOD=WB /data/textpresso/tpctools/incremental_build.sh
```

## Architecture

### Processing pipeline (data flow)

```
AGR Curation API
      │
      ▼  (tpctools/getPdfBiblio/download_pdfs_bib_files.py)
  raw_files/pdf/{organism}/   raw_files/bib/{organism}/
      │
      ▼  (articles2cas  ≡  symlink: tokenize)
  tpcas-1/   ← CAS-1 files: tokenized, no annotations
      │
      ▼  (runAECpp  ≡  symlink: annotate)
  tpcas-2/   ← CAS-2 files: annotated via UIMA + PostgreSQL ontologies
      │
      ▼  (indexmerger  ≡  symlink: index)
  luceneindex/   ← Lucene full-text index
      │
      ├─▶  textpressocentral  (Web UI, Wt C++ framework)
      └─▶  textpressoapi      (REST API, Crow C++ framework)
```

CAS files are gzip-compressed UIMA XMI documents. `.tpcas` = CAS-1 (tokenized only); `.tpcas.gz` after CAS-2 annotation.

### Key filesystem paths (inside container)

| Path | Contents |
|------|----------|
| `/data/textpresso/raw_files/pdf/{organism}/` | Downloaded PDFs |
| `/data/textpresso/raw_files/bib/{organism}/` | Bibliography `.bib` files |
| `/data/textpresso/raw_files/txt/{organism}/` | Plaintext fallback (when PDF extraction fails) |
| `/data/textpresso/tpcas-1/` | CAS-1 intermediate files |
| `/data/textpresso/tpcas-2/` | CAS-2 annotated files |
| `/data/textpresso/luceneindex/` | Production Lucene index |
| `/data/textpresso/luceneindex_new/` | New index being built (swapped atomically) |
| `/data/textpresso/obofiles4production/` | OBO category files used by annotator |
| `/data/textpresso/textpressoapi_data/tokens.db` | SQLite API auth tokens |
| `/data/textpresso/open_access_manifest.tsv` | Optional. Per-accession / per-corpus open-access status. Absent → every document served in full, no gating (default) |
| `/data/textpresso/textpressoapi_data/api_keys.txt` | Optional. API keys (one per line) that lift the open-access limit |

Host volume `${TEXTPRESSO_DATA_DIR}` is mounted at `/data/textpresso`.

### Databases

**PostgreSQL** (`www-data` database):
- Stores ontology lexica used by the UIMA annotator (`TpLexiconAnnotatorFromPg`) during CAS-2 generation
- Populated from OBO files via the lexica creation step
- Restored from `stopwords.postgres.tar.gz` on container startup
- Users: `www-data` (app), `textpresso`, `mueller`, `root`

**SQLite** (`tokens.db`):
- Two-column table: `token`, `superuser`
- Used only by `textpressoapi` for HTTP request authentication

**Lucene index**:
- Hierarchical structure: per-corpus sub-indices merged into master indices
- Up to 1,000,000 papers per master index; multiple masters merged at the end
- Configuration stored in `cc.cfg` inside the index directory

### C++ components

**`libtpc/`** — core library shared by all components:
- PDF → CAS-1 conversion (`Pdf2Tpcas/`)
- OBO ontology parsing (`OboFileAnalyzer/`, `CreateListofOntologies/`)
- UIMA annotators: `TdTokenizer`, `TpLexiconAnnotator`, `TpLexiconAnnotatorFromPg`
- Lucene index management (`IndexManager`)

**`textpressoapi/`** — Crow-based REST API:
- Full-text search with filters: keywords, categories, year, author, accession, journal, paper type, sentence section (abstract, methods, results, etc.)
- Token authentication (superuser flag for elevated access)
- Official endpoint: `https://textpressocentral.org:18080/textpresso/api/1.0/`

**`textpressocentral/`** — Wt-based web application:
- Search, curation, and ML/text mining interfaces
- Runs via FastCGI on Unix socket `/usr/wt/socket`, fronted by lighttpd

### Automation (cron)

| Schedule | Script | Purpose |
|----------|--------|---------|
| Weekly (Sun 19:00) | `incremental_build.sh` | Download new papers, ingest, reindex |
| Monthly (1st Tue) | `check_and_run_ontology_update.sh` → `update_ontology.sh` | Refresh OBO categories + reindex |

**`incremental_build.sh`** is the most important operational script. It:
1. Downloads PDFs and bib files from AGR API (with Cognito auth)
2. Converts PDFs to text if extraction fails
3. Runs tokenize (CAS-1) in parallel (4 workers)
4. Runs annotate (CAS-2) in parallel (2 workers)
5. Rsyncs staging dirs to production atomically
6. Runs full reindex
7. Emails a report via `send_report.py`

Batches with fewer than 4 PDFs are carried over to the next week's run.

### Python scripts

**`tpctools/getPdfBiblio/download_pdfs_bib_files.py`** — the only script that talks to the AGR API:
- Authenticates via `agr_cognito_py`
- Fetches reference list, downloads PDFs (PMC preferred), downloads `.bib` files
- Key args: `-m MOD`, `-d DAYS_AGO` (incremental window), `-p OUTPUT_PATH`

**`tpctools/getOntologies/get_categories.py`** — orchestrates ontology refresh:
- Calls `entities.py` (fetches genes, alleles, proteins, fish from AGR curation API)
- Calls `ontology.py` (downloads GO, DOID, ChEBI, SO, MOD-specific OBOs)
- MOD-specific entity sets are hardcoded (WB gets genes + alleles + proteins; ZFIN gets fish; etc.)

**`tpctools/run_tpc_pipeline_incremental.py`** — legacy comprehensive pipeline (predecessor to `incremental_build.sh`); still useful as a reference for how the pipeline steps connect.

**`tpctools/send_report.py`** — parses `/tmp/incremental_build.log` and emails an HTML summary table.

### Authentication

The system uses **AWS Cognito** (replaced legacy Okta). The `agr_cognito_py` library handles token generation. Required env vars: `COGNITO_REGION`, `COGNITO_USER_POOL_ID`, `COGNITO_CLIENT_ID`, `COGNITO_CLIENT_SECRET`.

API access to `textpressoapi` uses a separate SQLite token table (not Cognito).

**Open-access gating** (`textpressoapi/access_control.h`, `textpressoapi/access.py`):
an optional layer, off unless an open-access manifest file exists. When on, a
request with no valid API key gets a reduced payload (metadata + abstract +
first N matching sentences) for any document the manifest marks non-open-access;
open-access documents and API-key-bearing requests are unaffected. Enforced by
both `textpressoapi` (`search_documents`, `get_category_matches_document_fulltext`)
and `cas_annotate_server.py` (`/annotate`). Keys (`X-API-Key` / `Authorization:
Bearer` / body `api_key`) live in `api_keys.txt`, separate from `tokens.db`.
See `textpressoapi/docs/search_documents.rst` and
`textpressoapi/open_access_manifest.tsv.example`.

### Multi-MOD deployment

One container per MOD, each listening on a different port (81–85). The `reverse_proxy/` service (Nginx) routes traffic by subdomain:

| Subdomain | MOD | Port |
|-----------|-----|------|
| `fb-textpresso.alliancegenome.org` | FlyBase | 81 |
| `zfin-textpresso.alliancegenome.org` | ZFIN | 82 |
| `mgi-textpresso.alliancegenome.org` | MGI | 83 |
| `wb-textpresso.alliancegenome.org` | WormBase | 84 |
| `sgd-textpresso.alliancegenome.org` | SGD | 85 |

The `MOD` environment variable (e.g., `WB`, `MGI`) controls which organism's data each container processes.

## Required environment variables

See `.env_example` for the full list. The critical ones:

- `MOD` — organism identifier (WB, MGI, ZFIN, FB, SGD)
- `TEXTPRESSO_DATA_DIR` — host path mounted at `/data/textpresso`
- `API_URL` — AGR curation API base URL
- `COGNITO_*` — AWS Cognito credentials for AGR API auth
- `AWS_ACCESS_KEY_ID` / `AWS_SECRET_ACCESS_KEY` — AWS credentials
- `PERSISTENT_STORE_DB_*` — PostgreSQL connection for the persistent store

## Relationship to maizegdb_textpresso_implementation

That repo's `TextpressoDocumentClassifier` (scikit-learn based) classifies papers before they enter this pipeline. Classified paper lists feed into `download_pdfs_bib_files.py` as the input corpus. This repo does not call the classifier directly — the handoff is at the file/CSV level.
