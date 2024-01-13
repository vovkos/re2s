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

//..............................................................................

class RE2::SM::SharedState {
private:
  struct PersistentDFAState {
    DFA::State* state;
    std::vector<int> inst;
    uint32_t flag;

    PersistentDFAState() {
      state = NULL;
      flag = 0;
    }

    void save() {
      if (state <= SpecialStateMax)
        return;

      inst.assign(state->inst_, state->inst_ + state->ninst_);
      flag = state->flag_;
      state = NULL;
    }

    bool restore(DFA* dfa) {
      assert(!state && inst.size());
      return (state = dfa->CachedState(inst.data(), (int)inst.size(), flag)) != NULL;
    }
  };

public:
  SharedState() {
    sm_ = NULL;
    dfa_ = NULL;
  }

  ~SharedState() {
    if (sm_)
      sm_->detach_shared_state(this);
  }

  std::shared_ptr<SharedState> clone() const {
    assert(sm_ && dfa_);
    std::shared_ptr<SharedState> ptr = std::make_shared<SharedState>();
    sm_->attach_shared_state(ptr.get());
    ptr->dfa_ = dfa_;
    ptr->dfa_state_ = dfa_state_;
    ptr->dfa_start_state_ = dfa_start_state_;
    return ptr;
  }

  bool is_initialized() const {
    return dfa_ != NULL;
  }

  bool is_ready() const {
    return dfa_state_.state != NULL;
  }

  const SM* sm() const {
    return sm_;
  }

  DFA* dfa() const {
    assert(dfa_);
    return dfa_;
  }

  DFA::State* dfa_start_state() const {
    assert(dfa_start_state_.state);
    return dfa_start_state_.state;
  }

  DFA::State* dfa_state() const {
    assert(dfa_state_.state);
    return dfa_state_.state;
  }

  void set_dfa_state(DFA::State* dfa_state) {
    dfa_state_.state = dfa_state;
  }

  void init(const SM* sm, DFA* dfa, DFA::State* dfa_state) {
    if (!sm_)
      sm->attach_shared_state(this);
    else if (sm_ != sm) {
      sm_->detach_shared_state(this);
      sm->attach_shared_state(this);
    }

    dfa_ = dfa;
    dfa_state_.state = dfa_start_state_.state = dfa_state;
  }

  void save() {
    dfa_state_.save();
    dfa_start_state_.save();
  }

  bool restore() {
    return dfa_state_.restore(dfa_) && dfa_start_state_.restore(dfa_);
  }

  void reset() {
    dfa_ = NULL;
    dfa_state_.state = dfa_start_state_.state = NULL;
  }

private:
  const SM* sm_;
  DFA* dfa_;
  PersistentDFAState dfa_start_state_;
  PersistentDFAState dfa_state_;
  std::list<SharedState*>::iterator it_;

  friend class SM;
};

//..............................................................................

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

size_t RE2::SM::Module::capture_submatches(
  StringPiece match,
  StringPiece* submatches,
  size_t nsubmatches
) const {
  if (nsubmatches > capture_count_ + 1)
    nsubmatches = capture_count_ + 1;

  bool can_one_pass = prog_->IsOnePass() && nsubmatches <= Prog::kMaxOnePassCapture;
  bool can_bit_state = prog_->CanBitState() && match.length() <= prog_->bit_state_text_max_size();

  bool result =
    can_one_pass ? prog_->SearchOnePass(match, match, Prog::kAnchored, Prog::kFullMatch, submatches, (int)nsubmatches) :
    can_bit_state ? prog_->SearchBitState(match, match, Prog::kAnchored, Prog::kFullMatch, submatches, (int)nsubmatches) :
    prog_->SearchNFA(match, match, Prog::kAnchored, Prog::kFullMatch, submatches, (int)nsubmatches);

  return result ? nsubmatches : -1;
}

// . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . .

bool RE2::SM::nullable() const {
  assert(main_module_.prog_); // only after compilation
  return main_module_.prog_->nullable();
}

void RE2::SM::clear() {
  main_module_.clear();
  delete rprog_;
  rprog_ = NULL;
  error_code_ = NoError;
  error_.clear();
  error_arg_.clear();
  options_ = RE2::DefaultOptions;
  kind_ = kUndefined;

  for (intptr_t i = switch_case_module_array_.size() - 1; i >= 0; i--)
    delete switch_case_module_array_[i];

  switch_case_module_array_.clear();

  std::lock_guard<std::mutex> lock(shared_state_list_lock_);
  std::list<SharedState*>::iterator it = shared_state_list_.begin();
  for (; it != shared_state_list_.end(); it++)
    (*it)->sm_ = NULL; // detach

  shared_state_list_.clear();
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
  assert(kind_ == kRegexpSwitch);

  int match_id = (int)switch_case_module_array_.size();
  std::unique_ptr<Module> module(new Module(match_id));
  bool result =
    parse_module(module.get(), pattern) &&
    compile_prog(module.get());

  if (!result)
    return -1;

  switch_case_module_array_.push_back(module.release());
  return match_id;
}

bool RE2::SM::finalize_switch() {
  assert(kind_ == kRegexpSwitch);

  assert(
    main_module_.regexp_ == NULL &&
    main_module_.prog_ == NULL &&
    rprog_ == NULL
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
    sub[i] = v[i]->regexp_->Incref(); // will be Decref-ed by main_module_.regexp_

  re2::Regexp* prev_regexp = main_module_.regexp_;
  main_module_.regexp_ = re2::Regexp::AlternateNoFactor(sub.data(), count, flags);
  bool result = compile_rprog();
  if (!result)
    return false;

  main_module_.regexp_->Decref();

  for (int i = 0; i < count; i++) {
    Module* module = v[i];
    sub[i] = append_regexp_match_id(module->regexp_, module->match_id_);
    module->regexp_ = NULL; // after factoring Alternate below, this regexp can't be reused anyway
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
  DFA* dfa;
  DFA::StartInfo* info;
  uint32_t flags;

  SelectDfaStartStateParams(RE2::SM::State* state0, DFA::RWLocker* cache_lock0, DFA* dfa0):
    DfaBaseParams(state0, cache_lock0),
    dfa(dfa0) {}
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
    !(state->state_flags_ & State::kStateInvalid) &&
    kind_ != kUndefined
  );

  if (!chunk.data()) {
    assert(chunk.length() == 0);
    chunk = StringPiece("", 0); // make sure we don't pass NULL pointers to dfa_loop
  }

  if (state->state_flags_ & State::kStateMatch) // restart after match
    state->resume();

  if (state->state_flags_ & State::kStateReverse) { // reverse scan state
    if (state->offset_ > state->match_end_offset_) { // overshoot
      uint64_t overshoot_size = state->offset_ - state->match_end_offset_;
      if (overshoot_size > chunk.length()) {
        state->offset_ -= chunk.length();
        return kContinueBackward;
      }

      state->offset_ = state->match_end_offset_;
      state->match_next_char_ = (uint8_t)chunk[chunk.length() - (size_t)overshoot_size];
      chunk.remove_suffix((size_t)overshoot_size);
    }

    uint64_t leftover_size = state->offset_ - state->base_offset_;
    if (leftover_size < chunk.length()) // don't go beyond base_offset
      chunk = StringPiece(chunk.data() + chunk.length() - (size_t)leftover_size, (size_t)leftover_size);

    DFA::RWLocker cache_lock(&state->shared_->dfa()->cache_mutex_);
    DfaLoopParams loop_params(state, &cache_lock, chunk);
    return dfa_loop(&loop_params);
  }

  // forward scan

  Prog* prog = main_module_.prog_;
  uint64_t prev_offset = state->offset_;
  bool is_initialized = state->shared_ && state->shared_->sm() == this && state->shared_->is_initialized();

  if (
    is_initialized &&
    (state->state_flags_ & (State::kStateReverse | State::kStateFullMatch)) == State::kStateFullMatch
  ) { // scan all the way to the EOF
    state->offset_ += chunk.length();
    if (state->offset_ < state->eof_offset_) {
      if (!chunk.empty())
        state->last_char_ = (uint8_t)chunk[chunk.length() - 1];
      return kContinue;
    }

    state->match_end_offset_ = state->offset_;
    state->match_last_char_ = !chunk.empty() ? (uint8_t)chunk[chunk.length() - 1] : state->last_char_;
    state->match_next_char_ = state->eof_char_;
    assert(state->match_id_ != -1); // should have been set in dfa_loop
  } else if (is_initialized) {
    DFA::RWLocker cache_lock(&state->shared_->dfa()->cache_mutex_);
    DfaLoopParams loop_params(state, &cache_lock, chunk);
    ExecResult exec_result = dfa_loop(&loop_params);
    if (exec_result != kContinueBackward)
      return exec_result;
  } else {
    if (prog->anchor_start() || (state->exec_flags_ & (kAnchored | kFullMatch)))
      state->state_flags_ |= State::kStateAnchored;

    Prog::MatchKind progKind = options_.longest_match() || (state->exec_flags_ & kFullMatch) ?
      Prog::kLongestMatch :
      Prog::kFirstMatch;

    DFA* dfa = prog->GetDFA(progKind);
    dfa->want_match_id_ = true;

    DFA::RWLocker cache_lock(&dfa->cache_mutex_);
    SelectDfaStartStateParams select_start_params(state, &cache_lock, dfa);
    if (!select_dfa_start_state(&select_start_params)) {
      state->mark_invalid();
      return kErrorOutOfMemory;
    }

    DfaLoopParams loop_params(state, &cache_lock, chunk);
    ExecResult exec_result = dfa_loop(&loop_params);
    if (exec_result != kContinueBackward)
      return exec_result;
  }

  // reverse scan

  assert(state->match_end_offset_ != -1 && state->match_id_ != -1);

  if (state->exec_flags_ & (kAnchored | kFullMatch)) {
    // we know where the match starts -- no need to scan back
    uint64_t chunk_end_offset = prev_offset + chunk.length();

    if (
      (state->exec_flags_ & kFullMatch) && (
        state->match_end_offset_ < state->eof_offset_ || // eof_offset_ is not precise, so we
        state->match_end_offset_ != chunk_end_offset     // also check the "real" eof offset
      )
    ) {
      state->mark_invalid();
      return kMismatch;
    }

    state->match_offset_ = state->base_offset_;
    state->finalize_match(chunk_end_offset, chunk);
    return kMatch;
  }

  if (state->exec_flags_ & kEndOffsetOnly) { // we only want the match end offset -- no need to scan back
    state->match_offset_ = state->match_end_offset_;
    state->state_flags_ |= State::kStateMatch;
    return kMatch;
  }

  DFA* dfa = rprog_->GetDFA(Prog::kLongestMatch);
  state->state_flags_ = State::kStateReverse | State::kStateAnchored;

  DFA::RWLocker cache_lock(&dfa->cache_mutex_);
  SelectDfaStartStateParams select_start_params(state, &cache_lock, dfa);
  if (!select_dfa_start_state(&select_start_params)) {
    state->mark_invalid();
    return kErrorOutOfMemory;
  }

  if (prev_offset >= state->match_end_offset_) { // overshoot
    state->offset_ = prev_offset;
    return kContinueBackward;
  }

  uint64_t prefix_size = state->match_end_offset_ - prev_offset;
  assert(prefix_size <= chunk.length());
  state->offset_ = state->match_end_offset_;

  DfaLoopParams loop_params(state, &cache_lock, StringPiece(chunk.data(), (size_t)prefix_size));
  return dfa_loop(&loop_params);
}

bool RE2::SM::select_dfa_start_state(SelectDfaStartStateParams* params) const {
  State* state = params->state;
  DFA* dfa = params->dfa;

  // Determine correct search type.
  int start;
  uint32_t flags;
  if (!(state->state_flags_ & State::kStateReverse)) {
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
    if (state->match_end_offset_ >= state->eof_offset_) {
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

  if (state->state_flags_ & State::kStateAnchored)
    start |= DFA::kStartAnchored;

  DFA::StartInfo* info = &dfa->start_[start];
  params->info = info;
  params->flags = flags;

  // Try once without cache_lock for writing.
  // Try again after resetting the cache
  // (ResetCache will relock cache_lock for writing).
  if (!select_dfa_start_state_impl(params)) {
    save_shared_states();
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
    !(state->state_flags_ & State::kStateAnchored) &&
    start_state > SpecialStateMax &&
    start_state->flag_ >> DFA::kFlagNeedShift == 0
  )
    state->state_flags_ |= State::kStateCanPrefixAccel;

  if (!state->shared_ || state->shared_.use_count() > 1)
    state->shared_ = std::make_shared<SharedState>();

  state->shared_->init(this, dfa, start_state);
  return true;
}

// Fills in info if needed.  Returns true on success, false on failure.
bool RE2::SM::select_dfa_start_state_impl(SelectDfaStartStateParams* params) {
  State* state = params->state;
  DFA* dfa = params->dfa;

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
    (state->state_flags_ & State::kStateAnchored) ?
      dfa->prog_->start() :
      dfa->prog_->start_unanchored(),
    params->flags
  );

  start = dfa->WorkqToCachedState(dfa->q0_, NULL, params->flags);
  if (!start)
    return false;

  // Synchronize with "quick check" above.
  params->info->start.store(start, std::memory_order_release);
  return true;
}

void RE2::SM::attach_shared_state(SharedState* sstate) const {
  assert(sstate->sm() == NULL);
  std::lock_guard<std::mutex> lock(shared_state_list_lock_);
  shared_state_list_.push_front(sstate);
  sstate->sm_ = this;
  sstate->it_ = shared_state_list_.begin();
}

void RE2::SM::detach_shared_state(SharedState* sstate) const {
  assert(sstate->sm() == this);
  std::lock_guard<std::mutex> lock(shared_state_list_lock_);
  shared_state_list_.erase(sstate->it_);
  sstate->sm_ = NULL;
}

void RE2::SM::save_shared_states() const {
  std::lock_guard<std::mutex> lock(shared_state_list_lock_);
  std::list<SharedState*>::iterator it = shared_state_list_.begin();
  for (; it != shared_state_list_.end(); it++)
    (*it)->save();
}

template <
  bool can_prefix_accel,
  bool reverse
>
RE2::SM::ExecResult RE2::SM::dfa_loop_impl(DfaLoopParams* params) {
  State* state = params->state;
  if (state->shared_.use_count() > 1)
    state->shared_ = state->shared_->clone();

  SharedState* sstate = state->shared_.get();
  if (!sstate->is_ready()) {
    bool result = sstate->restore();
    if (!result) {
      state->mark_invalid();
      return kErrorOutOfMemory;
    }
  }

  DFA* dfa = sstate->dfa();
  Prog* prog = dfa->prog_;
  DFA::State* start = sstate->dfa_start_state();
  DFA::State* s = sstate->dfa_state();
  uint64_t chunk_end_offset;

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
    chunk_end_offset = state->offset_;
    std::swap(p, end);
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
        DFA::StateSaver save_s(dfa, s);
        DFA::StateSaver save_lastmatch_state(dfa, lastmatch_state);

        sstate->sm()->save_shared_states();
        dfa->ResetCache(params->cache_lock);

        if (
          !(s = save_s.Restore()) ||
          !reverse && lastmatch_state && !(lastmatch_state = save_lastmatch_state.Restore()) ||
          !sstate->restore() ||
          !(ns = dfa->RunStateOnByteUnlocked(s, c))
        ) {
          state->mark_invalid();
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
            state->mark_invalid();
            return kErrorInconsistent;
          }

          state->finalize_match(chunk_end_offset, params->chunk);
          return kMatch;
        } else {
          if (lastmatch) {
            assert(lastmatch_state); // lastmatch_state is set together with lastmatch
            state->match_id_ = lastmatch_state->MatchId();
            state->match_end_offset_ = state->offset_ + (lastmatch - bp);
            state->match_last_char_ = bp < lastmatch ? lastmatch[-1] : state->last_char_;
            state->match_next_char_ = *lastmatch;
          } else if (state->match_end_offset_ == -1) {
            state->mark_invalid();
            return kMismatch;
          }

          if (state->offset_ < state->match_end_offset_)
            state->offset_ = state->match_end_offset_;

          return kContinueBackward;
        }
      } else { // match all the way to the end
        assert(ns == FullMatchState);
        if (reverse) {
          state->match_offset_ = state->base_offset_;
          state->finalize_match(chunk_end_offset, params->chunk);
          return kMatch;
        } else {
          assert(lastmatch_state); // the state leading to FullMatchState is always a matching state
          state->match_id_ = lastmatch_state->MatchId();
          state->offset_ += length;
          if (state->offset_ < state->eof_offset_) {
            state->state_flags_ |= State::kStateFullMatch;
            if (bp < ep)
              state->last_char_ = ep[-1];
            return kContinue;
          }

          state->match_end_offset_ = state->offset_;
          state->match_last_char_ = bp < ep ? ep[-1] : state->last_char_;
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

  if (lastmatch) { // the DFA notices the match one byte late
    if (reverse)
      state->match_offset_ = state->offset_ - (ep - lastmatch);
    else {
      assert(lastmatch_state); // lastmatch_state is set together with lastmatch
      state->match_id_ = lastmatch_state->MatchId();
      state->match_end_offset_ = state->offset_ + (lastmatch - bp);
      state->match_last_char_ = bp < lastmatch ? lastmatch[-1] : state->last_char_;
      state->match_next_char_ = *lastmatch;
    }
  }

  sstate->set_dfa_state(s);

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
  size_t i = c == kByteEndText ? prog->bytemap_range() : bytemap[c];
  DFA::State* ns = s->next_[i].load(std::memory_order_acquire);
  if (!ns) {
    ns = dfa->RunStateOnByteUnlocked(s, c);
    if (!ns) {
      DFA::StateSaver save_s(dfa, s);
      DFA::StateSaver save_lastmatch_state(dfa, lastmatch_state);

      sstate->sm()->save_shared_states();
      dfa->ResetCache(params->cache_lock);

      if (
        !(s = save_s.Restore()) ||
        !reverse && lastmatch_state && !(lastmatch_state = save_lastmatch_state.Restore()) ||
        !sstate->restore() ||
        !(ns = dfa->RunStateOnByteUnlocked(s, c))
      ) {
        state->mark_invalid();
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
          state->mark_invalid();
          return kErrorInconsistent;
        }

        state->finalize_match(chunk_end_offset, params->chunk);
        return kMatch;
      } else {
        if (state->match_end_offset_ == -1) {
          state->mark_invalid();
          return kMismatch;
        }

        return kContinueBackward;
      }
    } else { // match all the way to the end
      assert(ns == FullMatchState);
      if (reverse) {
        state->match_offset_ = state->base_offset_;
        state->finalize_match(chunk_end_offset, params->chunk);
        return kMatch;
      } else {
        assert(lastmatch_state); // the state leading to FullMatchState is always a matching state
        state->match_id_ = lastmatch_state->MatchId();
        state->match_end_offset_ = state->offset_;
        state->match_last_char_ = bp < ep ? ep[-1] : state->last_char_;
        state->match_next_char_ = state->eof_char_;
        return kContinueBackward;
      }
    }
  }

  if (ns->IsMatch()) {
    if (reverse) {
      state->match_offset_ = state->offset_;
      state->finalize_match(chunk_end_offset, params->chunk);
      return kMatch;
    } else {
      state->match_id_ = ns->MatchId();
      state->match_end_offset_ = state->offset_;
      state->match_last_char_ = bp < ep ? ep[-1] : state->last_char_;
      state->match_next_char_ = state->eof_char_;
      return kContinueBackward;
    }
  }

  if (reverse) {
    if (state->match_offset_ == -1) {
      state->mark_invalid();
      return kErrorInconsistent;
    }

    state->finalize_match(chunk_end_offset, params->chunk);
    return kMatch;
  } else {
    if (state->match_end_offset_ == -1) {
      state->mark_invalid();
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

  size_t i = params->state->state_flags_ & 0x03; // State::kStateCanPrefixAccel | State::kStateReverse;
  return funcTable[i](params);
}

// . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . .

void RE2::SM::State::finalize_match(uint64_t chunk_end_offset, StringPiece chunk) {
  assert(match_offset_ != -1 && match_end_offset_ != -1);
  state_flags_ |= State::kStateMatch;
  uint64_t chunk_offset = chunk_end_offset - chunk.length();
  if (match_offset_ >= chunk_offset && match_end_offset_ <= chunk_end_offset)
    match_text_ = StringPiece(chunk.data() + (size_t)(match_offset_ - chunk_offset), (size_t)match_length());
}

void RE2::SM::State::reset_shared() {
  if (shared_) {
    if (shared_.use_count() == 1)
      shared_->reset();
    else
      shared_ = NULL;
  }
}

//..............................................................................

}  // namespace re2
