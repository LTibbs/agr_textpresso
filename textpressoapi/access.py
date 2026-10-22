"""Open-access gating shared by the Textpresso API sidecar services.

Python counterpart of access_control.h. Same two input files, same formats:

  * open-access manifest  (default: /data/textpresso/open_access_manifest.tsv,
    override with $TEXTPRESSO_OA_MANIFEST)
  * API keys file         (default:
    /data/textpresso/textpressoapi_data/api_keys.txt, override with
    $TEXTPRESSO_API_KEYS_FILE)

If the manifest file is absent, `enforcing` is False and callers must not
restrict anything -- every document is treated as open access. This mirrors
the C++ service and keeps the feature opt-in.

Manifest lines (whitespace-separated, '#' comments, blank lines ignored):

    @default             open
    @corpus:SorghumBase  closed
    10.1093/genetics/iyaf266   open
    10.1021_acs.jafc.5c10234   closed

status tokens: open|oa|true|1|yes  and  closed|restricted|false|0|no
Accessions compare case-insensitively with '/' normalised to '_'.
"""

import os

DEFAULT_MANIFEST = os.environ.get(
    "TEXTPRESSO_OA_MANIFEST", "/data/textpresso/open_access_manifest.tsv")
DEFAULT_API_KEYS = os.environ.get(
    "TEXTPRESSO_API_KEYS_FILE", "/data/textpresso/textpressoapi_data/api_keys.txt")

_OPEN_WORDS = {"open", "oa", "true", "1", "yes"}
_CLOSED_WORDS = {"closed", "restricted", "false", "0", "no"}


def _norm_accession(s):
    return (s or "").strip().lower().replace("/", "_")


def _status_to_open(raw, fallback=True):
    v = raw.strip().lower()
    if v in _OPEN_WORDS:
        return True
    if v in _CLOSED_WORDS:
        return False
    return fallback


def identifier_parts(identifier):
    """Split an API document identifier into (corpus, accession).

    "MaizeTest100//10.1038_srep35479/10.1038_srep35479.tpcas"
        -> ("MaizeTest100", "10.1038_srep35479")
    """
    segments = [seg for seg in identifier.replace("//", "/").split("/") if seg]
    if not segments:
        return "", ""
    corpus = segments[0]
    if len(segments) >= 3:
        accession = segments[-2]
    else:
        accession = segments[-1]
        for suffix in (".tpcas.gz", ".tpcas"):
            if accession.endswith(suffix):
                accession = accession[: -len(suffix)]
                break
    return corpus, accession


class AccessControl:
    def __init__(self, manifest_path=None, api_keys_path=None):
        self.manifest_path = manifest_path or DEFAULT_MANIFEST
        self.api_keys_path = api_keys_path or DEFAULT_API_KEYS
        self._enforcing = False
        self._default_open = True
        self._corpus = {}
        self._accession = {}
        self._keys = set()
        self._load()

    def _load(self):
        try:
            with open(self.manifest_path) as fh:
                lines = fh.readlines()
        except OSError:
            self._enforcing = False
            return
        for line in lines:
            t = line.strip()
            if not t or t.startswith("#"):
                continue
            parts = t.split(None, 1)
            if len(parts) != 2:
                continue
            key, val = parts[0], parts[1].strip()
            lkey = key.lower()
            if lkey in ("@default", "default:"):
                self._default_open = _status_to_open(val, self._default_open)
            elif lkey.startswith("@corpus:"):
                self._corpus[key[len("@corpus:"):].lower()] = _status_to_open(val)
            else:
                self._accession[_norm_accession(key)] = _status_to_open(val)
        self._enforcing = True

        try:
            with open(self.api_keys_path) as fh:
                for line in fh:
                    t = line.strip()
                    if not t or t.startswith("#"):
                        continue
                    self._keys.add(t.split()[0])
        except OSError:
            pass

    @property
    def enforcing(self):
        return self._enforcing

    @property
    def has_keys(self):
        return bool(self._keys)

    def is_valid_key(self, key):
        return bool(key) and key in self._keys

    def is_open_access(self, corpus, accession):
        acc = _norm_accession(accession)
        if acc in self._accession:
            return self._accession[acc]
        if corpus and corpus.lower() in self._corpus:
            return self._corpus[corpus.lower()]
        return self._default_open


def key_from_request(headers, qs):
    """Extract an API key: X-API-Key header, Authorization: Bearer, then ?api_key=."""
    key = headers.get("X-API-Key")
    if key:
        return key.strip()
    auth = headers.get("Authorization", "") or ""
    if auth.startswith("Bearer "):
        return auth[len("Bearer "):].strip()
    val = (qs.get("api_key") or [None])[0]
    return val.strip() if val else ""
