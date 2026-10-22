/*
 * GreekSynonymFilter.h -- adds the spelled-out form of a token containing a
 * free-standing Greek letter ("β" -> "beta", "α1-antitrypsin" ->
 * "alpha1-antitrypsin") at the same position (2026-10-21).
 *
 * Applied at index and query time by CaseSensitiveAnalyzer and
 * CaseInsensitiveAnalyzer, so "beta carotene" finds "β-carotene" and the
 * reverse: a query token "β" also becomes "beta", and QueryParser turns two
 * tokens at one position into an OR (a MultiPhraseQuery inside a phrase).
 * The rule is tp_greek::spell_out (GreekLetters.h), shared with the lexicon.
 */

#ifndef TP_GREEKSYNONYMFILTER_H
#define TP_GREEKSYNONYMFILTER_H

#include <lucene++/LuceneHeaders.h>
#include <lucene++/TokenFilter.h>
#include <lucene++/TermAttribute.h>
#include <lucene++/PositionIncrementAttribute.h>
#include "GreekLetters.h"
#include <algorithm>

class GreekSynonymFilter : public Lucene::TokenFilter {
public:
    explicit GreekSynonymFilter(const Lucene::TokenStreamPtr & input) : Lucene::TokenFilter(input) {
        termAtt = addAttribute<Lucene::TermAttribute>();
        posIncrAtt = addAttribute<Lucene::PositionIncrementAttribute>();
    }

    virtual ~GreekSynonymFilter() {
    }

    LUCENE_CLASS(GreekSynonymFilter);

    virtual bool incrementToken() {
        if (pendingState) {
            restoreState(pendingState);
            pendingState.reset();
            termAtt->setTermBuffer(pending);
            posIncrAtt->setPositionIncrement(0);
            return true;
        }
        if (!input->incrementToken()) return false;
        Lucene::String term(termAtt->term());
        // A typed micro sign (Option-m) becomes Greek mu everywhere, as the
        // ingest text already is: "µM" must find "μM", which no synonym rule
        // reaches because the letter touches "M".
        if (term.find(wchar_t(0x00b5)) != Lucene::String::npos) {
            std::replace(term.begin(), term.end(), wchar_t(0x00b5), wchar_t(0x03bc));
            termAtt->setTermBuffer(term);
        }
        Lucene::String spelled(tp_greek::spell_out(term));
        if (spelled != term) {
            pending = spelled;
            pendingState = captureState();
        }
        return true;
    }

    virtual void reset() {
        Lucene::TokenFilter::reset();
        pendingState.reset();
    }

protected:
    Lucene::TermAttributePtr termAtt;
    Lucene::PositionIncrementAttributePtr posIncrAtt;
    Lucene::String pending;
    Lucene::AttributeSourceStatePtr pendingState;
};

#endif /* TP_GREEKSYNONYMFILTER_H */
