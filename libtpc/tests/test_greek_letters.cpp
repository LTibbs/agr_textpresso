/*
 * Tests for lucene-custom/GreekLetters.h (and so GreekSynonymFilter and the
 * lexicon's Greek variants). Header-only, no Lucene needed:
 *   g++ -std=c++14 -I/googletest/googletest/include tests/test_greek_letters.cpp \
 *       -L/googletest/cmake-build-release/lib -lgtest -lgtest_main -pthread \
 *       -o /tmp/test_greek_letters && /tmp/test_greek_letters
 */

#include "gtest/gtest.h"
#include "../lucene-custom/GreekLetters.h"

using tp_greek::spell_out;
using tp_greek::to_letters;

namespace {

    TEST(GreekLetters, SpellOutFreeStandingLetters) {
        EXPECT_EQ(spell_out(L"β"), L"beta");
        EXPECT_EQ(spell_out(L"β-carotene"), L"beta-carotene");
        EXPECT_EQ(spell_out(L"19-kD α-zein"), L"19-kD alpha-zein");
        EXPECT_EQ(spell_out(L"α1-antitrypsin"), L"alpha1-antitrypsin");
        EXPECT_EQ(spell_out(L"Δ9"), L"Delta9");                   // capital
        EXPECT_EQ(spell_out(L"µ"), L"mu");                         // micro sign
    }

    TEST(GreekLetters, SpellOutLeavesLettersTouchingLetters) {
        EXPECT_EQ(spell_out(L"μM"), L"μM");                   // a unit, not mu
        EXPECT_EQ(spell_out(L"βglucan"), L"βglucan");
        EXPECT_EQ(spell_out(L"carotene"), L"carotene");
    }

    TEST(GreekLetters, ToLettersWholeLowercaseNames) {
        EXPECT_EQ(to_letters(L"beta-carotene"), L"β-carotene");
        EXPECT_EQ(to_letters(L"1,3-beta-glucan"), L"1,3-β-glucan");
        EXPECT_EQ(to_letters(L"alpha-amylase 1"), L"α-amylase 1");
        EXPECT_EQ(to_letters(L"beta1"), L"β1");
    }

    TEST(GreekLetters, ToLettersLeavesWordsAndBareNames) {
        EXPECT_EQ(to_letters(L"betaine"), L"betaine");                  // inside a word
        EXPECT_EQ(to_letters(L"alphabet"), L"alphabet");
        EXPECT_EQ(to_letters(L"theta"), L"theta");                      // the whole string
        EXPECT_EQ(to_letters(L"Mu transposon"), L"Mu transposon");      // not lowercase
        EXPECT_EQ(to_letters(L"zeta-carotene"), L"ζ-carotene");    // not "z" + eta
    }

    TEST(GreekLetters, RoundTrip) {
        EXPECT_EQ(spell_out(to_letters(L"beta-carotene")), L"beta-carotene");
    }
}
