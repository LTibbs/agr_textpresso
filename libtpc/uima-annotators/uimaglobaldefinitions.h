// Global file containing all global definitions.

#ifndef UIMAGLOBALDEFINITIONS_H
#define UIMAGLOBALDEFINITIONS_H

#include <uima/api.hpp>
#include <set>
#include <string>

// If a composite delimiter exists, then there cannot be another delimiter
// that is a subset of that composite token delimiter. Decompose it accordingly.
// This applies to token and sentence delimiter
namespace tp_uima_globals {

// 2026-10-21: the em dash was written as a UTF-8 char* literal, which
// UnicodeString decodes with the platform default codepage, so it never
// matched U+2014 ("packages—" stayed one token). Non-ASCII delimiters are now
// given by code point. Curly quotes, bullet, en dash and minus were added
// when tai text stopped dropping non-ASCII (PdfTextNormalizer.h): without
// them a lexicon term inside quotes ("“tagging”") or next to a bullet could
// not match, because matches must start and end at a delimiter. jats text
// keeps these characters too.
// Punctuation before a line break (".\n", ",\n", ...) likewise: only ". " was a
// delimiter, so a term ending a line before its full stop or comma (1.2% of
// words in pdftotext output, which wraps lines) could never match.
inline const UnicodeString* token_delimiters() {
    static const auto* values = new const UnicodeString[31]{
        " ", "\n", "\t", "'", "\"",
        "/", UnicodeString((UChar32) 0x2014), "(", ")", "[",
        "]", "{", "}", ":", ". ",
        "; ", ", ", "! ", "? ",
        ".\n", ";\n", ",\n", "!\n", "?\n",
        UnicodeString((UChar32) 0x2013), // en dash
        UnicodeString((UChar32) 0x2212), // minus sign
        UnicodeString((UChar32) 0x2018), UnicodeString((UChar32) 0x2019), // single quotes
        UnicodeString((UChar32) 0x201c), UnicodeString((UChar32) 0x201d), // double quotes
        UnicodeString((UChar32) 0x2022) // bullet
    };
    return values;
}

// 2026-10-21: plus end punctuation + closing quote + whitespace (`weight.”\n`),
// 27 more. utf8Printable used to drop curly quotes, so ". " still matched;
// with them kept, a quoted sentence end ran into the next sentence. The
// ASCII double quote had the same gap and is covered too. Keep the array
// size equal to G_initS_No below.
inline const UnicodeString* sentence_delimiters() {
    static const UnicodeString* values = [] {
        UnicodeString* v = new UnicodeString[39]{
            ".\n", "!\n", "?\n", ". ", "! ", "? ",
            ".\t", "!\t", "?\t", ".<", "!<", "?<"
        };
        int n = 12;
        for (UChar32 p : {0x2e, 0x21, 0x3f}) // . ! ?
            for (UChar32 q : {0x201d, 0x2019, 0x22}) // closing double, closing single, ASCII "
                for (UChar32 w : {0x20, 0x0a, 0x09})
                    v[n++] = UnicodeString(p).append(q).append(w);
        return v;
    }();
    return values;
}

inline const UnicodeString* pdf_tags() {
    static const auto* values = new const UnicodeString[8]{
        "<_pdf _image", "<_pdf _sbr", "<_pdf _hbr", "<_pdf _fsc",
        "<_pdf _fnc", "<_pdf _ydiff", "<_pdf _cr", "<_pdf _page"
    };
    return values;
}

inline const std::set<UnicodeString>& sectionArticleB() {
    static const auto* values = new std::set<UnicodeString>{"Beginning of Article\n"};
    return *values;
}

inline const std::set<UnicodeString>& sectionArticleE() {
    static const auto* values = new std::set<UnicodeString>{"End of Article\n"};
    return *values;
}

inline const std::set<UnicodeString>& sectionAbstract() {
    static const auto* values = new std::set<UnicodeString>{
        "Abstract\n", "A b s t r a c t\n", "ABSTRACT\n", "A B S T R A C T\n",
        "Summary\n", "S u m m a r y\n", "SUMMARY\n", "S U M M A R Y\n"
    };
    return *values;
}

inline const std::set<UnicodeString>& sectionIntroduction() {
    static const auto* values = new std::set<UnicodeString>{
        "Introduction\n", "I n t r o d u c t i o n\n", "INTRODUCTION\n", "I N T R O D U C T I O N\n",
        "Literature Review\n", "L i t e r a t u r e   R e v i e w\n",
        "LITERATURE REVIEW\n", "L I T E R A T U R E   R E V I E W\n",
        // sentence case / all-lowercase -- some journals don't title-case every
        // word of a multi-word heading (see uimaglobaldefinitions.h header note)
        "literature review\n", "Literature review\n"
    };
    return *values;
}

inline const std::set<UnicodeString>& sectionResult() {
    static const auto* values = new std::set<UnicodeString>{
        "Result\n", "R e s u l t\n", "RESULT\n", "R E S U L T\n",
        "Results\n", "R e s u l t s\n", "RESULTS\n", "R E S U L T S\n",
        // combined header: also tagged "discussion" (see sectionDiscussion()) --
        // TdTokenizer::combineSectionAnnotations() creates one annotation per
        // type independently, so membership in both sets makes this span
        // findable under both --type result and --type discussion.
        "Results and Discussion\n", "R e s u l t s   a n d   D i s c u s s i o n\n",
        "RESULTS AND DISCUSSION\n", "R E S U L T S   A N D   D I S C U S S I O N\n",
        "results and discussion\n", "Results and discussion\n"
    };
    return *values;
}

inline const std::set<UnicodeString>& sectionDiscussion() {
    static const auto* values = new std::set<UnicodeString>{
        "Discussion\n", "D i s c u s s i o n\n", "DISCUSSION\n", "D I S C U S S I O N\n",
        // combined header, also tagged "result" -- see sectionResult()
        "Results and Discussion\n", "R e s u l t s   a n d   D i s c u s s i o n\n",
        "RESULTS AND DISCUSSION\n", "R E S U L T S   A N D   D I S C U S S I O N\n",
        "results and discussion\n", "Results and discussion\n",
        // combined header, also tagged "conclusion" -- see sectionConclusion()
        "Discussion and Conclusion\n", "D i s c u s s i o n   a n d   C o n c l u s i o n\n",
        "DISCUSSION AND CONCLUSION\n", "D I S C U S S I O N   A N D   C O N C L U S I O N\n",
        "discussion and conclusion\n", "Discussion and conclusion\n",
        "Discussion and Conclusions\n", "D i s c u s s i o n   a n d   C o n c l u s i o n s\n",
        "DISCUSSION AND CONCLUSIONS\n", "D I S C U S S I O N   A N D   C O N C L U S I O N S\n",
        "discussion and conclusions\n", "Discussion and conclusions\n"
    };
    return *values;
}

inline const std::set<UnicodeString>& sectionConclusion() {
    static const auto* values = new std::set<UnicodeString>{
        "Conclusion\n", "C o n c l u s i o n\n", "CONCLUSION\n", "C O N C L U S I O N\n",
        "Conclusions\n", "C o n c l u s i o n s\n", "CONCLUSIONS\n", "C O N C L U S I O N S\n",
        "Concluding Remarks\n", "C o n c l u d i n g   R e m a r k s\n",
        "CONCLUDING REMARKS\n", "C O N C L U D I N G   R E M A R K S\n",
        "concluding remarks\n", "Concluding remarks\n",
        // combined header, also tagged "discussion" -- see sectionDiscussion()
        "Discussion and Conclusion\n", "D i s c u s s i o n   a n d   C o n c l u s i o n\n",
        "DISCUSSION AND CONCLUSION\n", "D I S C U S S I O N   A N D   C O N C L U S I O N\n",
        "discussion and conclusion\n", "Discussion and conclusion\n",
        "Discussion and Conclusions\n", "D i s c u s s i o n   a n d   C o n c l u s i o n s\n",
        "DISCUSSION AND CONCLUSIONS\n", "D I S C U S S I O N   A N D   C O N C L U S I O N S\n",
        "discussion and conclusions\n", "Discussion and conclusions\n"
    };
    return *values;
}

inline const std::set<UnicodeString>& sectionBackground() {
    static const auto* values = new std::set<UnicodeString>{
        "Background\n", "B a c k g r o u n d\n", "BACKGROUND\n", "B A C K G R O U N D\n"
    };
    return *values;
}

inline const std::set<UnicodeString>& sectionMaterialsMethods() {
    static const auto* values = new std::set<UnicodeString>{
        "Material\n", "M a t e r i a l\n", "MATERIAL\n", "M A T E R I A L\n",
        "Materials\n", "M a t e r i a l s\n", "MATERIALS\n", "M A T E R I A L S\n",
        "Method\n", "M e t h o d\n", "METHOD\n", "M E T H O D\n",
        "Methods\n", "M e t h o d s\n", "METHODS\n", "M E T H O D S\n",
        "Material and Method\n", "M a t e r i a l   a n d   M e t h o d\n", "MATERIAL AND METHOD\n",
        "M A T E R I A L   A N D   M E T H O D\n", "Material and Methods\n",
        "M a t e r i a l   a n d   M e t h o d s\n", "MATERIAL AND METHODS\n",
        "M A T E R I A L   A N D   M E T H O D S\n", "Materials and Method\n",
        "M a t e r i a l s   a n d  M e t h o d\n", "MATERIALS AND METHOD\n",
        "M A T E R I A L S   A N D   M E T H O D\n", "Materials and Methods\n",
        "M a t e r i a l s   a n d   M e t h o d s\n", "MATERIALS AND METHODS\n",
        "M A T E R I A L S   A N D   M E T H O D S\n", "Material And Method\n",
        "M a t e r i a l   A n d   M e t h o d\n", "Material And Methods\n",
        "M a t e r i a l   A n d   M e t h o d s\n", "Materials And Method\n",
        "M a t e r i a l s   A n d   M e t h o d\n", "Materials And Methods\n",
        "M a t e r i a l s   A n d   M e t h o d s", "Material and method\n",
        "M a t e r i a l   a n d   m e t h o d\n", "Material and methods\n",
        "M a t e r i a l   a n d   m e t h o d s\n", "Materials and method\n",
        "M a t e r i a l s   a n d   m e t h o d\n", "Materials and methods\n",
        "M a t e r i a l s   a n d   m e t h o d s\n",
        "Experimental Procedures\n", "E x p e r i m e n t a l   P r o c e d u r e s\n",
        "EXPERIMENTAL PROCEDURES\n", "E X P E R I M E N T A L   P R O C E D U R E S\n",
        "experimental procedures\n", "Experimental procedures\n",
        // added 2026-08-14: classic Nature "Article" format ("Methods Summary")
        // and the Nature Genetics/Nature Methods "Online Methods" convention --
        // found while auditing zero-section SorghumBase papers; neither was in
        // the original synonym set despite being common, consistent headings
        // across that journal family, not one-off formatting quirks.
        "Methods Summary\n", "M e t h o d s   S u m m a r y\n",
        "METHODS SUMMARY\n", "M E T H O D S   S U M M A R Y\n",
        "methods summary\n", "Methods summary\n",
        "Online Methods\n", "O n l i n e   M e t h o d s\n",
        "ONLINE METHODS\n", "O N L I N E   M E T H O D S\n",
        "online methods\n", "Online methods\n"
    };
    return *values;
}

inline const std::set<UnicodeString>& sectionDesign() {
    static const auto* values = new std::set<UnicodeString>{
        "Design\n", "D e s i g n\n", "DESIGN\n", "D E S I G N\n",
        "Designs\n", "D e s i g n s\n", "DESIGNS\n", "D E S I G N S\n"
    };
    return *values;
}

inline const std::set<UnicodeString>& sectionAcknowledgments() {
    static const auto* values = new std::set<UnicodeString>{
        "Acknowledgment\n", "A c k n o w l e d g m e n t\n", "ACKNOWLEDGMENT\n", "A C K N O W L E D G M E N T\n",
        "Acknowledgments\n", "A c k n o w l e d g m e n t s\n", "ACKNOWLEDGMENTS\n", "A C K N O W L E D G M E N T S\n",
        "Acknowledgement\n", "A c k n o w l e d g e m e n t\n", "ACKNOWLEDGEMENT\n", "A C K N O W L E D G E M E N T\n",
        "Acknowledgements\n", "A c k n o w l e d g e m e n t s\n", "ACKNOWLEDGEMENTS\n", "A C K N O W L E D G E M E N T S\n"
    };
    return *values;
}

inline const std::set<UnicodeString>& sectionReferences() {
    static const auto* values = new std::set<UnicodeString>{
        "Reference\n", "R e f e r e n c e\n", "REFERENCE\n", "R E F E R E N C E\n",
        "References\n", "R e f e r e n c e s\n", "REFERENCES\n", "R E F E R E N C E S\n",
        "Bibliography\n", "B i b l i o g r a p h y\n", "BIBLIOGRAPHY\n", "B I B L I O G R A P H Y\n",
        "Literature Cited\n", "L i t e r a t u r e   C i t e d\n",
        "LITERATURE CITED\n", "L I T E R A T U R E   C I T E D\n",
        "literature cited\n", "Literature cited\n",
        "References Cited\n", "R e f e r e n c e s   C i t e d\n",
        "REFERENCES CITED\n", "R E F E R E N C E S   C I T E D\n",
        "references cited\n", "References cited\n",
        "Works Cited\n", "W o r k s   C i t e d\n", "WORKS CITED\n", "W O R K S   C I T E D\n",
        "works cited\n", "Works cited\n"
    };
    return *values;
}

inline const std::string* server_names() {
    static const auto* values = new const std::string[3]{
        "http://goldturtle.caltech.edu/cgi-bin/ReceivePost.cgi",
        "http://go-genkisugi.rhcloud.com/capella",
        "http://localhost/cgi-bin/ReceivePost.cgi"
    };
    return values;
}

inline const UnicodeString& pdf_tag_start() {
    static const auto* value = new UnicodeString("<_pdf ");
    return *value;
}

inline const UnicodeString& pdf_tag_end() {
    static const auto* value = new UnicodeString("/>");
    return *value;
}

}  // namespace tp_uima_globals

const int G_initT_No = 31;
const int G_initS_No = 39;
const int G_initP_No = 8;
const int ServerNames_No = 3;

#endif
