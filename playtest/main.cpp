#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string>

#include "re2.h"
#include "re2/sm.h"
#include "re2/set.h"

int main() {
	printf("main\n");

	re2::RE2::Options options;
	options.set_longest_match(true);
/*
	const char* patterns[] = {
		"a?b+c*",
		"i*n+t?",
		"[a-z]+"
	};

	RE2::Set set(options, re2::RE2::UNANCHORED);
	std::string error;

	for (size_t i = 0; i < sizeof(patterns) / sizeof(patterns[0]); i++) {
		int k = set.Add(patterns[i], &error);
		printf("%s -> %d\n", patterns[i], k);
	}

	bool result = set.Compile();
	assert(result);

	const char* text = "abc";

	std::vector<int> v;
	result = set.Match(text, &v);
	assert(result);

	size_t count = v.size();
	for (size_t i = 0; i < count; i++)
		printf("matched: %d\n", v[i]);
*/

	const char pattern[] = "^hui(int|[a-z]+)";
	const char text[] = "   \nhuiinteger; \nhuiabc; \nhuiint; ";

	re2::RE2 re(pattern, options);
	absl::string_view match;
	bool result = re.RE2::PartialMatch(text, re, &match);
	if (!result)
		printf("not found\n");
	else
		printf("found '%s' at %zd\n", std::string(match).c_str(), match.data() - text);
/*
	re2::RE2::SM sm(pattern, options);
	re2::RE2::SM::State state;

	const char* p = text;
	const char* eof = text + sizeof(text) - 1;
	const char* next;
	while (p < eof) {
		if (p + 1 == eof)
			state.set_eof();

		re2::RE2::SM::ExecResult result = sm.exec(absl::string_view(p, 1), &state);
		switch (result) {
		case re2::RE2::SM::kContinue:
			p += state.consumed_size();
			break;

		case re2::RE2::SM::kMismatch:
		  printf("mismatch\n");
			return -1;

		case re2::RE2::SM::kMatch:
		  printf(
				"forward-match @%zd: '%s'\n",
				state.match_start_offset(),
				std::string(text + state.match_start_offset(), state.match_length()).c_str()
			);

			p += state.consumed_size();
			break; // keep searching forward

		case re2::RE2::SM::kReverse:
			printf("end-of-match @%zd\n", state.match_end_offset());

			next = p + state.consumed_size();
			while (p > text) {
				re2::RE2::SM::ExecResult result = sm.exec(absl::string_view(p - 1, 1), &state);
				switch (result) {
				case re2::RE2::SM::kContinue:
					p -= state.consumed_size();
					break;

				case re2::RE2::SM::kMatch:
					printf(
						"reverse-match @%zd: '%s'\n",
						state.match_start_offset(),
						std::string(text + state.match_start_offset(), state.match_length()).c_str()
					);
					p = text; // break out of the parent while loop
					break;

				default:
					assert(false && "unexpected RE2::SM::exec result");
					return -2;
				}
			}

			p = next; // keep searching forward
			break;

		default:
			assert(false && "unexpected RE2::SM::exec result");
			return -3;
		}
	}

*/

	return 0;
}
