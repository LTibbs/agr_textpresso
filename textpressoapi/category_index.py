"""Build a searchable index of Textpresso ontology categories from OBO files.

Used by cas_annotate_server.py's /v1/textpresso/category_search endpoint to
let clients look up the exact stored category string (e.g. "seed
(PO:0009010)") that --category requires, instead of guessing.

Reads the same OBO files the annotator/indexer were built from -- the set
listed in ontology.conf, under obofiles4production/ -- and reconstructs each
category's canonical string exactly as it's stored in the Lucene index:
"<name> (<id>)". Regex patterns for OBO [Term] blocks are lifted from
bin/ontology_synonym_audit.py (sorghumbase_textpresso_implementation repo),
which already parses this same format for a different purpose.
"""

import os
import re

ONTOLOGY_CONF_DEFAULT = "/usr/local/etc/ontology.conf"
OBO_DIR_DEFAULT = "/data/textpresso/obofiles4production"

_STANZA_RE = re.compile(r"^\[(\S+)\]\s*$")
_ID_RE = re.compile(r"^id:\s*(\S+)")
_NAME_RE = re.compile(r"^name:\s*(.*)$")
_SYN_RE = re.compile(r'^synonym:\s*"([^"]*)"')
_OBSOLETE_RE = re.compile(r"^is_obsolete:\s*true\b")
_IS_A_RE = re.compile(r"^is_a:\s*(\S+)")
_REL_RE = re.compile(r"^relationship:\s*(\S+)\s+(\S+)")


def _iter_obo_terms(obo_path):
    """Yield {id, name, synonyms, parents} for each non-obsolete [Term] block in an OBO file.

    parents maps relationship type ("is_a", "part_of", "regulates", ...) to the
    list of parent term ids reached via that relationship, so callers can build
    typed child->parent (and reverse, parent->child) edges for closure queries.
    """
    stanza = None
    term_id = term_name = None
    synonyms = []
    parents = {}
    obsolete = False

    def _flush():
        if stanza == "Term" and term_id and term_name and not obsolete:
            return {"id": term_id, "name": term_name, "synonyms": synonyms, "parents": parents}
        return None

    with open(obo_path, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            line = line.rstrip("\n")
            m = _STANZA_RE.match(line)
            if m:
                rec = _flush()
                if rec:
                    yield rec
                stanza = m.group(1)
                term_id = term_name = None
                synonyms = []
                parents = {}
                obsolete = False
                continue
            if stanza != "Term":
                continue
            m = _ID_RE.match(line)
            if m:
                term_id = m.group(1)
                continue
            m = _NAME_RE.match(line)
            if m:
                term_name = m.group(1)
                continue
            m = _SYN_RE.match(line)
            if m:
                synonyms.append(m.group(1))
                continue
            m = _IS_A_RE.match(line)
            if m:
                parents.setdefault("is_a", []).append(m.group(1))
                continue
            m = _REL_RE.match(line)
            if m:
                parents.setdefault(m.group(1), []).append(m.group(2))
                continue
            if _OBSOLETE_RE.match(line):
                obsolete = True
    rec = _flush()
    if rec:
        yield rec


def _active_obo_paths(ontology_conf=ONTOLOGY_CONF_DEFAULT, obo_dir=OBO_DIR_DEFAULT):
    """Return the list of OBO file paths listed in ontology.conf.

    Each line's first whitespace-separated token is the OBO path (remaining
    tokens are subset names, e.g. "go.obo 6 goslim_generic ..."). Falls back
    to every *.obo in obo_dir if ontology.conf isn't readable, so the index
    still builds (possibly over-inclusive) rather than coming up empty.
    """
    try:
        with open(ontology_conf) as fh:
            paths = [line.split()[0] for line in fh if line.strip()]
        if paths:
            return paths
    except OSError:
        pass
    import glob
    return sorted(glob.glob(os.path.join(obo_dir, "*.obo")))


def build_index(ontology_of, ontology_conf=ONTOLOGY_CONF_DEFAULT, obo_dir=OBO_DIR_DEFAULT):
    """Build the category index.

    ontology_of -- casannot._ontology_of, so a category string ("name (id)")
    gets classified into GO/PO/TO/MAIZE_GENES/OTHER exactly the same way an
    actual CAS2 annotation would be, for consistency.

    Returns {"categories": [...], "synonyms": {lowercased_text: [record, ...]},
    "by_id": {id: record}, "children_by_type": {rel_type: {parent_id: [child_id, ...]}},
    "parents_by_type": {rel_type: {child_id: [parent_id, ...]}},
    "relationship_types_by_id": {id: [rel_type, ...]},
    "parent_relationship_types_by_id": {id: [rel_type, ...]},
    "surface_forms": {lowercased_text: {original-case spelling, ...}}}
    where each record is {"id", "name", "category", "ontology"}. surface_forms
    keeps the case the OBO stores for every name and synonym, which records
    drop; annotation_hygiene.OntologyView needs it for its abbreviation case check.

    children_by_type holds, per relationship type (is_a, part_of, regulates, ...),
    the reverse (parent -> children) edges needed to walk descendant closures --
    see _descendants(). parents_by_type is the forward direction (child ->
    parents), straight off the OBO file, needed to walk ancestor closures --
    see _ancestors(). relationship_types_by_id / parent_relationship_types_by_id
    are their respective inverse indexes: for a given term, which relationship
    types actually have children/parents under/above it (so callers can
    discover, per term, which --expand-relationship-type values would return
    anything in each direction -- see relationship_types_for() and
    parent_relationship_types_for()) -- since there's no fixed list of
    relationship types across GO/PO/TO, this is the only way to know in
    advance.
    """
    categories = []
    synonym_index = {}
    by_id = {}
    children_by_type = {}
    parents_by_type = {}
    surface_forms = {}
    seen_ids = set()

    for obo_path in _active_obo_paths(ontology_conf, obo_dir):
        if not os.path.exists(obo_path):
            continue
        for term in _iter_obo_terms(obo_path):
            if term["id"] in seen_ids:
                continue
            seen_ids.add(term["id"])
            category = f"{term['name']} ({term['id']})"
            record = {
                "id": term["id"],
                "name": term["name"],
                "category": category,
                "ontology": ontology_of(category),
            }
            categories.append(record)
            by_id[term["id"]] = record
            surface_forms.setdefault(term["name"].lower(), set()).add(term["name"])
            for syn in term["synonyms"]:
                synonym_index.setdefault(syn.lower(), []).append(record)
                surface_forms.setdefault(syn.lower(), set()).add(syn)
            for rel_type, parent_ids in term["parents"].items():
                for parent_id in parent_ids:
                    children_by_type.setdefault(rel_type, {}).setdefault(
                        parent_id, []).append(term["id"])
                    parents_by_type.setdefault(rel_type, {}).setdefault(
                        term["id"], []).append(parent_id)

    def _invert(edges_by_type):
        inverted = {}
        for rel_type, node_map in edges_by_type.items():
            for node_id in node_map:
                inverted.setdefault(node_id, set()).add(rel_type)
        return {node_id: sorted(rel_types) for node_id, rel_types in inverted.items()}

    return {
        "categories": categories,
        "synonyms": synonym_index,
        "by_id": by_id,
        "children_by_type": children_by_type,
        "parents_by_type": parents_by_type,
        "relationship_types_by_id": _invert(children_by_type),
        "parent_relationship_types_by_id": _invert(parents_by_type),
        "surface_forms": surface_forms,
    }


def relationship_types_for(index, term_id):
    """Return the sorted list of relationship types that have at least one
    child under term_id (e.g. ["is_a", "part_of"]) -- i.e. the
    --expand-relationship-type values that would actually expand this term
    into descendants. Empty list for leaf terms or unknown ids.
    """
    return list(index.get("relationship_types_by_id", {}).get(term_id, ()))


def parent_relationship_types_for(index, term_id):
    """Return the sorted list of relationship types that have at least one
    parent above term_id (e.g. ["is_a", "part_of"]) -- i.e. the relationship
    types that would actually expand this term into ancestors. Empty list for
    root terms (no parents) or unknown ids.
    """
    return list(index.get("parent_relationship_types_by_id", {}).get(term_id, ()))


def _walk(edges_by_type, term_id, relationship_types):
    rel_key = frozenset(relationship_types)
    seen = set()
    stack = [term_id]
    while stack:
        current = stack.pop()
        for rel_type in rel_key:
            for next_id in edges_by_type.get(rel_type, {}).get(current, ()):
                if next_id not in seen:
                    seen.add(next_id)
                    stack.append(next_id)
    return seen


def _descendants(index, term_id, relationship_types):
    """Return the set of term ids reachable from term_id via the given relationship
    types, following parent -> child edges transitively (e.g. all is_a/part_of
    descendants). Memoized per (term_id, relationship_types) on the index itself,
    since the index is immutable after build_index() and rebuilt only at startup.
    """
    rel_key = frozenset(relationship_types)
    cache = index.setdefault("_descendant_cache", {})
    cache_key = (term_id, rel_key)
    if cache_key in cache:
        return cache[cache_key]

    seen = _walk(index.get("children_by_type", {}), term_id, rel_key)
    cache[cache_key] = seen
    return seen


def _ancestors(index, term_id, relationship_types):
    """Return the set of term ids reachable from term_id by following child ->
    parent edges transitively (e.g. all is_a/part_of ancestors) -- the mirror
    of _descendants(). Memoized the same way.
    """
    rel_key = frozenset(relationship_types)
    cache = index.setdefault("_ancestor_cache", {})
    cache_key = (term_id, rel_key)
    if cache_key in cache:
        return cache[cache_key]

    seen = _walk(index.get("parents_by_type", {}), term_id, rel_key)
    cache[cache_key] = seen
    return seen


def search(index, query, ontology_filter=None, limit=20, relationship_types=None,
           ancestor_relationship_types=None):
    """Rank category matches for a free-text query.

    Match quality, best first: exact name/id/category match, name starts
    with query, exact synonym match, name contains query, synonym contains
    query. Deduplicates by category id (keeps the best rank seen), then
    sorts by (rank, name) and truncates to limit.

    relationship_types -- optional iterable of OBO relationship types (e.g.
    {"is_a", "part_of"}). When given, each directly matched term's descendants
    along those relationship types are added too (see _descendants()), ranked
    just after the direct matches so exact/prefix/substring hits still come
    first. Omitted/None preserves prior literal-match-only behavior.

    ancestor_relationship_types -- same idea, mirrored: when given, each
    directly matched term's ancestors (parent terms, e.g. "seed" is an is_a
    ancestor of "hypocotyl") along those relationship types are added too
    (see _ancestors()), tagged "+ancestor" instead of "+descendant" so the
    two directions are distinguishable in matched_on. Can be combined with
    relationship_types to expand both directions from the same query.
    """
    q = query.strip().lower()
    if not q:
        return []

    best = {}  # id -> (rank, record, matched_on)

    def _consider(rec, rank, matched_on):
        rid = rec["id"]
        if rid not in best or rank < best[rid][0]:
            best[rid] = (rank, rec, matched_on)

    for rec in index["categories"]:
        name_l = rec["name"].lower()
        if name_l == q or rec["id"].lower() == q or rec["category"].lower() == q:
            _consider(rec, 0, "exact")
        elif name_l.startswith(q):
            _consider(rec, 1, "name_prefix")
        elif q in name_l:
            _consider(rec, 3, "name_substring")

    for syn_text, recs in index["synonyms"].items():
        if syn_text == q:
            for rec in recs:
                _consider(rec, 2, "synonym_exact")
        elif q in syn_text:
            for rec in recs:
                _consider(rec, 4, "synonym_substring")

    if relationship_types or ancestor_relationship_types:
        by_id = index.get("by_id", {})
        for term_id, (rank, rec, matched_on) in list(best.items()):
            if relationship_types:
                for descendant_id in _descendants(index, term_id, relationship_types):
                    descendant_rec = by_id.get(descendant_id)
                    if descendant_rec is None:
                        continue
                    _consider(descendant_rec, rank + 5, f"{matched_on}+descendant")
            if ancestor_relationship_types:
                for ancestor_id in _ancestors(index, term_id, ancestor_relationship_types):
                    ancestor_rec = by_id.get(ancestor_id)
                    if ancestor_rec is None:
                        continue
                    _consider(ancestor_rec, rank + 5, f"{matched_on}+ancestor")

    results = list(best.values())
    if ontology_filter:
        results = [r for r in results if r[1]["ontology"] in ontology_filter]
    results.sort(key=lambda r: (r[0], r[1]["name"]))

    return [
        {"id": rec["id"], "name": rec["name"], "category": rec["category"],
         "ontology": rec["ontology"], "matched_on": matched_on,
         "relationship_types": relationship_types_for(index, rec["id"]),
         "parent_relationship_types": parent_relationship_types_for(index, rec["id"])}
        for _, rec, matched_on in results[:limit]
    ]
