# Moonlight fork development workflow

## Branch roles

- `development` — maintained integration branch and **only** branch that
  publishes a GPG-signed Flatpak OSTree repository and update index to the
  existing Cloudflare R2 `flatpak/` prefix. New commits on this branch
  automatically build and publish OTA releases after a successful build.
- `feature/<topic>` — isolated work from `development`, for implementation and
  testing. Builds a downloadable GitHub Actions Flatpak artifact on every
  push, **without** writing to the R2 OTA repository.
- `master` — original fork default/upstream-tracking branch; it is not the
  development integration branch and is not the OTA release channel.
- Legacy feature branches remain available for reference; create new features
  from the latest `development` rather than from legacy feature branches.

## Workflow

1. Create a new `feature/<topic>` branch from `development`.
2. Make and test changes on that branch. Inspect the actual GitHub Actions
   build result and test Moonlight on SteamOS hardware before promotion.
3. Open a PR targeting `development`. Review the diff and compatibility with
   custom VRR capture, telemetry, input handling, image adjustments,
   Sunshine remote settings, and the update UI. Resolve any conflicts
   contextually; do not blindly choose a merge side.
4. Merge **only after explicit approval and successful testing**. Preserve
   meaningful commit history; use merge commits for upstream synchronization.
5. A push to `development` triggers a signed OTA Flatpak build. The publisher
   updates OSTree content objects before signing/publishing summary and
   finally updates `moonlight-latest.json`. The Flatpak app ID and its OSTree
   branch remain `com.moonlight_stream.Moonlight` and `master` for existing
   installations — distinct from this Git branch name.
6. Confirm GitHub Actions compilation, the signed R2 repository and GitHub
   bundle artifact before considering a promotion deployed. OTA availability
   does not prove an install succeeded on a SteamOS device.

## Review and release policy

Keep PR merges into `development` manual and review the full diff first.
GitHub branch protection or rulesets can be enabled to require reviews and
successful test builds before promotion, but do not protect the `master`
upstream reference branch instead. The integration branch itself may receive
reviewed upstream merges from scheduled weekly maintenance, which automatically
create the next signed OTA update when publishing succeeds.

## Scheduled upstream maintenance

The weekly "Weekly Moonlight fork maintenance" task targets `development`
only. It reviews and merges original upstream `moonlight-stream/moonlight-qt`
master into `development`, updates the customized
`mspeedo/moonlight-common-c` fork if needed, keeps the gitlink and Flatpak
manifest commit pins synchronized, triggers a `development` build and verifies
its final results. It **never** auto-merges feature branches.

## OTA safety

The sole production update endpoint is
`https://pub-3a75f66f021b46f3a095b41d1a3c553b.r2.dev/flatpak/`.
Do not move or delete signed OSTree objects or switch GPG keys without a
migration plan. Do not enable R2 publication from feature branches. Release
publication is gated on successful compilation and repository signing.
