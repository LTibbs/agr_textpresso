#!/usr/bin/env python3
"""Minimal stdlib HTTP server exposing CAS2 annotation and ontology-category data.

Lets off-server clients get the same (sentences, annotations, sections) data
that previously required direct filesystem access to this container's CAS2
files under /data/textpresso/tpcas-2, and look up the exact stored ontology
category strings --category requires -- see
sorghumbase_textpresso_implementation/bin/tpc_search_internal.py and
bin/tpc_search.py, which call these endpoints instead of reading local files
or guessing at category strings.

Runs as a separate process alongside textpressoapi, proxied by lighttpd at
/v1/textpresso/annotate and /v1/textpresso/category_search (see
lighttpd.conf). Both paths are deliberately disjoint from the existing
/v1/textpresso/api proxy rule (share only the "/v1/textpresso/" prefix, not
the "api" segment) so the new routes can't match or interact with it.

Binds to 127.0.0.1 only -- reachable exclusively through the lighttpd proxy,
never directly.

Endpoints:
  GET /v1/textpresso/annotate
      ?identifier=<doc identifier, e.g. "MaizeTest100//10.1038_srep35479/10.1038_srep35479.tpcas">
      &ontology=GO&ontology=PO           (repeatable; default: all except *_RELATED)
      &related_synonyms=1                (include *_RELATED annotations; ignored if ontology given)

      &api_key=<key>                     (also read from an "X-API-Key" header or
                                           "Authorization: Bearer <key>")

    200 -> {"sentences": [...], "annotations": [...], "sections": [...]}
    200 -> {"access_limited": true, "sentences": [...], "annotations": [...],
             "sections": [...]}
             (non-open-access document, no valid API key: same shape and keys as
              the full payload, but every sentence "text" and every annotation
              "term" is blanked to "". Offsets, categories and sections are
              kept. Governed by the open-access manifest -- see access.py. With
              no manifest file present, every document returns the full payload
              as before.)
    400 -> {"error": "..."}  (missing identifier)
    404 -> {"error": "..."}  (no CAS2 file for that identifier)
    500 -> {"error": "..."}  (parse failure)

  GET /v1/textpresso/category_search
      ?q=<free-text query, e.g. "seed">
      &ontology=PO                       (repeatable; default: all)
      &limit=20                          (default 20, capped at 100 -- or at
                                           EXPANDED_LIMIT_MAX when either
                                           relationship param below is given, so
                                           a whole closure fits in one call)
      &relationship_type=is_a            (repeatable; default: none -- literal
                                           match only. When given, each match's
                                           descendants along the named OBO
                                           relationship(s) -- e.g. is_a, part_of,
                                           regulates -- are included too.)
      &ancestor_relationship_type=is_a   (repeatable; mirrors relationship_type
                                           but walks upward -- each match's
                                           ancestors/parent terms along the
                                           named relationship(s) are included
                                           too. Combine both params to expand
                                           in both directions at once.)

    200 -> {"query": "...", "matches": [{"id","name","category","ontology","matched_on",
             "relationship_types","parent_relationship_types"}, ...],
             "truncated": bool}   (true when more matches existed than were returned)
    400 -> {"error": "..."}  (missing q)

    Each match's "relationship_types" lists the OBO relationship types (e.g.
    ["is_a", "part_of"]) that actually have children under that term in the
    indexed OBO files -- i.e. the values &relationship_type=... would need to
    return anything for that specific term. "parent_relationship_types" is the
    mirror: the relationship types that have a parent above that term, i.e.
    what &ancestor_relationship_type=... would need to return anything. There's
    no fixed list of relationship types across GO/PO/TO (see category_index.py),
    so these are how a client discovers, per term and per direction, what's
    available to expand with, without guessing.
"""

import json
import os
import socketserver
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer
from urllib.parse import urlparse, parse_qs

import access
import annotation_hygiene as hygiene
import annotation_italics as italics
import annotation_tiers as tiers_mod
import casannot
import category_index

HOST = "127.0.0.1"
PORT = int(os.environ.get("CAS_ANNOTATE_PORT", "8082"))
ANNOTATE_SUFFIX = "/v1/textpresso/annotate"
CATEGORY_SEARCH_SUFFIX = "/v1/textpresso/category_search"
LIMIT_MAX = 100              # category_search matches for a plain lookup
EXPANDED_LIMIT_MAX = 5000    # ...and with relationship_type/ancestor_relationship_type
                             # (cellular_component, GO's largest root, has ~4,100)

_CATEGORY_INDEX = None  # built once at startup, see main()
_ACCESS = None          # access.AccessControl, built once at startup, see main()
_VIEW = None            # hygiene.OntologyView over _CATEGORY_INDEX, see main()
_ITALICS = {}           # per-paper italic counts, see main()
_OBO_CLASS = None       # surface form -> OBO provenance class, see main()
_ENGLISH_FORMS = frozenset()   # gene forms that are English words, see main()
_TIERS = None           # annotation_tiers.TierIndex, or None when not configured

# Default ON since the 2026-10 rollout (the instance had not been shared
# publicly, so no published scripts depended on the unfiltered default).
# hygiene=0 returns the unfiltered set; the `hygiene` field in the response
# reports what was dropped and why. TEXTPRESSO_HYGIENE_DEFAULT=0 reverts.
_HYGIENE_DEFAULT = os.environ.get("TEXTPRESSO_HYGIENE_DEFAULT", "1")


def _load_annotation(identifier, ontology_filter, include_related,
                     clean=False, drop_sections=None, use_italics=True,
                     use_tiers=True):
    """Return {sentences, annotations, sections} for identifier, or None if no CAS2 file.

    With clean=True, applies the shared hygiene layer: references/acknowledgments
    and per-span ancestors are dropped, abbreviation synonyms are case-checked,
    and short non-italicised alias forms are removed (see
    annotation_italics.never_italicised). Every drop carries a reason and is
    reversible with hygiene=0, so nothing becomes unreachable.
    """
    path = casannot.identifier_to_cas_path(identifier)
    if not os.path.exists(path):
        return None
    sentences, annotations, sections = casannot.parse_cas_file(path)
    if ontology_filter:
        def visible(a):
            return a["ontology"] in ontology_filter
    elif not include_related:
        def visible(a):
            return not a["ontology"].endswith("_RELATED")
    else:
        visible = None
    if not clean:
        if visible is not None:
            annotations = [a for a in annotations if visible(a)]
        return {"sentences": sentences, "annotations": annotations, "sections": sections}
    # With hygiene on, everything is filtered and then the hidden channels are
    # removed (include=), so a RELATED gene synonym that is not shown still
    # counts as evidence for the english-word rule.
    out = {"sentences": sentences, "annotations": annotations, "sections": sections}

    paper_italics = italics.for_paper(_ITALICS, identifier) if use_italics else None
    annotations, dropped = hygiene.clean_annotations(
        annotations, sections,
        drop_sections=(drop_sections if drop_sections is not None
                       else hygiene.DEFAULT_DROP_SECTIONS),
        ontology_view=_VIEW,
        section_types_at=casannot.section_types_at,
        italics=paper_italics,
        italic_rule=italics.never_italicised,
        obo_class_of=_OBO_CLASS,
        english_forms=_ENGLISH_FORMS,
        include=visible,
        tiers=_TIERS if use_tiers else None,
        tier_rule=tiers_mod.tier_rule,
        sentence_at=casannot.sentence_lookup(sentences),
        # the TD long-form test always reads the body without references and
        # acknowledgments, whatever drop_sections a request asks for
        body_text=casannot.body_text(sentences, sections, hygiene.DEFAULT_DROP_SECTIONS))
    out["annotations"] = annotations
    out["hygiene"] = hygiene.hygiene_report(annotations, dropped)
    if paper_italics:
        out["hygiene"]["italic_coverage"] = italics.coverage(
            _ITALICS, annotations + dropped, identifier)
    return out


def _redact_annotation(result):
    """Blank out running text in an annotation payload for a restricted document.

    Keeps every key and every element (character offsets, ontology categories,
    section boundaries -- so a client can still see *what* is annotated and
    *where*, and existing offset-join code keeps working), but empties the
    sentence ``text`` and the verbatim matched ``term``. A valid API key returns
    the full payload instead.
    """
    return {
        "access_limited": True,
        "sentences": [dict(s, text="") for s in result["sentences"]],
        "annotations": [dict(a, term="") for a in result["annotations"]],
        "sections": result["sections"],
        **({"hygiene": result["hygiene"]} if "hygiene" in result else {}),
    }


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        try:
            sys.stderr.write("cas_annotate_server: " + (fmt % args) + "\n")
        except OSError:
            # Never let a logging failure (e.g. disk full -> ENOSPC) take
            # down the request thread before a response has been sent.
            pass

    def _send_json(self, status, payload):
        body = json.dumps(payload).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        parsed = urlparse(self.path)
        path = parsed.path.rstrip("/")
        qs = parse_qs(parsed.query)

        if path.endswith(ANNOTATE_SUFFIX):
            self._handle_annotate(qs)
        elif path.endswith(CATEGORY_SEARCH_SUFFIX):
            self._handle_category_search(qs)
        else:
            self._send_json(404, {"error": "not found"})

    def _handle_annotate(self, qs):
        identifier = (qs.get("identifier") or [None])[0]
        if not identifier:
            self._send_json(400, {"error": "missing identifier"})
            return

        ontology_filter = set(qs.get("ontology") or ()) or None
        include_related = (qs.get("related_synonyms") or ["0"])[0] == "1"
        clean = (qs.get("hygiene") or [_HYGIENE_DEFAULT])[0] == "1"
        use_italics = (qs.get("italics") or ["1"])[0] != "0"
        use_tiers = (qs.get("tiers") or ["1"])[0] != "0"
        drop_sections = set(qs.get("exclude_sections") or ()) or None

        try:
            result = _load_annotation(identifier, ontology_filter, include_related,
                                      clean=clean, drop_sections=drop_sections,
                                      use_italics=use_italics, use_tiers=use_tiers)
        except Exception as e:
            self._send_json(500, {"error": str(e)})
            return

        if result is None:
            self._send_json(404, {"error": "CAS2 file not found for identifier"})
            return

        if _ACCESS is not None and _ACCESS.enforcing:
            key = access.key_from_request(self.headers, qs)
            if not _ACCESS.is_valid_key(key):
                corpus, accession = access.identifier_parts(identifier)
                if not _ACCESS.is_open_access(corpus, accession):
                    self._send_json(200, _redact_annotation(result))
                    return

        self._send_json(200, result)

    def _handle_category_search(self, qs):
        query = (qs.get("q") or [None])[0]
        if not query:
            self._send_json(400, {"error": "missing q"})
            return

        ontology_filter = set(qs.get("ontology") or ()) or None
        relationship_types = set(qs.get("relationship_type") or ()) or None
        ancestor_relationship_types = set(qs.get("ancestor_relationship_type") or ()) or None
        # An expansion is used as a search filter, so it must be the whole
        # closure: cutting it at 100 kept the alphabetically first terms and
        # silently dropped the rest ("organelle" has 420 is_a descendants;
        # "nucleus" is #285).
        cap = (EXPANDED_LIMIT_MAX if relationship_types or ancestor_relationship_types
               else LIMIT_MAX)
        try:
            limit = max(1, min(int((qs.get("limit") or ["20"])[0]), cap))
        except ValueError:
            limit = 20

        # One extra match tells the caller whether the list was cut.
        matches = category_index.search(
            _CATEGORY_INDEX, query, ontology_filter, limit + 1, relationship_types,
            ancestor_relationship_types)
        self._send_json(200, {"query": query, "matches": matches[:limit],
                              "truncated": len(matches) > limit})


class ThreadingHTTPServer(socketserver.ThreadingMixIn, HTTPServer):
    daemon_threads = True


def main():
    global _CATEGORY_INDEX, _ACCESS

    _ACCESS = access.AccessControl()
    if _ACCESS.enforcing:
        print(f"cas_annotate_server: access control ENFORCING "
              f"(manifest: {_ACCESS.manifest_path}); non-open-access documents "
              f"return offsets/categories only to unauthenticated clients", file=sys.stderr)
        if not _ACCESS.has_keys:
            print(f"cas_annotate_server: WARNING no API keys loaded from "
                  f"{_ACCESS.api_keys_path}", file=sys.stderr)
    else:
        print(f"cas_annotate_server: access control disabled (no manifest at "
              f"{_ACCESS.manifest_path}); all documents served in full", file=sys.stderr)

    print("cas_annotate_server: building category index from OBO files...", file=sys.stderr)
    _CATEGORY_INDEX = category_index.build_index(casannot._ontology_of)
    global _VIEW, _ITALICS, _OBO_CLASS, _TIERS, _ENGLISH_FORMS
    # reuse the index already built above rather than parsing the OBO twice
    _VIEW = hygiene.OntologyView.from_category_index(_CATEGORY_INDEX)
    _ITALICS = italics.load_index(os.environ.get(
        "TEXTPRESSO_ITALICS_INDEX", "/data/textpresso/italics/MaizeOpenAccess.json"))
    # ~98 KB map, so the gene OBO is not re-parsed here; rebuild with
    # bin/build_class_map.py whenever the OBO changes (unlisted forms resolve to
    # 'unknown', which is NOT exempt from the italics rule)
    _OBO_CLASS = italics.load_class_map(os.environ.get(
        "TEXTPRESSO_OBO_CLASS_MAP", "/data/textpresso/italics/obo_short_form_classes.json"))
    print("[cas_annotate_server] italics index: %d papers" % len(_ITALICS), file=sys.stderr)
    # Gene forms that are English words ('late', 'red', 'promoter'): annotated
    # only with italic or other-synonym support. Built with
    # bin/build_class_map.py --english-out from the same OBO as the class map;
    # a missing file turns the rule off. Read at startup: restart, not re-index.
    english_path = os.environ.get("TEXTPRESSO_ENGLISH_GENE_FORMS",
                                  "/data/textpresso/italics/english_gene_forms.json")
    _ENGLISH_FORMS = italics.load_english_forms(english_path)
    if _ENGLISH_FORMS:
        print("[cas_annotate_server] english-word gene rule: %d forms from %s"
              % (len(_ENGLISH_FORMS), english_path), file=sys.stderr)
    else:
        print("[cas_annotate_server] english-word gene rule: off -- WARNING %s not found or empty"
              % english_path, file=sys.stderr)
    # The tier gate is on by default (2026-10-22): it reads the
    # match_dictionary.csv(.gz) from build_match_dictionary.py, by default the
    # annotation build's. TEXTPRESSO_MATCH_DICTIONARY names another file, or
    # "0" turns the gate off. Tiers are read at startup, so a rebuilt
    # dictionary needs a restart, not a re-index. tiers=0 on a request skips
    # the gate for that request.
    tiers_path = os.environ.get("TEXTPRESSO_MATCH_DICTIONARY",
                                "/data/textpresso/annotation_build/match_dictionary.csv.gz")
    if tiers_path not in ("", "0") and os.path.exists(tiers_path):
        _TIERS = tiers_mod.TierIndex.from_csv(tiers_path)
        print("[cas_annotate_server] tier gate: %d strings from %s" % (len(_TIERS), tiers_path),
              file=sys.stderr)
    elif tiers_path in ("", "0"):
        print("[cas_annotate_server] tier gate: off (TEXTPRESSO_MATCH_DICTIONARY=0)",
              file=sys.stderr)
    else:
        print("[cas_annotate_server] tier gate: off -- WARNING %s not found" % tiers_path,
              file=sys.stderr)
    print(f"cas_annotate_server: indexed {len(_CATEGORY_INDEX['categories'])} categories, "
          f"{len(_CATEGORY_INDEX['synonyms'])} distinct synonyms", file=sys.stderr)

    server = ThreadingHTTPServer((HOST, PORT), Handler)
    print(f"cas_annotate_server listening on {HOST}:{PORT}", file=sys.stderr)
    server.serve_forever()


if __name__ == "__main__":
    main()
