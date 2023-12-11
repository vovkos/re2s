#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <assert.h>
#include <string>

#include "re2.h"
#include "re2/sm.h"
#include "re2/set.h"
#include "re2/stringpiece.h"

void print_submatches(const re2::StringPiece* submatches, size_t count) {
  for (size_t i = 0; i < count; i++)
    printf("  submatch[%zd]: '%s'\n", i, std::string(submatches[i]).c_str());
}

void print_re2_sm_submatches(const re2::RE2::SM& sm, int match_id, re2::StringPiece match) {
  enum {
    MaxSubmatchCount = 8
  };

  re2::StringPiece submatches[MaxSubmatchCount];
  size_t capture_count;
  bool result;

  if (sm.kind() == re2::RE2::SM::kSingleRegexp) {
    capture_count = sm.capture_count() + 1;
    assert(capture_count < MaxSubmatchCount);
    result = sm.capture_submatches(match, submatches, capture_count);
  } else {
    capture_count = sm.capture_count(match_id) + 1;
    assert(capture_count < MaxSubmatchCount);
    result = sm.capture_submatches(match_id, match, submatches, capture_count);
  }

  if (!result)
    printf("can't capture submatches\n");
  else
    print_submatches(submatches, capture_count);
}

int main() {
  printf("re2::RE2::SM playtest\n");

  re2::RE2::Options options;
  options.set_longest_match(true);
  options.set_case_sensitive(false);
  options.set_multi_line(true);

  do {
    printf("\nchecking full-match\n");

    const char pattern[] = "abc";
    const char text[] = "abcd";

    printf("RE2:\n");

    re2::RE2 re(pattern);
    bool result = RE2::FullMatch(text, re);
    if (!result)
      printf("not found\n");
    else
      printf("match!\n");

    printf("RE2::SM:\n");

    re2::RE2::SM sm(pattern);
    re2::RE2::SM::State state = sm.exec(text, re2::RE2::SM::kFullMatch);
    if (!state)
      printf("not found\n");
    else
      printf("match at %" PRIu64 ":%" PRIu64 " '%s'\n", state.match_offset(), state.match_end_offset(), state.match_text().ToString().c_str());
  } while (0);

  do {
    printf("\nchecking switch with common prefixes\n");

    const char* switch_patterns[] = {
      "a1",
      "b2",
      "b3",
    };

    const char text[] = "b2";

    printf("RE2::Set:\n");

    std::string error;
    re2::RE2::Set set(options, re2::RE2::ANCHOR_BOTH);

    for (size_t i = 0; i < sizeof(switch_patterns) / sizeof(switch_patterns[0]); i++)
      set.Add(switch_patterns[i], &error);

    set.Compile();

    std::vector<int> v;
    bool result = set.Match(text, &v);
    if (!result)
      printf("not found\n");
    else
      printf("match id: %d\n", v[0]);

    printf("RE2::SM:\n");

    re2::RE2::SM sm;
    sm.create_switch(options);
    for (size_t i = 0; i < sizeof(switch_patterns) / sizeof(switch_patterns[0]); i++)
      sm.add_switch_case(switch_patterns[i]);
    result = sm.finalize_switch();
    if (!result) {
      printf("error: %s\n", sm.error().c_str());
      return -1;
    }

    re2::RE2::SM::State state(re2::RE2::SM::kAnchored);
    std::string match_text;
    re2::RE2::SM::ExecResult exec_result = sm.exec_eof(&state, re2::StringPiece(text));
    assert(exec_result == re2::RE2::SM::kMatch);

    printf("match id: %d\n", state.match_id());
  } while (0);

  const char single_pattern[] = "^abc(\\d+)$";
  const char* switch_patterns[] = {
    "^abc(\\d+)$",
    "^def(\\d+)$",
    "^ghi(\\d+)$",
  };

  const char text[] = "ghi12\n \ndef34\n \nabc56";
  size_t length = sizeof(text) - 1;

  do {
    printf("\nusing re2::RE2...\n");

    re2::RE2 re(std::string("(") + single_pattern + ")", options);
    re2::StringPiece match;
    re2::StringPiece submatch;
    bool result = RE2::PartialMatch(text, re, &match, &submatch);
    if (!result)
      printf("not found\n");
    else {
      printf(
        "match at %zd:%zd '%s'\n",
        match.begin() - text,
        match.end() - text,
        std::string(match).c_str()
      );
      print_submatches(&submatch, 1);
    }
  } while (0);

  do {
    printf("\nusing re2::RE2::SM (single regexp, full text)...\n");

    re2::RE2::SM sm;
    bool result = sm.create(single_pattern, options);
    if (!result) {
      printf("error: %s\n", sm.error().c_str());
      return -1;
    }

    re2::RE2::SM::State state;
    state.set_eof_offset(length);

    re2::RE2::SM::ExecResult exec_result = sm.exec(&state, text);
    if (exec_result != re2::RE2::SM::kMatch)
      printf("not found\n");
    else {
      printf(
        "match at %" PRIu64 ":%" PRIu64 " '%s'\n",
        state.match_offset(),
        state.match_end_offset(),
        state.match_text().ToString().c_str()
      );

      print_re2_sm_submatches(sm, state.match_id(), state.match_text());
    }
  } while (0);

  do {
    printf("\nusing re2::RE2::SM (regexp switch, full text)...\n");

    re2::RE2::SM sm;
    sm.create_switch(options);
    for (size_t i = 0; i < sizeof(switch_patterns) / sizeof(switch_patterns[0]); i++)
      sm.add_switch_case(switch_patterns[i]);
    bool result = sm.finalize_switch();
    if (!result) {
      printf("error: %s\n", sm.error().c_str());
      return -1;
    }

    re2::RE2::SM::State state;
    state.set_eof_offset(length);

    re2::RE2::SM::ExecResult exec_result = sm.exec(&state, text);
    if (exec_result != re2::RE2::SM::kMatch)
      printf("not found\n");
    else {
      printf(
        "match id: %d at %" PRIu64 ":%" PRIu64 " '%s' { 0x%02x; 0x%02x }\n",
        state.match_id(),
        state.match_offset(),
        state.match_end_offset(),
        state.match_text().ToString().c_str(),
        state.match_last_char(),
        state.match_next_char()
      );

      print_re2_sm_submatches(sm, state.match_id(), state.match_text());
    }
  } while (0);

  do {
    printf("\nusing re2::RE2::SM (regexp switch, char-by-char)...\n");

    re2::RE2::SM sm;
    sm.create_switch(options);
    for (size_t i = 0; i < sizeof(switch_patterns) / sizeof(switch_patterns[0]); i++)
      sm.add_switch_case(switch_patterns[i]);
    bool result = sm.finalize_switch();
    if (!result) {
      printf("error: %s\n", sm.error().c_str());
      return -1;
    }

    re2::RE2::SM::State state;
    state.set_eof_offset(length);

    std::string match_text;
    const char* p = text;
    const char* eof = text + length;
    while (p < eof) {
      re2::RE2::SM::ExecResult result = sm.exec(&state, re2::StringPiece(p, 1));
      assert(result != re2::RE2::SM::kMatch);
      switch (result) {
      case re2::RE2::SM::kContinue:
        p++;
        break;

      case re2::RE2::SM::kMismatch:
        printf("not found\n");
        return -1;

      case re2::RE2::SM::kContinueBackward:
        printf("end-of-match id: %d at %" PRIu64 "\n", state.match_id(), state.match_end_offset());

        while (p > text) {
          re2::RE2::SM::ExecResult result = sm.exec(&state, re2::StringPiece(p - 1, 1));
          switch (result) {
          case re2::RE2::SM::kContinueBackward:
            p--;
            break;

          case re2::RE2::SM::kMatch:
            p = text; // break out of the parent while loop
            break;

          default:
            assert(false);
            return -2;
          }
        }
        // fallthrough

      case re2::RE2::SM::kMatch:
        match_text = std::string(text + state.match_offset(), state.match_length());
        printf(
          "match id: %d at %" PRIu64 ":%" PRIu64 " '%s' { 0x%02x; 0x%02x }\n",
          state.match_id(),
          state.match_offset(),
          state.match_end_offset(),
          match_text.c_str(),
          state.match_last_char(),
          state.match_next_char()
        );

        print_re2_sm_submatches(sm, state.match_id(), match_text);
        printf("keep searching...\n");
        p = text + state.match_end_offset();
        break;

      default:
        assert(false);
        return -3;
      }
    }
  } while (0);

  return 0;
}
