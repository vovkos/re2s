// Copyright 2010 The RE2 Authors.  All Rights Reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#ifndef RE2_SM_H_
#define RE2_SM_H_

#include <string>
#include <list>
#include <assert.h>

#include "re2/stringpiece.h"
#include "re2/re2.h"

namespace re2 {

class Prog;
class Regexp;
class DFA;
class RWLocker;

class RE2::SM {
 private:
  struct DfaBaseParams;
  struct SelectDfaStartStateParams;
  struct DfaLoopParams;

  struct Module {
    Module(int match_id = 0);
    ~Module() {
      clear();
    }

    int match_id() const {
      return match_id_;
    }

    const std::string& pattern() const {
      return pattern_;
    }

    Prog* prog() const {
      return prog_;
    }

    size_t capture_count() const {
      return capture_count_;
    }

    void clear();

    bool capture_submatches(
      StringPiece match,
      StringPiece* submatches,
      size_t nsubmatches
    ) const;

   private:
    std::string pattern_;
    re2::Regexp* regexp_;
    Prog* prog_;
    size_t capture_count_;
	  int match_id_;

    friend class RE2::SM;
  };

 public:
  class State;

  enum ExecFlags {
    kAnchored      = 0x01, // = RE2::ANCHOR_START
    kFullMatch     = 0x02, // = RE2::ANCHOR_BOTH
    kEndOffsetOnly = 0x04, // no need to run the reverse DFA to find the match start
  };

  enum ExecResult {
	  kErrorInconsistent = -2, // reverse scan couldn't find a match (inconsistent data)
	  kErrorOutOfMemory  = -1, // DFA run out-of-memory
	  kMismatch          = 0,  // match can't be found; next exec will reset & restart
	  kContinue,               // match not found yet; continue feeding next chunks of data
	  kContinueBackward,       // match end found; continue feeding previous chunks of data
	  kMatch,                  // match end & start found
  };

  enum Kind {
    kUndefined,
    kSingleRegexp,
    kRegexpSwitch,
  };

   enum {
     kByteEndText = 256 // same as DFA::kByteEndText
   };

 public:
  SM() {
    init();
  }

  SM(StringPiece pattern, const Options& options = RE2::DefaultOptions) {
    init(), create(pattern, options);
  }

  ~SM() {
    clear();
  }

  // not copyable or movable

  SM(const SM&) = delete;
  SM& operator=(const SM&) = delete;
  SM(SM&&) = delete;
  SM& operator=(SM&&) = delete;

  // properties

  bool ok() const {
    return error_code_ == NoError;
  }

  ErrorCode error_code() const {
    return error_code_;
  }

  const std::string& error() const {
    return error_;
  }

  const std::string& error_arg() const {
    return error_arg_;
  }

  Kind kind() const {
    return kind_;
  }

  const Options& options() const {
    return options_;
  }

  size_t switch_case_count() {
    assert(kind_ == kRegexpSwitch);
    return switch_case_module_array_.size();
  }

  const std::string& pattern() const {
    assert(kind_ == kSingleRegexp);
    return main_module_.pattern();
  }

  const std::string& pattern(int id) const {
    assert(kind_ == kRegexpSwitch);
    return switch_case_module_array_[id]->pattern();
  }

  size_t capture_count() const {
    assert(kind_ == kSingleRegexp);
    return main_module_.capture_count();
  }

  size_t capture_count(int id) const {
    assert(kind_ == kRegexpSwitch);
    return switch_case_module_array_[id]->capture_count();
  }

  // compilation

  void clear();

  bool create(StringPiece pattern, const Options& options = RE2::DefaultOptions);

  void create_switch(const Options& options = RE2::DefaultOptions);
  int add_switch_case(StringPiece pattern);
  bool finalize_switch();

  // execution (single block of text)

  State exec(StringPiece text, int exec_flags = 0) const;

  // execution (stream matcher interface)

  ExecResult exec(State* state, StringPiece chunk) const;
  ExecResult exec_eof(State* state, StringPiece last_chunk, int eof_char = kByteEndText) const;
  ExecResult exec_eof(State* state, int eof_char = kByteEndText) const {
    return exec_eof(state, StringPiece("", 0), eof_char);
  }

  bool capture_submatches(
    StringPiece match,
    StringPiece* submatches,
    size_t nsubmatches
  ) const {
    assert(kind_ == kSingleRegexp);
    return main_module_.capture_submatches(match, submatches, nsubmatches);
  }

  bool capture_submatches(
    int id,
    StringPiece match,
    StringPiece* submatches,
    size_t nsubmatches
  ) const {
    assert(kind_ == kRegexpSwitch);
    return switch_case_module_array_[id]->capture_submatches(match, submatches, nsubmatches);
  }

 private:
  void init();
  bool parse_module(Module* module, StringPiece pattern);
  bool compile_prog(Module* module);
  bool compile_rprog();
  re2::Regexp* append_regexp_match_id(re2::Regexp* regexp, int match_id);

  static bool select_dfa_start_state(SelectDfaStartStateParams* params);
  static bool select_dfa_start_state_impl(SelectDfaStartStateParams* params);

  template <
    bool can_prefix_accel,
    bool run_forward
  >
  static ExecResult dfa_loop_impl(DfaLoopParams* params);

  static inline ExecResult dfa_loop_ff(DfaLoopParams* params);
  static inline ExecResult dfa_loop_ft(DfaLoopParams* params);
  static inline ExecResult dfa_loop_tf(DfaLoopParams* params);
  static inline ExecResult dfa_loop_tt(DfaLoopParams* params);

  static ExecResult dfa_loop(DfaLoopParams* params);

 private:
  Options options_;
  Kind kind_;
  ErrorCode error_code_;
  std::string error_;
  std::string error_arg_;

  std::vector<Module*> switch_case_module_array_;
  Module main_module_;
  Prog* rprog_;
};

class RE2::SM::State {
  friend class RE2::SM;

 protected:
   enum StateFlags {
     kReverse        = 0x0001, // revere DFA scan to find the start of a match
     kCanPrefixAccel = 0x0002, // can use memchr to fast-forward to a potential match
     kStartAnchored  = 0x0010, // anchored search
     kInitialized    = 0x0020, // state is initialized
     kFullMatch      = 0x0040, // in this state, DFA matches all the way to the very end
     kMatchReady     = 0x0100, // post match; will auto-restart on the next exec
     kInvalid        = 0x0200, // post error or mismatch; needs a manual reset
   };

 public:
  State(int exec_flags = 0) { // ExecFlags
    reset(exec_flags);
  }
  State(
    int exec_flags,
    uint64_t base_offset,
    int base_char,
    uint64_t eof_offset = -1,
    int eof_char = kByteEndText
  ) {
    reset(exec_flags, base_offset, base_char, eof_offset, eof_char);
  }

  operator bool () const {
    return is_match();
  }

  // properties

  int exec_flags() const {
    return exec_flags_;
  }
  uint64_t base_offset() const {
    return base_offset_;
  }
  int base_char() const {
    return base_char_;
  }
  uint64_t eof_offset() const {
    return eof_offset_;
  }
  int eof_char() const {
    return eof_char_;
  }

  // these two are for testing & debugging

  int match_last_char() const {
    return match_last_char_;
  }
  int match_next_char() const {
    return match_next_char_;
  }

  // match info

  bool is_match() const {
    return (state_flags_ & kMatchReady) != 0;
  }
  bool has_match_text() const {
    return match_text_.data() != NULL;
  }
  uint64_t match_offset() const {
    assert(is_match());
    return match_offset_;
  }
  uint64_t match_end_offset() const {
    return match_end_offset_;
  }
  uint64_t match_length() const {
    assert(is_match());
    return match_end_offset_ - match_offset_;
  }
  StringPiece match_text() const {
    assert(is_match() && has_match_text());
    return match_text_;
  }
  int match_id() const {
    return match_id_;
  }

  // state setup

  void reset(int exec_flags = 0) {
    reset(exec_flags, 0, kByteEndText, -1, kByteEndText);
  }
  void reset(
    int exec_flags,
    uint64_t base_offset,
    int base_char,
    uint64_t eof_offset = -1,
    int eof_char = kByteEndText
  );

  void set_eof_offset(uint64_t offset, int eof_char = kByteEndText);
  void set_eof(int eof_char = kByteEndText) {
    set_eof_offset(offset_, eof_char);
  }

 protected:
  // adds kMatch to state->flags_  and sets up state->match_text_ when it's available
  void finalize_match(uint64_t chunk_end_offset, StringPiece chunk);

 private:
  DFA* dfa_;
  void* dfa_state_;       // DFA::State*
  void* dfa_start_state_; // DFA::State*
  uint64_t offset_;
  uint64_t base_offset_;
  uint64_t eof_offset_;
  uint64_t match_offset_;
  uint64_t match_end_offset_;
  StringPiece match_text_; // only when available
  int match_id_;
  int base_char_;
  int eof_char_;
  int match_last_char_;
  int match_next_char_;
  int last_char_;
  int exec_flags_  : 16;
  int state_flags_ : 16;
};

inline void RE2::SM::State::reset(
  int exec_flags,
  uint64_t base_offset,
  int base_char,
  uint64_t eof_offset,
  int eof_char
) {
  dfa_ = NULL;
  dfa_state_ = NULL;
  dfa_start_state_ = NULL;
  offset_ = base_offset;
  base_offset_ = base_offset;
  eof_offset_ = eof_offset;
  match_offset_ = -1;
  match_end_offset_ = -1;
  match_text_ = StringPiece();
  match_id_ = -1;
  base_char_ = base_char;
  eof_char_ = eof_char;
  match_last_char_ = base_char;
  match_next_char_ = eof_char;
  last_char_ = base_char;
  exec_flags_ = exec_flags;
  state_flags_ = 0;
}

inline void RE2::SM::State::set_eof_offset(uint64_t offset, int eof_char) {
  assert(!(state_flags_ & kReverse) || (state_flags_ & kMatchReady)); // after match we still have kReverse
  assert(offset >= offset_);
  eof_offset_ = offset;
  eof_char = eof_char;
}

inline RE2::SM::State RE2::SM::exec(StringPiece text, int exec_flags) const {
  State state(exec_flags);
  exec_eof(&state, text);
  return state;
}

inline RE2::SM::ExecResult RE2::SM::exec_eof(State* state, StringPiece last_chunk, int eof_char) const {
  state->set_eof_offset(state->offset_ + last_chunk.size(), eof_char);
  return exec(state, last_chunk);
}

}  // namespace re2

#endif  // RE2_SM_H_
