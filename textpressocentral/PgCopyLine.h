/*
 * PgCopyLine.h -- write a tablewriter row without libpqxx 4's escaping.
 *
 * 2026-10-21: libpqxx 4.0.1's tablewriter escapes every byte above 127 as
 * "\\ooo" (two backslashes), so COPY stores the escape text itself:
 * "PYR1<U+2010>LIKE" became the 20 ASCII bytes "PYR1\342\200\220LIKE"
 * instead of 11 bytes of UTF-8. Every non-ASCII lexicon term (alpha-zein,
 * 5'-AMP with a prime, accented names, the Japanese PO synonyms) and category
 * name written by tpso or generatelexicalvariations was unmatchable.
 *
 * pg_copy::push_back(tw, row) replaces tw.push_back(row): it builds the COPY
 * text line itself (escaping only backslash and control characters, UTF-8
 * passed through) and hands it to tablewriter::write_raw_line. As with
 * tablewriter's default null string, an empty field is written as NULL.
 *
 * Used by every tablewriter in textpressocentral: tpso, generatelexicalvariations
 * and the TpC/TpOntology/TpCuration copies of TpOntApi and TpCurApi. Rows
 * already in the database keep the escape text until rewritten.
 */

#ifndef PGCOPYLINE_H
#define PGCOPYLINE_H

#include <pqxx/pqxx>
#include <string>

namespace pg_copy {

    template <typename C>
    std::string line(const C & fields) {
        static const char * const octal = "01234567";
        std::string out;
        bool first = true;
        for (const std::string & f : fields) {
            if (!first) out += '\t';
            first = false;
            if (f.empty()) {
                out += "\\N";
                continue;
            }
            for (char ch : f) {
                unsigned char c = (unsigned char) ch;
                switch (c) {
                    case '\\': out += "\\\\"; break;
                    case '\t': out += "\\t"; break;
                    case '\n': out += "\\n"; break;
                    case '\r': out += "\\r"; break;
                    default:
                        if (c < 0x20 || c == 0x7f) {
                            out += '\\';
                            out += octal[(c >> 6) & 7];
                            out += octal[(c >> 3) & 7];
                            out += octal[c & 7];
                        } else {
                            out += ch;
                        }
                }
            }
        }
        return out;
    }

    template <typename C>
    void push_back(pqxx::tablewriter & tw, const C & fields) {
        tw.write_raw_line(line(fields));
    }
}

#endif /* PGCOPYLINE_H */
