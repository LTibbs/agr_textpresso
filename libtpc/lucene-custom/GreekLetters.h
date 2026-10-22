/*
 * GreekLetters.h -- Greek letters <-> their names, so "β-carotene" and
 * "beta-carotene" find each other (2026-10-21).
 *
 * Used in two places, which must agree:
 *   - GreekSynonymFilter (Lucene, index and query time): a token gets its
 *     spelled-out form as a synonym at the same position.
 *   - TpLexiconAnnotatorFromPg (lexicon load): each term is also added in the
 *     other spelling, so ChEBI's "beta-carotene" matches "β-carotene" in text
 *     and the gene OBO's "α-zein" matches "alpha-zein".
 *
 * A letter is spelled out, or a name turned into its letter, only when no
 * other letter touches it: "β-carotene", "α1-antitrypsin", "beta 1" change;
 * "μM", "βglucan", "alphabet", "betaine" do not. Names become letters only in
 * lowercase, and never when the name is the whole string, so the maize Mu
 * transposon, "pi" or "eta" on their own are left alone. The micro sign
 * U+00B5 counts as mu (PdfTextNormalizer and jats_text.py already fold it).
 *
 * Pure C++ on std::wstring (UTF-32 on Linux; every letter here is in the BMP).
 */

#ifndef TP_GREEKLETTERS_H
#define TP_GREEKLETTERS_H

#include <string>

namespace tp_greek {

    struct Letter {
        wchar_t lower, upper;
        const wchar_t * name;
    };

    inline const Letter * letters(size_t & n) {
        static const Letter table[] = {
            {0x03b1, 0x0391, L"alpha"}, {0x03b2, 0x0392, L"beta"}, {0x03b3, 0x0393, L"gamma"},
            {0x03b4, 0x0394, L"delta"}, {0x03b5, 0x0395, L"epsilon"}, {0x03b6, 0x0396, L"zeta"},
            {0x03b7, 0x0397, L"eta"}, {0x03b8, 0x0398, L"theta"}, {0x03b9, 0x0399, L"iota"},
            {0x03ba, 0x039a, L"kappa"}, {0x03bb, 0x039b, L"lambda"}, {0x03bc, 0x039c, L"mu"},
            {0x03bd, 0x039d, L"nu"}, {0x03be, 0x039e, L"xi"}, {0x03bf, 0x039f, L"omicron"},
            {0x03c0, 0x03a0, L"pi"}, {0x03c1, 0x03a1, L"rho"}, {0x03c3, 0x03a3, L"sigma"},
            {0x03c4, 0x03a4, L"tau"}, {0x03c5, 0x03a5, L"upsilon"}, {0x03c6, 0x03a6, L"phi"},
            {0x03c7, 0x03a7, L"chi"}, {0x03c8, 0x03a8, L"psi"}, {0x03c9, 0x03a9, L"omega"},
        };
        n = sizeof (table) / sizeof (table[0]);
        return table;
    }

    // -> the table entry for a Greek letter (and its variant forms), or nullptr;
    // upper is set when the letter is a capital.
    inline const Letter * lookup(wchar_t c, bool & upper) {
        switch (c) { // variant forms
            case 0x00b5: c = 0x03bc; break; // micro sign
            case 0x03c2: c = 0x03c3; break; // final sigma
            case 0x03d0: c = 0x03b2; break; // beta symbol
            case 0x03d1: c = 0x03b8; break; // theta symbol
            case 0x03d5: c = 0x03c6; break; // phi symbol
            case 0x03d6: c = 0x03c0; break; // pi symbol
            case 0x03f0: c = 0x03ba; break; // kappa symbol
            case 0x03f1: c = 0x03c1; break; // rho symbol
            case 0x03f5: c = 0x03b5; break; // lunate epsilon
        }
        size_t n;
        const Letter * t = letters(n);
        for (size_t i = 0; i < n; i++) {
            if (t[i].lower == c) { upper = false; return &t[i]; }
            if (t[i].upper == c) { upper = true; return &t[i]; }
        }
        return nullptr;
    }

    // Latin (incl. accented) or Greek letter: what "touching another letter" means.
    inline bool is_letter(wchar_t c) {
        bool up;
        return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z')
                || (c >= 0x00c0 && c <= 0x024f && c != 0x00d7 && c != 0x00f7)
                || lookup(c, up) != nullptr || (c >= 0x0370 && c <= 0x03ff);
    }

    // "β-carotene" -> "beta-carotene", "Δ9" -> "Delta9"; unchanged if nothing applies.
    inline std::wstring spell_out(const std::wstring & s) {
        std::wstring out;
        for (size_t i = 0; i < s.size(); i++) {
            bool upper;
            const Letter * l = lookup(s[i], upper);
            bool alone = l && (i == 0 || !is_letter(s[i - 1]))
                    && (i + 1 == s.size() || !is_letter(s[i + 1]));
            if (!alone) {
                out += s[i];
                continue;
            }
            std::wstring name(l->name);
            if (upper) name[0] = (wchar_t) (name[0] - L'a' + L'A');
            out += name;
        }
        return out;
    }

    // "beta-carotene" -> "β-carotene"; lowercase whole-word names only, and not
    // when the name is the whole string.
    inline std::wstring to_letters(const std::wstring & s) {
        size_t n;
        const Letter * t = letters(n);
        std::wstring out;
        size_t i = 0;
        while (i < s.size()) {
            bool done = false;
            if (i == 0 || !is_letter(s[i - 1])) {
                for (size_t k = 0; k < n && !done; k++) {
                    std::wstring name(t[k].name);
                    size_t e = i + name.size();
                    if (s.compare(i, name.size(), name) == 0
                            && (e == s.size() || !is_letter(s[e]))
                            && !(i == 0 && e == s.size())) {
                        out += t[k].lower;
                        i = e;
                        done = true;
                    }
                }
            }
            if (!done) out += s[i++];
        }
        return out;
    }
}

#endif /* TP_GREEKLETTERS_H */
