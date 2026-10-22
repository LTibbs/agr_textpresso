"""Tests for category_index.py's OBO parsing and relationship-type expansion.

Run with: python3 -m pytest textpressoapi/tests/test_category_index.py
(from the repo root, or anywhere with textpressoapi/ on sys.path).
"""

import os
import sys
import tempfile

import pytest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import category_index

# A small synthetic PO-shaped hierarchy:
#
#   seed (PO:0009010)
#     -- is_a -->     hypocotyl (PO:0020123)
#     -- part_of -->  seed coat (PO:0030124)
#                        -- is_a --> seed coat epidermis (PO:0040001)
#
# So from "seed": is_a reaches only hypocotyl; is_a+part_of reaches all three.
_OBO_TEXT = """format-version: 1.2

[Term]
id: PO:0009010
name: seed
synonym: "seeds" EXACT []

[Term]
id: PO:0020123
name: hypocotyl
is_a: PO:0009010 ! seed

[Term]
id: PO:0030124
name: seed coat
relationship: part_of PO:0009010 ! seed

[Term]
id: PO:0040001
name: seed coat epidermis
is_a: PO:0030124 ! seed coat

[Term]
id: PO:0099999
name: obsolete term
is_a: PO:0009010 ! seed
is_obsolete: true
"""


@pytest.fixture
def obo_path(tmp_path):
    path = tmp_path / "test.obo"
    path.write_text(_OBO_TEXT)
    return str(path)


@pytest.fixture
def index(obo_path, monkeypatch):
    monkeypatch.setattr(category_index, "_active_obo_paths", lambda *a, **k: [obo_path])
    return category_index.build_index(ontology_of=lambda category: "PO")


def test_iter_obo_terms_captures_relationships(obo_path):
    terms = {t["id"]: t for t in category_index._iter_obo_terms(obo_path)}

    assert terms["PO:0009010"]["parents"] == {}
    assert terms["PO:0020123"]["parents"] == {"is_a": ["PO:0009010"]}
    assert terms["PO:0030124"]["parents"] == {"part_of": ["PO:0009010"]}
    assert terms["PO:0040001"]["parents"] == {"is_a": ["PO:0030124"]}
    # obsolete terms are dropped entirely, same as before this feature
    assert "PO:0099999" not in terms


def test_build_index_children_by_type(index):
    assert index["children_by_type"]["is_a"] == {
        "PO:0009010": ["PO:0020123"],
        "PO:0030124": ["PO:0040001"],
    }
    assert index["children_by_type"]["part_of"] == {"PO:0009010": ["PO:0030124"]}
    assert set(index["by_id"]) == {"PO:0009010", "PO:0020123", "PO:0030124", "PO:0040001"}


def test_build_index_surface_forms_keep_stored_case(tmp_path, monkeypatch):
    path = tmp_path / "go.obo"
    path.write_text(_OBO_TEXT + '\n[Term]\nid: GO:0060292\nname: long term synaptic depression\n'
                    'synonym: "LTD" RELATED []\n')
    monkeypatch.setattr(category_index, "_active_obo_paths", lambda *a, **k: [str(path)])
    index = category_index.build_index(ontology_of=lambda category: "GO")
    assert index["surface_forms"]["ltd"] == {"LTD"}
    assert index["surface_forms"]["seed"] == {"seed"}
    assert index["surface_forms"]["seeds"] == {"seeds"}
    # records themselves are unchanged
    assert set(index["synonyms"]["ltd"][0]) == {"id", "name", "category", "ontology"}


def test_descendants_single_relationship_type(index):
    assert category_index._descendants(index, "PO:0009010", {"is_a"}) == {"PO:0020123"}


def test_descendants_multiple_relationship_types_transitive(index):
    # part_of reaches "seed coat", then is_a from there reaches "seed coat epidermis" too
    assert category_index._descendants(index, "PO:0009010", {"is_a", "part_of"}) == {
        "PO:0020123", "PO:0030124", "PO:0040001",
    }


def test_descendants_unknown_relationship_type_is_empty(index):
    assert category_index._descendants(index, "PO:0009010", {"regulates"}) == set()


def test_search_without_relationship_types_is_unchanged(index):
    results = category_index.search(index, "seed")
    ids = {r["id"] for r in results}
    assert ids == {"PO:0009010", "PO:0030124", "PO:0040001"}
    exact = next(r for r in results if r["id"] == "PO:0009010")
    assert exact["matched_on"] == "exact"


def test_search_with_is_a_expansion_adds_descendant(index):
    results = category_index.search(index, "seed", relationship_types={"is_a"})
    ids = {r["id"] for r in results}
    assert ids == {"PO:0009010", "PO:0030124", "PO:0040001", "PO:0020123"}
    hypocotyl = next(r for r in results if r["id"] == "PO:0020123")
    assert hypocotyl["matched_on"] == "exact+descendant"


def test_search_expansion_never_downgrades_a_direct_match(index):
    # "seed coat epidermis" already matches "seed" by name_prefix; expansion
    # via is_a+part_of also reaches it as a descendant, but the better
    # (direct) rank/matched_on must win, not be overwritten by the expansion.
    results = category_index.search(index, "seed", relationship_types={"is_a", "part_of"})
    epidermis = next(r for r in results if r["id"] == "PO:0040001")
    assert epidermis["matched_on"] == "name_prefix"


def test_search_respects_ontology_filter_with_expansion(index):
    results = category_index.search(
        index, "seed", ontology_filter={"OTHER"}, relationship_types={"is_a"})
    assert results == []


def test_relationship_types_for_reports_available_expansions(index):
    # "seed" has both an is_a child (hypocotyl) and a part_of child (seed coat)
    assert category_index.relationship_types_for(index, "PO:0009010") == ["is_a", "part_of"]
    # "seed coat" only has an is_a child (seed coat epidermis)
    assert category_index.relationship_types_for(index, "PO:0030124") == ["is_a"]


def test_relationship_types_for_leaf_term_is_empty(index):
    assert category_index.relationship_types_for(index, "PO:0040001") == []


def test_relationship_types_for_unknown_id_is_empty(index):
    assert category_index.relationship_types_for(index, "PO:9999999") == []


def test_search_results_include_relationship_types(index):
    results = category_index.search(index, "seed")
    by_id = {r["id"]: r for r in results}
    assert by_id["PO:0009010"]["relationship_types"] == ["is_a", "part_of"]
    assert by_id["PO:0030124"]["relationship_types"] == ["is_a"]
    assert by_id["PO:0040001"]["relationship_types"] == []


def test_ancestors_single_relationship_type(index):
    assert category_index._ancestors(index, "PO:0040001", {"is_a"}) == {"PO:0030124"}


def test_ancestors_multiple_relationship_types_transitive(index):
    # "seed coat epidermis" --is_a--> "seed coat" --part_of--> "seed"
    assert category_index._ancestors(index, "PO:0040001", {"is_a", "part_of"}) == {
        "PO:0030124", "PO:0009010",
    }


def test_ancestors_root_term_is_empty(index):
    assert category_index._ancestors(index, "PO:0009010", {"is_a", "part_of"}) == set()


def test_parent_relationship_types_for_reports_available_expansions(index):
    assert category_index.parent_relationship_types_for(index, "PO:0040001") == ["is_a"]
    assert category_index.parent_relationship_types_for(index, "PO:0020123") == ["is_a"]
    assert category_index.parent_relationship_types_for(index, "PO:0030124") == ["part_of"]


def test_parent_relationship_types_for_root_term_is_empty(index):
    assert category_index.parent_relationship_types_for(index, "PO:0009010") == []


def test_parent_relationship_types_for_unknown_id_is_empty(index):
    assert category_index.parent_relationship_types_for(index, "PO:9999999") == []


def test_search_with_ancestor_expansion_adds_ancestor(index):
    results = category_index.search(
        index, "seed coat epidermis", ancestor_relationship_types={"is_a", "part_of"})
    ids = {r["id"] for r in results}
    assert ids == {"PO:0040001", "PO:0030124", "PO:0009010"}
    seed = next(r for r in results if r["id"] == "PO:0009010")
    assert seed["matched_on"] == "exact+ancestor"


def test_search_can_expand_both_directions_at_once(index):
    results = category_index.search(
        index, "seed coat", relationship_types={"is_a"}, ancestor_relationship_types={"part_of"})
    ids = {r["id"] for r in results}
    # "seed coat" itself, its is_a descendant "seed coat epidermis",
    # and its part_of ancestor "seed"
    assert ids == {"PO:0030124", "PO:0040001", "PO:0009010"}


def test_search_results_include_parent_relationship_types(index):
    results = category_index.search(index, "seed")
    by_id = {r["id"]: r for r in results}
    assert by_id["PO:0009010"]["parent_relationship_types"] == []
    assert by_id["PO:0030124"]["parent_relationship_types"] == ["part_of"]
    assert by_id["PO:0040001"]["parent_relationship_types"] == ["is_a"]


def test_iter_obo_terms_resolves_obo_escapes(tmp_path):
    # ChEBI escapes braces in names; an escaped quote must not end a synonym.
    p = tmp_path / "esc.obo"
    p.write_text('[Term]\nid: CHEBI:1\nname: 8-\\{(Z)-pent-2-en-1-yl\\}octanoic acid\n'
                 'synonym: "the \\"blue\\" form" EXACT []\n', encoding="utf-8")
    (term,) = list(category_index._iter_obo_terms(str(p)))
    assert term["name"] == "8-{(Z)-pent-2-en-1-yl}octanoic acid"
    assert term["synonyms"] == ['the "blue" form']


def test_annotation_build_stubs_are_named_and_lose_to_full_terms(tmp_path, monkeypatch):
    # *_annot.obo writes pruned parents as nameless stubs with a stub_label;
    # the same parent can be a stub in one channel and a full term in another
    to_annot = tmp_path / "to_annot.obo"
    to_annot.write_text(
        'format-version: 1.2\n\n[Term]\nid: TO:0000387\nname: plant phenotype\n'
        'is_a: PATO:0000001\n\n'
        '[Term]\nid: PATO:0000001\nproperty_value: graph_stub "true" xsd:string\n'
        'property_value: stub_label "quality" xsd:string\nis_a: BFO:0000020\n')
    pato_annot = tmp_path / "pato_annot.obo"
    pato_annot.write_text(
        'format-version: 1.2\n\n[Term]\nid: PATO:0000001\nname: quality\n'
        'synonym: "trait" RELATED []\n')
    monkeypatch.setattr(category_index, "_active_obo_paths",
                        lambda *a, **k: [str(to_annot), str(pato_annot)])
    index = category_index.build_index(ontology_of=lambda category: "OTHER")
    assert index["by_id"]["PATO:0000001"]["category"] == "quality (PATO:0000001)"
    # the full PATO term won, so its synonym is indexed
    assert index["synonyms"]["trait"][0]["id"] == "PATO:0000001"
    assert index["parents_by_type"]["is_a"]["TO:0000387"] == ["PATO:0000001"]


def test_stub_label_names_a_stub(tmp_path):
    p = tmp_path / "c.obo"
    p.write_text('format-version: 1.2\n\n[Term]\nid: GO:0009059\n'
                 'property_value: graph_stub "true" xsd:string\n'
                 'property_value: stub_label "macromolecule biosynthetic process" xsd:string\n'
                 'is_a: GO:0009058\n')
    (term,) = list(category_index._iter_obo_terms(str(p)))
    assert (term["name"], term["stub"]) == ("macromolecule biosynthetic process", True)
    assert term["parents"] == {"is_a": ["GO:0009058"]}
