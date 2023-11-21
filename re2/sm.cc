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

RE2::SM::Module::Module(int match_id) {
  prog_ = NULL;
  regexp_ = NULL;
  match_id_ = match_id;
}

void RE2::SM::Module::clear() {
  if (regexp_) {
    regexp_->Decref();
    regexp_ = NULL;
  }

  delete prog_;
  prog_ = NULL;
  pattern_.clear();
}

bool RE2::SM::Module::capture_submatches(
  StringPiece match,
  StringPiece* submatches,
  size_t nsubmatches
) const {
  if (nsubmatches > capture_count_ + 1)
    nsubmatches = capture_count_ + 1;

  bool can_one_pass = prog_->IsOnePass() && nsubmatches <= Prog::kMaxOnePassCapture;
  bool can_bit_state = prog_->CanBitState() && match.size() <= prog_->bit_state_text_max_size();

  return
    can_one_pass ? prog_->SearchOnePass(match, match, Prog::kAnchored, Prog::kFullMatch, submatches, (int)nsubmatches) :
    can_bit_state ? prog_->SearchBitState(match, match, Prog::kAnchored, Prog::kFullMatch, submatches, (int)nsubmatches) :
    prog_->SearchNFA(match, match, Prog::kAnchored, Prog::kFullMatch, submatches, (int)nsubmatches);
}

void RE2::SM::clear() {
  main_module_.clear();
  delete rprog_;
  rprog_ = NULL;
  error_code_ = NoError;
  error_.clear();
  error_arg_.clear();
  options_ = RE2::DefaultOptions;
  kind_ = kUninitialized;

  for (intptr_t i = switch_case_module_array_.size() - 1; i >= 0; i--)
    delete switch_case_module_array_[i];

  switch_case_module_array_.clear();
}

void RE2::SM::init() {
  kind_ = kUninitialized;
  rprog_ = NULL;
  error_code_ = NoError;
}

bool RE2::SM::parse_module(Module* module, StringPiece pattern)  {
  RegexpStatus status;
  module->regexp_ = Regexp::Parse(
    pattern,
    (Regexp::ParseFlags)options_.ParseFlags(),
    &status
  );

  if (!module->regexp_) {
    if (options_.log_errors())
      LOG(ERROR) << "Error parsing '" << pattern << "': " << status.Text();

    error_ = status.Text();
    error_code_ = RegexpErrorToRE2(status.code());
    error_arg_ = std::string(status.error_arg());
    return false;
  }

  module->capture_count_ = module->regexp_->NumCaptures();
  module->pattern_ = std::string(pattern);
  return true;
}

bool RE2::SM::compile_prog(Module* module) {
  module->prog_ = module->regexp_->CompileToProg(options_.max_mem() * 2 / 3);
  if (!module->prog_) {
    if (options_.log_errors())
      LOG(ERROR) << "Error compiling forward prog for '" << module->pattern_ << "'";

    error_code_ = RE2::ErrorPatternTooLarge;
    error_ = "pattern too large - compile forward prog failed";
    return false;
  }

  return true;
}

bool RE2::SM::compile_rprog() {
  rprog_ = main_module_.regexp_->CompileToReverseProg(options_.max_mem() / 3);
  if (!rprog_) {
    if (options_.log_errors())
      LOG(ERROR) << "Error compiling reverse prog for '" << main_module_.pattern_ << "'";

    error_code_ = RE2::ErrorPatternTooLarge;
    error_ = "pattern too large - compile reverse prog failed";
    return false;
  }

  return true;
}

re2::Regexp* RE2::SM::append_regexp_match_id(re2::Regexp* regexp, int match_id) {
  re2::Regexp::ParseFlags flags = (re2::Regexp::ParseFlags)options_.ParseFlags();
  re2::Regexp* match = re2::Regexp::HaveMatch(match_id, flags);

  if (regexp->op() != kRegexpConcat) {
    re2::Regexp* sub[2];
    sub[0] = regexp;
    sub[1] = match;
    regexp = re2::Regexp::Concat(sub, 2, flags);
  } else {
    int nsub = regexp->nsub();
    PODArray<re2::Regexp*> sub(nsub + 1);
    for (int i = 0; i < nsub; i++)
      sub[i] = regexp->sub()[i]->Incref();
    sub[nsub] = match;
    regexp->Decref();
    regexp = re2::Regexp::Concat(sub.data(), nsub + 1, flags);
  }

  return regexp;
}

bool RE2::SM::create(StringPiece pattern, const Options& options) {
  clear();

  kind_ = kSingleRegexp;
  options_ = options;

  return
    parse_module(&main_module_, pattern) &&
    compile_prog(&main_module_) &&
    compile_rprog();
}

void RE2::SM::create_switch(const Options& options) {
  clear();

  kind_ = kRegexpSwitch;
  options_ = options;
}

int RE2::SM::add_switch_case(StringPiece pattern) {
  assert(kind_ == kRegexpSwitch && "invalid RE2::SM use (regexp kind mismatch)");

  int match_id = (int)switch_case_module_array_.size();
  std::unique_ptr<Module> module = std::make_unique<Module>(match_id);
  bool result =
    parse_module(module.get(), pattern) &&
    compile_prog(module.get());

  if (!result)
    return -1;

  switch_case_module_array_.push_back(module.release());
  return match_id;
}

bool RE2::SM::finalize_switch() {
  assert(kind_ == kRegexpSwitch && "invalid RE2::SM use (regexp kind mismatch)");

  assert(
    main_module_.regexp_ == NULL &&
    main_module_.prog_ == NULL &&
    rprog_ == NULL &&
    "invalid RE2::SM use (already finalized)"
  );

  // sort to help Regex::Simpify()

  std::vector<Module*> v = switch_case_module_array_;

  std::sort(
    v.begin(),
    v.end(),
    [](const Module* m1, const Module* m2) -> bool {
      return m1->pattern_ < m2->pattern_;
    }
  );

  Regexp::ParseFlags flags = (Regexp::ParseFlags)options_.ParseFlags();

  int count = (int)switch_case_module_array_.size();
  PODArray<re2::Regexp*> sub(count);
  for (int i = 0; i < count; i++)
    sub[i] = v[i]->regexp_->Incref(); // will be Decref-ed by the parent Regexp

  re2::Regexp* prev_regexp = main_module_.regexp_;
  main_module_.regexp_ = re2::Regexp::Alternate(sub.data(), count, flags);

  bool result = compile_rprog();

  main_module_.regexp_->Decref();
  main_module_.regexp_ = NULL;

  if (!result)
    return false;

  for (int i = 0; i < count; i++) {
    Module* module = v[i];
    module->regexp_ = append_regexp_match_id(module->regexp_, module->match_id_);
    module->regexp_->Incref(); // will be Decref-ed by the parent Regexp
    sub[i] = module->regexp_;
  }

  main_module_.regexp_ = re2::Regexp::Alternate(sub.data(), count, flags);
  return compile_prog(&main_module_);
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
  StringPiece chunk;

  DfaLoopParams(RE2::SM::State* state0, DFA::RWLocker* cache_lock0, StringPiece chunk0):
    DfaBaseParams(state0, cache_lock0),
    chunk(chunk0) {}
};

RE2::SM::ExecResult RE2::SM::exec(State* state, StringPiece chunk) const {
  assert(
    ok() &&
    !(state->flags_ & State::kInvalid) &&
    kind_ &&
    "invalid usage"
  );

  if (state->flags_ & State::kMatch) // restart after match
    state->reset(state->anchor_, state->match_end_offset_, state->match_last_char_, state->eof_offset_, state->eof_char_);

  if (state->flags_ & State::kReverse) { // reverse scan state
    if (state->offset_ > state->match_end_offset_) { // overshoot
      uint64_t overshoot_size = state->offset_ - state->match_end_offset_;
      if (overshoot_size > chunk.size()) {
        state->offset_ -= chunk.size();
        return kContinueBackward;
      }

      state->offset_ = state->match_end_offset_;
      state->match_next_char_ = (uint8_t)chunk[chunk.size() - overshoot_size];
      chunk.remove_suffix(overshoot_size);
    }

    uint64_t leftover_size = state->offset_ - state->base_offset_;
    if (leftover_size < chunk.size()) // don't go beyond base_offset
      chunk = StringPiece(chunk.data() + chunk.size() - leftover_size, leftover_size);

    DFA::RWLocker cache_lock(&state->dfa_->cache_mutex_);
    DfaLoopParams loop_params(state, &cache_lock, chunk);
    return dfa_loop(&loop_params);
  }

  // forward scan

  Prog* prog = main_module_.prog_;
  uint64_t prev_offset = state->offset_;

  if (
    (state->flags_ & (State::kInitialized | State::kReverse | State::kFullMatch)) ==
    (State::kInitialized | State::kFullMatch)
  ) { // scan all the way to the EOF
    if (!chunk.empty()) {
      state->offset_ += chunk.length();
      state->match_last_char_ = (uint8_t)chunk[chunk.length() - 1];
    }

    if (state->offset_ < state->eof_offset_)
      return kContinue;

    state->match_end_offset_ = state->offset_;
    state->match_next_char_ = state->eof_char_;
  } else if (state->flags_ & State::kInitialized) {
    DFA::RWLocker cache_lock(&state->dfa_->cache_mutex_);
    DfaLoopParams loop_params(state, &cache_lock, chunk);
    ExecResult exec_result = dfa_loop(&loop_params);
    if (exec_result != kContinueBackward)
      return exec_result;
  } else {
    if (prog->anchor_start() || state->anchor_)
      state->flags_ |= State::kAnchored;

    // we want a match_id, so we never skip the forward scan even if it has the end anchor

    Prog::MatchKind progKind =
      state->anchor_ == RE2::ANCHOR_BOTH ? Prog::kFullMatch :
      options_.longest_match() ? Prog::kLongestMatch :
      Prog::kFirstMatch;

    state->dfa_ = prog->GetDFA(progKind);
    state->dfa_->want_match_id_ = true;

    DFA::RWLocker cache_lock(&state->dfa_->cache_mutex_);
    SelectDfaStartStateParams select_start_params(state, &cache_lock);
    if (!select_dfa_start_state(&select_start_params)) {
      state->flags_ |= State::kInvalid;
      return kErrorOutOfMemory;
    }

    state->flags_ |= State::kInitialized;

    DfaLoopParams loop_params(state, &cache_lock, chunk);
    ExecResult exec_result = dfa_loop(&loop_params);
    if (exec_result != kContinueBackward)
      return exec_result;
  }

  // reverse scan

  assert(state->match_end_offset_ != -1 && "reverse scan without end-of-match");

  state->dfa_ = rprog_->GetDFA(Prog::kLongestMatch);
  state->flags_ = State::kReverse | State::kAnchored | State::kInitialized;

  DFA::RWLocker cache_lock(&state->dfa_->cache_mutex_);
  SelectDfaStartStateParams select_start_params(state, &cache_lock);
  if (!select_dfa_start_state(&select_start_params)) {
    state->flags_ |= State::kInvalid;
    return kErrorOutOfMemory;
  }

  if (prev_offset >= state->match_end_offset_) { // overshoot
    state->offset_ = prev_offset;
    return kContinueBackward;
  }

  size_t prefix_size = state->match_end_offset_ - prev_offset;
  assert(prefix_size <= chunk.size() && "inconsistent match end offset");
  state->offset_ = state->match_end_offset_;

  DfaLoopParams loop_params(state, &cache_lock, StringPiece(chunk.data(), prefix_size));
  return dfa_loop(&loop_params);
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

  MutexLock l(&dfa->mutex_);
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
  const uint8_t* bytemap = prog->bytemap();
  const uint8_t* lastmatch = NULL;  // most recent matching position in text
  DFA::State* lastmatch_state = NULL;

  if (ExtraDebug)
    printf("%s ->\n", dfa->DumpState(s).c_str());

  if (reverse) {
    std::swap(p, end);
    if (state->offset_ == state->match_end_offset_ && bp < ep)
      state->match_last_char_ = ep[-1];
  }

  if (s->IsMatch()) {
    lastmatch = reverse ? p + 1 : p - 1;
    if (!reverse)
      lastmatch_state = s;
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
        DFA::StateSaver save_lastmatch_state(dfa, lastmatch_state);

        dfa->ResetCache(params->cache_lock);

        if (
          !(start = save_start.Restore()) ||
          !(s = save_s.Restore()) ||
          !reverse && lastmatch_state && !(lastmatch_state = save_lastmatch_state.Restore())
        ) {
          state->flags_ |= State::kInvalid;
          return kErrorOutOfMemory;
        }

        state->dfa_start_state_ = start; // update start state

        ns = dfa->RunStateOnByteUnlocked(s, c);
        if (!ns) {
          state->flags_ |= State::kInvalid;
          return kErrorOutOfMemory;
        }
      }
    }

    if (ExtraDebug)
      printf("  '%c' -> %s\n", c, dfa->DumpState(ns).c_str());

    if (ns <= SpecialStateMax) {
      if (ns == DeadState) {
        if (reverse) {
          if (lastmatch)
            state->match_offset_ = state->offset_ - (ep - lastmatch);
          else if (state->match_offset_ == -1) {
            state->flags_ |= State::kInvalid;
            return kErrorInconsistent;
          }

          state->flags_ |= State::kMatch;
          return kMatch;
        } else {
          if (lastmatch) {
            state->match_end_offset_ = state->offset_ + (lastmatch - bp);
            state->match_next_char_ = *lastmatch;
            state->match_id_ = lastmatch_state->MatchId();
          } else if (state->match_end_offset_ == -1) {
            state->flags_ |= State::kInvalid;
            return kMismatch;
          }

          if (state->offset_ < state->match_end_offset_)
            state->offset_ = state->match_end_offset_;

          return kContinueBackward;
        }
      } else { // match all the way to the end
        assert(ns == FullMatchState && "invalid DFA special state");
        if (reverse) {
          state->match_offset_ = state->base_offset_;
          state->flags_ |= State::kMatch;
          return kMatch;
        } else {
          state->offset_ += length;
          if (state->offset_ < state->eof_offset_) {
            state->flags_ |= State::kFullMatch;
            return kContinue;
          }

          state->match_end_offset_ = state->offset_;
          state->match_next_char_ = state->eof_char_;
          return kContinueBackward;
        }
      }
    }

    s = ns;
    if (s->IsMatch()) {
      lastmatch = reverse ? p + 1 : p - 1;
      if (!reverse)
        lastmatch_state = s;
    }
  }

  if (lastmatch) // the DFA notices the match one byte late
    if (reverse)
      state->match_offset_ = state->offset_ - (ep - lastmatch);
    else {
      state->match_end_offset_ = state->offset_ + (lastmatch - bp);
      state->match_next_char_ = *lastmatch;
      state->match_id_ = lastmatch_state->MatchId();
    }

  state->dfa_state_ = s;

  if (reverse) {
    state->offset_ = state->offset_ - length;
    if (state->offset_ > state->base_offset_)
      return kContinueBackward;
  } else {
    state->offset_ = state->offset_ + length;
    if (state->offset_ < state->eof_offset_)
      return kContinue;
  }

  // process eof -- triggers either match or mismatch

  int c = reverse ? state->base_char_ : state->eof_char_;
  size_t i = c == kByteEndText ? prog->bytemap_range() : bytemap[c];
  DFA::State* ns = s->next_[i].load(std::memory_order_acquire);
  if (!ns) {
    ns = dfa->RunStateOnByteUnlocked(s, c);
    if (!ns) {
      DFA::StateSaver save_s(dfa, s);
      DFA::StateSaver save_lastmatch_state(dfa, lastmatch_state);

      dfa->ResetCache(params->cache_lock);

      if (
        !(s = save_s.Restore()) ||
        !reverse && lastmatch_state && !(lastmatch_state = save_lastmatch_state.Restore())
      ) {
        state->flags_ |= State::kInvalid;
        return kErrorOutOfMemory;
      }

      ns = dfa->RunStateOnByteUnlocked(s, c);
      if (!ns) {
        state->flags_ |= State::kInvalid;
        return kErrorOutOfMemory;
      }
    }
  }

  if (ExtraDebug)
    printf("  '\\%03d' -> %s\n", c, dfa->DumpState(ns).c_str());

  if (ns <= SpecialStateMax) {
    if (ns == DeadState) {
      if (reverse) {
        if (state->match_offset_ == -1) {
          state->flags_ |= State::kInvalid;
          return kErrorInconsistent;
        }

        state->flags_ |= State::kMatch;
        return kMatch;
      } else {
        if (state->match_end_offset_ == -1) {
          state->flags_ |= State::kInvalid;
          return kMismatch;
        }

        return kContinueBackward;
      }
    } else { // match all the way to the end
        assert(ns == FullMatchState && "invalid DFA special state");
      if (reverse) {
        state->match_offset_ = state->offset_;
        state->flags_ |= State::kMatch;
        return kMatch;
      } else {
        state->match_end_offset_ = state->offset_;
        state->match_next_char_ = state->eof_char_;
        return kContinueBackward;
      }
    }
  }

  if (ns->IsMatch()) {
    if (reverse) {
      state->match_offset_ = state->offset_;
      state->flags_ |= State::kMatch;
      return kMatch;
    } else {
      state->match_end_offset_ = state->offset_;
      state->match_next_char_ = state->eof_char_;
      state->match_id_ = ns->MatchId();
      return kContinueBackward;
    }
  }

  if (reverse) {
    if (state->match_offset_ == -1) {
      state->flags_ |= State::kInvalid;
      return kErrorInconsistent;
    }

    state->flags_ |= State::kMatch;
    return kMatch;
  } else {
    if (state->match_end_offset_ == -1) {
      state->flags_ |= State::kInvalid;
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

}  // namespace re2
