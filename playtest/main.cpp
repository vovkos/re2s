#include <stdio.h>
#include <stdlib.h>
#include <string>

#include "re2.h"

int main() {
	printf("main\n");

	const char pattern[] = "hui(int|[a-z]+)";
	const char text[] = "   huiinteger;  ";

	re2::RE2::Options options;
	options.set_longest_match(true);

	re2::RE2 re(pattern, options);
	std::string_view match;
	bool result = re.RE2::PartialMatch(text, re, &match);
	if (!result)
		printf("not found\n");
	else
		printf("found '%s' at %d\n", std::string(match).c_str(), match.data() - text);

	return 0;
}
