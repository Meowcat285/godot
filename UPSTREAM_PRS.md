# Unmerged upstream PRs carried in this fork

These are `godotengine/godot` pull requests that are not yet merged upstream
(slated for 4.9, on hold during the 4.8 beta feature freeze) but have been
merged into `slop` early. Each was merged as-is (no fork-side edits) with a
`Merge upstream PR godotengine/godot#<number>` merge commit.

| PR | Title | Merged head SHA | Merged into fork |
| --- | --- | --- | --- |
| [#124862](https://github.com/godotengine/godot/pull/124862) | Fix AnimationTree edge delta calculation | `4d2c29d58e706859cdfcebc583e1623fd2226174` | 2026-10-29 |
| [#124897](https://github.com/godotengine/godot/pull/124897) | Reorder directional light shadow checks in culling code to early out on cheap checks and avoid expensive checks altogether | `3998b9d6f1144dcfe98464115459c38b5fa4745d` | 2026-10-29 |
| [#108586](https://github.com/godotengine/godot/pull/108586) | Close scene tab when file is removed | `61fa347213872befcdad572d203489aeab81d458` | 2026-10-29 |
| [#122337](https://github.com/godotengine/godot/pull/122337) | Exclude `EngineDebugger` and related code from release builds | `625b5c9193d2e760c7ca66951cb13dd3b486e52e` | 2026-10-29 |
| [#111541](https://github.com/godotengine/godot/pull/111541) | Add missing `NodePath.Slice()` method to the C# API | `420d3f6e431bc12779b32d75897616ae13067abd` | 2026-10-29 |
| [#117613](https://github.com/godotengine/godot/pull/117613) | Add option to create new material from shader | `c94e44f82cd6a831f0d231b4ebca903b3ede009d` | 2026-10-29 |
| [#118532](https://github.com/godotengine/godot/pull/118532) | Make Camera3D inspector preview match viewport project settings | `f7f8061a733df9385890fe20b5c573d661a12d18` | 2026-10-29 |
| [#116392](https://github.com/godotengine/godot/pull/116392) | Add curve auto-tangent button for Path2D and Path3D | `ed8fb928f2f7ed2f39c642cc120b58cb7d649fc3` | 2026-10-29 |
| [#106794](https://github.com/godotengine/godot/pull/106794) | Add multi-node 3D collision based snapping | `244d8b40a7bb3595c902068df9b38cd07d07af30` | 2026-10-29 |

## Re-syncing

For each PR, fetch its current head and compare with the recorded SHA:

```sh
git fetch https://github.com/godotengine/godot pull/<number>/head:up-pr-<number>
git log --oneline <recorded-sha>..up-pr-<number>   # new commits since last sync
```

- **New commits, PR still open:** contributors usually force-push, so the old
  head may not be an ancestor. Merge the new head (`git merge up-pr-<number>`),
  resolve any conflicts against what was merged before, then update the SHA
  and date in the table.
- **PR merged upstream:** it arrives with the next upstream `master` merge.
  Remove its row once that merge lands in `slop`. If the upstream version
  differs from what was carried here, the master merge may conflict; prefer
  the upstream version.
- **PR closed without merging:** decide with the maintainer whether to keep or
  revert it, and note the decision here.
