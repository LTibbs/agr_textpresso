/* 
 * File:   TdTokenizer.h
 * Author: mueller
 *
 * Created on December 13, 2021, 2:15 PM
 */

#ifndef TDTOKENIZER_H
#define	TDTOKENIZER_H

#include <set>
#include <uima/api.hpp>
#include "TpTrie.h"
#include "AnnotationCounter.h"


using namespace uima;

class TdTokenizer : public Annotator {
public:
    TdTokenizer();
    TdTokenizer(const TdTokenizer & orig);
    virtual ~TdTokenizer();
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
    Type pagetype_;
    Feature pagetype_value_;
    Type dblbrktype_;
    Type rawsectiontype_;
    Feature rawsectiontype_content_;
    Type sectiontype_;
    Feature sectiontype_content_;
    Feature sectiontype_type_;
    Type imagetype_;
    Feature imagetype_filename_;
    Feature imagetype_page_;
    
    CAS * tcas_;
    std::set<UnicodeString> dlsetToken_;
    TpTrie * trieToken_;
    std::set<UnicodeString> dlsetSentence_;
    vector<string> disqSentence_;
    int32_t maxfrontdisqcharlength_;
    int32_t maxbackdisqcharlength_;
    TpTrie * trieSentence_;
    std::set<UnicodeString> dlsetSection_;
    TpTrie * trieSection_;
    bool hasSection(const set<UnicodeString>& sectionNames, const vector<UnicodeString>& sections);
    // Which raw-section headings are real, and where each one's section ends
    // (at the next real heading) -- see the 2026-10-14 note in TdTokenizer.cpp.
    // Headings at or after supplementStart (an appended supplement's banner;
    // the text length if none) are not headings, and no section runs past it.
    void validateSectionHeadings(const UnicodeString & text, const vector<UnicodeString>& sections,
        const vector<size_t>& b, const vector<size_t>& e, size_t supplementStart,
        vector<bool>& valid, vector<size_t>& spanEnd);
    void combineSectionAnnotations(CAS & cas, const set<UnicodeString>& sectionNames,
        const vector<UnicodeString>& sections, const vector<size_t>& b, const vector<size_t>& e,
        const vector<bool>& valid, const vector<size_t>& spanEnd,
        const UnicodeString type, AnnotationCounter ac);
};

#endif	/* TDTOKENIZER_H */
