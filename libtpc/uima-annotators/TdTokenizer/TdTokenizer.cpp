/* 
 * File:   TdTokenizer.cpp
 * Author: mueller
 * 
 * Created on December 13, 2021, 2:15 PM
 */

#include "TdTokenizer.h"
#include "../uimaglobaldefinitions.h"
#include "textAndImageManager.h"
#include "../PdfTextNormalizer.h"
#include <boost/regex.hpp>
#include <algorithm>
#include <map>
#include <boost/filesystem.hpp>
#include <sstream>
#include <iomanip>

using namespace std;
using namespace boost::filesystem;

/* magic numbers from http://www.isthe.com/chongo/tech/comp/fnv/ */
static const uint64_t InitialFNV = 14695981039346656037U;
static const uint64_t FNVMultiple = 1099511628211;

/* Fowler / Noll / Vo (FNV) Hash */
string tpfnv(const UnicodeStringRef &s) {
    uint64_t hash = InitialFNV;
    for (uint64_t i = 0; i < s.length(); i++) {
        hash = hash ^ (s[i]); /* xor  the low 8 bits */
        hash = hash * FNVMultiple; /* multiply by the magic number */
    }
    stringstream shex;
    shex << setw(16) << setfill('0') << hex << hash;
    return shex.str();
}

pair<int32_t, int32_t> MaximizeBoundaries(pair<int32_t, int32_t> i,
        pair<int32_t, int32_t> j) {
    // need to make these modifications (offsets) because
    // trie class has different boundary convention than UIMA
    int32_t ifi = i.first;
    int32_t ise = i.second + 1;
    int32_t jfi = j.first;
    int32_t jse = j.second + 1;
    if (ifi != jfi) {
        if (ise != jse) {
            // at least one boundaries needs to be the same;
            // return error code -1.
            return make_pair(-1, -1);
        } else {
            // upper boundary is the same;
            // adjust lower boundary.
            return make_pair((ifi < jfi) ? ifi : jfi, ise);
        }
    } else {
        // lower boundary is the same;
        // adjust upper boundary (trivial if upper boundary are the same).
        return make_pair(ifi, (ise > jse) ? ise : jse);
    }
}

void WriteOutAnnotations(CAS & tcas, const UnicodeStringRef usdocref,
        vector< pair<int32_t, int32_t> > & p, Type t, Feature f,
        AnnotationCounter & ac, bool writeoutdelimiters) {
    FSIndexRepository & indexRep = tcas.getIndexRepository();
    vector< pair<int32_t, int32_t> > merged;
    merged.clear();
    vector< pair<int32_t, int32_t> >::iterator it = p.begin();
    while (it < p.end()) {
        vector< pair<int32_t, int32_t> >::iterator it2 = it + 1;
        pair<int32_t, int32_t> res = MaximizeBoundaries(*it, *it2);
        if (res.first < 0) {
            res = make_pair((*it).first, (*it).second + 1);
        } else {
            it++;
        }
        merged.push_back(res);
        it++;
    }
    // use unique beginnings and endings of delimiters to annotate items at hand.
    int32_t laste = 0;
    for (it = merged.begin(); it != merged.end(); it++) {
        int32_t b = (*it).first;
        int32_t e = (*it).second;
        if (b != laste) {
            AnnotationFS fsNewTok = tcas.createAnnotation(t, laste, b);
            UnicodeString wd;
            usdocref.extract(laste, b - laste, wd);
            fsNewTok.setStringValue(f, wd);
            Feature faid = t.getFeatureByBaseName("aid");
            if (faid.isValid())
                fsNewTok.setIntValue(faid, ac.GetNextId());
            indexRep.addFS(fsNewTok);
        }
        laste = e;
        if (writeoutdelimiters) {
            AnnotationFS fsNewTok = tcas.createAnnotation(t, b, e);
            UnicodeString wd;
            usdocref.extract(b, e - b, wd);
            fsNewTok.setStringValue(f, wd);
            Feature faid = t.getFeatureByBaseName("aid");
            if (faid.isValid())
                fsNewTok.setIntValue(faid, ac.GetNextId());
            indexRep.addFS(fsNewTok);
        }
    }
}

vector< pair<int32_t, int32_t> > RemoveDelimiters(const UnicodeStringRef usdocref,
        vector<string> & regex,
        vector< pair<int32_t, int32_t> > & p,
        int32_t mfcl, int32_t mbcl) {
    vector< pair<int32_t, int32_t> > result;
    vector< pair<int32_t, int32_t> >::iterator it;
    for (it = p.begin(); it != p.end(); it++) {
        int32_t b = (*it).first;
        int32_t e = (*it).second + 1;
        string strip;
        usdocref.extract(b - mfcl, e - b + mfcl + mbcl, strip);
        bool matches = false;
        vector<string>::iterator is;
        for (is = regex.begin(); is != regex.end(); is++) {
            boost::regex expr(*is);
            matches = (matches || boost::regex_search(strip, expr));
        }
        if (!matches) {
            result.push_back(*it);
        } else {
            b = ((b - 100 > 0)) ? b - 100 : 0;
            usdocref.extract(b, 200, strip);
        }
    }
    return result;
}

// The raw document text carries inline PDF layout markers produced by text
// extraction (e.g. "<_pdf _cr/>", "<_pdf _fsc=+11/>") interspersed directly
// in the character stream. A section heading is typically followed by one
// of these markers before the actual line break -- e.g. the literal text
// "References <_pdf _cr/>\n" -- which never contains the exact substring
// "References\n" that the section trie searches for, so headings never
// match and no section boundaries are ever detected. This builds a copy of
// the text with every "<_pdf ...>" marker removed, and any space left
// dangling directly before a newline as a result (the marker is almost
// always preceded by a word-separating space) collapsed away, while
// recording, for each character kept, its offset in the original text so
// match positions found in the cleaned copy can be translated back to the
// original document for annotation creation.
UnicodeString CleanPdfTagsForSectionSearch(const UnicodeString & src,
        vector<int32_t> & posmap) {
    static const UnicodeString tagPrefix("<_pdf ");
    UnicodeString cleaned;
    posmap.clear();
    int32_t n = src.length();
    bool pendingSpace = false;
    int32_t pendingSpacePos = -1;
    for (int32_t i = 0; i < n;) {
        if (i + tagPrefix.length() <= n &&
                src.compare(i, tagPrefix.length(), tagPrefix) == 0) {
            int32_t j = i + tagPrefix.length();
            while (j < n && src.charAt(j) != '>') j++;
            if (j < n) j++; // consume the closing '>'
            i = j;
            continue;
        }
        UChar c = src.charAt(i);
        if (c == ' ') {
            if (pendingSpace) {
                cleaned.append((UChar) ' ');
                posmap.push_back(pendingSpacePos);
            }
            pendingSpace = true;
            pendingSpacePos = i;
        } else if (c == '\n') {
            pendingSpace = false; // drop dangling space before the newline
            cleaned.append(c);
            posmap.push_back(i);
        } else {
            if (pendingSpace) {
                cleaned.append((UChar) ' ');
                posmap.push_back(pendingSpacePos);
                pendingSpace = false;
            }
            cleaned.append(c);
            posmap.push_back(i);
        }
        i++;
    }
    if (pendingSpace) {
        cleaned.append((UChar) ' ');
        posmap.push_back(pendingSpacePos);
    }
    return cleaned;
}

// Some journals' articles print their bibliography as a bare numbered
// list with no "References" (or any other registered synonym) heading
// text anywhere in the document -- no literal-string trie match can ever
// find a heading that was never printed, so this scans for the numbered-
// list pattern itself as a fallback signal. Added 2026-08-14 after
// auditing a batch of SorghumBase zero-section papers and finding several
// genuinely well-structured papers permanently blocked on this exact
// case; generalized the same day after the first version (position-gated
// to the back half of the document, format-gated to "N. Author..." all on
// one line) turned out to miss a second real variant: modern Nature-family
// papers that place references right after the main text (as little as
// ~25% through the document, followed by the full Methods and dozens of
// pages of Extended Data figures) and whose text extraction sometimes
// splits the citation number onto its own line, separate from the author
// name ("2.\nRich-Griffin, C. et al...." rather than "2. Rich-Griffin, C.
// et al....").
//
// Per-match pattern: newline, 1-3 digits, '.', then whitespace (spaces/
// tabs, or up to one newline to cover the split-line case above), then an
// uppercase letter -- the start of an author surname. Position is no
// longer used as a filter (references can legitimately sit anywhere from
// roughly a quarter of the way through a modern paper to the very end of
// an older one) -- instead, each candidate match must be followed within
// a short window by a parenthesized 4-digit year, "(2006)"/"(2020)" style,
// which is close to universal for a real citation and essentially never
// occurs in an unrelated numbered list (protocol steps, supplementary
// item lists). This is a stronger and more format-independent signal than
// position ever was.
//
// Because position is no longer a filter, matches are then clustered by
// proximity (kMaxGap) and only the largest tight cluster is used, ending
// shortly after its own last entry -- not "run to End of Article" -- so a
// modern paper's references don't swallow the many unrelated pages of
// Methods and Extended Data that typically follow them.
//
// Operates on the same PDF-tag-cleaned buffer the heading trie searches
// (CleanPdfTagsForSectionSearch above), for the same reason that cleanup
// exists: a marker sitting between the newline and the digits would
// otherwise break the match. Returns [-1,-1] (in the cleaned buffer's own
// coordinates -- the caller maps back through posmap) if no qualifying
// span was found.
pair<int32_t, int32_t> DetectImplicitReferencesSpan(const UnicodeString & cleaned) {
    static const size_t kMinMatches = 5;
    static const int32_t kYearWindow = 400;   // chars to look ahead for "(YYYY)"
    static const int32_t kMaxGap = 600;       // chars between entries before a cluster breaks
    static const int32_t kTrailingBuffer = 500; // chars kept past the last entry in a cluster
    int32_t len = cleaned.length();
    vector<int32_t> matchPositions;
    for (int32_t i = 0; i < len - 3; i++) {
        if (cleaned.charAt(i) != (UChar) '\n') continue;
        int32_t j = i + 1;
        int32_t numStart = j;
        while (j < len && (j - numStart) < 3 &&
                cleaned.charAt(j) >= (UChar) '0' && cleaned.charAt(j) <= (UChar) '9') j++;
        if (j == numStart) continue; // no digits right after the newline
        if (j >= len || cleaned.charAt(j) != (UChar) '.') continue;
        j++;
        int32_t wsStart = j;
        int32_t newlineCount = 0;
        while (j < len && newlineCount <= 1 &&
                (cleaned.charAt(j) == (UChar) ' ' || cleaned.charAt(j) == (UChar) '\t' ||
                 cleaned.charAt(j) == (UChar) '\n')) {
            if (cleaned.charAt(j) == (UChar) '\n') newlineCount++;
            j++;
        }
        if (j == wsStart) continue; // require whitespace (or one newline) right after the period
        if (j >= len || cleaned.charAt(j) < (UChar) 'A' || cleaned.charAt(j) > (UChar) 'Z') continue;
        // require a parenthesized 4-digit year within the next kYearWindow
        // characters -- the real per-entry bibliography signal, now doing
        // the work position used to do.
        bool hasYear = false;
        int32_t scanEnd = min(j + kYearWindow, len - 6);
        for (int32_t k = j; k < scanEnd; k++) {
            if (cleaned.charAt(k) != (UChar) '(') continue;
            bool allDigits = true;
            for (int32_t d = 1; d <= 4; d++)
                if (cleaned.charAt(k + d) < (UChar) '0' || cleaned.charAt(k + d) > (UChar) '9')
                    allDigits = false;
            if (allDigits && cleaned.charAt(k + 5) == (UChar) ')') { hasYear = true; break; }
        }
        if (!hasYear) continue;
        matchPositions.push_back(i + 1); // position of the digit itself
    }
    if (matchPositions.size() < kMinMatches) return make_pair(-1, -1);
    // cluster by proximity; keep only the largest tight cluster, so an
    // isolated qualifying match far from the real bibliography (a false
    // positive that individually passed the year check) can't drag the
    // span out or get chosen over the real list.
    vector< pair<int32_t, int32_t> > clusters; // [firstIdx, lastIdx] into matchPositions
    size_t clusterStart = 0;
    for (size_t k = 1; k <= matchPositions.size(); k++) {
        if (k == matchPositions.size() || matchPositions[k] - matchPositions[k - 1] > kMaxGap) {
            clusters.push_back(make_pair((int32_t) clusterStart, (int32_t) (k - 1)));
            clusterStart = k;
        }
    }
    size_t bestCluster = 0;
    size_t bestSize = 0;
    for (size_t c = 0; c < clusters.size(); c++) {
        size_t sz = clusters[c].second - clusters[c].first + 1;
        if (sz > bestSize) { bestSize = sz; bestCluster = c; }
    }
    if (bestSize < kMinMatches) return make_pair(-1, -1);
    int32_t spanBegin = matchPositions[clusters[bestCluster].first];
    int32_t lastMatch = matchPositions[clusters[bestCluster].second];
    int32_t spanEnd = min(lastMatch + kTrailingBuffer, len);
    return make_pair(spanBegin, spanEnd);
}

// --- 2026-10-14: false and missed section headings ---------------------------
// The section trie accepts any line ending in a heading word ("Materials\n",
// "Reference\n", ...), and a section used to run until the next such match.
// In PDF text that misfires on a wrapped in-text cross-reference ("...(see
// Materials\nand methods)"), a table column header ("Reference"), or a
// wrapped journal name inside the reference list ("Methods\nEcol Evol ...");
// one false heading then relabels everything up to the next real one. It
// also misses headings the PDF letter-spaces unevenly ("AC K N OW L E D G M
// E N T S", "M ATE R I A L S A N D M E TH O DS"), so the section before them
// runs on -- e.g. an "abstract" covering a whole review. Measured over
// MaizeOA: ~30% of papers had a false heading (including ~120 with a table
// labelled "references" before body text, which search's default references
// exclusion then drops) and 35 had missed letter-spaced headings.
//
// Letter-spaced heading lines are now rewritten to their known form before
// the trie search (CollapseLetterSpacedHeadings) and recognised as headings
// (CanonicalHeading); validateSectionHeadings decides which headings are
// real, and a section runs to the next *real* heading, so a false one is
// folded into the section before it. Same rules, applied at read time, as
// maizegdb_textpresso_implementation's textpresso_classifiers/casannot.py
// (add_letter_spaced_headings, repair_sections).
namespace {
    enum : unsigned {
        kSecArticleB = 1u << 0, kSecArticleE = 1u << 1, kSecAbstract = 1u << 2,
        kSecIntroduction = 1u << 3, kSecResult = 1u << 4, kSecDiscussion = 1u << 5,
        kSecConclusion = 1u << 6, kSecBackground = 1u << 7, kSecMaterialsMethods = 1u << 8,
        kSecDesign = 1u << 9, kSecAcknowledgments = 1u << 10, kSecReferences = 1u << 11
    };
    const unsigned kSecFrames = kSecArticleB | kSecArticleE;
    const unsigned kSecBody = kSecIntroduction | kSecResult | kSecDiscussion | kSecConclusion
            | kSecBackground | kSecMaterialsMethods | kSecDesign;

    // Which section types a raw-section string is a heading for (0 = body text).
    unsigned SectionMask(const UnicodeString & s) {
        unsigned m = 0;
        if (tp_uima_globals::sectionArticleB().count(s)) m |= kSecArticleB;
        if (tp_uima_globals::sectionArticleE().count(s)) m |= kSecArticleE;
        if (tp_uima_globals::sectionAbstract().count(s)) m |= kSecAbstract;
        if (tp_uima_globals::sectionIntroduction().count(s)) m |= kSecIntroduction;
        if (tp_uima_globals::sectionResult().count(s)) m |= kSecResult;
        if (tp_uima_globals::sectionDiscussion().count(s)) m |= kSecDiscussion;
        if (tp_uima_globals::sectionConclusion().count(s)) m |= kSecConclusion;
        if (tp_uima_globals::sectionBackground().count(s)) m |= kSecBackground;
        if (tp_uima_globals::sectionMaterialsMethods().count(s)) m |= kSecMaterialsMethods;
        if (tp_uima_globals::sectionDesign().count(s)) m |= kSecDesign;
        if (tp_uima_globals::sectionAcknowledgments().count(s)) m |= kSecAcknowledgments;
        if (tp_uima_globals::sectionReferences().count(s)) m |= kSecReferences;
        return m;
    }

    bool IsAsciiLetter(UChar c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
    bool IsBlank(UChar c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

    // --- letter-spaced headings --------------------------------------------

    // A whole line of 1-5 letter fragments separated by single spaces, at
    // least 3 fragments ("AC K N OW L E D G M E N T S").
    bool IsLetterSpaced(const UnicodeString & line) {
        int32_t i = 0, end = line.length();
        while (i < end && IsBlank(line.charAt(i))) i++;
        while (end > i && IsBlank(line.charAt(end - 1))) end--;
        int32_t frags = 0, fragLen = 0;
        for (; i < end; i++) {
            UChar c = line.charAt(i);
            if (IsAsciiLetter(c)) {
                if (++fragLen > 5) return false;
            } else if (c == ' ' && fragLen > 0) {
                frags++;
                fragLen = 0;
            } else {
                return false;
            }
        }
        if (fragLen == 0) return false;
        return frags + 1 >= 3;
    }

    UnicodeString LettersLower(const UnicodeString & s) {
        UnicodeString out;
        for (int32_t i = 0; i < s.length(); i++) {
            UChar c = s.charAt(i);
            if (c >= 'A' && c <= 'Z') out.append((UChar) (c - 'A' + 'a'));
            else if (c >= 'a' && c <= 'z') out.append(c);
        }
        return out;
    }

    // Known (non-frame) heading strings keyed by their letters, lowercased;
    // the value is the member with the fewest spaces (e.g. "MATERIALS AND
    // METHODS\n" rather than "M A T E R I A L S ...").
    const std::map<UnicodeString, UnicodeString>& HeadingsByLetters() {
        static const std::map<UnicodeString, UnicodeString>* byLetters = []() {
            auto* m = new std::map<UnicodeString, UnicodeString>();
            const std::set<UnicodeString>* sets[] = {
                &tp_uima_globals::sectionAbstract(), &tp_uima_globals::sectionIntroduction(),
                &tp_uima_globals::sectionResult(), &tp_uima_globals::sectionDiscussion(),
                &tp_uima_globals::sectionConclusion(), &tp_uima_globals::sectionBackground(),
                &tp_uima_globals::sectionMaterialsMethods(), &tp_uima_globals::sectionDesign(),
                &tp_uima_globals::sectionAcknowledgments(), &tp_uima_globals::sectionReferences()};
            auto spaces = [](const UnicodeString & s) {
                int32_t k = 0;
                for (int32_t i = 0; i < s.length(); i++) if (s.charAt(i) == ' ') k++;
                return k;
            };
            for (const auto* names : sets)
                for (const auto & h : *names) {
                    UnicodeString key(LettersLower(h));
                    auto it = m->find(key);
                    if (it == m->end()) (*m)[key] = h;
                    else if (spaces(h) < spaces(it->second)) it->second = h;
                }
            return m;
        }();
        return *byLetters;
    }

    // "AC K N OW L E D G M E N T S\n" -> the known "ACKNOWLEDGMENTS\n"-style
    // string, so the section-name sets recognise it; anything else unchanged.
    UnicodeString CanonicalHeading(const UnicodeString & s) {
        if (!IsLetterSpaced(s)) return s;
        const auto & byLetters = HeadingsByLetters();
        auto it = byLetters.find(LettersLower(s));
        return it == byLetters.end() ? s : it->second;
    }

    // Rewrites letter-spaced heading lines in the section-search text to
    // their known form so the trie matches them; posmap (one entry per
    // character) is kept in step, the rewritten line mapping to the original
    // line's span.
    void CollapseLetterSpacedHeadings(UnicodeString & cleaned, vector<int32_t> & posmap) {
        const auto & byLetters = HeadingsByLetters();
        UnicodeString out;
        vector<int32_t> outmap;
        int32_t n = cleaned.length();
        for (int32_t ls = 0; ls < n;) {
            int32_t le = cleaned.indexOf((UChar) '\n', ls);
            if (le < 0) le = n;
            UnicodeString line;
            cleaned.extract(ls, le - ls, line);
            auto it = (le > ls && IsLetterSpaced(line)) ? byLetters.find(LettersLower(line)) : byLetters.end();
            if (it != byLetters.end()) {
                UnicodeString canon(it->second);
                if (canon.endsWith(UnicodeString("\n"))) canon.truncate(canon.length() - 1);
                for (int32_t k = 0; k < canon.length(); k++) {
                    out.append(canon.charAt(k));
                    outmap.push_back(k + 1 < canon.length() ? posmap[ls] : posmap[le - 1]);
                }
            } else {
                for (int32_t k = ls; k < le; k++) {
                    out.append(cleaned.charAt(k));
                    outmap.push_back(posmap[k]);
                }
            }
            if (le < n) {
                out.append((UChar) '\n');
                outmap.push_back(posmap[le]);
            }
            ls = le + 1;
        }
        cleaned = out;
        posmap.swap(outmap);
    }

    // --- false headings ----------------------------------------------------

    UnicodeString StripPdfTags(const UnicodeString & s) {
        static const UnicodeString tagPrefix("<_pdf ");
        UnicodeString out;
        for (int32_t i = 0; i < s.length();) {
            if (s.compare(i, tagPrefix.length(), tagPrefix) == 0) {
                int32_t j = s.indexOf((UChar) '>', i);
                if (j < 0) break;
                i = j + 1;
                continue;
            }
            out.append(s.charAt(i++));
        }
        return out;
    }

    // Section numbering before a heading: "3 | ", "2.1 ", "IV. ".
    bool IsNumbering(const UnicodeString & prefix) {
        static const UnicodeString allowed = []() {
            UnicodeString a(" \t\r\n0123456789.|:()-");
            a.append((UChar) 0x2013).append((UChar) 0x2014).append((UChar) 0x2022); // en/em dash, bullet
            return a;
        }();
        int32_t roman = 0;
        for (int32_t i = 0; i < prefix.length(); i++) {
            UChar c = prefix.charAt(i);
            if (allowed.indexOf(c) >= 0) continue;
            if (c == 'I' || c == 'V' || c == 'X' || c == 'i' || c == 'v' || c == 'x') {
                if (++roman > 4) return false;
                continue;
            }
            return false;
        }
        return true;
    }

    bool HasLetter(const UnicodeString & s) {
        for (int32_t i = 0; i < s.length(); i++) if (IsAsciiLetter(s.charAt(i))) return true;
        return false;
    }

    // The text after the heading starts lowercase or with closing punctuation.
    bool ContinuesSentence(const UnicodeString & following) {
        for (int32_t i = 0; i < following.length(); i++) {
            UChar c = following.charAt(i);
            if (IsBlank(c)) continue;
            return (c >= 'a' && c <= 'z') || c == ')' || c == ']' || c == '}'
                    || c == ',' || c == ';' || c == '.' || c == ':';
        }
        return false;
    }

    // Is the heading at text[pos] mid-sentence? A real heading starts its
    // line, possibly after section numbering; a false one has prose before it
    // ("...(Fig. 2; see Materials"). Text glued to the heading word is part of
    // it ("STAR+METHODS", Cell's STAR-Methods). A heading on a line of its own
    // is false if sandwiched mid-sentence: the text after it continues a
    // sentence and the previous line is non-blank and doesn't end one. (Text
    // after a real heading can start lowercase on its own in two-column PDFs
    // -- the rest of a hyphenated word from the other column.)
    bool HeadingMisplaced(const UnicodeString & text, size_t pos, const UnicodeString & following) {
        int32_t p = (int32_t) pos;
        int32_t lo = max(0, p - 2000);
        int32_t nl = text.lastIndexOf((UChar) '\n', lo, p - lo);
        if (nl < 0 && p > 2000) return true; // no line break anywhere near: deep inside prose
        int32_t lineStart = nl < 0 ? 0 : nl + 1;
        UnicodeString prefix;
        text.extract(lineStart, p - lineStart, prefix);
        prefix = StripPdfTags(prefix);
        if (!IsNumbering(prefix) && prefix.length() > 0
                && IsBlank(prefix.charAt(prefix.length() - 1)) && HasLetter(prefix))
            return true;
        if (lineStart == 0 || !ContinuesSentence(following)) return false;
        int32_t prevEnd = lineStart - 1; // the '\n' ending the previous line
        int32_t lo2 = max(0, prevEnd - 2000);
        int32_t pnl = text.lastIndexOf((UChar) '\n', lo2, prevEnd - lo2);
        int32_t prevStart = pnl < 0 ? 0 : pnl + 1;
        UnicodeString prev;
        text.extract(prevStart, prevEnd - prevStart, prev);
        prev = StripPdfTags(prev);
        int32_t k = prev.length();
        while (k > 0 && IsBlank(prev.charAt(k - 1))) k--;
        if (k == 0) return false;
        UChar last = prev.charAt(k - 1);
        return !(last == '.' || last == '!' || last == '?' || last == ':');
    }

    // At least 3 reference-list entries at line starts ("Surname AB, ...",
    // "Surname, A. B.,", "1. Surname AB", "[12] Surname, A.") in the first
    // 3000 characters.
    bool LooksLikeReferenceList(const UnicodeString & text) {
        static const boost::regex entry(
                "^\\s*(?:\\[?\\d{1,3}[\\].)]?\\s+)?"
                "[A-Z](?:[\\w'\\-]|\\xE2\\x80\\x99)+(?:\\s[A-Z](?:[\\w'\\-]|\\xE2\\x80\\x99)+)?"
                ",?\\s(?:[A-Z]\\.?\\s?-?){1,3}[,.]");
        UnicodeString head;
        text.extract(0, min(text.length(), (int32_t) 3000), head);
        string utf8;
        head.toUTF8String(utf8);
        size_t count = 0;
        for (boost::sregex_iterator it(utf8.begin(), utf8.end(), entry), end; it != end; ++it)
            if (++count >= 3) return true;
        return false;
    }

    // The first non-empty line is the tail of a citation: volume(issue):pages,
    // a DOI, or a "(2019)" year -- e.g. "Ecol Evol 12(11):2084 ..." after a
    // "Methods" that was really the journal name "Methods in Ecology and
    // Evolution" wrapped across lines.
    bool ContinuesCitation(const UnicodeString & text) {
        static const boost::regex tail(
                "\\b\\d+\\s*\\(\\d+\\)\\s*:|doi\\.org/|\\bdoi:|\\(\\s*(?:19|20)\\d{2}[a-z]?\\s*\\)");
        int32_t start = 0, n = text.length();
        while (start < n) {
            int32_t end = text.indexOf((UChar) '\n', start);
            if (end < 0) end = n;
            UnicodeString line;
            text.extract(start, end - start, line);
            line.trim();
            if (line.length() > 0) {
                string utf8;
                line.toUTF8String(utf8);
                return boost::regex_search(utf8, tail);
            }
            start = end + 1;
        }
        return false;
    }

    // --- appended supplements (2026-10-22) ---------------------------------
    // The MaizeGDB literature pipeline appends supplementary files to the
    // article PDF behind a banner page:
    //   SUPPLEMENTARY MATERIAL\n<file name>\n<description>\n\ninserted by the
    //   MaizeGDB literature pipeline
    // No heading follows the reference list, so everything appended used to
    // be labelled "references" (1,052 MaizeOpenAccess papers, 224k
    // annotations), and the default references exclusion dropped it. Now
    // everything from the banner on is one "supplementary" section, and the
    // supplement's own headings ("Methods", "References" inside an appended
    // PDF) are not headings: they neither label text nor count towards the
    // section-detection score. Same rule as casannot.py's
    // split_supplementary(), which applies it at read time to older CAS2s.

    // Offset of the first pipeline supplement banner in text, or -1. The
    // pattern is casannot.py's _SUPPLEMENT_BANNER_RE; keep the two in step.
    int32_t FindSupplementBanner(const UnicodeString & text) {
        static const UnicodeString head("SUPPLEMENTARY MATERIAL");
        static const boost::regex banner(
                "SUPPLEMENTARY MATERIAL\\s*\\n(?:[^\\n]*\\n){0,6}?\\s*"
                "inserted by the MaizeGDB literature pipeline");
        for (int32_t pos = text.indexOf(head); pos >= 0; pos = text.indexOf(head, pos + 1)) {
            UnicodeString window;
            text.extract(pos, min(text.length() - pos, (int32_t) 20000), window);
            string utf8;
            window.toUTF8String(utf8);
            if (boost::regex_search(utf8, banner, boost::match_continuous)) return pos;
        }
        return -1;
    }

    // What a heading is judged on (2026-10-22): the text after its line, up
    // to the supplement banner, 3000 characters or the next references-
    // heading line -- the whole window for the reference-list test, its
    // first non-blank line for the citation test, its first non-blank
    // character for the mid-sentence test. It used to be the raw segment up
    // to the next heading of any kind, which a false heading inside a
    // reference list cut short. The window stops at a references heading
    // because a reference list belongs to the nearest one above it:
    // otherwise a "Reference" table column header just before "LITERATURE
    // CITED" (10.1534_g3.112.005207) took the list as its own.
    // casannot.py's following_text() is the same window over the
    // sofaString, so read-time repair agrees with this whichever CAS2 it
    // reads; keep the two in step.
    const int32_t kFollowingChars = 3000;

    // A line that is only a references heading: letters and spaces, PDF tags
    // aside, whose letters are one of the references heading words (spacing
    // and case ignored, as for letter-spaced headings). casannot.py's
    // _is_references_heading_line().
    bool IsReferencesHeadingLine(const UnicodeString & raw) {
        static const std::set<UnicodeString>* keys = []() {
            auto* k = new std::set<UnicodeString>();
            for (const auto & h : tp_uima_globals::sectionReferences()) k->insert(LettersLower(h));
            return k;
        }();
        UnicodeString line(StripPdfTags(raw));
        int32_t i = 0, end = line.length();
        while (i < end && IsBlank(line.charAt(i))) i++;
        while (end > i && IsBlank(line.charAt(end - 1))) end--;
        if (i == end) return false;
        for (int32_t k = i; k < end; k++) {
            UChar c = line.charAt(k);
            if (!IsAsciiLetter(c) && c != ' ' && c != '\t') return false;
        }
        return keys->count(LettersLower(line)) > 0;
    }

    UnicodeString FollowingText(const UnicodeString & text, size_t headingBegin, size_t limit) {
        UnicodeString out;
        int32_t nl = text.indexOf((UChar) '\n', (int32_t) headingBegin);
        if (nl < 0) return out;
        int32_t start = nl + 1;
        int32_t end = min((int32_t) min(limit, (size_t) text.length()), start + kFollowingChars);
        if (end > start) text.extract(start, end - start, out);
        for (int32_t ls = 0; ls < out.length();) {
            int32_t le = out.indexOf((UChar) '\n', ls);
            if (le < 0) le = out.length();
            UnicodeString line;
            out.extract(ls, le - ls, line);
            if (IsReferencesHeadingLine(line)) {
                out.truncate(ls);
                break;
            }
            ls = le + 1;
        }
        return out;
    }
}

void TdTokenizer::validateSectionHeadings(const UnicodeString & text,
        const vector<UnicodeString>& sections,
        const vector<size_t>& b, const vector<size_t>& e,
        size_t supplementStart,
        vector<bool>& valid, vector<size_t>& spanEnd) {
    size_t n = sections.size();
    vector<unsigned> mask(n);
    for (size_t i = 0; i < n; i++) mask[i] = SectionMask(sections[i]);
    // A heading inside an appended supplement is supplement text (see
    // FindSupplementBanner); the article frames are left alone.
    for (size_t i = 0; i < n; i++)
        if (b[i] >= supplementStart && !(mask[i] & kSecFrames)) mask[i] = 0;
    auto following = [&](size_t i) { return FollowingText(text, b[i], supplementStart); };
    auto isImplicitRefs = [](const UnicodeString & f) { return f.startsWith(UnicodeString("(implicit")); };
    // 1. a mid-sentence "heading" (see HeadingMisplaced) isn't a heading
    valid.assign(n, false);
    for (size_t i = 0; i < n; i++)
        if (mask[i]) valid[i] = (mask[i] & kSecFrames) || !HeadingMisplaced(text, b[i], following(i));
    bool haveBody = false;
    size_t lastBody = 0;
    for (size_t i = 0; i < n; i++)
        if (valid[i] && (mask[i] & kSecBody)) { haveBody = true; lastBody = b[i]; }
    // "references" headings that are followed by an actual reference list
    vector<bool> realRefs(n, false);
    for (size_t i = 0; i < n; i++)
        if (valid[i] && mask[i] == kSecReferences) realRefs[i] = LooksLikeReferenceList(following(i));
    long prev = -1; // previous real non-frame heading
    for (size_t i = 0; i < n; i++) {
        if (!valid[i] || (mask[i] & kSecFrames)) continue;
        UnicodeString f(following(i));
        if (mask[i] == kSecReferences) {
            // 2. "references" not followed by a reference list (a table column
            // header, say): false before body text, or when a later
            // "references" heading is followed by the real list.
            if (!realRefs[i] && !isImplicitRefs(f)) {
                bool laterReal = false;
                for (size_t j = i + 1; j < n && !laterReal; j++) laterReal = realRefs[j];
                if ((haveBody && b[i] < lastBody) || laterReal) valid[i] = false;
            }
        } else if (prev >= 0 && (mask[prev] & kSecReferences) && ContinuesCitation(f)) {
            // 3. right after the references, and the next line is the rest of
            // a citation: part of the reference list
            valid[i] = false;
        }
        // 4. the same section heading again right after itself: one section
        if (valid[i] && prev >= 0 && mask[prev] == mask[i]) valid[i] = false;
        if (valid[i]) prev = (long) i;
    }
    // Each real heading's section runs to the next real heading. (Previously
    // it ran to e[i + 1], i.e. the next heading of any kind.)
    // Headings are at most a few hundred per paper, so a plain scan is fine.
    spanEnd.assign(n, 0);
    for (size_t i = 0; i < n; i++) {
        size_t end = e[n - 1];
        for (size_t j = i + 2; j < n; j++)
            if (valid[j]) { end = b[j]; break; }
        spanEnd[i] = (i + 1 < n) ? max(end, e[i + 1]) : e[i];
        // the article's last section ends where the supplement begins
        if (b[i] < supplementStart && !(mask[i] & kSecFrames))
            spanEnd[i] = min(spanEnd[i], supplementStart);
    }
}

bool TdTokenizer::hasSection(const set<UnicodeString>& sectionNames,
        const vector<UnicodeString>& sections) {
    bool ret = false;
    for (auto x : sections) if (sectionNames.find(x) != sectionNames.end()) return true;
    return ret;
}

void TdTokenizer::combineSectionAnnotations(CAS & cas,
        const set<UnicodeString>& sectionNames,
        const vector<UnicodeString>& sections,
        const vector<size_t>& b, const vector<size_t>& e,
        const vector<bool>& valid, const vector<size_t>& spanEnd,
        const UnicodeString type,
        AnnotationCounter ac) {
    FSIndexRepository & indexRep = cas.getIndexRepository();
    UnicodeStringRef text = cas.getDocumentText();
    for (size_t i = 0; i + 1 < sections.size(); i++) {
        if (valid[i] && sectionNames.find(sections[i]) != sectionNames.end())
            if (e[i] == b[i + 1]) {
                AnnotationFS fsSection = cas.createAnnotation(sectiontype_, b[i], spanEnd[i]);
                fsSection.setStringValue(sectiontype_type_, type);
                // content is what Tpcas2SingleIndex indexes for the section
                // field, so it covers the whole span: everything up to the
                // next *real* heading, but not past a supplement banner.
                // (sections[i + 1] stopped at the next heading of any kind,
                // so section-scoped search missed text after a false heading
                // and, once spans were clipped at the banner, would have
                // still searched the supplement as "references".)
                UnicodeString content;
                if (spanEnd[i] > e[i]) text.extract(e[i], spanEnd[i] - e[i], content);
                fsSection.setStringValue(sectiontype_content_, content);
                Feature faid = sectiontype_.getFeatureByBaseName("aid");
                if (faid.isValid()) fsSection.setIntValue(faid, ac.GetNextId());
                indexRep.addFS(fsSection);
            }
    }
}

TdTokenizer::TdTokenizer() {
    dlsetToken_ = set<UnicodeString >(
            tp_uima_globals::token_delimiters(),
            tp_uima_globals::token_delimiters() + G_initT_No);
    dlsetSentence_ = set<UnicodeString >(
            tp_uima_globals::sentence_delimiters(),
            tp_uima_globals::sentence_delimiters() + G_initS_No);
    disqSentence_.push_back("[\\(>\\s][A-Z]\\.[<\\s]$");
    const string part1 = "[\\(>\\s](Prof|Ph\\.D|Dr|[Ff]igs?|[Vv]ol|i\\.e|e\\.g";
    const string part2 = "|[Nn]o|[Vv]s|[Ee]x|al|ca)\\.[<\\s]$";
    disqSentence_.push_back(part1 + part2);
    maxfrontdisqcharlength_ = 4; // sniplet length in front of sentence delimiter
    maxbackdisqcharlength_ = 0; // sniplet length  following sentence delimiter.
    dlsetSection_.clear();
    for (auto x : tp_uima_globals::sectionArticleB()) dlsetSection_.insert(x);
    for (auto x : tp_uima_globals::sectionArticleE()) dlsetSection_.insert(x);
    for (auto x : tp_uima_globals::sectionAbstract()) dlsetSection_.insert(x);
    for (auto x : tp_uima_globals::sectionIntroduction()) dlsetSection_.insert(x);
    for (auto x : tp_uima_globals::sectionResult()) dlsetSection_.insert(x);
    for (auto x : tp_uima_globals::sectionDiscussion()) dlsetSection_.insert(x);
    for (auto x : tp_uima_globals::sectionConclusion()) dlsetSection_.insert(x);
    for (auto x : tp_uima_globals::sectionBackground()) dlsetSection_.insert(x);
    for (auto x : tp_uima_globals::sectionMaterialsMethods()) dlsetSection_.insert(x);
    for (auto x : tp_uima_globals::sectionDesign()) dlsetSection_.insert(x);
    for (auto x : tp_uima_globals::sectionAcknowledgments()) dlsetSection_.insert(x);
    for (auto x : tp_uima_globals::sectionReferences()) dlsetSection_.insert(x);
}

TdTokenizer::TdTokenizer(const TdTokenizer & orig) {
}

TdTokenizer::~TdTokenizer() {
}

TyErrorId TdTokenizer::initialize(AnnotatorContext & rclAnnotatorContext) {
    set<UnicodeString>::iterator it;
    trieToken_ = new TpTrie();
    for (it = dlsetToken_.begin(); it != dlsetToken_.end(); it++) {
        trieToken_->addWord(*it);
    }
    trieSentence_ = new TpTrie();
    for (it = dlsetSentence_.begin(); it != dlsetSentence_.end(); it++) {
        trieSentence_->addWord(*it);
    }
    trieSection_ = new TpTrie();
    for (it = dlsetSection_.begin(); it != dlsetSection_.end(); it++) {

        trieSection_->addWord(*it);
    }
    return (TyErrorId) UIMA_ERR_NONE;
}

TyErrorId TdTokenizer::typeSystemInit(TypeSystem const & crTypeSystem) {
    filenametype_ = crTypeSystem.getType("org.apache.uima.textpresso.filename");
    if (!filenametype_.isValid()) {
        getAnnotatorContext().getLogger().logError(
                "Error getting Type object for org.apache.uima.textpresso.filename.");
        cerr << "TdTokenizer::typeSystemInit - Error. See logfile." << endl;
        return (TyErrorId) UIMA_ERR_RESMGR_INVALID_RESOURCE;
    }
    filenametype_name_ = filenametype_.getFeatureByBaseName("value");
    rawsourcetype_ = crTypeSystem.getType("org.apache.uima.textpresso.rawsource");
    if (!rawsourcetype_.isValid()) {
        getAnnotatorContext().getLogger().logError(
                "Error getting Type object for org.apache.uima.textpresso.rawsource.");
        cerr << "TdTokenizer::typeSystemInit - Error. See logfile." << endl;
        return (TyErrorId) UIMA_ERR_RESMGR_INVALID_RESOURCE;
    }
    rawsourcetype_type_ = rawsourcetype_.getFeatureByBaseName("value");
    tokentype_ = crTypeSystem.getType("org.apache.uima.textpresso.token");
    if (!tokentype_.isValid()) {
        getAnnotatorContext().getLogger(). logError(
                "Error getting Type object for org.apache.uima.textpresso.token.");
        cerr << "TdTokenizer::typeSystemInit - Error. See logfile." << endl;
        return (TyErrorId) UIMA_ERR_RESMGR_INVALID_RESOURCE;
    }
    tokentype_content_ = tokentype_.getFeatureByBaseName("content");
    sentencetype_ = crTypeSystem.getType("org.apache.uima.textpresso.sentence");
    if (!sentencetype_.isValid()) {
        getAnnotatorContext().getLogger(). logError(
                "Error getting Type object for org.apache.uima.textpresso.sentence.");
        cerr << "TdTokenizer::typeSystemInit - Error. See logfile." << endl;
        return (TyErrorId) UIMA_ERR_RESMGR_INVALID_RESOURCE;
    }
    sentencetype_content_ = sentencetype_.getFeatureByBaseName("content");
    tpfnvhashtype_ =
            crTypeSystem.getType("org.apache.uima.textpresso.tpfnvhash");
    if (!tpfnvhashtype_.isValid()) {
        getAnnotatorContext().getLogger().logError(
                "Error getting Type object for org.apache.uima.textpresso.tpfnvhash.");
        cerr << "TdTokenizer::typeSystemInit - Error. See logfile." << endl;
        return (TyErrorId) UIMA_ERR_RESMGR_INVALID_RESOURCE;
    }
    tpfnvhashtype_content_ = tpfnvhashtype_.getFeatureByBaseName("content");
    pagetype_ = crTypeSystem.getType("org.apache.uima.textpresso.page");
    if (!pagetype_.isValid()) {
        getAnnotatorContext().getLogger().logError(
                "Error getting Type object for org.apache.uima.textpresso.page.");
        cerr << "TdTokenizer::typeSystemInit - Error. See logfile." << endl;
        return (TyErrorId) UIMA_ERR_RESMGR_INVALID_RESOURCE;
    }
    pagetype_value_ = pagetype_.getFeatureByBaseName("value");
    dblbrktype_ = crTypeSystem.getType("org.apache.uima.textpresso.dblbrk");
    if (!dblbrktype_.isValid()) {
        getAnnotatorContext().getLogger().logError(
                "Error getting Type object for org.apache.uima.textpresso.dblbrk.");
        cerr << "TdTokenizer::typeSystemInit - Error. See logfile." << endl;
        return (TyErrorId) UIMA_ERR_RESMGR_INVALID_RESOURCE;
    }
    rawsectiontype_ =
            crTypeSystem.getType("org.apache.uima.textpresso.rawsection");
    if (!rawsectiontype_.isValid()) {
        getAnnotatorContext().getLogger().logError(
                "Error getting Type object for org.apache.uima.textpresso.rawsection.");
        cerr << "TdTokenizer::typeSystemInit - Error. See logfile." << endl;
        return (TyErrorId) UIMA_ERR_RESMGR_INVALID_RESOURCE;
    }
    rawsectiontype_content_ = rawsectiontype_.getFeatureByBaseName("content");

    sectiontype_ =
            crTypeSystem.getType("org.apache.uima.textpresso.section");
    if (!sectiontype_.isValid()) {
        getAnnotatorContext().getLogger().logError(
                "Error getting Type object for org.apache.uima.textpresso.section.");
        cerr << "TdTokenizer::typeSystemInit - Error. See logfile." << endl;
        return (TyErrorId) UIMA_ERR_RESMGR_INVALID_RESOURCE;
    }
    sectiontype_content_ = sectiontype_.getFeatureByBaseName("content");
    sectiontype_type_ = sectiontype_.getFeatureByBaseName("type");
    imagetype_ =
            crTypeSystem.getType("org.apache.uima.textpresso.image");
    if (!imagetype_.isValid()) {
        getAnnotatorContext().getLogger().logError(
                "Error getting Type object for org.apache.uima.textpresso.image.");
        cerr << "TdTokenizer::typeSystemInit - Error. See logfile." << endl;
        return (TyErrorId) UIMA_ERR_RESMGR_INVALID_RESOURCE;
    }
    imagetype_filename_ = imagetype_.getFeatureByBaseName("filename");
    imagetype_page_ = imagetype_.getFeatureByBaseName("page");

    return (TyErrorId) UIMA_ERR_NONE;
}

TyErrorId TdTokenizer::destroy() {

    return (TyErrorId) UIMA_ERR_NONE;
}

TyErrorId TdTokenizer::process(CAS & tcas, ResultSpecification const & crResultSpecification) {
    AnnotationCounter ac(tcas);
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
    UnicodeString usaux;
    usprelimref.extract(result, usprelimref.length() - result, usaux);

    string dummy;
    string f(usaux.toUTF8String<string >(dummy));

    textAndImageManager taim = textAndImageManager(path(f).parent_path());
    taim.loadImageFilenames();
    taim.loadTextFilenames();
    taim.loadTextfiles();
    string doc("\nBeginning of Article\n");
    vector<int> pageLengths;
    for (auto x : taim.textFile()) {
        // 2026-10-21: PdfTextNormalizer.h replaces utf8Printable(), which
        // dropped all non-ASCII. Page lengths are UIMA offsets, i.e. UTF-16
        // units of the normalised text; the old count read the unfiltered
        // string over the filtered length, so pages drifted in any paper
        // with non-ASCII characters.
        UnicodeString page(tp_pdf_text::normalize(x.second));
        std::string s;
        page.toUTF8String(s);
        doc += s;
        pageLengths.push_back(page.length());
    }
    doc += "\nEnd of Article\n";
    UnicodeString ustrInputText;
    ustrInputText.append(UnicodeString::fromUTF8(StringPiece(doc)));
    UnicodeStringRef usdocref(ustrInputText);
    tcas.setDocumentText(usdocref);
    getAnnotatorContext().getLogger().logMessage("process called");
    UnicodeString dst;
    usdocref.extract(0, usdocref.length(), dst);

    vector< pair<int32_t, int32_t> > p = trieToken_->searchAllWords(dst);
    sort(p.begin(), p.end()); // just to make sure it is sorted.
    WriteOutAnnotations(tcas, usdocref, p, tokentype_, tokentype_content_, ac, true);
    p.clear();

    p = trieSentence_->searchAllWords(dst);
    p = RemoveDelimiters(usdocref, disqSentence_, p,
            maxfrontdisqcharlength_, maxbackdisqcharlength_);
    sort(p.begin(), p.end());
    WriteOutAnnotations(tcas, usdocref, p, sentencetype_, sentencetype_content_, ac, false);
    p.clear();

    vector<int32_t> sectionPosMap;
    UnicodeString dstForSections = CleanPdfTagsForSectionSearch(dst, sectionPosMap);
    CollapseLetterSpacedHeadings(dstForSections, sectionPosMap); // 2026-10-14, see above
    p = trieSection_->searchAllWords(dstForSections);
    for (auto & match : p) {
        match.first = sectionPosMap[match.first];
        match.second = sectionPosMap[match.second];
    }
    sort(p.begin(), p.end()); // just to make sure it is sorted.
    WriteOutAnnotations(tcas, usdocref, p, rawsectiontype_, rawsectiontype_content_, ac, true);
    p.clear();

    ANIndex allannindex = tcas.getAnnotationIndex();
    ANIterator aait = allannindex.iterator();
    aait.moveToFirst();
    vector<UnicodeString> sections;
    vector<size_t> sectionsB;
    vector<size_t> sectionsE;
    while (aait.isValid()) {
        Type currentType = aait.get().getType();
        string annType = currentType.getName().asUTF8();
        if (annType == "org.apache.uima.textpresso.rawsection") {
            sectionsB.push_back(aait.get().getBeginPosition());
            sectionsE.push_back(aait.get().getEndPosition());
            Feature f = currentType.getFeatureByBaseName("content");
            UnicodeString raw(aait.get().getStringValue(f).getBuffer());
            sections.push_back(CanonicalHeading(raw)); // letter-spaced -> known form
        }
        aait.moveToNext();
    }

    // Drop false headings before anything else looks at them (see the
    // 2026-10-14 note above validateSectionHeadings), including the score
    // below -- a false "Materials" + "Reference" pair shouldn't be enough to
    // turn section labelling on.
    // 2026-10-22: an appended supplement is its own section (see
    // FindSupplementBanner); headings inside it are ignored throughout.
    int32_t supplementStart = FindSupplementBanner(dst);
    size_t sectionCut = supplementStart >= 0 ? (size_t) supplementStart : (size_t) dst.length();
    vector<bool> sectionValid;
    vector<size_t> sectionSpanEnd;
    validateSectionHeadings(dst, sections, sectionsB, sectionsE, sectionCut, sectionValid, sectionSpanEnd);
    vector<UnicodeString> realHeadings;
    for (size_t i = 0; i < sections.size(); i++)
        if (sectionValid[i]) realHeadings.push_back(sections[i]);

    bool hasResult(hasSection(tp_uima_globals::sectionResult(), realHeadings));
    bool hasIntroduction(hasSection(tp_uima_globals::sectionIntroduction(), realHeadings));
    bool hasBackground(hasSection(tp_uima_globals::sectionBackground(), realHeadings));
    bool hasDiscussion(hasSection(tp_uima_globals::sectionDiscussion(), realHeadings));
    bool hasConclusion(hasSection(tp_uima_globals::sectionConclusion(), realHeadings));
    bool hasMM(hasSection(tp_uima_globals::sectionMaterialsMethods(), realHeadings));
    bool hasDesign(hasSection(tp_uima_globals::sectionDesign(), realHeadings));
    bool hasReferences(hasSection(tp_uima_globals::sectionReferences(), realHeadings));
    // 2026-08-14: fall back to detecting a heading-less numbered reference
    // list (see DetectImplicitReferencesSpan above) when the trie found no
    // "References"-family heading at all.
    pair<int32_t, int32_t> implicitRefSpan(-1, -1);
    if (!hasReferences) {
        // only the article's own text: a numbered list in a supplement
        // isn't its reference list
        int32_t cleanedCut = (int32_t) (lower_bound(sectionPosMap.begin(), sectionPosMap.end(),
                (int32_t) sectionCut) - sectionPosMap.begin());
        UnicodeString articleForSections(dstForSections, 0, cleanedCut);
        pair<int32_t, int32_t> cleanedSpan = DetectImplicitReferencesSpan(articleForSections);
        if (cleanedSpan.first >= 0) {
            int32_t endIdx = min(cleanedSpan.second, (int32_t) sectionPosMap.size() - 1);
            implicitRefSpan = make_pair(sectionPosMap[cleanedSpan.first], sectionPosMap[endIdx]);
            hasReferences = true;
        }
    }
    int score(0);
    if (hasIntroduction || hasBackground) score++;
    if (hasDiscussion || hasConclusion) score++;
    if (hasResult) score++;
    if (hasMM || hasDesign) score++;
    if (hasReferences) score++;
    // Threshold lowered 2026-08-14: >3 (4-of-5 categories) suppressed
    // section detection for 65% of a sampled 81-paper zero-section set
    // (score 2-3, one/two categories short), even when categories that
    // did match (typically references + one other) were correct. >1
    // still requires 2 independently-matched categories, guarding against
    // a single spurious isolated-line match, while recovering the
    // majority of legitimately-structured papers the old threshold blocked.
    if (score > 1) {
        combineSectionAnnotations(tcas, tp_uima_globals::sectionArticleB(), sections,
                sectionsB, sectionsE, sectionValid, sectionSpanEnd, "beginning of article", ac);
        combineSectionAnnotations(tcas, tp_uima_globals::sectionArticleE(), sections,
                sectionsB, sectionsE, sectionValid, sectionSpanEnd, "end of article", ac);
        combineSectionAnnotations(tcas, tp_uima_globals::sectionAbstract(), sections,
                sectionsB, sectionsE, sectionValid, sectionSpanEnd, "abstract", ac);
        combineSectionAnnotations(tcas, tp_uima_globals::sectionIntroduction(), sections,
                sectionsB, sectionsE, sectionValid, sectionSpanEnd, "introduction", ac);
        combineSectionAnnotations(tcas, tp_uima_globals::sectionResult(), sections,
                sectionsB, sectionsE, sectionValid, sectionSpanEnd, "result", ac);
        combineSectionAnnotations(tcas, tp_uima_globals::sectionDiscussion(), sections,
                sectionsB, sectionsE, sectionValid, sectionSpanEnd, "discussion", ac);
        combineSectionAnnotations(tcas, tp_uima_globals::sectionConclusion(), sections,
                sectionsB, sectionsE, sectionValid, sectionSpanEnd, "conclusion", ac);
        combineSectionAnnotations(tcas, tp_uima_globals::sectionBackground(), sections,
                sectionsB, sectionsE, sectionValid, sectionSpanEnd, "background", ac);
        combineSectionAnnotations(tcas, tp_uima_globals::sectionMaterialsMethods(), sections,
                sectionsB, sectionsE, sectionValid, sectionSpanEnd, "materials and methods", ac);
        combineSectionAnnotations(tcas, tp_uima_globals::sectionDesign(), sections,
                sectionsB, sectionsE, sectionValid, sectionSpanEnd, "design", ac);
        combineSectionAnnotations(tcas, tp_uima_globals::sectionAcknowledgments(), sections,
                sectionsB, sectionsE, sectionValid, sectionSpanEnd, "acknowledgments", ac);
        combineSectionAnnotations(tcas, tp_uima_globals::sectionReferences(), sections,
                sectionsB, sectionsE, sectionValid, sectionSpanEnd, "references", ac);
        if (implicitRefSpan.first >= 0 && implicitRefSpan.second > implicitRefSpan.first) {
            FSIndexRepository & indexRepImplicit = tcas.getIndexRepository();
            AnnotationFS fsImplicitRef = tcas.createAnnotation(sectiontype_,
                    implicitRefSpan.first, implicitRefSpan.second);
            fsImplicitRef.setStringValue(sectiontype_type_, "references");
            fsImplicitRef.setStringValue(sectiontype_content_,
                    UnicodeString("(implicit: numbered-list reference block, no heading text found)"));
            Feature faidImplicit = sectiontype_.getFeatureByBaseName("aid");
            if (faidImplicit.isValid()) fsImplicitRef.setIntValue(faidImplicit, ac.GetNextId());
            indexRepImplicit.addFS(fsImplicitRef);
        }
    }
    // Written whatever the score: the banner is the pipeline's own marker,
    // not a guessed heading (casannot.split_supplementary does the same).
    if (supplementStart >= 0) {
        FSIndexRepository & indexRepSupp = tcas.getIndexRepository();
        AnnotationFS fsSupp = tcas.createAnnotation(sectiontype_, supplementStart, usdocref.length());
        fsSupp.setStringValue(sectiontype_type_, "supplementary");
        UnicodeString suppContent;
        usdocref.extract(supplementStart, usdocref.length() - supplementStart, suppContent);
        fsSupp.setStringValue(sectiontype_content_, suppContent);
        Feature faidSupp = sectiontype_.getFeatureByBaseName("aid");
        if (faidSupp.isValid()) fsSupp.setIntValue(faidSupp, ac.GetNextId());
        indexRepSupp.addFS(fsSupp);
    }
    FSIndexRepository & indexRep = tcas.getIndexRepository();
    AnnotationFS fsNewTok = tcas.createAnnotation(tpfnvhashtype_, 0, usdocref.length());
    string shex = tpfnv(usdocref);
    UnicodeString ushex(shex.c_str());
    fsNewTok.setStringValue(tpfnvhashtype_content_, ushex);
    indexRep.addFS(fsNewTok);
    AnnotationFS fsNT2 = tcas.createAnnotation(rawsourcetype_, 0, usdocref.length());
    fsNT2.setStringValue(rawsourcetype_type_, "tai"); // stands for text and image
    // (the new pdftotext system)
    indexRep.addFS(fsNT2);
    AnnotationFS fsNT3 = tcas.createAnnotation(filenametype_, 0, usdocref.length());
    fsNT3.setStringValue(filenametype_name_, filename);
    indexRep.addFS(fsNT3);
    int b(0);
    std::map<int, std::pair<size_t, size_t>> pageBE;
    for (int i = 0; i != pageLengths.size(); i++) {
        AnnotationFS fsPage = tcas.createAnnotation(pagetype_, b, b + pageLengths[i]);
        // store page B and E for image annotation below
        pageBE.insert(std::make_pair<int, std::pair<size_t, size_t >>
                (i + 1, std::make_pair<size_t, size_t>(b, b + pageLengths[i])));
        fsPage.setIntValue(pagetype_value_, i + 1);
        b += pageLengths[i];
        Feature faid = pagetype_.getFeatureByBaseName("aid");
        if (faid.isValid())
            fsPage.setIntValue(faid, ac.GetNextId());
        indexRep.addFS(fsPage);
    }
    for (auto x : taim.imageFilenames()) {
        path p(x);
        string s(p.stem().stem().extension().string());
        int pagenumber(stoi(s.substr(1, s.size() - 1)));
        std::pair<size_t, size_t> auxpair(pageBE[pagenumber]);
        AnnotationFS fsImage = tcas.createAnnotation(imagetype_, auxpair.first, auxpair.second);
        fsImage.setIntValue(imagetype_page_, pagenumber);
        fsImage.setStringValue(imagetype_filename_, UnicodeString(x.c_str()));
        Feature faid = imagetype_.getFeatureByBaseName("aid");
        if (faid.isValid())
            fsImage.setIntValue(faid, ac.GetNextId());
        indexRep.addFS(fsImage);
    }
    // UTF-16 offsets like every other annotation: doc.find() gave UTF-8 byte
    // offsets, which only agreed while the text was pure ASCII (and the old
    // loop also annotated one bogus break at npos after the last match).
    for (int32_t found = ustrInputText.indexOf(UnicodeString("\n\n"), 1); found >= 0;
            found = ustrInputText.indexOf(UnicodeString("\n\n"), found + 1)) {
        AnnotationFS fsPage = tcas.createAnnotation(dblbrktype_, found, found + 2);
        Feature faid = dblbrktype_.getFeatureByBaseName("aid");
        if (faid.isValid())
            fsPage.setIntValue(faid, ac.GetNextId());
        indexRep.addFS(fsPage);
    }
    Type rawsection = tcas.getTypeSystem().getType("org.apache.uima.textpresso.rawsection");
    ANIndex rawsectionindex = tcas.getAnnotationIndex(rawsection);
    ANIterator aait2 = rawsectionindex.iterator();
    aait2.moveToFirst();
    std::vector<AnnotationFS> removeList;
    while (aait2.isValid()) {
        removeList.push_back(aait2.get());
        aait2.moveToNext();
    }
    while (!removeList.empty()) {
        indexRep.removeFS(removeList.back());
        removeList.pop_back();
    }
    return (TyErrorId) UIMA_ERR_NONE;
}

MAKE_AE(TdTokenizer);
