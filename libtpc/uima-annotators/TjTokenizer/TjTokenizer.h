/*
 * File:   TjTokenizer.h
 *
 * CAS-1 generator for the "jats" layout: papers whose main text comes from
 * PubMed Central JATS. The JATS itself is converted at staging time by
 * maizegdb_textpresso_implementation's textpresso_classifiers/jats_text.py
 * (bin/fetch_jats.py writes <accession>.jats.json); this annotator reads that
 * JSON, sets the document text, and writes the annotations -- tokens with the
 * same delimiters as TdTokenizer, sentences that never cross a block
 * (paragraph, heading, table row, ...), sections taken from the JSON rather
 * than detected from heading lines, format spans (italic, bold, sup, sub, sc,
 * ...), dblbrk, rawsource "jats", filename and tpfnvhash.
 *
 * Keeping the JATS rules in one (Python, tested) place means a rule change is
 * a re-run of staging, not a C++ change and an image rebuild.
 */

#ifndef TJTOKENIZER_H
#define TJTOKENIZER_H

#include <set>
#include <string>
#include <vector>
#include <uima/api.hpp>
#include "../TdTokenizer/TpTrie.h"
#include "../TdTokenizer/AnnotationCounter.h"

using namespace uima;

class TjTokenizer : public Annotator {
public:
    TjTokenizer();
    TjTokenizer(const TjTokenizer & orig);
    virtual ~TjTokenizer();
    TyErrorId initialize(AnnotatorContext & rclAnnotatorContext);
    TyErrorId typeSystemInit(TypeSystem const & crTypeSystem);
    TyErrorId destroy();
    TyErrorId process(CAS & tcas, ResultSpecification const & crResultSpecification);
private:
    Type rawsourcetype_;
    Feature rawsourcetype_type_;
    Type filenametype_;
    Feature filenametype_name_;
    Type tokentype_;
    Feature tokentype_content_;
    Type sentencetype_;
    Feature sentencetype_content_;
    Type tpfnvhashtype_;
    Feature tpfnvhashtype_content_;
    Type dblbrktype_;
    Type sectiontype_;
    Feature sectiontype_content_;
    Feature sectiontype_type_;
    Type formattype_;
    Feature formattype_value_;
    std::set<UnicodeString> dlsetToken_;
    TpTrie * trieToken_;
    std::set<UnicodeString> dlsetSentence_;
    std::vector<std::string> disqSentence_;
    int32_t maxfrontdisqcharlength_;
    int32_t maxbackdisqcharlength_;
    TpTrie * trieSentence_;
    Type requireType(TypeSystem const & ts, const char * name, TyErrorId & err);
};

#endif /* TJTOKENIZER_H */
