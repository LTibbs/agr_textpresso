"""Tests for access.py -- open-access manifest + API-key parsing.

Run with: python3 -m pytest textpressoapi/tests/test_access.py
"""

import os
import sys
import tempfile

import pytest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import access


def _write(tmp_path, name, text):
    p = tmp_path / name
    p.write_text(text)
    return str(p)


def test_no_manifest_means_not_enforcing(tmp_path):
    ac = access.AccessControl(str(tmp_path / "missing.tsv"), str(tmp_path / "missing.txt"))
    assert ac.enforcing is False
    # Everything is open access when not enforcing.
    assert ac.is_open_access("AnyCorpus", "10.1/x") is True


def test_default_open_with_overrides(tmp_path):
    manifest = _write(tmp_path, "m.tsv", "\n".join([
        "# comment",
        "@default open",
        "@corpus:SorghumClosed closed",
        "10.1093/genetics/iyaf266 open",
        "10.1021/acs.jafc.5c10234 closed",
        "",
    ]))
    ac = access.AccessControl(manifest, str(tmp_path / "none.txt"))
    assert ac.enforcing is True
    # unlisted -> default
    assert ac.is_open_access("MaizeOA", "10.999/unlisted") is True
    # corpus rule
    assert ac.is_open_access("SorghumClosed", "10.999/unlisted") is False
    # accession beats corpus
    assert ac.is_open_access("SorghumClosed", "10.1093/genetics/iyaf266") is True
    assert ac.is_open_access("MaizeOA", "10.1021/acs.jafc.5c10234") is False


def test_default_closed(tmp_path):
    manifest = _write(tmp_path, "m.tsv", "@default closed\n10.1/open-one open\n")
    ac = access.AccessControl(manifest, str(tmp_path / "none.txt"))
    assert ac.is_open_access("C", "10.1/whatever") is False
    assert ac.is_open_access("C", "10.1/open-one") is True


def test_accession_slash_underscore_equivalence(tmp_path):
    manifest = _write(tmp_path, "m.tsv", "@default closed\n10.1093_genetics_iyaf266 open\n")
    ac = access.AccessControl(manifest, str(tmp_path / "none.txt"))
    assert ac.is_open_access("C", "10.1093/genetics/iyaf266") is True
    assert ac.is_open_access("C", "10.1093_GENETICS_IYAF266") is True


def test_api_keys(tmp_path):
    manifest = _write(tmp_path, "m.tsv", "@default closed\n")
    keys = _write(tmp_path, "k.txt", "# keys\nabc123\ndef456  institution-of-somewhere\n\n")
    ac = access.AccessControl(manifest, keys)
    assert ac.has_keys is True
    assert ac.is_valid_key("abc123") is True
    assert ac.is_valid_key("def456") is True
    assert ac.is_valid_key("institution-of-somewhere") is False
    assert ac.is_valid_key("") is False
    assert ac.is_valid_key("nope") is False


def test_identifier_parts():
    assert access.identifier_parts(
        "MaizeTest100//10.1038_srep35479/10.1038_srep35479.tpcas"
    ) == ("MaizeTest100", "10.1038_srep35479")
    assert access.identifier_parts(
        "SorghumBase//10.1007_978-1-0716-1816-5_12/10.1007_978-1-0716-1816-5_12.tpcas"
    ) == ("SorghumBase", "10.1007_978-1-0716-1816-5_12")
    corpus, accession = access.identifier_parts("Corpus//acc.tpcas")
    assert corpus == "Corpus"
    assert accession == "acc"


def test_key_from_request():
    assert access.key_from_request({"X-API-Key": "k1"}, {}) == "k1"
    assert access.key_from_request({"Authorization": "Bearer k2"}, {}) == "k2"
    assert access.key_from_request({}, {"api_key": ["k3"]}) == "k3"
    assert access.key_from_request({}, {}) == ""


def test_redact_annotation_keeps_shape_blanks_text():
    import cas_annotate_server

    full = {
        "sentences": [{"begin": 0, "end": 5, "text": "hello"},
                      {"begin": 6, "end": 9, "text": "bye"}],
        "annotations": [{"begin": 1, "end": 4, "term": "ell",
                         "category": "x (GO:1)", "ontology": "GO", "onto_id": "GO:1"}],
        "sections": [{"begin": 0, "end": 9, "type": "abstract"}],
    }
    red = cas_annotate_server._redact_annotation(full)
    assert red["access_limited"] is True
    # same number of elements, same keys, offsets intact
    assert len(red["sentences"]) == 2
    assert red["sentences"][0] == {"begin": 0, "end": 5, "text": ""}
    assert set(red["annotations"][0]) == set(full["annotations"][0])
    assert red["annotations"][0]["term"] == ""
    assert red["annotations"][0]["category"] == "x (GO:1)"
    assert red["annotations"][0]["onto_id"] == "GO:1"
    assert red["sections"] == full["sections"]
    # input not mutated
    assert full["sentences"][0]["text"] == "hello"
    assert full["annotations"][0]["term"] == "ell"
