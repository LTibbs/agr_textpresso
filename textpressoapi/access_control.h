#ifndef TEXTPRESSO_API_ACCESS_CONTROL_H
#define TEXTPRESSO_API_ACCESS_CONTROL_H

// Open-access gating for textpressoapi.
//
// Two inputs, both optional plain-text files:
//
//   * an "open-access manifest" that says which documents may be served in
//     full to anyone, and
//   * an "API keys" file listing keys that grant unrestricted full-text
//     access regardless of a document's open-access status.
//
// If the manifest file is absent the API behaves exactly as it did before
// this feature existed: every document is treated as open access and no
// request is ever restricted. This keeps the default deployment unchanged
// and makes the feature opt-in by dropping the manifest file into place.
//
// Manifest format (whitespace-separated, '#' comments, blank lines ignored):
//
//   @default            open              # status for anything not listed below
//   @corpus:SorghumBase  closed            # per-corpus default
//   10.1093/genetics/iyaf266   open        # per-accession override (wins over corpus)
//   10.1021_acs.jafc.5c10234   closed
//
// status tokens accepted: open|oa|true|1|yes  and  closed|restricted|false|0|no
// Accessions are compared case-insensitively with '/' normalised to '_', so
// "10.1093/genetics/iyaf266" and "10.1093_genetics_iyaf266" are equivalent.
//
// API keys file: one key per line (an optional label may follow, separated by
// whitespace); '#' comments and blank lines ignored.

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <set>
#include <string>

namespace tpc_access {

inline std::string trim(const std::string &s) {
    std::size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) {
        return "";
    }
    std::size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

inline std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// Normalise an accession/DOI for comparison: lowercase, '/' -> '_'.
inline std::string normalize_accession(std::string s) {
    s = to_lower(trim(s));
    std::replace(s.begin(), s.end(), '/', '_');
    return s;
}

class AccessControl {
public:
    AccessControl() : manifest_loaded_(false), default_open_(true) {}

    void load(const std::string &manifest_path, const std::string &api_keys_path) {
        load_manifest(manifest_path);
        load_api_keys(api_keys_path);
    }

    // False until a manifest file has been read. While false the caller must
    // not restrict anything (legacy behaviour).
    bool enforcing() const { return manifest_loaded_; }

    bool has_keys() const { return !api_keys_.empty(); }

    bool is_valid_key(const std::string &key) const {
        return !key.empty() && api_keys_.count(key) > 0;
    }

    // corpus may be empty; accession is matched case-insensitively, '/'->'_'.
    bool is_open_access(const std::string &corpus, const std::string &accession) const {
        std::map<std::string, bool>::const_iterator ai =
                accession_status_.find(normalize_accession(accession));
        if (ai != accession_status_.end()) {
            return ai->second;
        }
        std::map<std::string, bool>::const_iterator ci =
                corpus_status_.find(to_lower(trim(corpus)));
        if (ci != corpus_status_.end()) {
            return ci->second;
        }
        return default_open_;
    }

    // "MaizeTest100//10.1038_x/10.1038_x.tpcas" -> "MaizeTest100"
    static std::string corpus_of(const std::string &filepath) {
        std::size_t slash = filepath.find('/');
        return slash == std::string::npos ? std::string() : filepath.substr(0, slash);
    }

private:
    static bool status_to_open(const std::string &raw, bool fallback) {
        std::string v = to_lower(trim(raw));
        if (v == "open" || v == "oa" || v == "true" || v == "1" || v == "yes") {
            return true;
        }
        if (v == "closed" || v == "restricted" || v == "false" || v == "0" || v == "no") {
            return false;
        }
        return fallback;
    }

    void load_manifest(const std::string &path) {
        std::ifstream in(path.c_str());
        if (!in.good()) {
            manifest_loaded_ = false;
            return;
        }
        std::string line;
        while (std::getline(in, line)) {
            std::string t = trim(line);
            if (t.empty() || t[0] == '#') {
                continue;
            }
            std::size_t sp = t.find_first_of(" \t");
            if (sp == std::string::npos) {
                continue;
            }
            std::string key = trim(t.substr(0, sp));
            std::string val = trim(t.substr(sp + 1));
            std::string lkey = to_lower(key);
            if (lkey == "@default" || lkey == "default:") {
                default_open_ = status_to_open(val, default_open_);
            } else if (lkey.compare(0, 8, "@corpus:") == 0) {
                corpus_status_[to_lower(key.substr(8))] = status_to_open(val, true);
            } else {
                accession_status_[normalize_accession(key)] = status_to_open(val, true);
            }
        }
        manifest_loaded_ = true;
    }

    void load_api_keys(const std::string &path) {
        std::ifstream in(path.c_str());
        if (!in.good()) {
            return;
        }
        std::string line;
        while (std::getline(in, line)) {
            std::string t = trim(line);
            if (t.empty() || t[0] == '#') {
                continue;
            }
            std::size_t sp = t.find_first_of(" \t");
            std::string key = sp == std::string::npos ? t : trim(t.substr(0, sp));
            if (!key.empty()) {
                api_keys_.insert(key);
            }
        }
    }

    bool manifest_loaded_;
    bool default_open_;
    std::set<std::string> api_keys_;
    std::map<std::string, bool> corpus_status_;
    std::map<std::string, bool> accession_status_;
};

// Pull an API key out of a request: "X-API-Key" header, then
// "Authorization: Bearer <key>", then a JSON body field "api_key".
template <typename Request, typename Json>
std::string extract_api_key(const Request &req, const Json &json_req) {
    std::string key = req.get_header_value("X-API-Key");
    if (!key.empty()) {
        return key;
    }
    std::string auth = req.get_header_value("Authorization");
    if (auth.compare(0, 7, "Bearer ") == 0) {
        return trim(auth.substr(7));
    }
    if (json_req && json_req.has("api_key")) {
        try {
            return json_req["api_key"].s();
        } catch (...) {
        }
    }
    return "";
}

} // namespace tpc_access

#endif // TEXTPRESSO_API_ACCESS_CONTROL_H
