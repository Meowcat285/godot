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
