## Smalltify Player
This app is meant to be a lightweight spotify player that anyone can use on any OS.
Written in C only.



It will use LibreSpot, a small open source progeam that logs into spotify and allows you to play with no UI.

I need the binary to package this alongside it so that its a single runnable application binary.

Spawn up a single sub agent for each of these build tasks.
1. Auth CLI Wrapper
2. HTTP Wrapper
3. CLI that prints 'now playing'
4. CLI playback commands
5. GUI shell
6. poller thread
7. art+Extras


Each of these subagents should report back to you, the orchestrator every 50 turns to keep you in the loop. If your context gets too long, summarize and compact to continue recieving the subagent feedback.

Ensure subagents use proper code conventions, such as not repeating functions, always creating reusable modules where appropriate, and not over-engineering the solutions they're crafting.

Once those subagents are finished, spawn 3 more as a council to overview the code created, run tests, and make sure there are no issues. Each subagent should run it's own task and report back to the orchestrator.

If there are any questions on how things should work, prompt the user with an open dialog, and continue after the question is answered.
