/*
 * CaseInsensitiveAnalyzer.h -- StandardAnalyzer (LUCENE_30) plus
 * GreekSynonymFilter (2026-10-21).
 *
 * Same chain as Lucene++'s StandardAnalyzer -- StandardTokenizer (max token
 * length 255), StandardFilter, LowerCaseFilter, StopFilter on the English stop
 * set -- with the Greek filter last. It replaces StandardAnalyzer wherever
 * text is indexed or queried, so index and query treat "β" and "beta" alike;
 * an index built with one and queried with the other would only half work.
 * Header-only so the textpressocentral copy needs no build change.
 */

#ifndef TP_CASEINSENSITIVEANALYZER_H
#define TP_CASEINSENSITIVEANALYZER_H

#include <lucene++/LuceneHeaders.h>
#include "GreekSynonymFilter.h"

class CaseInsensitiveAnalyzer : public Lucene::Analyzer {
public:
    explicit CaseInsensitiveAnalyzer(Lucene::LuceneVersion::Version matchVersion)
    : matchVersion(matchVersion),
      stopSet(Lucene::StopAnalyzer::ENGLISH_STOP_WORDS_SET()),
      enableStopPositionIncrements(Lucene::StopFilter::getEnablePositionIncrementsVersionDefault(matchVersion)) {
    }

    virtual ~CaseInsensitiveAnalyzer() {
    }

    LUCENE_CLASS(CaseInsensitiveAnalyzer);

    virtual Lucene::TokenStreamPtr tokenStream(const Lucene::String & fieldName, const Lucene::ReaderPtr & reader) {
        Lucene::StandardTokenizerPtr tokenizer(Lucene::newLucene<Lucene::StandardTokenizer>(matchVersion, reader));
        tokenizer->setMaxTokenLength(255);
        Lucene::TokenStreamPtr result(Lucene::newLucene<Lucene::StandardFilter>(tokenizer));
        result = Lucene::newLucene<Lucene::LowerCaseFilter>(result);
        result = Lucene::newLucene<Lucene::StopFilter>(enableStopPositionIncrements, result, stopSet);
        return Lucene::newLucene<GreekSynonymFilter>(result);
    }

protected:
    Lucene::LuceneVersion::Version matchVersion;
    Lucene::HashSet<Lucene::String> stopSet;
    bool enableStopPositionIncrements;
};

#endif /* TP_CASEINSENSITIVEANALYZER_H */
