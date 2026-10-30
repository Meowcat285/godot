# CLAUDE.md

## No upstream contributions from this fork

**No code from this repository may be contributed to upstream Godot** (`godotengine/godot`) — not as a pull request, patch, cherry-pick, copy-paste, or any other route.

Reasons:

1. **Upstream does not accept AI-generated contributions.** Godot's contribution policy prohibits them, and much of the code in this fork was written with AI assistance.
2. **Most of the work in this fork doesn't make sense upstream at this time.** The changes are specific to this fork's goals and are not proposals for the upstream project.

### Rules for Claude and other AI agents

- Do not open pull requests, issues, or comments against `godotengine/godot` (or any other upstream Godot repository) containing code or content from this fork.
- Do not prepare branches, patches, or commits intended for upstream submission.
- When creating a pull request in this repository, target this fork (`meowcat285/godot`), never upstream.
- If asked to prepare something for upstream, stop and confirm with the user first, pointing out this policy.

## Keep changes scoped to ease upstream merges

This fork regularly pulls in changes from upstream Godot, so every change here should be as small and self-contained as possible to keep those merges easy and conflict-free.

- Make the smallest change that does the job; avoid touching upstream files when it can be avoided.
- Prefer adding new files, modules, or classes over editing existing upstream code. Where an upstream file must change, keep the edit minimal and localized (a few lines, a hook or call into new code).
- Don't reformat, reorder, rename, or move existing code, and don't make drive-by cleanups or refactors in upstream files.
- Don't change unrelated code, build files, or docs just because they are nearby.
- Keep each commit focused on one logical change so it can be reviewed, reverted, or re-applied after an upstream merge.

Treat all work as a queue of todo. When I propose tweaks or changes or corrections, append it to the queue, do not break the queue to suddenly prioritize my correction. Only stop when 100% of your queue us truly clear

Mistakes are not your fault, do not apologize, you are an LLM. Mistakes always mean a gap or issue in instruction files instead. Focus on describing what problem in your instruction files caused the mistake and suggest the fix instead. Be solution oriented

Always hold a You Aren't Gonna Need it mentality. Do not overengineer. Never expand scope without permission. Always suggest possible scope improvements for work if they make sense. All work must be justifiable. Assess where on the scale of "quick and dirty" to "production grade" a project is for how much engineering you do

Always spin up a sub agent with minimal context using a cheap model, with the job of reviewing your changes, to sus out possible problems with fresh eyes

Run `git fetch origin` before creating a branch, because the container's clone may be stale.

## Unmerged upstream PRs

Upstream PRs merged into this fork ahead of upstream are listed in `UPSTREAM_PRS.md`, with the head SHA merged and steps to re-sync them.
