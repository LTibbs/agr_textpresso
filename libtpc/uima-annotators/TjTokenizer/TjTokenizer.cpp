/*
 * File:   TjTokenizer.cpp
 *
 * See TjTokenizer.h. Input: the document text Stream2Tpcas hands every
 * tokenizer ("<n>#<filename>#<rest>"), where <rest> is the path of an
 * <accession>.jats.json file written by jats_text.py:
 *
 *   {"rawsource": "jats", "text": "...",
 *    "blocks":   [{"begin", "end", "kind"}],
 *    "sections": [{"begin", "end", "type", "content"}],
 *    "formats":  [{"begin", "end", "value"}], ...}
 *
 * Offsets in the JSON count Unicode code points (Python str indices); UIMA
 * offsets count UTF-16 code units, so they are converted here -- otherwise a
 * single character outside the BMP (mathematical italic letters are common
 * in JATS) would shift every later annotation.
 *
 * Unlike TdTokenizer, the text is not passed through utf8Printable(): JATS
 * text keeps its Unicode (minus signs, <=, Greek letters), which is much of
 * the point of the jats layout.
 */

#include "TjTokenizer.h"
#include "../uimaglobaldefinitions.h"
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/regex.hpp>
#include <algorithm>
#include <iomanip>
#include <sstream>
#include <unicode/utf16.h>

using namespace std;
namespace pt = boost::property_tree;

namespace {
    /* magic numbers from http://www.isthe.com/chongo/tech/comp/fnv/ -- same
     * hash as TdTokenizer/TxTokenizer, so tpfnvhash values are comparable */
    const uint64_t InitialFNV = 14695981039346656037U;
    const uint64_t FNVMultiple = 1099511628211;

    string tpfnv(const UnicodeStringRef & s) {
        uint64_t hash = InitialFNV;
        for (uint64_t i = 0; i < (uint64_t) s.length(); i++) {
            hash = hash ^ (s[i]);
            hash = hash * FNVMultiple;
        }
        stringstream shex;
        shex << setw(16) << setfill('0') << hex << hash;
        return shex.str();
    }

    // TdTokenizer's MaximizeBoundaries/WriteOutAnnotations, for tokens only,
    // so tokenisation is identical to the tai layout. Trie matches are
    // [first, second] inclusive; annotations are [begin, end).
    pair<int32_t, int32_t> MaximizeBoundaries(pair<int32_t, int32_t> i, pair<int32_t, int32_t> j) {
        int32_t ifi = i.first, ise = i.second + 1, jfi = j.first, jse = j.second + 1;
        if (ifi != jfi) {
            if (ise != jse) return make_pair(-1, -1);
            return make_pair(min(ifi, jfi), ise);
        }
        return make_pair(ifi, max(ise, jse));
    }

    void WriteOutTokens(CAS & tcas, const UnicodeStringRef usdocref,
            vector< pair<int32_t, int32_t> > & p, Type t, Feature f, AnnotationCounter & ac) {
        FSIndexRepository & indexRep = tcas.getIndexRepository();
        Feature faid = t.getFeatureByBaseName("aid");
        vector< pair<int32_t, int32_t> > merged;
        for (size_t k = 0; k < p.size(); k++) {
            // TdTokenizer reads p[k + 1] past the end on the last match;
            // here the last match is simply taken as is.
            pair<int32_t, int32_t> res = (k + 1 < p.size())
                    ? MaximizeBoundaries(p[k], p[k + 1]) : make_pair(-1, -1);
            if (res.first < 0) res = make_pair(p[k].first, p[k].second + 1);
            else k++;
            merged.push_back(res);
        }
        auto add = [&](int32_t b, int32_t e) {
            AnnotationFS fs = tcas.createAnnotation(t, b, e);
            UnicodeString wd;
            usdocref.extract(b, e - b, wd);
            fs.setStringValue(f, wd);
            if (faid.isValid()) fs.setIntValue(faid, ac.GetNextId());
            indexRep.addFS(fs);
        };
        int32_t laste = 0;
        for (auto & m : merged) {
            if (m.first > laste) add(laste, m.first);
            laste = max(laste, m.second);
            add(m.first, m.second);
        }
    }

    vector< pair<int32_t, int32_t> > RemoveDelimiters(const UnicodeStringRef usdocref,
            const vector<string> & regex, const vector< pair<int32_t, int32_t> > & p,
            int32_t mfcl, int32_t mbcl) {
        vector<boost::regex> exprs;
        for (auto & r : regex) exprs.emplace_back(r);
        vector< pair<int32_t, int32_t> > result;
        for (auto & m : p) {
            int32_t b = max(0, m.first - mfcl);
            int32_t e = m.second + 1 + mbcl;
            string strip;
            usdocref.extract(b, e - b, strip);
            bool matches = false;
            for (auto & expr : exprs) matches = matches || boost::regex_search(strip, expr);
            if (!matches) result.push_back(m);
        }
        return result;
    }

    // code point index -> UTF-16 index, for every code point plus the end.
    vector<int32_t> CodePointToUtf16(const UnicodeString & s) {
        vector<int32_t> map;
        map.reserve(s.length() + 1);
        int32_t i = 0;
        while (i < s.length()) {
            map.push_back(i);
            UChar32 c;
            U16_NEXT(s.getBuffer(), i, s.length(), c);
        }
        map.push_back(s.length());
        return map;
    }

    int32_t ToUtf16(const vector<int32_t> & map, int cp) {
        if (cp < 0) return 0;
        if ((size_t) cp >= map.size()) return map.back();
        return map[cp];
    }
}

TjTokenizer::TjTokenizer() {
    dlsetToken_ = set<UnicodeString>(tp_uima_globals::token_delimiters(),
            tp_uima_globals::token_delimiters() + G_initT_No);
    dlsetSentence_ = set<UnicodeString>(tp_uima_globals::sentence_delimiters(),
            tp_uima_globals::sentence_delimiters() + G_initS_No);
    // Same disqualifiers as TdTokenizer: "A." initials, "Fig.", "e.g.", "et al." ...
    disqSentence_.push_back("[\\(>\\s][A-Z]\\.[<\\s]$");
    const string part1 = "[\\(>\\s](Prof|Ph\\.D|Dr|[Ff]igs?|[Vv]ol|i\\.e|e\\.g";
    const string part2 = "|[Nn]o|[Vv]s|[Ee]x|al|ca)\\.[<\\s]$";
    disqSentence_.push_back(part1 + part2);
    maxfrontdisqcharlength_ = 4;
    maxbackdisqcharlength_ = 0;
    trieToken_ = nullptr;
    trieSentence_ = nullptr;
}

TjTokenizer::TjTokenizer(const TjTokenizer & orig) {
}

TjTokenizer::~TjTokenizer() {
}

TyErrorId TjTokenizer::initialize(AnnotatorContext & rclAnnotatorContext) {
    trieToken_ = new TpTrie();
    for (auto & w : dlsetToken_) trieToken_->addWord(w);
    trieSentence_ = new TpTrie();
    for (auto & w : dlsetSentence_) trieSentence_->addWord(w);
    return (TyErrorId) UIMA_ERR_NONE;
}

Type TjTokenizer::requireType(TypeSystem const & ts, const char * name, TyErrorId & err) {
    Type t = ts.getType(name);
    if (!t.isValid()) {
        getAnnotatorContext().getLogger().logError(string("Error getting Type object for ") + name);
        cerr << "TjTokenizer::typeSystemInit - missing type " << name << endl;
        err = (TyErrorId) UIMA_ERR_RESMGR_INVALID_RESOURCE;
    }
    return t;
}

TyErrorId TjTokenizer::typeSystemInit(TypeSystem const & crTypeSystem) {
    TyErrorId err = (TyErrorId) UIMA_ERR_NONE;
    filenametype_ = requireType(crTypeSystem, "org.apache.uima.textpresso.filename", err);
    rawsourcetype_ = requireType(crTypeSystem, "org.apache.uima.textpresso.rawsource", err);
    tokentype_ = requireType(crTypeSystem, "org.apache.uima.textpresso.token", err);
    sentencetype_ = requireType(crTypeSystem, "org.apache.uima.textpresso.sentence", err);
    tpfnvhashtype_ = requireType(crTypeSystem, "org.apache.uima.textpresso.tpfnvhash", err);
    dblbrktype_ = requireType(crTypeSystem, "org.apache.uima.textpresso.dblbrk", err);
    sectiontype_ = requireType(crTypeSystem, "org.apache.uima.textpresso.section", err);
    formattype_ = requireType(crTypeSystem, "org.apache.uima.textpresso.format", err);
    if (err != UIMA_ERR_NONE) return err;
    filenametype_name_ = filenametype_.getFeatureByBaseName("value");
    rawsourcetype_type_ = rawsourcetype_.getFeatureByBaseName("value");
    tokentype_content_ = tokentype_.getFeatureByBaseName("content");
    sentencetype_content_ = sentencetype_.getFeatureByBaseName("content");
    tpfnvhashtype_content_ = tpfnvhashtype_.getFeatureByBaseName("content");
    sectiontype_content_ = sectiontype_.getFeatureByBaseName("content");
    sectiontype_type_ = sectiontype_.getFeatureByBaseName("type");
    formattype_value_ = formattype_.getFeatureByBaseName("value");
    return err;
}

TyErrorId TjTokenizer::destroy() {
    delete trieToken_;
    delete trieSentence_;
    return (TyErrorId) UIMA_ERR_NONE;
}

TyErrorId TjTokenizer::process(CAS & tcas, ResultSpecification const & crResultSpecification) {
    AnnotationCounter ac(tcas);
    // "<n>#<filename>#<path>", as for TdTokenizer
    UnicodeStringRef usprelimref = tcas.getDocumentText();
    int32_t firsthash = usprelimref.indexOf('#', 0);
    UnicodeString numberstring;
    usprelimref.extract(0, firsthash, numberstring);
    stringstream auxstream;
    int32_t result;
    auxstream << numberstring;
    auxstream >> result;
    UnicodeString filename;
    usprelimref.extract(firsthash + 1, result - firsthash - 2, filename);
    UnicodeString uspath;
    usprelimref.extract(result, usprelimref.length() - result, uspath);
    string jsonpath;
    uspath.trim().toUTF8String(jsonpath);

    pt::ptree doc;
    try {
        pt::read_json(jsonpath, doc);
    } catch (const pt::json_parser_error & e) {
        getAnnotatorContext().getLogger().logError(string("TjTokenizer: cannot read ") + jsonpath);
        cerr << "TjTokenizer: cannot read " << jsonpath << ": " << e.what() << endl;
        return (TyErrorId) UIMA_ERR_USER_ANNOTATOR_COULD_NOT_PROCESS;
    }
    UnicodeString text = UnicodeString::fromUTF8(StringPiece(doc.get<string>("text", "")));
    vector<int32_t> cp2u = CodePointToUtf16(text);
    tcas.setDocumentText(UnicodeStringRef(text));
    UnicodeStringRef usdocref = tcas.getDocumentText();
    FSIndexRepository & indexRep = tcas.getIndexRepository();

    // tokens: same delimiters and merging as the tai layout
    vector< pair<int32_t, int32_t> > p = trieToken_->searchAllWords(text);
    sort(p.begin(), p.end());
    WriteOutTokens(tcas, usdocref, p, tokentype_, tokentype_content_, ac);

    // sentences: the usual delimiters, but never across a block boundary, so
    // a heading, caption or table row is never glued to the next sentence.
    // As in the tai layout, a sentence ends before its delimiter (". ").
    p = trieSentence_->searchAllWords(text);
    sort(p.begin(), p.end());
    p = RemoveDelimiters(usdocref, disqSentence_, p, maxfrontdisqcharlength_, maxbackdisqcharlength_);
    Feature sentaid = sentencetype_.getFeatureByBaseName("aid");
    auto addSentence = [&](int32_t b, int32_t e) {
        UnicodeString wd;
        usdocref.extract(b, e - b, wd);
        if (wd.trim().isEmpty()) return;
        usdocref.extract(b, e - b, wd);
        AnnotationFS fs = tcas.createAnnotation(sentencetype_, b, e);
        fs.setStringValue(sentencetype_content_, wd);
        if (sentaid.isValid()) fs.setIntValue(sentaid, ac.GetNextId());
        indexRep.addFS(fs);
    };
    size_t k = 0;
    if (doc.get_child_optional("blocks")) {
        for (auto & node : doc.get_child("blocks")) {
            int32_t bb = ToUtf16(cp2u, node.second.get<int>("begin"));
            int32_t be = ToUtf16(cp2u, node.second.get<int>("end"));
            while (k < p.size() && p[k].first < bb) k++;
            int32_t cur = bb;
            for (; k < p.size() && p[k].first < be; k++) {
                if (p[k].first < cur) continue;           // overlapping delimiter
                if (p[k].first > cur) addSentence(cur, p[k].first);
                cur = p[k].second + 1;
            }
            if (cur < be) addSentence(cur, be);
        }
    }

    // sections, from the document structure
    Feature secaid = sectiontype_.getFeatureByBaseName("aid");
    if (doc.get_child_optional("sections")) {
        for (auto & node : doc.get_child("sections")) {
            int32_t b = ToUtf16(cp2u, node.second.get<int>("begin"));
            int32_t e = ToUtf16(cp2u, node.second.get<int>("end"));
            if (e <= b) continue;
            AnnotationFS fs = tcas.createAnnotation(sectiontype_, b, e);
            fs.setStringValue(sectiontype_type_,
                    UnicodeString::fromUTF8(StringPiece(node.second.get<string>("type", ""))));
            fs.setStringValue(sectiontype_content_,
                    UnicodeString::fromUTF8(StringPiece(node.second.get<string>("content", ""))));
            if (secaid.isValid()) fs.setIntValue(secaid, ac.GetNextId());
            indexRep.addFS(fs);
        }
    }

    // formatting spans (italic, bold, sup, sub, sc, monospace, underline)
    Feature fmtaid = formattype_.getFeatureByBaseName("aid");
    if (doc.get_child_optional("formats")) {
        for (auto & node : doc.get_child("formats")) {
            int32_t b = ToUtf16(cp2u, node.second.get<int>("begin"));
            int32_t e = ToUtf16(cp2u, node.second.get<int>("end"));
            if (e <= b) continue;
            AnnotationFS fs = tcas.createAnnotation(formattype_, b, e);
            fs.setStringValue(formattype_value_,
                    UnicodeString::fromUTF8(StringPiece(node.second.get<string>("value", ""))));
            if (fmtaid.isValid()) fs.setIntValue(fmtaid, ac.GetNextId());
            indexRep.addFS(fs);
        }
    }

    // paragraph breaks, as the tai layout marks them (but in UTF-16 offsets)
    Feature brkaid = dblbrktype_.getFeatureByBaseName("aid");
    for (int32_t pos = text.indexOf(UnicodeString("\n\n")); pos >= 0;
            pos = text.indexOf(UnicodeString("\n\n"), pos + 1)) {
        AnnotationFS fs = tcas.createAnnotation(dblbrktype_, pos, pos + 2);
        if (brkaid.isValid()) fs.setIntValue(brkaid, ac.GetNextId());
        indexRep.addFS(fs);
    }

    AnnotationFS fsHash = tcas.createAnnotation(tpfnvhashtype_, 0, usdocref.length());
    fsHash.setStringValue(tpfnvhashtype_content_, UnicodeString(tpfnv(usdocref).c_str()));
    indexRep.addFS(fsHash);
    AnnotationFS fsSource = tcas.createAnnotation(rawsourcetype_, 0, usdocref.length());
    fsSource.setStringValue(rawsourcetype_type_, "jats");
    indexRep.addFS(fsSource);
    AnnotationFS fsName = tcas.createAnnotation(filenametype_, 0, usdocref.length());
    fsName.setStringValue(filenametype_name_, filename);
    indexRep.addFS(fsName);
    return (TyErrorId) UIMA_ERR_NONE;
}

MAKE_AE(TjTokenizer);
