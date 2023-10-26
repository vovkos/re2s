// Copyright 2023 The RE2 Authors.  All Rights Reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include "re2/sm.h"
#include "re2/dfa.h"
#include "re2/regexp.h"
#include "re2/prog.h"

namespace re2 {

enum {
  ExtraDebug = false,
};

RE2::ErrorCode RegexpErrorToRE2(re2::RegexpStatusCode code);

RE2::SM::~SM() {
  if (regexp_)
    regexp_->Decref();

  delete prog_;
  delete rprog_;
}

void RE2::SM::init(absl::string_view pattern, const Options& options) {
  prog_ = NULL;
  rprog_ = NULL;
  error_code_ = NoError;

  RegexpStatus status;
  regexp_ = Regexp::Parse(
    pattern,
    static_cast<Regexp::ParseFlags>(options.ParseFlags()),
    &status
  );

  if (!regexp_) {
    if (options.log_errors())
      LOG(ERROR) << "Error parsing '" << pattern << "': " << status.Text();

    error_ = status.Text();
    error_code_ = RegexpErrorToRE2(status.code());
    error_arg_ = std::string(status.error_arg());
    return;
  }

  prog_ = regexp_->CompileToProg(options.max_mem() * 2 / 3);
  rprog_ = regexp_->CompileToReverseProg(options.max_mem() / 3);
  if (!prog_ || !rprog_) {
    if (options.log_errors())
      LOG(ERROR) << "Error compiling '" << pattern << "'";

    error_code_ = RE2::ErrorPatternTooLarge;
    error_ = "pattern too large - compile failed";
    return;
  }

  options_.Copy(options);
}

struct RE2::SM::DfaBaseParams {
  RE2::SM::State* state;
  DFA::RWLocker* cache_lock;

  DfaBaseParams(RE2::SM::State* state0, DFA::RWLocker* cache_lock0):
    state(state0),
    cache_lock(cache_lock0) {}
};

struct RE2::SM::SelectDfaStartStateParams: DfaBaseParams {
  DFA::StartInfo* info;
  uint32_t flags;

  SelectDfaStartStateParams(RE2::SM::State* state0, DFA::RWLocker* cache_lock0):
    DfaBaseParams(state0, cache_lock0) {}
};

struct RE2::SM::DfaLoopParams: DfaBaseParams {
  absl::string_view chunk;

  DfaLoopParams(RE2::SM::State* state0, DFA::RWLocker* cache_lock0, absl::string_view chunk0):
    DfaBaseParams(state0, cache_lock0),
    chunk(chunk0) {}
};

RE2::SM::ExecResult RE2::SM::exec(State* state, absl::string_view chunk) const {
  if (!ok() || (state->flags_ & State::kMismatch))
    return kErrorInconsistent;

  if (state->flags_ & State::kMatch) // restart after match
    state->reset(state->match_end_offset_, state->match_end_char_, state->eof_offset_, state->eof_char_);

  // already in the reverse scan state

  if (state->flags_ & State::kReverse) {
    if (state->offset_ > state->match_end_offset_) { // overshoot
      uint64_t overshoot_size = state->offset_ - state->match_end_offset_;
      if (overshoot_size > chunk.size()) {
        state->offset_ -= chunk.size();
        return kContinueBackward;
      }

      chunk.remove_suffix(overshoot_size);
      state->offset_ = state->match_end_offset_;
    }

    DFA::RWLocker cache_lock(&state->dfa_->cache_mutex_);
    DfaLoopParams loop_params(state, &cache_lock, chunk);
    return reverse_dfa_loop(&loop_params);
  }

  // forward scan

  uint64_t prev_offset = state->offset_;

  if (
    (state->flags_ & (State::kInitialized | State::kReverse | State::kFullMatch)) ==
    (State::kInitialized | State::kFullMatch)
  ) { // scan all the way to the EOF
    state->offset_ += chunk.length();
    if (state->offset_ < state->eof_offset_) {
      if (!chunk.empty())
        state->last_char_ = chunk.back();
      return kContinue;
    }

    state->match_end_offset_ = state->offset_;
    state->match_end_char_ = !chunk.empty() ? chunk.back() : state->last_char_;
    state->match_next_char_ = state->eof_char_;
  } else if (state->flags_ & State::kInitialized) {
    DFA::RWLocker cache_lock(&state->dfa_->cache_mutex_);
    DfaLoopParams loop_params(state, &cache_lock, chunk);
    ExecResult exec_result = dfa_loop(&loop_params);
    if (exec_result != kContinueBackward)
      return exec_result;
  } else {
    if (prog_->anchor_start()) {
      if (state->base_offset() != 0) { // mismatch on the start anchor
        state->flags_ |= State::kMismatch;
        return kMismatch;
      }

      state->flags_ |= State::kAnchored;
    }

    if (prog_->anchor_end()) { // can skip forward scan and start from eof
      state->offset_ += chunk.length();
      if (state->offset_ < state->eof_offset_) {
        state->flags_ |= State::kInitialized | State::kFullMatch;
        if (!chunk.empty())
          state->last_char_ = chunk.back();
        return kContinue;
      }

      state->match_end_offset_ = state->offset_;
      state->match_end_char_ = !chunk.empty() ? chunk.back() : state->last_char_;
      state->match_next_char_ = state->eof_char_;
    } else { // main forward scan loop
      Prog::MatchKind kind = options_.longest_match() ? Prog::kLongestMatch : Prog::kFirstMatch;
      state->dfa_ = prog_->GetDFA(kind);
      state->dfa_->want_match_id_ = true;

      DFA::RWLocker cache_lock(&state->dfa_->cache_mutex_);
      SelectDfaStartStateParams select_start_params(state, &cache_lock);
      if (!select_dfa_start_state(&select_start_params))
        return kErrorOutOfMemory;

      state->flags_ |= State::kInitialized;

      DfaLoopParams loop_params(state, &cache_lock, chunk);
      ExecResult exec_result = dfa_loop(&loop_params);
      if (exec_result != kContinueBackward)
        return exec_result;
    }
  }

  // reverse scan

  assert(state->match_end_offset_ != -1);

  state->dfa_ = rprog_->GetDFA(Prog::kLongestMatch);
  state->flags_ = State::kReverse | State::kAnchored | State::kInitialized;

  DFA::RWLocker cache_lock(&state->dfa_->cache_mutex_);
  SelectDfaStartStateParams select_start_params(state, &cache_lock);
  if (!select_dfa_start_state(&select_start_params))
    return kErrorOutOfMemory;

  if (prev_offset >= state->match_end_offset_) { // overshoot
    state->offset_ = prev_offset;
    return kContinueBackward;
  }

  size_t prefix_size = state->match_end_offset_ - prev_offset;
  assert(prefix_size <= chunk.size() && "inconsistent match end offset");
  state->offset_ = state->match_end_offset_;

  DfaLoopParams loop_params(state, &cache_lock, absl::string_view(chunk.data(), prefix_size));
  return reverse_dfa_loop(&loop_params);
}

bool RE2::SM::select_dfa_start_state(SelectDfaStartStateParams* params) {
  State* state = params->state;
  DFA* dfa = state->dfa_;

  // Determine correct search type.
  int start;
  uint32_t flags;
  if (!(state->flags_ & State::kReverse)) {
    if (state->base_offset_ == 0) {
      start = DFA::kStartBeginText;
      flags = kEmptyBeginText | kEmptyBeginLine;
    } else if (state->base_char_ == '\n') {
      start = DFA::kStartBeginLine;
      flags = kEmptyBeginLine;
    } else if (Prog::IsWordChar(state->base_char_)) {
      start = DFA::kStartAfterWordChar;
      flags = DFA::kFlagLastWord;
    } else {
      start = DFA::kStartAfterNonWordChar;
      flags = 0;
    }
  } else {
    if (state->match_end_offset_ == state->eof_offset_) {
      start = DFA::kStartBeginText;
      flags = kEmptyBeginText | kEmptyBeginLine;
    } else if (state->match_next_char_ == '\n') {
      start = DFA::kStartBeginLine;
      flags = kEmptyBeginLine;
    } else if (Prog::IsWordChar(state->match_next_char_)) {
      start = DFA::kStartAfterWordChar;
      flags = DFA::kFlagLastWord;
    } else {
      start = DFA::kStartAfterNonWordChar;
      flags = 0;
    }
  }

  if (state->flags_ & State::kAnchored)
    start |= DFA::kStartAnchored;

  DFA::StartInfo* info = &dfa->start_[start];
  params->info = info;
  params->flags = flags;

  // Try once without cache_lock for writing.
  // Try again after resetting the cache
  // (ResetCache will relock cache_lock for writing).
  if (!select_dfa_start_state_impl(params)) {
    dfa->ResetCache(params->cache_lock);
    if (!select_dfa_start_state_impl(params)) {
      LOG(DFATAL) << "Failed to analyze start state.";
      return false;
    }
  }

  DFA::State* start_state = info->start.load(std::memory_order_acquire);

  // Even if we could prefix accel, we cannot do so when anchored and,
  // less obviously, we cannot do so when we are going to need flags.
  // This trick works only when there is a single byte that leads to a
  // different state!
  if (
    dfa->prog_->can_prefix_accel() &&
    !dfa->prog_->prefix_foldcase() &&
    !(state->flags_ & State::kAnchored) &&
    start_state > SpecialStateMax &&
    start_state->flag_ >> DFA::kFlagNeedShift == 0
  )
    state->flags_ |= State::kCanPrefixAccel;

  state->dfa_state_ = state->dfa_start_state_ = start_state;
  return true;
}

// Fills in info if needed.  Returns true on success, false on failure.
bool RE2::SM::select_dfa_start_state_impl(SelectDfaStartStateParams* params) {
  State* state = params->state;
  DFA* dfa = state->dfa_;

  // Quick check.
  DFA::State* start = params->info->start.load(std::memory_order_acquire);
  if (start != NULL)
    return true;

  absl::MutexLock l(&dfa->mutex_);
  start = params->info->start.load(std::memory_order_relaxed);
  if (start != NULL)
    return true;

  dfa->q0_->clear();
  dfa->AddToQueue(
    dfa->q0_,
    (state->flags_ & State::kAnchored) ? state->dfa_->prog_->start() : state->dfa_->prog_->start_unanchored(),
    params->flags
  );

  start = dfa->WorkqToCachedState(dfa->q0_, NULL, params->flags);
  if (!start)
    return false;

  // Synchronize with "quick check" above.
  params->info->start.store(start, std::memory_order_release);
  return true;
}

template <
  bool can_prefix_accel,
  bool reverse
>
RE2::SM::ExecResult RE2::SM::dfa_loop_impl(DfaLoopParams* params) {
  State* state = params->state;
  DFA* dfa = state->dfa_;
  Prog* prog = dfa->prog_;
  DFA::State* start = (DFA::State*)state->dfa_start_state_;
  DFA::State* s = (DFA::State*)state->dfa_state_;

  size_t length = params->chunk.length();
  const uint8_t* bp = (uint8_t*)params->chunk.data();
  const uint8_t* ep = bp + length;
  const uint8_t* p = bp;
  const uint8_t* end = ep;
  const uint8_t* lastmatch = NULL;  // most recent matching position in text
  const uint8_t* bytemap = prog->bytemap();
  int lastmatch_id;

  if (reverse)
    std::swap(p, end);

  if (s->IsMatch()) {
    lastmatch = p;
    lastmatch_id = s->match_id;
  }

  while (p != end) {
    if (can_prefix_accel && s == start) {
      p = (uint8_t*)memchr(p, prog->prefix_front(), ep - p);
      if (!p)
        break;
    }

    int c = reverse ? *--p : *p++;

    DFA::State* ns = s->next_[bytemap[c]].load(std::memory_order_acquire);
    if (!ns) {
      ns = dfa->RunStateOnByteUnlocked(s, c);
      if (!ns) {
        DFA::StateSaver save_start(dfa, start);
        DFA::StateSaver save_s(dfa, s);

        dfa->ResetCache(params->cache_lock);

        start = save_start.Restore();
        s = save_s.Restore();
        if (!start || !s)
          return kErrorOutOfMemory;

        state->dfa_start_state_ = start; // update start state

        ns = dfa->RunStateOnByteUnlocked(s, c);
        if (!ns)
          return kErrorOutOfMemory;
      }
    }

    if (ExtraDebug)
      printf(
        "%s -> '%c' -> %s\n",
        dfa->DumpState(s).c_str(),
        c,
        dfa->DumpState(ns).c_str()
      );

    if (ns <= SpecialStateMax) {
      if (ns == DeadState) {
        if (reverse) {
          if (lastmatch) // the DFA notices the match one byte late
            state->match_start_offset_ = state->offset_ - (ep - lastmatch) + 1;
          else if (state->match_start_offset_ == -1)
            return kErrorInconsistent;

          state->flags_ |= State::kMatch;
          return kMatch;
        } else {
          if (lastmatch) { // the DFA notices the match one byte late
            state->match_end_offset_ = state->offset_ + (lastmatch - bp) - 1;
            state->match_end_char_ = lastmatch > bp ? lastmatch[-1] : state->last_char_;
            state->match_next_char_ = *lastmatch;
            state->match_id_ = lastmatch_id;
          } else if (state->match_end_offset_ == -1) {
            state->flags_ |= State::kMismatch;
            return kMismatch;
          }

          if (state->offset_ < state->match_end_offset_)
            state->offset_ = state->match_end_offset_;

          return kContinueBackward;
        }
      } else {
        assert(ns == FullMatchState); // matches all the way to the end
        if (reverse) {
          state->match_start_offset_ = state->base_offset_;
          state->flags_ |= State::kMatch;
          return kMatch;
        } else {
          state->offset_ += length;
          if (state->offset_ < state->eof_offset_) {
            if (bp < ep)
              state->last_char_ = ep[-1];

            state->flags_ |= State::kFullMatch;
            return kContinue;
          }

          state->match_end_offset_ = state->offset_;
          state->match_end_char_ = bp < ep ? ep[-1] : state->last_char_;
          state->match_next_char_ = state->eof_char_;
          return kContinueBackward;
        }
      }
    }

    s = ns;
    if (s->IsMatch()) {
      lastmatch = p;
      lastmatch_id = s->match_id;
    }
  }

  if (lastmatch) // the DFA notices the match one byte late
    if (reverse)
      state->match_start_offset_ = state->offset_ - (ep - lastmatch) + 1;
    else {
      state->match_end_offset_ = state->offset_ + (lastmatch - bp) - 1;
      state->match_end_char_ = lastmatch > bp ? lastmatch[-1] : state->last_char_;
      state->match_next_char_ = *lastmatch;
      state->match_id_ = lastmatch_id;
    }

  state->dfa_state_ = s;

  if (reverse) {
    state->offset_ = state->offset_ - length;
    if (state->offset_ > state->base_offset_)
      return kContinueBackward;
  } else {
    state->offset_ = state->offset_ + length;
    if (state->offset_ < state->eof_offset_) {
      if (bp < ep)
        state->last_char_ = ep[-1];
      return kContinue;
    }
  }

  // process eof -- triggers either match or mismatch

  int c = reverse ? state->base_char_ : state->eof_char_;

  DFA::State* ns = s->next_[bytemap[c]].load(std::memory_order_acquire);
  if (!ns) {
    ns = dfa->RunStateOnByteUnlocked(s, c);
    if (!ns) {
      DFA::StateSaver save_s(dfa, s);

      dfa->ResetCache(params->cache_lock);

      s = save_s.Restore();
      if (!s)
        return kErrorOutOfMemory;

      ns = dfa->RunStateOnByteUnlocked(s, c);
      if (!ns)
        return kErrorOutOfMemory;
    }
  }

  if (ExtraDebug)
    printf(
      "%s -> '\\%03d' -> %s\n",
      dfa->DumpState(s).c_str(),
      c,
      dfa->DumpState(ns).c_str()
    );

  if (ns <= SpecialStateMax) {
    if (ns == DeadState) {
      if (reverse) {
        if (state->match_start_offset_ == -1)
          return kErrorInconsistent;

        state->flags_ |= State::kMatch;
        return kMatch;
      } else {
        if (state->match_end_offset_ == -1) {
          state->flags_ |= State::kMismatch;
          return kMismatch;
        }

        return kContinueBackward;
      }
    } else {
      assert(ns == FullMatchState); // matches all the way to the end
      if (reverse) {
        state->match_start_offset_ = state->offset_;
        state->flags_ |= State::kMatch;
        return kMatch;
      } else {
        state->match_end_offset_ = state->offset_;
        state->match_end_char_ = bp < ep ? ep[-1] : state->last_char_;
        state->match_next_char_ = state->eof_char_;
        return kContinueBackward;
      }
    }
  }

  if (ns->IsMatch()) {
    if (reverse) {
      state->match_start_offset_ = state->offset_;
      state->flags_ |= State::kMatch;
      return kMatch;
    } else {
      state->match_end_offset_ = state->offset_;
      state->match_end_char_ = bp < ep ? ep[-1] : state->last_char_;
      state->match_next_char_ = state->eof_char_;
      state->match_id_ = ns->match_id;
      return kContinueBackward;
    }
  }

  if (reverse) {
    if (state->match_start_offset_ == -1)
      return kErrorInconsistent;

    state->flags_ |= State::kMatch;
    return kMatch;
  } else {
    if (state->match_end_offset_ == -1) {
      state->flags_ |= State::kMismatch;
      return kMismatch;
    }

    return kContinueBackward;
  }
}

inline RE2::SM::ExecResult RE2::SM::dfa_loop_ff(DfaLoopParams* params) {
  return dfa_loop_impl<false, false>(params);
}

inline RE2::SM::ExecResult RE2::SM::dfa_loop_ft(DfaLoopParams* params) {
  return dfa_loop_impl<false, true>(params);
}

inline RE2::SM::ExecResult RE2::SM::dfa_loop_tf(DfaLoopParams* params) {
  return dfa_loop_impl<true, false>(params);
}

inline RE2::SM::ExecResult RE2::SM::dfa_loop_tt(DfaLoopParams* params) {
  return dfa_loop_impl<true, true>(params);
}

RE2::SM::ExecResult RE2::SM::dfa_loop(DfaLoopParams* params) {
  static ExecResult (*funcTable[])(DfaLoopParams* params) = {
    &SM::dfa_loop_ff,
    &SM::dfa_loop_ft,
    &SM::dfa_loop_tf,
    &SM::dfa_loop_tt,
  };

  size_t i = params->state->flags_ & 0x03; // State::kCanPrefixAccel | State::kReverse;
  return funcTable[i](params);
}

inline RE2::SM::ExecResult RE2::SM::reverse_dfa_loop(DfaLoopParams* params) {
  ExecResult exec_result = dfa_loop(params);
  switch (exec_result) {
  case kMatch:
  case kContinueBackward:
    return exec_result;

  default:
    assert(false && "unexpected result from reverse DFA loop");
    return kErrorInconsistent;
  }
}

}  // namespace re2
