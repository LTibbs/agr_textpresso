/*
 * Tests for uima-annotators/PdfTextNormalizer.h.
 *
 * Needs only ICU and googletest, not UIMA. In the textpresso build image:
 *   g++ -std=c++14 -I/googletest/googletest/include tests/test_pdf_text_normalizer.cpp \
 *       -L/googletest/cmake-build-release/lib -lgtest -lgtest_main -licuuc -pthread \
 *       -o /tmp/test_pdf_text_normalizer && /tmp/test_pdf_text_normalizer
 *
 * The "same as jats" cases mirror tests/test_jats_text.py in
 * maizegdb_textpresso_implementation; keep the two in step.
 */

#include "gtest/gtest.h"
#include "../uima-annotators/PdfTextNormalizer.h"

using tp_pdf_text::normalize;
using tp_pdf_text::normalizeToUTF8;

namespace {

    TEST(PdfTextNormalizer, AsciiUnchanged) {
        std::string s("ZmPR-1-8 (p < 0.05)\tq = 0.01\nline two\r\n");
        EXPECT_EQ(normalizeToUTF8(s), s);
    }

    TEST(PdfTextNormalizer, KeepsWhatUtf8PrintableDropped) {
        // minus sign, <=, Greek, accents, prime, en/em dash, mu, plus-minus
        for (const char * s : {u8"10−5", u8"q ≤ 0.05", u8"19-kD α-zein",
                u8"Lübberstedt", u8"5′-AMP", u8"1990–2000", u8"a—b",
                u8"10 μM", u8"5 ± 1", u8"© 2020"})
            EXPECT_EQ(normalizeToUTF8(s), s) << s;
    }

    TEST(PdfTextNormalizer, SameAsJats) {
        EXPECT_EQ(normalizeToUTF8(u8"PYR1‐LIKE"), "PYR1-LIKE");
        EXPECT_EQ(normalizeToUTF8(u8"10 µM"), u8"10 μM");
        EXPECT_EQ(normalizeToUTF8(u8"37°C"), "37 degree C");
        EXPECT_EQ(normalizeToUTF8(u8"ZmPR‑1"), "ZmPR-1");
        EXPECT_EQ(normalizeToUTF8(u8"hyphen­ation"), "hyphenation");
        // mathematical italic small e/x (outside the BMP) -> plain letters
        EXPECT_EQ(normalizeToUTF8(u8"\U0001d452\U0001d465"), "ex");
        // no BMP equivalent -> U+FFFD
        EXPECT_EQ(normalizeToUTF8(u8"a\U0001f33db"), u8"a�b");
    }

    TEST(PdfTextNormalizer, PdfArtefacts) {
        EXPECT_EQ(normalizeToUTF8(u8"ﬀﬁﬂﬃﬄ"), "fffiflffiffl");
        EXPECT_EQ(normalizeToUTF8(u8"10 mM 5 kb 3 h"), "10 mM 5 kb 3 h");
        EXPECT_EQ(normalizeToUTF8(u8"﻿zero​width‍"), "zerowidth");
        EXPECT_EQ(normalizeToUTF8(u8"glyph"), "glyph");
        EXPECT_EQ(normalizeToUTF8("ctrl\x01\x7f" "char"), "ctrlchar");
        EXPECT_EQ(normalizeToUTF8("page\f"), "page\n");
    }

    TEST(PdfTextNormalizer, InvalidUtf8Dropped) {
        EXPECT_EQ(normalizeToUTF8("bad\xc3(byte\xff"), "bad(byte");
    }

    TEST(PdfTextNormalizer, OutputIsBmpSoLengthIsCodePoints) {
        icu::UnicodeString out = normalize(u8"α\U0001d452\U0001f33dé");
        EXPECT_EQ(out.length(), out.countChar32());
        EXPECT_EQ(out.length(), 4);
    }
}
