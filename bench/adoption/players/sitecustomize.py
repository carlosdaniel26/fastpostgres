"""The adopted suite predates Python 3.13's end of implicitly created loops.

MagicStack's Python runner (2023) opens with
`set_event_loop_policy(uvloop.EventLoopPolicy())` and then
`asyncio.get_event_loop()`, which is how an asyncio program got its loop then;
uvloop 0.21 and later raise instead, and the suite's async cells would fail
before measuring anything.

This module is put on `PYTHONPATH` by `bench/jobs/adoption.sh`, so it is scoped
to this one class and is visible in the tree rather than baked into the image.
It restores the loop creation for that single call - so the suite's cells run on
uvloop, the loop they were built around, rather than being made slower by the
environment - and patches nothing else.
"""
import uvloop

_policy_get_event_loop = uvloop.EventLoopPolicy.get_event_loop


def get_event_loop(self):
    try:
        return _policy_get_event_loop(self)
    except RuntimeError:
        loop = uvloop.new_event_loop()
        self.set_event_loop(loop)
        return loop


uvloop.EventLoopPolicy.get_event_loop = get_event_loop
