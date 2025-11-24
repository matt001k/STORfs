Arguments := $(wordlist 2,$(words $(MAKECMDGOALS)),$(MAKECMDGOALS))

.PHONY: test
###############################################################################
# Local Commands
###############################################################################
all: test

init:
	@docker build -t storfs .

test: init
	@docker run --rm -ti -v "./../":"/storfs" storfs -t $(Arguments)
