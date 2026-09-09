#define CROW_ENABLE_SSL

#include <iostream>
#include "crow.h"
#include <textpresso/IndexManager.h>
#include <SQLiteCpp/SQLiteCpp.h>
#include <boost/program_options.hpp>
#include <fstream>
#include "access_control.h"

using namespace tpc::index;
using namespace std;
using namespace SQLite;
using namespace boost::filesystem;

namespace po = boost::program_options;

namespace {

struct BibMetadata {
    string accession;
    string title;
    string author;
    string journal;
    string year;
    string type;
    string abstract_text;
};

string unwrap_marked_field(const string& value) {
    static const string prefix = "BEGIN ";
    static const string suffix = " END";
    if (value.size() >= prefix.size() + suffix.size() &&
            value.compare(0, prefix.size(), prefix) == 0 &&
            value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0) {
        return value.substr(prefix.size(), value.size() - prefix.size() - suffix.size());
    }
    return value;
}

string basename_without_suffix(const string& filepath, const string& suffix) {
    auto slash = filepath.find_last_of('/');
    string basename = slash == string::npos ? filepath : filepath.substr(slash + 1);
    if (basename.size() >= suffix.size() &&
            basename.compare(basename.size() - suffix.size(), suffix.size(), suffix) == 0) {
        basename.resize(basename.size() - suffix.size());
    }
    return basename;
}

BibMetadata load_bib_metadata(const string& filepath) {
    BibMetadata metadata;
    path bib_path = path(CAS_ROOT_LOCATION) / path(filepath);
    bib_path.replace_extension(".bib");
    std::ifstream input(bib_path.string());
    if (!input.good()) {
        return metadata;
    }
    string line;
    while (getline(input, line)) {
        auto split = line.find('|');
        if (split == string::npos) {
            continue;
        }
        string key = line.substr(0, split);
        string value = line.substr(split + 1);
        if (key == "accession") {
            metadata.accession = value;
        } else if (key == "title") {
            metadata.title = value;
        } else if (key == "author") {
            metadata.author = value;
        } else if (key == "journal") {
            metadata.journal = value;
        } else if (key == "year") {
            metadata.year = value;
        } else if (key == "type") {
            metadata.type = value;
        } else if (key == "abstract") {
            metadata.abstract_text = value;
        }
    }
    return metadata;
}

}

bool is_token_valid(const string& db_path, const string& token) {
    try {
        SQLite::Database db(db_path);
        SQLite::Statement query(db, "SELECT * FROM tokens WHERE token = ?");
        query.bind(1, token);
        query.executeStep();
        return query.hasRow();
    } catch (std::exception& e) {
        std::cout << "exception: " << e.what() << std::endl;
        return false;
    }
}

bool is_superuser(const string& db_path, const string& token) {
    try {
        SQLite::Database db(db_path);
        SQLite::Statement query(db, "SELECT superuser FROM tokens WHERE token = ?");
        query.bind(1, token);
        query.executeStep();
        if (query.hasRow()) {
            return static_cast<bool> (static_cast<int> (query.getColumn("superuser")));
        } else {
            return false;
        }
    } catch (std::exception& e) {
        std::cout << "exception: " << e.what() << std::endl;
        return false;
    }
}

static bool sentence_before(const SentenceDetails &a, const SentenceDetails &b) {
    return a.doc_position_begin < b.doc_position_begin;
}

tpc::index::Query get_query(const crow::json::rvalue& json_req, const IndexManager& indexManager) {
    tpc::index::Query query;
    if (!json_req.has("query")) {
        throw runtime_error("query not specified");
    }
    if (json_req["query"].has("keywords")) {
        query.keyword = json_req["query"]["keywords"].s();
        query.keyword = "\\(" + query.keyword +"\\)";
    }
    if (json_req["query"].has("categories") && json_req["query"]["categories"].size() > 0) {
        for (auto& category : json_req["query"]["categories"]) {
            query.categories.push_back(category.s());
        }
    }
    if (json_req["query"].has("exclude_keywords"))
        query.exclude_keyword = json_req["query"]["exclude_keywords"].s();
    if (json_req["query"].has("year"))
        query.year = json_req["query"]["year"].s();
    if (json_req["query"].has("author"))
        query.author = json_req["query"]["author"].s();
    if (json_req["query"].has("accession"))
        query.accession = json_req["query"]["accession"].s();
    if (json_req["query"].has("journal"))
        query.journal = json_req["query"]["journal"].s();
    if (json_req["query"].has("paper_type"))
        query.paper_type = json_req["query"]["paper_type"].s();
    if (json_req["query"].has("exclude_keywords"))
        query.exclude_keyword = json_req["query"]["exclude_keywords"].s();
    if (json_req["query"].has("exact_match_author"))
        query.exact_match_author = json_req["query"]["exact_match_author"].b();
    if (json_req["query"].has("exact_match_journal"))
        query.exact_match_journal = json_req["query"]["exact_match_journal"].b();
    if (json_req["query"].has("categories_and_ed"))
        query.categories_and_ed = json_req["query"]["categories_and_ed"].b();
    if (json_req["query"].has("type")) {
        string type = json_req["query"]["type"].s();
        if (type == "abstract")
            query.type = QueryType::abstract;
        else if (type == "acknowledgments")
            query.type = QueryType::acknowledgments;
        else if (type == "background")
            query.type = QueryType::background;
        else if (type == "conclusion")
            query.type = QueryType::conclusion;
        else if (type == "design")
            query.type = QueryType::design;
        else if (type == "discussion")
            query.type = QueryType::discussion;
        else if (type == "document")
            query.type = QueryType::document;
        else if (type == "introduction")
            query.type = QueryType::introduction;
        else if (type == "materials and methods")
            query.type = QueryType::materials;
        else if (type == "references")
            query.type = QueryType::references;
        else if (type == "result")
            query.type = QueryType::result;
        else if (type == "sentence")
            query.type = QueryType::sentence;
        else if (type == "title")
            query.type = QueryType::title;
    } else {
        throw runtime_error("query type not specified");
    }
    if (json_req["query"].has("case_sensitive")) {
        query.case_sensitive = json_req["query"]["case_sensitive"].b();
    }
    if (json_req["query"].has("sort_by_year")) {
        query.sort_by_year = json_req["query"]["sort_by_year"].b();
    }
    if (json_req["query"].has("corpora") && json_req["query"]["corpora"].size() > 0) {
        for (auto& corpus : json_req["query"]["corpora"]) {
            query.literatures.push_back(corpus.s());
        }
    } else {
        query.literatures = indexManager.get_available_corpora(tpc::index::CAS_ROOT_LOCATION.c_str());
    }
    return query;
}

int main(int argc, const char* argv[]) {
    po::options_description desc("options");
    po::positional_options_description p;
    po::variables_map vm;

    // arguments
    string ssl_cert;
    string ssl_key;
    string index_path;
    string login_database;
    string oa_manifest;
    string api_keys_file;
    int max_anon_snippets = 3;

    try {
        desc.add_options()
                ("help,h", "produce help message")
                ("index,i", po::value<string>(&index_path)->default_value("/usr/local/textpresso/luceneindex"),
                "textpresso index")
                ("login-database,d", po::value<string>(&login_database)->default_value(""),
                "database for logins and tokens (optional, authentication disabled if not provided)")
                ("open-access-manifest", po::value<string>(&oa_manifest)->default_value(
                "/data/textpresso/open_access_manifest.tsv"),
                "manifest of per-accession / per-corpus open-access status; if the file is absent, "
                "every document is treated as open access and no request is restricted")
                ("api-keys-file", po::value<string>(&api_keys_file)->default_value(
                "/data/textpresso/textpressoapi_data/api_keys.txt"),
                "file of API keys (one per line) that grant unrestricted full-text access")
                ("max-anon-snippets", po::value<int>(&max_anon_snippets)->default_value(3),
                "maximum matching sentences returned per non-open-access document to "
                "unauthenticated clients")
                ("ssl_cert,c", po::value<string>(&ssl_cert)->default_value(""),
                "ssl certificate file")
                ("ssl_key,k", po::value<string>(&ssl_key)->default_value(""),
                "ssl key file");
        po::store(po::command_line_parser(argc, argv).options(desc).positional(p).run(), vm);
        po::notify(vm);

        if (vm.count("help")) {
            cout << desc << endl;
            return 1;
        }
    } catch (std::exception &e) {
        if (vm.count("help")) {
            cout << desc << endl;
            return (EXIT_SUCCESS);
        }
        std::cerr << "Error: " << e.what() << "\n";
        return (EXIT_FAILURE);
    }

    IndexManager indexManager(index_path);

    tpc_access::AccessControl access;
    access.load(oa_manifest, api_keys_file);
    if (max_anon_snippets < 0) {
        max_anon_snippets = 0;
    }
    if (access.enforcing()) {
        cout << "access control: ENFORCING (manifest: " << oa_manifest << "); "
                << "unauthenticated clients get at most " << max_anon_snippets
                << " matching sentences per non-open-access document" << endl;
        if (!access.has_keys()) {
            cout << "access control: WARNING no API keys loaded from " << api_keys_file
                    << " -- no client can retrieve full text of a non-open-access document" << endl;
        }
    } else {
        cout << "access control: disabled (no manifest at " << oa_manifest
                << "); all documents served as open access" << endl;
    }

    crow::SimpleApp app;

    CROW_ROUTE(app, "/v1/textpresso/api/search_documents")
            .methods("POST"_method)
            ([&indexManager, &login_database, &access, max_anon_snippets](const crow::request & req) {
                // parse request
                auto json_req = crow::json::load(req.body);
                if (!json_req)
                    return crow::response(400);
                // Open-access gating: an unauthenticated client gets the full
                // response for open-access documents, but only metadata +
                // abstract + the first `max_anon_snippets` matching sentences
                // for non-open-access ones. A valid API key lifts the limit.
                bool authenticated = access.is_valid_key(tpc_access::extract_api_key(req, json_req));
                bool enforce = access.enforcing() && !authenticated;
                int64_t since_num(0);
                int64_t count(200);
                if (json_req.has("since_num"))
                        since_num = json_req["since_num"].i();
                    if (json_req.has("count") && 0 < json_req["count"].i() && json_req["count"].i() < 200)
                            count = json_req["count"].i();
                            bool include_text(false);
                            bool include_match_sentences(false);
                            bool include_all_sentences(false);
                        if (json_req.has("include_fulltext")) {
                            include_text = json_req["include_fulltext"].b();
                        }
                tpc::index::Query query;
                try {
                    query = get_query(json_req, indexManager);
                } catch (const runtime_error& e) {
                    cerr << e.what() << endl;
                    return crow::response(400);
                }
                set<string> include_match_sentence_fields = {};
                if (json_req.has("include_match_sentences")) {
                    include_match_sentences = json_req["include_match_sentences"].b();
                            include_match_sentence_fields = {"sentence_compressed", "begin"};
                    if (include_match_sentences && (query.type != tpc::index::QueryType::sentence)) {
                        return crow::response(401);
                    }
                }
                set<string> include_all_sentence_fields = {};
                if (json_req.has("include_all_sentences")) {
                    include_all_sentences = json_req["include_all_sentences"].b();
                    include_all_sentence_fields = {"sentence_compressed", "begin"};
                }
                // call textpresso library
                SearchResults results = indexManager.search_documents(query);
                auto first_iter = results.hit_documents.begin() + since_num;
                if (first_iter > results.hit_documents.end()) {
                    first_iter = results.hit_documents.end();
                }
                auto last_iter = results.hit_documents.begin() + since_num + count;
                if (last_iter > results.hit_documents.end()) {
                    last_iter = results.hit_documents.end();
                }
                set<string> exclude_doc_fields = {"fulltext_compressed", "abstract_compressed",
                    "fulltext_cat_compressed"};
                if (include_text) {
                    exclude_doc_fields = {};
                }
                auto doc_details = indexManager.get_documents_details(
                        vector<tpc::index::DocumentSummary>(first_iter, last_iter), query.sort_by_year,
                        include_match_sentences, {"filepath", "year", "doc_id"}, include_match_sentence_fields,
                        exclude_doc_fields,{}, include_all_sentences, include_all_sentence_fields,{}, true, true);
                // response
                crow::json::wvalue json_resp;
                for (int i = 0; i < doc_details.size(); ++i) {
                    BibMetadata bib = load_bib_metadata(doc_details[i].filepath);
                    string accession = doc_details[i].accession.empty() ?
                            basename_without_suffix(doc_details[i].filepath, ".tpcas") :
                            doc_details[i].accession;
                    if (!bib.accession.empty()) {
                        accession = bib.accession;
                    }
                    string title = unwrap_marked_field(doc_details[i].title);
                    if (title.empty() && !bib.title.empty()) {
                        title = bib.title;
                    }
                    if (title.empty()) {
                        title = accession;
                    }
                    string author = unwrap_marked_field(doc_details[i].author);
                    if (author.empty() && bib.author != "<not uploaded>") {
                        author = bib.author;
                    }
                    string journal = unwrap_marked_field(doc_details[i].journal);
                    if (journal.empty() && bib.journal != "<not uploaded>") {
                        journal = bib.journal;
                    }
                    string doc_type = doc_details[i].type;
                    if (doc_type.empty() && !bib.type.empty() && bib.type != "<not uploaded>") {
                        doc_type = bib.type;
                    }
                    string year = doc_details[i].year;
                    if (year.empty() && !bib.year.empty() && bib.year != "<not uploaded>") {
                        year = bib.year;
                    }
                    json_resp[i]["identifier"] = doc_details[i].filepath;
                    json_resp[i]["score"] = doc_details[i].score;
                    json_resp[i]["title"] = title;
                    json_resp[i]["author"] = author;
                    json_resp[i]["accession"] = accession;
                    json_resp[i]["journal"] = journal;
                    json_resp[i]["doc_type"] = doc_type;
                    json_resp[i]["year"] = year;

                    bool open_access = access.is_open_access(
                            tpc_access::AccessControl::corpus_of(doc_details[i].filepath), accession);
                    if (access.enforcing()) {
                        json_resp[i]["open_access"] = open_access;
                    }
                    bool limited = enforce && !open_access;
                    if (limited) {
                        json_resp[i]["access_limited"] = true;
                    }

                    if (include_text) {
                        if (!limited) {
                            json_resp[i]["fulltext"] = doc_details[i].fulltext;
                        }
                        json_resp[i]["abstract"] = doc_details[i].abstract;
                        if (doc_details[i].abstract.empty() && !bib.abstract_text.empty() &&
                                bib.abstract_text != "<not uploaded>") {
                            json_resp[i]["abstract"] = bib.abstract_text;
                        }
                    }
                    if (include_match_sentences) {
                        sort(doc_details[i].sentences_details.begin(),
                                doc_details[i].sentences_details.end(), sentence_before);
                        size_t n_sentences = doc_details[i].sentences_details.size();
                        if (limited && n_sentences > static_cast<size_t>(max_anon_snippets)) {
                            n_sentences = static_cast<size_t>(max_anon_snippets);
                        }
                        for (size_t j = 0; j < n_sentences; ++j) {
                            json_resp[i]["matched_sentences"][j] = doc_details[i].sentences_details[j].sentence_text;
                        }
                    }
                    if (include_all_sentences && !limited) {
                        sort(doc_details[i].all_sentences_details.begin(), doc_details[i].all_sentences_details.end(),
                                sentence_before);
                        for (int j = 0; j < doc_details[i].all_sentences_details.size(); ++j) {
                            json_resp[i]["all_sentences"][j] = doc_details[i].all_sentences_details[j].sentence_text;
                        }
                    }
                }
                return crow::response(json_resp);
            });

    CROW_ROUTE(app, "/v1/textpresso/api/available_corpora")([&indexManager] {
        crow::json::wvalue x;
        auto corpora = indexManager.get_available_corpora(tpc::index::CAS_ROOT_LOCATION.c_str());
        for (int i = 0; i < corpora.size(); ++i) {
            x[i] = corpora[i];
        }
        return crow::response(x);
    });

    CROW_ROUTE(app, "/v1/textpresso/api/get_documents_count")
            .methods("POST"_method)
            ([&indexManager, &login_database](const crow::request & req) {
                // parse request
                auto json_req = crow::json::load(req.body);
                if (!json_req)
                    return crow::response(400);
                tpc::index::Query query;
                try {
                    query = get_query(json_req, indexManager);
                } catch (const runtime_error& e) {
                    return crow::response(400);
                }
                // call textpresso library
                SearchResults results = indexManager.search_documents(query);
                // response
                crow::json::wvalue json_resp;
                json_resp = results.hit_documents.size();
                return crow::response(json_resp);
            });

    CROW_ROUTE(app, "/v1/textpresso/api/get_category_matches_document_fulltext")
            .methods("POST"_method)
            ([&indexManager, &login_database, &access](const crow::request & req) {
                // parse request
                auto json_req = crow::json::load(req.body);
                if (!json_req)
                    return crow::response(400);
                bool authenticated = access.is_valid_key(tpc_access::extract_api_key(req, json_req));
                bool enforce = access.enforcing() && !authenticated;
                int64_t since_num(0);
                int64_t count(10000000);
                if (json_req.has("since_num"))
                        since_num = json_req["since_num"].i();
                    if (json_req.has("count") && json_req["count"].i() > 0)
                            count = json_req["count"].i();
                            tpc::index::Query query;
                        try {
                            query = get_query(json_req, indexManager);
                        } catch (const runtime_error& e) {
                            return crow::response(400);
                        }
                if (!json_req.has("category")) {
                    return crow::response(400);
                }
                string category = json_req["category"].s();
                // call textpresso library
                std::set<std::string> matches;
                SearchResults results = indexManager.search_documents(query);
                auto first_iter = results.hit_documents.begin() + since_num;
                if (first_iter > results.hit_documents.end()) {
                    first_iter = results.hit_documents.end();
                }
                auto last_iter = results.hit_documents.begin() + since_num + count;
                if (last_iter > results.hit_documents.end()) {
                    last_iter = results.hit_documents.end();
                }
                auto doc_details = indexManager.get_documents_details(
                        vector<tpc::index::DocumentSummary>(first_iter, last_iter),
                        false, false,{"filepath", "fulltext_compressed", "fulltext_cat_compressed"},
                {
                },
                {
                },
                {
                });
                // response
                crow::json::wvalue json_resp;
                for (int i = 0; i < doc_details.size(); ++i) {
                    json_resp[i]["identifier"] = doc_details[i].filepath;
                    // This endpoint derives its result from the full text, so a
                    // non-open-access document is withheld from unauthenticated
                    // clients (empty match list, flagged).
                    bool open_access = access.is_open_access(
                            tpc_access::AccessControl::corpus_of(doc_details[i].filepath),
                            basename_without_suffix(doc_details[i].filepath, ".tpcas"));
                    if (enforce && !open_access) {
                        json_resp[i]["access_limited"] = true;
                        continue;
                    }
                    matches = indexManager.get_words_belonging_to_category_from_document_fulltext(
                            doc_details[i].fulltext, doc_details[i].categories_string, category);
                    int j = 0;
                    for (auto& word : matches) {
                        json_resp[i]["matches"][j++] = word;
                    }
                }
                return crow::response(json_resp);
            });

    if (!ssl_cert.empty() && !ssl_key.empty()) {
        auto & appref = app
                .bindaddr("0.0.0.0")
                .port(18080)
                .ssl_file(ssl_cert, ssl_key);
        appref.ssl_context_.set_verify_mode(boost::asio::ssl::verify_none);
        appref.multithreaded().run();
    } else {
        app.port(18080).multithreaded().run();
    }
}
