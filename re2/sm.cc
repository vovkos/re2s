// Copyright 2023 The RE2 Authors.  All Rights Reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include "re2/dfa.h"
#include "re2/sm.h"
#include "re2/regexp.h"
#include "re2/prog.h"

namespace re2 {

static const bool ExtraDebug = false;

RE2::ErrorCode RegexpErrorToRE2(re2::RegexpStatusCode code);

RE2::SM::SM(const char* pattern) {
  Init(pattern, RE2::DefaultOptions);
}

RE2::SM::SM(const std::string& pattern) {
  Init(pattern, RE2::DefaultOptions);
}

RE2::SM::SM(absl::string_view pattern) {
  Init(pattern, RE2::DefaultOptions);
}

RE2::SM::SM(absl::string_view pattern, const Options& options) {
  Init(pattern, options);
}

RE2::SM::~SM() {
  if (regexp_)
    regexp_->Decref();
}

void RE2::SM::Init(absl::string_view pattern, const Options& options) {
  RegexpStatus status;
  regexp_ = Regexp::Parse(
    pattern,
    static_cast<Regexp::ParseFlags>(options.ParseFlags()),
    &status
  );

  if (regexp_ == NULL) {
    if (options.log_errors())
      LOG(ERROR) << "Error parsing '" << pattern << "': " << status.Text();

    error_ = status.Text();
    error_code_ = RegexpErrorToRE2(status.code());
    error_arg_ = std::string(status.error_arg());
    return;
  }

  prog_.reset(regexp_->CompileToProg(options.max_mem() * 2 / 3));
  if (prog_ == NULL) {
    if (options.log_errors())
      LOG(ERROR) << "Error compiling '" << pattern << "'";

    error_code_ = RE2::ErrorPatternTooLarge;
    error_ = "pattern too large - compile failed";
    return;
  }

  error_code_ = NoError;
  options_.Copy(options);
}

struct RE2::SM::DfaLoopParams {
  DfaLoopParams(
    State* state0,
    DFA* dfa0,
    absl::string_view chunk0,
    DFA::RWLocker* cache_lock0
  ):
    state(state0),
    dfa(dfa0),
    text(chunk0),
    cache_lock(cache_lock0),
    can_prefix_accel(false),
    run_forward(false),
    start(NULL),
    matches(NULL) {}

  absl::string_view text;
  State* state;
  DFA* dfa;
  DFA::RWLocker* cache_lock;
  DFA::State* start;
  bool can_prefix_accel;
  bool run_forward;
  SparseSet* matches;
};

RE2::SM::ExecResult RE2::SM::exec(absl::string_view chunk, State* state) const {
  if (state->flags_ & State::kFinal)
    state->reset(state->prev_char_, state->offset_);

  const uint8_t* p = (uint8_t*)chunk.data();
  const uint8_t* end = p + chunk.length();

  if (!(state->flags_ & State::kReverse)) { // forward scan
    if (prog_->anchor_start() && state->base_offset() != 0) { // mismatch on the start anchor
      state->flags_ = State::kFinal;
      if (!chunk.empty()) {
        state->offset_ += chunk.length();
        state->prev_char_ = chunk.back();
      }

      return RE2::SM::kMismatch;
    }

    if (prog_->anchor_end()) { // can skip forward scan
      state->offset_ += chunk.length();

      if (!(state->flags_ & State::kEof))
        return RE2::SM::kContinue;

      state->prev_char_ = 256; // DFA::kByteEndText;
      state->flags_ &= ~State::kEof;
      state->flags_ |= State::kReverse;
    } else { // main forward scan loop
      Prog::MatchKind kind = options_.longest_match() ? Prog::kLongestMatch : Prog::kFirstMatch;
      DFA* dfa = prog_->GetDFA(kind);
      /*
      bool matched = dfa->Search(
        text, context, anchored,
        want_earliest_match, !reversed_,
        failed, &ep, matches
      ); */

      DFA::RWLocker l(&dfa->cache_mutex_);

      DfaLoopParams params(state, dfa, chunk, &l);
      dfaLoop(&params);
    }
  }

  // backward scan
  assert(state->flags_ & State::kReverse);

  return RE2::SM::kMismatch;
}

/*

// The actual DFA search: calls AnalyzeSearch and then FastSearchLoop.
bool DFA::Search(absl::string_view text, absl::string_view context,
                 bool anchored, bool want_earliest_match, bool run_forward,
                 bool* failed, const char** epp, SparseSet* matches) {
  *epp = NULL;
  if (!ok()) {
    *failed = true;
    return false;
  }
  *failed = false;

  if (ExtraDebug) {
    absl::FPrintF(stderr, "\nprogram:\n%s\n", prog_->DumpUnanchored());
    absl::FPrintF(stderr, "text %s anchored=%d earliest=%d fwd=%d kind %d\n",
                  text, anchored, want_earliest_match, run_forward, kind_);
  }

  RWLocker l(&cache_mutex_);
  SearchParams params(text, context, &l);
  params.anchored = anchored;
  params.want_earliest_match = want_earliest_match;
  params.run_forward = run_forward;
  params.matches = matches;

  if (!AnalyzeSearch(&params)) {
    *failed = true;
    return false;
  }
  if (params.start == DeadState)
    return false;
  if (params.start == FullMatchState) {
    if (run_forward == want_earliest_match)
      *epp = text.data();
    else
      *epp = text.data() + text.size();
    return true;
  }
  if (ExtraDebug)
    absl::FPrintF(stderr, "start %s\n", DFA::DumpState(params.start));
  bool ret = FastSearchLoop(&params);
  if (params.failed) {
    *failed = true;
    return false;
  }
  *epp = params.ep;
  return ret;
}

*/

template <
  bool can_prefix_accel,
  bool run_forward
>
RE2::SM::ExecResult RE2::SM::dfaLoopImpl(DfaLoopParams* params) const {
  DFA* dfa = params->dfa;
  DFA::State* start = params->start;
  const uint8_t* bp = (uint8_t*)params->text.data();
  const uint8_t* p = bp;
  const uint8_t* ep = (uint8_t*)params->text.data() + params->text.size();

  if (!run_forward)
    std::swap(p, ep);

  const uint8_t* bytemap = prog_->bytemap();
  const uint8_t* lastmatch = NULL;  // most recent matching position in text
  bool matched = false;

  DFA::State* s = start;
  if (ExtraDebug)
    absl::FPrintF(stderr, "@stx: %s\n", DFA::DumpState(s));

  if (s->IsMatch()) {
    matched = true;
    lastmatch = p;
    if (ExtraDebug)
      absl::FPrintF(stderr, "match @stx! [%s]\n", DFA::DumpState(s));
    if (params->matches != NULL) {
      for (int i = s->ninst_ - 1; i >= 0; i--) {
        int id = s->inst_[i];
        if (id == DFA::MatchSep)
          break;
        params->matches->insert(id);
      }
    }
  }

  while (p != ep) {
    if (ExtraDebug)
      absl::FPrintF(stderr, "@%d: %s\n", p - bp, DFA::DumpState(s));

    if (can_prefix_accel && s == start) {
      // In start state, only way out is to find the prefix,
      // so we use prefix accel (e.g. memchr) to skip ahead.
      // If not found, we can skip to the end of the string.
      p = (uint8_t*)prog_->PrefixAccel(p, ep - p);
      if (p == NULL) {
        p = ep;
        break;
      }
    }

    int c = run_forward ? *p++ : *--p;

    // Note that multiple threads might be consulting
    // s->next_[bytemap[c]] simultaneously.
    // RunStateOnByte takes care of the appropriate locking,
    // including a memory barrier so that the unlocked access
    // (sometimes known as "double-checked locking") is safe.
    // The alternative would be either one DFA per thread
    // or one mutex operation per input byte.
    //
    // ns == DeadState means the state is known to be dead
    // (no more matches are possible).
    // ns == NULL means the state has not yet been computed
    // (need to call RunStateOnByteUnlocked).
    // RunStateOnByte returns ns == NULL if it is out of memory.
    // ns == FullMatchState means the rest of the string matches.
    //
    // Okay to use bytemap[] not ByteMap() here, because
    // c is known to be an actual byte and not kByteEndText.

    DFA::State* ns = s->next_[bytemap[c]].load(std::memory_order_acquire);
    if (ns == NULL) {
      ns = dfa->RunStateOnByteUnlocked(s, c);
      if (ns == NULL) {
        // Prepare to save start and s across the reset.
        DFA::StateSaver save_start(dfa, start);
        DFA::StateSaver save_s(dfa, s);

        // Discard all the States in the cache.
        dfa->ResetCache(params->cache_lock);

        // Restore start and s so we can continue.
        if ((start = save_start.Restore()) == NULL ||
            (s = save_s.Restore()) == NULL) {
          // Restore already did LOG(DFATAL).
          return kError;
        }
        ns = dfa->RunStateOnByteUnlocked(s, c);
        if (ns == NULL) {
          LOG(DFATAL) << "RunStateOnByteUnlocked failed after ResetCache";
          return kError;
        }
      }
    }
    if (ns <= SpecialStateMax) {
      if (ns == DeadState) {
        if (lastmatch)
          params->state->match_end_offset_ = run_forward ?
            params->state->offset_ + (lastmatch - bp) :
            params->state->offset_ - (ep - lastmatch);

        return kMatch;
      }

      params->state->flags_ |= State::kFullMatch;
      return kContinue;
    }

    s = ns;
    if (s->IsMatch()) {
      matched = true;
      // The DFA notices the match one byte late,
      // so adjust p before using it in the match.
      lastmatch = run_forward ? p - 1 : p + 1;
      if (ExtraDebug)
        absl::FPrintF(stderr, "match @%d! [%s]\n", lastmatch - bp, DFA::DumpState(s));
      if (params->matches != NULL) {
        for (int i = s->ninst_ - 1; i >= 0; i--) {
          int id = s->inst_[i];
          if (id == DFA::MatchSep)
            break;
          params->matches->insert(id);
        }
      }
    }
  }
/*
  // Process one more byte to see if it triggers a match.
  // (Remember, matches are delayed one byte.)
  if (ExtraDebug)
    absl::FPrintF(stderr, "@etx: %s\n", DFA::DumpState(s));

  int lastbyte;
  if (run_forward) {
    if (EndPtr(params->text) == EndPtr(params->context))
      lastbyte = DFA::kByteEndText;
    else
      lastbyte = EndPtr(params->text)[0] & 0xFF;
  } else {
    if (BeginPtr(params->text) == BeginPtr(params->context))
      lastbyte = DFA::kByteEndText;
    else
      lastbyte = BeginPtr(params->text)[-1] & 0xFF;
  }

  DFA::State* ns = s->next_[ByteMap(lastbyte)].load(std::memory_order_acquire);
  if (ns == NULL) {
    ns = RunStateOnByteUnlocked(s, lastbyte);
    if (ns == NULL) {
      DFA::StateSaver save_s(dfa, s);
      dfa->ResetCache(params->cache_lock);
      if ((s = save_s.Restore()) == NULL) {
        params->failed = true;
        return false;
      }
      ns = RunStateOnByteUnlocked(s, lastbyte);
      if (ns == NULL) {
        LOG(DFATAL) << "RunStateOnByteUnlocked failed after Reset";
        params->failed = true;
        return false;
      }
    }
  }
  if (ns <= SpecialStateMax) {
    if (ns == DeadState) {
      params->ep = reinterpret_cast<const char*>(lastmatch);
      return matched;
    }
    // FullMatchState
    params->ep = reinterpret_cast<const char*>(ep);
    return true;
  }

  s = ns;
  if (s->IsMatch()) {
    matched = true;
    lastmatch = p;
    if (ExtraDebug)
      absl::FPrintF(stderr, "match @etx! [%s]\n", DFA::DumpState(s));
    if (params->matches != NULL && kind_ == Prog::kManyMatch) {
      for (int i = s->ninst_ - 1; i >= 0; i--) {
        int id = s->inst_[i];
        if (id == DFA::MatchSep)
          break;
        params->matches->insert(id);
      }
    }
  }

  params->ep = reinterpret_cast<const char*>(lastmatch); */

  return kContinue;
}

inline RE2::SM::ExecResult RE2::SM::dfaLoop_ff(DfaLoopParams* params) const {
  return dfaLoopImpl<false, false>(params);
}

inline RE2::SM::ExecResult RE2::SM::dfaLoop_ft(DfaLoopParams* params) const {
  return dfaLoopImpl<false, true>(params);
}

inline RE2::SM::ExecResult RE2::SM::dfaLoop_tf(DfaLoopParams* params) const {
  return dfaLoopImpl<true, false>(params);
}

inline RE2::SM::ExecResult RE2::SM::dfaLoop_tt(DfaLoopParams* params) const {
  return dfaLoopImpl<true, true>(params);
}

RE2::SM::ExecResult RE2::SM::dfaLoop(DfaLoopParams* params) const {
  static ExecResult (SM::*funcTable[])(DfaLoopParams*) const= {
    &SM::dfaLoop_ff,
    &SM::dfaLoop_ft,
    &SM::dfaLoop_tf,
    &SM::dfaLoop_tt,
  };

  size_t i = 2 * params->can_prefix_accel + 1 * params->run_forward;
  return (this->*funcTable[i])(params);
}

}  // namespace re2
