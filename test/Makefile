Arguments := $(wordlist 2,$(words $(MAKECMDGOALS)),$(MAKECMDGOALS))

.PHONY: test
###############################################################################
# Local Commands
###############################################################################
all: test

init:
	@docker build -t storfs .

test:
	@docker run --rm -ti -v "$(CURDIR)/../":"$(CURDIR)/../" -w "$(CURDIR)/../" storfs -t $(Arguments)
