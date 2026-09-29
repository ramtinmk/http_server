---
plan_id: production-http-server-phase0-overload
title: Phase 0 overload policy implementation
category: implementation
status: done
owner: agent
created: 2026-09-29
updated: 2026-09-29
related: [production-http-server]
---

# Phase 0 overload policy implementation

## Purpose

Replace listener-disable starvation with bounded accept-and-reject behavior so
clients above the active-connection cap receive a complete `503` when the
nonblocking socket accepts it, or are refused promptly when it cannot.

## Scope

In: `src/event_loop.c`, the accept-batch limit in `include/server_config.h`,
overload integration evidence in `scripts/saturation_test.py`, and the related
architecture, environment, gotcha, and user documentation.

Out: runtime timeout/keep-alive configuration, graceful drain, `sd_notify`,
general configuration parsing, and structured logging; those remain separate
Phase 0 work items.

## Steps

1. [x] Update the saturation harness to accept the documented bounded overload
   outcomes and verify listener interest is not disabled.
2. [x] Limit each listener dispatch to a configured accept batch, continue
   accepting above capacity, and promptly return `503` or close.
3. [x] Document accept-queue behavior and update the Phase 0 progress checklist.
4. [x] Build and run the saturation E2E harness; retain its JSON artifact.

## Validation

- [x] `make`
- [x] `python3 scripts/saturation_test.py --capacity 16`
- [x] The artifact reports no client timeouts, nonzero overload rejections,
  `listener_disabled_count == 0`, and clean post-drain connection/buffer gauges.

## Exit criteria

- [x] Above-capacity clients get a complete 503 or a prompt refusal; no client
  waits until its timeout and the listener is never disabled for capacity.
- [x] The existing capacity bound remains enforced and the E2E artifact records
  the bounded result.

## Risks and rollback

- A large accept/reject burst can monopolize a loop → cap work per listener
  dispatch and let level-triggered epoll schedule another pass.
- A partial nonblocking 503 can be truncated → count it as a failed response
  and reset promptly; clients still receive a bounded refusal. Revert the accept
  policy independently if saturation evidence regresses.
