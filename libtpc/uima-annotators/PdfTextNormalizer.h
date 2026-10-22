/*
 * PdfTextNormalizer.h -- character clean-up for pdftotext output (tai layout).
 *
 * 2026-10-21: replaces the byte-level utf8Printable() in TdTokenizer.cpp,
 * which kept printable ASCII plus eight hand-coded sequences and silently
 * dropped every other character: minus signs (10^-5 became "105"), <=,
 * Greek letters and accented names. Text is now decoded to code points and
 * kept by default. The rules deliberately match the jats layout
 * (jats_text.py _map_char in maizegdb_textpresso_implementation), so a paper
 * reads the same whichever path ingested it:
 *
 *   same as jats:  U+2010/U+2011 hyphens -> "-" (the OBO uses ASCII "-");
 *                  soft hyphen dropped; characters outside the BMP are
 *                  NFKC-folded (mathematical italic letters -> plain), or
 *                  become U+FFFD if they stay outside it. The text is then
 *                  entirely BMP, so UIMA's UTF-16 offsets equal the code
 *                  point offsets casannot.py uses, and Tpcas2SingleIndex's
 *                  one-wchar_t-per-UTF-16-unit copy stays correct.
 *                  Degree sign -> " degree "; micro sign U+00B5 -> Greek mu
 *                  U+03BC (Lucene indexes "µM" and "μM" as different tokens).
 *                  Minus, <=, en/em dashes, Greek, accents: kept.
 *   pdf only:      ff/fi/fl/ffi/ffl/st ligatures -> letters; no-break and
 *                  thin spaces -> " "; zero-width characters, BOM, controls,
 *                  private-use glyphs and undecodable bytes dropped; form
 *                  feed (pdftotext page break) -> "\n".
 *
 * Header-only and ICU-only so it can be tested without UIMA
 * (tests/test_pdf_text_normalizer.cpp).
 */

#ifndef PDFTEXTNORMALIZER_H
#define PDFTEXTNORMALIZER_H

#include <string>
#include <unicode/normalizer2.h>
#include <unicode/unistr.h>
#include <unicode/utf16.h>

namespace tp_pdf_text {

    // Appends the normalised form of one code point to out.
    inline void appendNormalized(UChar32 c, icu::UnicodeString & out) {
        switch (c) {
            case 0x09: case 0x0a: case 0x0d:
                out.append(c);
                return;
            case 0x0c: // form feed: pdftotext page break
            case 0x2028: case 0x2029: // line / paragraph separator
                out.append((UChar32) 0x0a);
                return;
            case 0x00a0: case 0x1680: case 0x202f: case 0x205f: case 0x3000:
                out.append((UChar32) 0x20);
                return;
            case 0x00ad: // soft hyphen
            case 0x200b: case 0x200c: case 0x200d: case 0x200e: case 0x200f:
            case 0x2060: case 0x2061: case 0x2062: case 0x2063: case 0x2064:
            case 0xfeff: // BOM / zero-width no-break space
            case 0xfffd: // what fromUTF8 makes of undecodable bytes
            case 0xfffe: case 0xffff:
                return;
            case 0x2010: case 0x2011:
                out.append((UChar32) 0x2d);
                return;
            case 0x00b0:
                out.append(icu::UnicodeString(" degree ", -1, US_INV));
                return;
            case 0x00b5: // micro sign -> Greek mu: Lucene indexes "µM" and "μM" apart
                out.append((UChar32) 0x03bc);
                return;
        }
        if (c < 0x20 || (0x7f <= c && c <= 0x9f)) return; // C0/C1 controls
        if (0x2000 <= c && c <= 0x200a) { // en quad .. hair space
            out.append((UChar32) 0x20);
            return;
        }
        if (0xe000 <= c && c <= 0xf8ff) return; // private use: font-specific glyphs
        if (U_IS_SURROGATE(c)) return; // unpaired surrogate
        if ((0xfb00 <= c && c <= 0xfb06) || c > 0xffff) {
            if (c >= 0xf0000) return; // supplementary private use
            UErrorCode status = U_ZERO_ERROR;
            const icu::Normalizer2 * nfkc = icu::Normalizer2::getNFKCInstance(status);
            icu::UnicodeString folded;
            if (U_SUCCESS(status)) folded = nfkc->normalize(icu::UnicodeString(c), status);
            if (U_FAILURE(status) || folded.isEmpty()) {
                out.append((UChar32) 0xfffd);
                return;
            }
            for (int32_t i = 0; i < folded.length();) {
                UChar32 f = folded.char32At(i);
                i += U16_LENGTH(f);
                out.append(f > 0xffff ? (UChar32) 0xfffd : f);
            }
            return;
        }
        out.append(c);
    }

    // UTF-8 pdftotext page text -> normalised text. Its length() is the page
    // length in UIMA offsets.
    inline icu::UnicodeString normalize(const std::string & utf8) {
        icu::UnicodeString src = icu::UnicodeString::fromUTF8(icu::StringPiece(utf8));
        icu::UnicodeString out;
        for (int32_t i = 0; i < src.length();) {
            UChar32 c = src.char32At(i);
            i += U16_LENGTH(c);
            appendNormalized(c, out);
        }
        return out;
    }

    inline std::string normalizeToUTF8(const std::string & utf8) {
        std::string ret;
        normalize(utf8).toUTF8String(ret);
        return ret;
    }
}

#endif /* PDFTEXTNORMALIZER_H */
